#include "ui/CrashReport.h"

#include "core/Log.h"
#include "ui/Resource.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace crash {

namespace {

constexpr int kLinesPerLog = 150;
constexpr int kIdText = 100, kIdCopy = 101, kIdFolder = 102, kIdClose = 103;

std::wstring Hex(DWORD value) {
    wchar_t text[16];
    swprintf_s(text, L"0x%08X", value);
    return text;
}

std::wstring Widen(const std::string& bytes) {
    if (bytes.empty()) return {};
    // The mods write UTF-8 or the ANSI code page; try the first strictly.
    for (UINT page : { (UINT)CP_UTF8, (UINT)CP_ACP }) {
        const DWORD flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int size = ::MultiByteToWideChar(page, flags, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (size <= 0) continue;
        std::wstring out((size_t)size, L'\0');
        ::MultiByteToWideChar(page, flags, bytes.data(), (int)bytes.size(), out.data(), size);
        return out;
    }
    return {};
}

// The file's bytes, even while the writer still has it open.
bool ReadShared(const std::wstring& path, std::string& out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    ::GetFileSizeEx(file, &size);
    // The end is what matters; a few MB of an old log are not.
    const LONGLONG keep = std::min<LONGLONG>(size.QuadPart, 2 * 1024 * 1024);
    LARGE_INTEGER at{};
    at.QuadPart = size.QuadPart - keep;
    ::SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    out.assign((size_t)keep, '\0');
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, out.data(), (DWORD)keep, &read, nullptr);
    out.resize(read);
    ::CloseHandle(file);
    return ok != FALSE;
}

std::wstring Tail(const std::wstring& text, int lines) {
    size_t at = text.size();
    int found = 0;
    while (at > 0) {
        const size_t newline = text.rfind(L'\n', at - 1);
        if (newline == std::wstring::npos) { at = 0; break; }
        if (++found > lines) { at = newline + 1; break; }
        at = newline;
    }
    std::wstring tail = text.substr(at);
    if (at > 0) tail = L"[... earlier lines left out ...]\r\n" + tail;
    return tail;
}

// Every line ending in CRLF, as an edit control wants.
std::wstring Crlf(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 32);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') continue;
        if (text[i] == L'\n') out += L"\r\n";
        else out += text[i];
    }
    return out;
}

// What the command prints, run without a window.
std::string Capture(std::wstring command) {
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE read = nullptr, write = nullptr;
    if (!::CreatePipe(&read, &write, &security, 0)) return {};
    ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                          nullptr, &startup, &process);
    ::CloseHandle(write);
    std::string out;
    if (started) {
        char buffer[4096];
        DWORD got = 0;
        while (::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) out.append(buffer, got);
        ::WaitForSingleObject(process.hProcess, 5000);
        ::CloseHandle(process.hProcess);
        ::CloseHandle(process.hThread);
    }
    ::CloseHandle(read);
    return out;
}

// The reports Windows logged in the last few minutes about the game:
// "Application Error" (faulting module, its version and the offset) and
// "Windows Error Reporting".
std::wstring WindowsReport() {
    const std::string text = Capture(
        L"wevtutil.exe qe Application /rd:true /c:12 /f:text "
        L"/q:\"*[System[(EventID=1000 or EventID=1001 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 600000]]]\"");
    const std::wstring all = Widen(text);
    // Events are separated by "Event[n]:"; keep the ones naming the game.
    std::wstring out;
    size_t at = 0;
    while ((at = all.find(L"Event[", at)) != std::wstring::npos) {
        size_t next = all.find(L"Event[", at + 6);
        const std::wstring event = all.substr(at, next == std::wstring::npos ? std::wstring::npos : next - at);
        std::wstring upper = event;
        for (wchar_t& c : upper) c = (wchar_t)towupper(c);
        if (upper.find(L"SPEED2") != std::wstring::npos) out += event + L"\n";
        if (next == std::wstring::npos) break;
        at = next;
    }
    return out;
}

bool Newer(const FILETIME& a, const FILETIME& b) { return ::CompareFileTime(&a, &b) >= 0; }

std::wstring Folder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring EditorLogPath() {
    wchar_t module[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    return Folder(module) + L"\\FRSWorldEditor.log";
}

std::wstring ReportsFolder() {
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
    std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\crashes";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

// ---- the window ------------------------------------------------------------

struct Window {
    std::wstring text, savedPath, gameFolder;
    HWND edit = nullptr;
    HFONT mono = nullptr, ui = nullptr;
    HBRUSH background = nullptr;
};

void Layout(HWND window, Window& w) {
    RECT client{};
    ::GetClientRect(window, &client);
    const int pad = 10, buttonH = 30, buttonW = 150;
    ::MoveWindow(w.edit, pad, pad, client.right - 2 * pad, client.bottom - 3 * pad - buttonH, TRUE);
    int x = pad;
    for (int id : { kIdCopy, kIdFolder }) {
        ::MoveWindow(::GetDlgItem(window, id), x, client.bottom - pad - buttonH, buttonW, buttonH, TRUE);
        x += buttonW + pad;
    }
    ::MoveWindow(::GetDlgItem(window, kIdClose), client.right - pad - buttonW, client.bottom - pad - buttonH, buttonW,
                 buttonH, TRUE);
}

LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* w = (Window*)::GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
        case WM_CREATE: {
            w = (Window*)((CREATESTRUCTW*)lParam)->lpCreateParams;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)w);
            w->mono = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                    FIXED_PITCH | FF_MODERN, L"Consolas");
            w->ui = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                  L"Segoe UI");
            w->background = ::CreateSolidBrush(RGB(30, 30, 33));
            w->edit = ::CreateWindowExW(0, L"EDIT", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                            ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                        0, 0, 10, 10, window, (HMENU)(INT_PTR)kIdText, nullptr, nullptr);
            ::SendMessageW(w->edit, WM_SETFONT, (WPARAM)w->mono, TRUE);
            ::SendMessageW(w->edit, EM_SETLIMITTEXT, 0, 0);
            ::SetWindowTextW(w->edit, w->text.c_str());
            const struct { int id; const wchar_t* label; } buttons[] = {
                { kIdCopy, L"Copy all" }, { kIdFolder, L"Open the game's logs" }, { kIdClose, L"Close" } };
            for (const auto& b : buttons) {
                HWND button = ::CreateWindowExW(0, L"BUTTON", b.label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10,
                                                10, window, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
                ::SendMessageW(button, WM_SETFONT, (WPARAM)w->ui, TRUE);
            }
            Layout(window, *w);
            return 0;
        }
        case WM_SIZE:
            if (w) Layout(window, *w);
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lParam)->ptMinTrackSize = { 560, 320 };
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if (w && (HWND)lParam == w->edit) {
                ::SetTextColor((HDC)wParam, RGB(220, 220, 220));
                ::SetBkColor((HDC)wParam, RGB(30, 30, 33));
                return (LRESULT)w->background;
            }
            break;
        case WM_ERASEBKGND:
            if (w) {
                RECT client{};
                ::GetClientRect(window, &client);
                HBRUSH frame = ::CreateSolidBrush(RGB(24, 24, 27));
                ::FillRect((HDC)wParam, &client, frame);
                ::DeleteObject(frame);
                return 1;
            }
            break;
        case WM_COMMAND:
            if (!w) break;
            switch (LOWORD(wParam)) {
                case kIdCopy:
                    if (::OpenClipboard(window)) {
                        ::EmptyClipboard();
                        const size_t bytes = (w->text.size() + 1) * sizeof(wchar_t);
                        if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                            std::memcpy(::GlobalLock(memory), w->text.c_str(), bytes);
                            ::GlobalUnlock(memory);
                            ::SetClipboardData(CF_UNICODETEXT, memory);
                        }
                        ::CloseClipboard();
                    }
                    return 0;
                case kIdFolder:
                    // The mods write their logs beside themselves.
                    if (!w->gameFolder.empty())
                        ::ShellExecuteW(window, L"open", (w->gameFolder + L"\\scripts").c_str(), nullptr, nullptr,
                                        SW_SHOWNORMAL);
                    return 0;
                case kIdClose:
                    ::DestroyWindow(window);
                    return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { ::DestroyWindow(window); return 0; }
            break;
        case WM_NCDESTROY:
            if (w) {
                ::DeleteObject(w->mono);
                ::DeleteObject(w->ui);
                ::DeleteObject(w->background);
                delete w;
            }
            return 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

std::wstring ExitCodeName(DWORD code) {
    switch (code) {
        case 0xC0000005: return L"ACCESS_VIOLATION (read or write of memory it does not own)";
        case 0xC00000FD: return L"STACK_OVERFLOW (endless recursion, often two hooks calling each other)";
        case 0xC000001D: return L"ILLEGAL_INSTRUCTION";
        case 0xC0000094: return L"INTEGER_DIVIDE_BY_ZERO";
        case 0xC0000096: return L"PRIVILEGED_INSTRUCTION";
        case 0xC0000409: return L"STACK_BUFFER_OVERRUN / fast fail";
        case 0xC0000374: return L"HEAP_CORRUPTION";
        case 0xC0000135: return L"DLL_NOT_FOUND";
        case 0xC0000142: return L"DLL_INIT_FAILED";
        case 0xC000008E: return L"FLOAT_DIVIDE_BY_ZERO";
        case 0xC0000090: return L"FLOAT_INVALID_OPERATION";
        case 0xC0000006: return L"IN_PAGE_ERROR (the file it was reading went away)";
        case 0x80000003: return L"BREAKPOINT";
        case 0x40000015: return L"FATAL_APP_EXIT";
        case 0xE06D7363: return L"C++ exception not caught";
        case 3:          return L"abort()";
        default:         return IsCrash(code) ? L"an exception" : L"";
    }
}

bool IsCrash(DWORD code) { return (code & 0xC0000000u) == 0xC0000000u || code == 0x80000003u || code == 0xE06D7363u; }

std::wstring BuildReport(const std::wstring& gameFolder, const FILETIME& launched, DWORD exitCode) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    FILETIME nowFile{};
    ::GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = launched.dwLowDateTime; a.HighPart = launched.dwHighDateTime;
    b.LowPart = nowFile.dwLowDateTime; b.HighPart = nowFile.dwHighDateTime;
    const double seconds = b.QuadPart > a.QuadPart ? (double)(b.QuadPart - a.QuadPart) / 1e7 : 0.0;

    wchar_t header[512];
    swprintf_s(header,
               L"FRS World Editor -- game crash report, %04u-%02u-%02u %02u:%02u:%02u\n"
               L"The game (SPEED2.EXE) ended on its own after %.0f s with exit code %s%s%s.\n"
               L"Game folder: %s\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, seconds, Hex(exitCode).c_str(),
               ExitCodeName(exitCode).empty() ? L"" : L" = ", ExitCodeName(exitCode).c_str(), gameFolder.c_str());
    std::wstring report = header;

    const std::wstring windows = WindowsReport();
    report += L"\n==================== Windows error report ====================\n";
    report += windows.empty() ? L"(none logged in the last 10 minutes)\n" : windows;

    // Every log in the game folder and its scripts folder written since the
    // launch: the mods that were running, and what they said last.
    FILETIME since = launched;
    ULARGE_INTEGER margin{};
    margin.LowPart = since.dwLowDateTime; margin.HighPart = since.dwHighDateTime;
    margin.QuadPart -= 50000000ull;   // 5 s
    since.dwLowDateTime = margin.LowPart; since.dwHighDateTime = margin.HighPart;
    std::vector<std::wstring> logs, older;
    for (const std::wstring& folder : { gameFolder, gameFolder + L"\\scripts" }) {
        for (const wchar_t* pattern : { L"\\*.log", L"\\*.txt" }) {
            WIN32_FIND_DATAW found{};
            HANDLE search = ::FindFirstFileW((folder + pattern).c_str(), &found);
            if (search == INVALID_HANDLE_VALUE) continue;
            do {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring path = folder + L"\\" + found.cFileName;
                const bool isLog = std::wstring(pattern).find(L".log") != std::wstring::npos;
                if (Newer(found.ftLastWriteTime, since)) logs.push_back(path);
                else if (isLog) older.push_back(found.cFileName);
            } while (::FindNextFileW(search, &found));
            ::FindClose(search);
        }
    }
    for (const std::wstring& path : logs) {
        std::string bytes;
        report += L"\n==================== " + path + L" (last lines) ====================\n";
        report += ReadShared(path, bytes) ? Tail(Widen(bytes), kLinesPerLog) : L"(could not be read)";
        report += L"\n";
    }
    if (logs.empty()) report += L"\n(no log in the game folder was written during this run)\n";
    if (!older.empty()) {
        report += L"\nLogs not touched by this run: ";
        for (size_t i = 0; i < older.size(); ++i) report += (i ? L", " : L"") + older[i];
        report += L"\n";
    }

    std::string editor;
    report += L"\n==================== FRS World Editor log (last lines) ====================\n";
    report += ReadShared(EditorLogPath(), editor) ? Tail(Widen(editor), 80) : L"(could not be read)";
    return Crlf(report);
}

void Show(HWND owner, const std::wstring& report, const std::wstring& gameFolder) {
    auto* w = new Window;
    w->text = report;
    w->gameFolder = gameFolder;

    // Saved first, so it survives whatever happens next.
    const std::wstring folder = ReportsFolder();
    if (!folder.empty()) {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        wchar_t name[64];
        swprintf_s(name, L"\\crash-%04u%02u%02u-%02u%02u%02u.txt", now.wYear, now.wMonth, now.wDay, now.wHour,
                   now.wMinute, now.wSecond);
        w->savedPath = folder + name;
        FILE* file = nullptr;
        if (_wfopen_s(&file, w->savedPath.c_str(), L"wb") == 0 && file) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            std::fwrite(bom, 1, 3, file);
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), nullptr, 0, nullptr,
                                                   nullptr);
            std::string utf8((size_t)size, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), utf8.data(), size, nullptr, nullptr);
            std::fwrite(utf8.data(), 1, utf8.size(), file);
            std::fclose(file);
            LOG_WARN("game crash: report saved to %S", w->savedPath.c_str());
            w->text = L"Saved as " + w->savedPath + L"\r\n\r\n" + w->text;
        } else {
            w->savedPath.clear();
        }
    }

    static bool registered = false;
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = Procedure;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"FRSCrashReport";
        wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
        registered = ::RegisterClassExW(&wc) != 0;
    }
    HWND window = ::CreateWindowExW(0, L"FRSCrashReport", L"The game crashed -- logs for the diagnosis",
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720, owner, nullptr,
                                    instance, w);
    LOG_INFO("crash window %p (error %lu, thread %lu)", (void*)window, window ? 0ul : ::GetLastError(),
             ::GetCurrentThreadId());
    if (!window) { delete w; return; }
    ::ShowWindow(window, SW_SHOWNORMAL);
    ::SetForegroundWindow(window);
}


} // namespace crash

#include "ui/EmbeddedGame.h"

#include "core/Log.h"

#include <shellapi.h>

namespace {

struct Search { DWORD processId; HWND found; };

// The game's main window: visible, unowned and big enough to be the one the
// game draws into rather than a splash or a console.
BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) {
    auto* search = (Search*)parameter;
    DWORD processId = 0;
    ::GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !::IsWindowVisible(window) || ::GetWindow(window, GW_OWNER))
        return TRUE;
    RECT client{};
    ::GetClientRect(window, &client);
    if (client.right < 320 || client.bottom < 200) return TRUE;
    search->found = window;
    return FALSE;
}

} // namespace

bool EmbeddedGame::Launch(const std::wstring& gameFolder, HWND owner) {
    Release();
    const std::wstring executable = gameFolder + L"\\SPEED2.EXE";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = executable.c_str();
    // From its own folder: the game looks for its data relative to it.
    info.lpDirectory = gameFolder.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&info) || !info.hProcess) return false;
    process_ = info.hProcess;
    processId_ = ::GetProcessId(process_);
    owner_ = owner;
    shown_ = true;
    fullscreen_ = false;
    LOG_INFO("game started, process %lu", processId_);
    return true;
}

bool EmbeddedGame::Running() const {
    return process_ && ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool EmbeddedGame::Adopt(HWND window) {
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
    // A window without a frame that covers its monitor is the fullscreen game:
    // its device owns the screen and cannot be moved into the viewport.
    MONITORINFO monitor{ sizeof(monitor) };
    ::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT rect{};
    ::GetWindowRect(window, &rect);
    if (!(style & WS_CAPTION) && ::EqualRect(&rect, &monitor.rcMonitor)) {
        fullscreen_ = true;
        LOG_WARN("game: it runs fullscreen, so it stays out of the viewport (set a windowed mode)");
        return false;
    }
    style_ = style;
    ::SetWindowLongPtrW(window, GWL_STYLE,
                        (style & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
                                             WS_MAXIMIZEBOX)) | WS_POPUP);
    // The owner, across processes: the game stays over the editor and follows
    // it when it is minimised or hidden.
    ::SetWindowLongPtrW(window, GWLP_HWNDPARENT, (LONG_PTR)owner_);
    window_ = window;
    placed_ = RECT{};
    LOG_INFO("game: window %p taken over the viewport", (void*)window);
    return true;
}

void EmbeddedGame::Update(const RECT& area) {
    if (!Running()) return;
    if (!window_) {
        if (fullscreen_) return;
        Search search{ processId_, nullptr };
        ::EnumWindows(&FindGameWindow, (LPARAM)&search);
        if (!search.found || !Adopt(search.found)) return;
    }
    if (!::IsWindow(window_)) {   // the game made a new window (a device reset)
        window_ = nullptr;
        return;
    }
    if (!shown_ || ::EqualRect(&area, &placed_)) return;
    ::SetWindowPos(window_, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    placed_ = area;
}

void EmbeddedGame::SetShown(bool shown) {
    shown_ = shown;
    if (!window_) return;
    ::ShowWindow(window_, shown ? SW_SHOWNA : SW_HIDE);
    placed_ = RECT{};   // placed again on the next update
}

void EmbeddedGame::Release() {
    if (window_ && ::IsWindow(window_)) {
        ::SetWindowLongPtrW(window_, GWLP_HWNDPARENT, 0);
        ::SetWindowLongPtrW(window_, GWL_STYLE, style_);
        ::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                       SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(window_, SW_SHOWNA);
    }
    window_ = nullptr;
    if (process_) ::CloseHandle(process_);
    process_ = nullptr;
    processId_ = 0;
}

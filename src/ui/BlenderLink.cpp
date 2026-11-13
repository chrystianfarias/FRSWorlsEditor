#include "ui/BlenderLink.h"

#include "core/Log.h"
#include "ui/Resource.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdio>
#include <cwchar>

#pragma comment(lib, "shlwapi.lib")

namespace {

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

// The association usually names blender-launcher.exe, which exists only to
// start blender.exe without a console flashing up. Going to blender.exe
// directly keeps the process handle the one that stays open with the window.
std::wstring PreferBlenderExe(const std::wstring& path) {
    const std::wstring direct = DirectoryOf(path) + L"\\blender.exe";
    return FileExists(direct) ? direct : path;
}

std::wstring StoredBlender(const wchar_t* settingsKey) {
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = sizeof(buffer);
    if (::RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"BlenderPath", RRF_RT_REG_SZ, nullptr,
                       buffer, &size) != ERROR_SUCCESS)
        return {};
    return FileExists(buffer) ? std::wstring(buffer) : std::wstring{};
}

void StoreBlender(const wchar_t* settingsKey, const std::wstring& path) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, settingsKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS)
        return;
    ::RegSetValueExW(key, L"BlenderPath", 0, REG_SZ, (const BYTE*)path.c_str(),
                     (DWORD)((path.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
}

std::wstring AssociatedBlender() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".blend", L"open", buffer,
                                   &size)))
        return {};
    const std::wstring path = buffer;
    // Only trusted when it really is Blender: a .blend opened by some viewer
    // would start that viewer on an OBJ.
    const std::wstring lower = [&] {
        std::wstring s = path;
        for (wchar_t& c : s) c = (wchar_t)::towlower(c);
        return s;
    }();
    if (lower.find(L"blender") == std::wstring::npos || !FileExists(path)) return {};
    return PreferBlenderExe(path);
}

// "Blender 5.2" -> 5002, so the newest of several installs wins.
int VersionOf(const wchar_t* folder) {
    int major = 0, minor = 0;
    if (::swscanf_s(folder, L"Blender %d.%d", &major, &minor) < 1) return 0;
    return major * 1000 + minor;
}

std::wstring InstalledBlender() {
    wchar_t programFiles[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, programFiles)))
        return {};

    const std::wstring root = std::wstring(programFiles) + L"\\Blender Foundation";
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((root + L"\\Blender*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return {};

    std::wstring best;
    int bestVersion = -1;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring exe = root + L"\\" + found.cFileName + L"\\blender.exe";
        const int version = VersionOf(found.cFileName);
        if (FileExists(exe) && version > bestVersion) {
            best = exe;
            bestVersion = version;
        }
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
    return best;
}

std::wstring AskForBlender(HWND owner, const std::wstring& current = {}) {
    wchar_t path[MAX_PATH] = L"blender.exe";
    if (!current.empty()) ::wcsncpy_s(path, current.c_str(), _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = owner;
    dialog.lpstrFilter = L"Blender (blender.exe)\0blender.exe\0Programs\0*.exe\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"Where is Blender?";
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return ::GetOpenFileNameW(&dialog) ? PreferBlenderExe(path) : std::wstring{};
}

// The script is carried as a resource and written beside the OBJ, so the copy
// Blender runs is always the one this editor was built with.
bool WriteScript(const std::wstring& path) {
    HMODULE module = ::GetModuleHandleW(nullptr);
    HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(IDR_BLENDER_SCRIPT), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = ::LoadResource(module, resource);
    const void* data = loaded ? ::LockResource(loaded) : nullptr;
    const DWORD size = ::SizeofResource(module, resource);
    if (!data || !size) return false;

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = std::fwrite(data, 1, size, file) == size;
    std::fclose(file);
    return ok;
}

bool LastWrite(const std::wstring& path, FILETIME& time) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    time = data.ftLastWriteTime;
    return true;
}

} // namespace

std::wstring BlenderLink::FindBlender(HWND owner, const wchar_t* settingsKey) {
    std::wstring path = StoredBlender(settingsKey);
    if (path.empty()) path = AssociatedBlender();
    if (path.empty()) path = InstalledBlender();
    if (path.empty()) path = AskForBlender(owner);
    if (!path.empty()) StoreBlender(settingsKey, path);
    return path;
}

std::wstring BlenderLink::ChooseBlender(HWND owner, const wchar_t* settingsKey) {
    std::wstring current = StoredBlender(settingsKey);
    if (current.empty()) current = AssociatedBlender();
    if (current.empty()) current = InstalledBlender();
    const std::wstring path = AskForBlender(owner, current);
    if (!path.empty()) StoreBlender(settingsKey, path);
    return path;
}

std::wstring BlenderLink::NewSessionFolder(const std::wstring& label) {
    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
        return {};

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t stamp[32];
    ::_snwprintf_s(stamp, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond);

    const std::wstring folder = std::wstring(localAppData) + L"\\FRSWorldEditor\\quickedit\\" +
                                label + L"-" + stamp;
    const int created = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS) return {};
    return folder;
}

bool BlenderLink::Start(const std::wstring& blender, const std::wstring& objPath,
                        std::wstring& error) {
    Stop();

    const std::wstring script = DirectoryOf(objPath) + L"\\frs_quickedit.py";
    if (!WriteScript(script)) {
        error = L"The Blender script could not be written next to the OBJ.";
        return false;
    }

    std::wstring command = L"\"" + blender + L"\" --python \"" + script + L"\" -- \"" +
                           objPath + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring folder = DirectoryOf(objPath);
    if (!::CreateProcessW(blender.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process)) {
        error = L"Blender could not be started:\n" + blender;
        LOG_ERROR("could not start Blender (%lu): %S", ::GetLastError(), blender.c_str());
        return false;
    }
    ::CloseHandle(process.hThread);

    process_  = process.hProcess;
    objPath_  = objPath;
    lastSent_ = {};
    LastWrite(objPath_ + L".sent", lastSent_);
    LOG_INFO("quick edit: Blender started on %S", objPath.c_str());
    return true;
}

void BlenderLink::Stop() {
    if (process_) ::CloseHandle(process_);
    process_ = nullptr;
    objPath_.clear();
}

bool BlenderLink::Running() const {
    return process_ && ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool BlenderLink::PollSaved() {
    if (objPath_.empty()) return false;
    FILETIME sent{};
    if (!LastWrite(objPath_ + L".sent", sent)) return false;
    if (::CompareFileTime(&sent, &lastSent_) == 0) return false;
    lastSent_ = sent;
    return true;
}

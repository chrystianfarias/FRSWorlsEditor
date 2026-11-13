#include "ui/SetupWizard.h"

#include "core/Log.h"
#include "ui/Resource.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <prsht.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

const wchar_t* kTitle = L"FRS World Editor Setup";
const wchar_t* kSupportUrl = L"https://buymeacoffee.com/chrystianfarias";

// The executable the loader and FRSFastBoot were written against: SPEED2.EXE
// v1.2, 4 800 512 bytes. Any other build still opens in the editor, but the
// plugins leave the game alone.
const char* kSupportedMd5 = "6445adbf4e8d64be51e96a69bba168a4";

// Older copies of what FRSWorldLoader.asi now does on its own. Left running
// beside it they would apply the same edits twice and spawn every car twice.
const wchar_t* const kOldPlugins[] = { L"FRSModelLoader.asi", L"SpawnCar.asi", L"FRSCameraBridge.asi" };

// The names an ASI loader goes by in a game folder.
const wchar_t* const kAsiLoaderNames[] = { L"dinput8.dll", L"dsound.dll", L"winmm.dll",
                                           L"version.dll", L"wininet.dll" };

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Trimmed(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/' || path.back() == L' '))
        path.pop_back();
    while (!path.empty() && path.front() == L' ') path.erase(path.begin());
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
    return path;
}

std::wstring EditorFolder() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder = path;
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : folder.substr(0, slash);
}

// What the release package carries for the game, beside the editor.
std::wstring PayloadFolder() { return EditorFolder() + L"\\game"; }

std::wstring Md5OfFile(const std::wstring& path) {
    std::wstring result;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<unsigned char> buffer(1 << 16);
        DWORD read = 0;
        bool ok = true;
        while (::ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read)
            ok = ok && BCRYPT_SUCCESS(::BCryptHashData(hash, buffer.data(), read, 0));
        unsigned char digest[16] = {};
        if (ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            wchar_t text[33] = {};
            for (int i = 0; i < 16; ++i) ::swprintf_s(text + i * 2, 3, L"%02x", digest[i]);
            result = text;
        }
    }
    if (hash) ::BCryptDestroyHash(hash);
    if (algorithm) ::BCryptCloseAlgorithmProvider(algorithm, 0);
    ::CloseHandle(file);
    return result;
}

bool SupportedExecutable(const std::wstring& folder) {
    const std::wstring md5 = Md5OfFile(folder + L"\\SPEED2.EXE");
    wchar_t expected[33] = {};
    ::MultiByteToWideChar(CP_ACP, 0, kSupportedMd5, -1, expected, 33);
    return md5 == expected;
}

// The ASI loader already in the game folder, or empty.
std::wstring AsiLoaderIn(const std::wstring& folder) {
    for (const wchar_t* name : kAsiLoaderNames)
        if (FileExists(folder + L"\\" + name)) return name;
    return std::wstring();
}

std::vector<std::wstring> OldPluginsIn(const std::wstring& folder) {
    std::vector<std::wstring> found;
    for (const wchar_t* name : kOldPlugins)
        if (FileExists(folder + L"\\scripts\\" + name)) found.push_back(name);
    return found;
}

std::wstring Joined(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (const std::wstring& name : names) out += (out.empty() ? L"" : L", ") + name;
    return out;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

enum class InstallStatus { Ok, AccessDenied, Failed };

struct Installer {
    const SetupChoices& choices;
    std::wstring report;
    InstallStatus status = InstallStatus::Ok;

    void Fail(const std::wstring& what, DWORD error) {
        if (error == ERROR_ACCESS_DENIED) {
            if (status == InstallStatus::Ok) status = InstallStatus::AccessDenied;
        } else {
            status = InstallStatus::Failed;
        }
        wchar_t text[256] = {};
        ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                         text, 256, nullptr);
        std::wstring reason = text;
        while (!reason.empty() && (reason.back() == L'\n' || reason.back() == L'\r')) reason.pop_back();
        if (error == ERROR_SHARING_VIOLATION) reason += L" Close the game and run the setup again.";
        report += L"FAILED: " + what + L" -- " + reason + L"\r\n";
    }

    bool Folder(const std::wstring& path) {
        if (DirectoryExists(path) || ::CreateDirectoryW(path.c_str(), nullptr)) return true;
        Fail(L"creating " + path, ::GetLastError());
        return false;
    }

    // `keep`: an existing file is the player's (their settings) and stays.
    bool Copy(const std::wstring& from, const std::wstring& to, const wchar_t* label, bool keep = false) {
        if (!FileExists(from)) {
            status = InstallStatus::Failed;
            report += std::wstring(L"FAILED: ") + label + L" -- not in the package (" + from + L")\r\n";
            return false;
        }
        if (keep && FileExists(to)) {
            report += std::wstring(L"Kept your ") + label + L"\r\n";
            return true;
        }
        if (!::CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            Fail(std::wstring(L"copying ") + label, ::GetLastError());
            return false;
        }
        report += std::wstring(L"Installed ") + label + L"\r\n";
        return true;
    }
};

bool RunningElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                          elevation.TokenIsElevated;
    ::CloseHandle(token);
    return elevated;
}

// Lets every user of the machine change `path` (and, for a folder, what is
// made in it later): the editor runs without elevation and writes its edits
// to <game>\models, and it turns FRSFastBoot on and off in its .ini.
void GrantUsersModify(const std::wstring& path, bool folder) {
    PSID users = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!::AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS,
                                    0, 0, 0, 0, 0, 0, &users))
        return;
    PACL current = nullptr, updated = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                &current, nullptr, &descriptor) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = folder ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        access.Trustee.ptstrName = (LPWSTR)users;
        if (::SetEntriesInAclW(1, &access, current, &updated) == ERROR_SUCCESS)
            ::SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, updated, nullptr);
    }
    if (updated) ::LocalFree(updated);
    if (descriptor) ::LocalFree(descriptor);
    ::FreeSid(users);
}

InstallStatus Install(const SetupChoices& choices, std::wstring& report) {
    Installer in{ choices };
    const std::wstring game = choices.folder;
    const std::wstring scripts = game + L"\\scripts";
    const std::wstring payload = PayloadFolder();

    if (!in.Folder(scripts) || !in.Folder(game + L"\\models")) {
        report = in.report;
        return in.status;
    }

    const std::wstring loader = AsiLoaderIn(game);
    if (!loader.empty()) {
        in.report += L"Kept the ASI loader already in the game (" + loader + L")\r\n";
    } else if (choices.asiLoader) {
        in.Copy(payload + L"\\dinput8.dll", game + L"\\dinput8.dll", L"Ultimate ASI Loader (dinput8.dll)");
    } else {
        in.report += L"No ASI loader installed: the plugins only load with one\r\n";
    }

    in.Copy(payload + L"\\scripts\\FRSWorldLoader.asi", scripts + L"\\FRSWorldLoader.asi",
            L"FRS World Loader (scripts\\FRSWorldLoader.asi)");
    in.Copy(payload + L"\\scripts\\FRSWorldLoader.ini", scripts + L"\\FRSWorldLoader.ini",
            L"FRSWorldLoader.ini", true);

    if (choices.fastBoot) {
        const bool haveIni = FileExists(FastBootIni(game));
        in.Copy(payload + L"\\scripts\\FRSFastBoot.asi", scripts + L"\\FRSFastBoot.asi",
                L"FRSFastBoot (scripts\\FRSFastBoot.asi)");
        in.Copy(payload + L"\\scripts\\FRSFastBoot.ini", FastBootIni(game), L"FRSFastBoot.ini", true);
        in.Copy(payload + L"\\scripts\\FRSFastBoot-Tracks.txt", scripts + L"\\FRSFastBoot-Tracks.txt",
                L"FRSFastBoot-Tracks.txt");
        if (FileExists(FastBootIni(game))) {
            if (!SetFastBootEnabled(game, choices.fastBootAlways))
                in.Fail(L"setting FRSFastBoot.ini", ::GetLastError());
            else
                in.report += choices.fastBootAlways
                                 ? L"FRSFastBoot: on for every start of the game\r\n"
                                 : L"FRSFastBoot: only when the editor launches the game\r\n";
        }
        (void)haveIni;
    }

    if (choices.disableOld) {
        for (const std::wstring& name : OldPluginsIn(game)) {
            const std::wstring from = scripts + L"\\" + name;
            const std::wstring to = from + L".off";
            if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
                in.report += L"Turned off the older " + name + L" (now " + name + L".off)\r\n";
            else
                in.Fail(L"turning off " + name, ::GetLastError());
        }
    }

    if (RunningElevated()) {
        GrantUsersModify(game + L"\\models", true);
        if (FileExists(FastBootIni(game))) GrantUsersModify(FastBootIni(game), false);
        in.report += L"The editor may now write to models\\ without administrator rights\r\n";
    }

    report = in.report;
    return in.status;
}

// Runs the install again elevated, and waits for it. The report comes back
// through a file.
InstallStatus InstallElevated(const SetupChoices& choices, HWND owner, std::wstring& report) {
    wchar_t temp[MAX_PATH] = {}, reportPath[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    ::GetTempFileNameW(temp, L"frs", 0, reportPath);

    wchar_t executable[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, executable, MAX_PATH);
    wchar_t arguments[2048] = {};
    ::swprintf_s(arguments,
                 L"--install \"%s\" --report \"%s\" --asi-loader %d --fastboot %d --fastboot-always %d "
                 L"--disable-old %d",
                 choices.folder.c_str(), reportPath, choices.asiLoader ? 1 : 0, choices.fastBoot ? 1 : 0,
                 choices.fastBootAlways ? 1 : 0, choices.disableOld ? 1 : 0);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = executable;
    info.lpParameters = arguments;
    info.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&info) || !info.hProcess) {
        report = L"The installation as administrator was cancelled.\r\n";
        ::DeleteFileW(reportPath);
        return InstallStatus::AccessDenied;
    }
    ::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(info.hProcess, &exitCode);
    ::CloseHandle(info.hProcess);

    report.clear();
    FILE* file = nullptr;
    if (::_wfopen_s(&file, reportPath, L"rt, ccs=UTF-8") == 0 && file) {
        wchar_t line[1024];
        while (std::fgetws(line, 1024, file)) {
            std::wstring text = line;
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
            report += text + L"\r\n";
        }
        std::fclose(file);
    }
    ::DeleteFileW(reportPath);
    return exitCode == 0 ? InstallStatus::Ok : InstallStatus::Failed;
}

// ---------------------------------------------------------------------------
// The wizard
// ---------------------------------------------------------------------------

struct Wizard {
    SetupChoices choices;
    bool installed = false;
    bool installOk = false;
    std::wstring report;
};

void SetText(HWND dialog, int id, const std::wstring& text) {
    ::SetDlgItemTextW(dialog, id, text.c_str());
}

std::wstring TextOf(HWND dialog, int id) {
    wchar_t text[MAX_PATH * 2] = {};
    ::GetDlgItemTextW(dialog, id, text, MAX_PATH * 2);
    return text;
}

void Check(HWND dialog, int id, bool on) { ::CheckDlgButton(dialog, id, on ? BST_CHECKED : BST_UNCHECKED); }
bool Checked(HWND dialog, int id) { return ::IsDlgButtonChecked(dialog, id) == BST_CHECKED; }
void Enable(HWND dialog, int id, bool on) { ::EnableWindow(::GetDlgItem(dialog, id), on); }

Wizard* WizardOf(HWND dialog) { return (Wizard*)::GetWindowLongPtrW(dialog, DWLP_USER); }

void Stay(HWND dialog) { ::SetWindowLongPtrW(dialog, DWLP_MSGRESULT, -1); }

// ---- page 1: the game folder

void DescribeFolder(HWND dialog) {
    const std::wstring folder = Trimmed(TextOf(dialog, IDC_SETUP_PATH));
    std::wstring text;
    bool valid = false;
    if (folder.empty()) {
        text = L"Choose the folder that holds SPEED2.EXE.";
    } else if (!FileExists(folder + L"\\SPEED2.EXE")) {
        text = L"There is no SPEED2.EXE in this folder.";
    } else if (!DirectoryExists(folder + L"\\TRACKS")) {
        text = L"SPEED2.EXE is here, but the TRACKS folder is not: the game looks incomplete.";
    } else {
        valid = true;
        text = L"Need for Speed Underground 2 found.\r\n";
        text += SupportedExecutable(folder)
                    ? L"SPEED2.EXE is v1.2, the version the plugins are written for.\r\n"
                    : L"SPEED2.EXE is not v1.2: the editor opens its files, but the plugins leave "
                      L"this game alone (they are written for v1.2).\r\n";
        std::vector<std::wstring> present;
        const std::wstring loader = AsiLoaderIn(folder);
        if (!loader.empty()) present.push_back(L"an ASI loader (" + loader + L")");
        if (WorldLoaderInstalled(folder)) present.push_back(L"FRS World Loader");
        if (FastBootInstalled(folder)) present.push_back(L"FRSFastBoot");
        if (!present.empty()) text += L"Already installed: " + Joined(present) + L".";
    }
    SetText(dialog, IDC_SETUP_FOLDER_STATUS, text);
    PropSheet_SetWizButtons(::GetParent(dialog), valid ? PSWIZB_NEXT : 0);
}

void BrowseForFolder(HWND dialog) {
    IFileOpenDialog* picker = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&picker))))
        return;
    DWORD options = 0;
    picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"The Need for Speed Underground 2 folder (the one with SPEED2.EXE)");
    const std::wstring current = Trimmed(TextOf(dialog, IDC_SETUP_PATH));
    IShellItem* start = nullptr;
    if (!current.empty() &&
        SUCCEEDED(::SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        picker->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(picker->Show(dialog))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetText(dialog, IDC_SETUP_PATH, path);
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    picker->Release();
}

INT_PTR CALLBACK FolderPage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: {
            auto* wizard = (Wizard*)((PROPSHEETPAGEW*)lParam)->lParam;
            ::SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)wizard);
            SetText(dialog, IDC_SETUP_PATH, wizard->choices.folder);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_SETUP_BROWSE) BrowseForFolder(dialog);
            if (LOWORD(wParam) == IDC_SETUP_PATH && HIWORD(wParam) == EN_CHANGE) DescribeFolder(dialog);
            return TRUE;
        case WM_NOTIFY:
            switch (((NMHDR*)lParam)->code) {
                case PSN_SETACTIVE:
                    DescribeFolder(dialog);
                    return TRUE;
                case PSN_WIZNEXT: {
                    const std::wstring folder = Trimmed(TextOf(dialog, IDC_SETUP_PATH));
                    if (!IsGameFolder(folder)) { Stay(dialog); return TRUE; }
                    WizardOf(dialog)->choices.folder = folder;
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

// ---- page 2: the plugins

bool PayloadHas(const wchar_t* relative) { return FileExists(PayloadFolder() + L"\\" + relative); }

void DescribeParts(HWND dialog) {
    Wizard* wizard = WizardOf(dialog);
    const std::wstring& folder = wizard->choices.folder;

    const std::wstring loader = AsiLoaderIn(folder);
    const bool haveLoaderFile = PayloadHas(L"dinput8.dll");
    Check(dialog, IDC_SETUP_ASILOADER, loader.empty() && haveLoaderFile);
    Enable(dialog, IDC_SETUP_ASILOADER, loader.empty() && haveLoaderFile);
    SetText(dialog, IDC_SETUP_ASILOADER_NOTE,
            !loader.empty() ? L"The game already has one (" + loader + L"); it is kept."
            : haveLoaderFile ? std::wstring(L"Loads the plugins below when the game starts. By ThirteenAG, MIT license.")
                             : std::wstring(L"Not in this package: install an ASI loader yourself."));

    Check(dialog, IDC_SETUP_LOADER, true);
    Enable(dialog, IDC_SETUP_LOADER, false);

    const bool haveFastBoot = PayloadHas(L"scripts\\FRSFastBoot.asi");
    Check(dialog, IDC_SETUP_FASTBOOT, haveFastBoot);
    Enable(dialog, IDC_SETUP_FASTBOOT, haveFastBoot);
    Check(dialog, IDC_SETUP_FASTBOOT_ALWAYS, FastBootInstalled(folder) && FastBootEnabled(folder));
    Enable(dialog, IDC_SETUP_FASTBOOT_ALWAYS, haveFastBoot);

    const std::vector<std::wstring> old = OldPluginsIn(folder);
    ::ShowWindow(::GetDlgItem(dialog, IDC_SETUP_DISABLE_OLD), old.empty() ? SW_HIDE : SW_SHOW);
    SetText(dialog, IDC_SETUP_DISABLE_OLD,
            L"Turn off the older " + Joined(old) + L" (renamed to .off): FRS World Loader does "
            L"their work now, and both running would apply everything twice");
    Check(dialog, IDC_SETUP_DISABLE_OLD, !old.empty());

    const bool complete = PayloadHas(L"scripts\\FRSWorldLoader.asi");
    SetText(dialog, IDC_SETUP_PARTS_NOTE,
            complete ? L"Everything goes into the game folder: dinput8.dll beside SPEED2.EXE, the "
                       L"plugins in scripts\\. The game's own files are never changed; the editor's "
                       L"work lives in models\\ and is undone by deleting it."
                     : L"This copy of the editor has no \"game\" folder beside it, so there is nothing "
                       L"to install. Use the release package, or copy the plugins by hand.");
    PropSheet_SetWizButtons(::GetParent(dialog), PSWIZB_BACK | (complete ? PSWIZB_NEXT : 0));
    PropSheet_SetNextText(::GetParent(dialog), L"&Install");
}

INT_PTR CALLBACK PartsPage(HWND dialog, UINT message, WPARAM, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            ::SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)((PROPSHEETPAGEW*)lParam)->lParam);
            return TRUE;
        case WM_NOTIFY:
            switch (((NMHDR*)lParam)->code) {
                case PSN_SETACTIVE:
                    DescribeParts(dialog);
                    return TRUE;
                case PSN_WIZNEXT: {
                    Wizard* wizard = WizardOf(dialog);
                    SetupChoices& choices = wizard->choices;
                    choices.asiLoader = Checked(dialog, IDC_SETUP_ASILOADER);
                    choices.fastBoot = Checked(dialog, IDC_SETUP_FASTBOOT);
                    choices.fastBootAlways = choices.fastBoot && Checked(dialog, IDC_SETUP_FASTBOOT_ALWAYS);
                    choices.disableOld = Checked(dialog, IDC_SETUP_DISABLE_OLD);

                    ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
                    InstallStatus status = Install(choices, wizard->report);
                    if (status == InstallStatus::AccessDenied) {
                        const int answer = ::MessageBoxW(
                            dialog,
                            L"Windows does not let the editor write into the game folder (games under "
                            L"Program Files are protected).\n\nInstall as administrator? The editor will "
                            L"also be allowed to write its edits to the models folder afterwards.",
                            kTitle, MB_ICONQUESTION | MB_YESNO);
                        if (answer == IDYES) status = InstallElevated(choices, dialog, wizard->report);
                    }
                    wizard->installed = true;
                    wizard->installOk = status == InstallStatus::Ok;
                    LOG_INFO("setup: install into %S %s", choices.folder.c_str(),
                             wizard->installOk ? "done" : "incomplete");
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

// ---- page 3: done

INT_PTR CALLBACK DonePage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            ::SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)((PROPSHEETPAGEW*)lParam)->lParam);
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
            // The report is a read-only edit, which Windows paints grey; the
            // page around it is white. Only this page's own background: the
            // wizard's frame asks through here as well.
            if (message == WM_CTLCOLORDLG && (HWND)lParam != dialog) break;
            ::SetBkColor((HDC)wParam, ::GetSysColor(COLOR_WINDOW));
            return (INT_PTR)::GetSysColorBrush(COLOR_WINDOW);
        case WM_NOTIFY: {
            const NMHDR* header = (NMHDR*)lParam;
            if (header->code == PSN_SETACTIVE) {
                Wizard* wizard = WizardOf(dialog);
                std::wstring text = wizard->installOk
                    ? L"All set. The editor opens the game's regions from:\r\n" + wizard->choices.folder +
                          L"\r\n\r\n"
                    : L"The game folder is set, but not everything was installed -- see below. You can "
                      L"run this setup again from File > Game Folder and Plugins.\r\n\r\n";
                text += wizard->report;
                SetText(dialog, IDC_SETUP_RESULT, text);
                PropSheet_SetWizButtons(::GetParent(dialog), PSWIZB_FINISH);
                PropSheet_ShowWizButtons(::GetParent(dialog), 0, PSWIZB_BACK | PSWIZB_CANCEL);
                return TRUE;
            }
            if ((header->code == NM_CLICK || header->code == NM_RETURN) && header->idFrom == IDC_SETUP_LINK) {
                ::ShellExecuteW(dialog, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

PROPSHEETPAGEW Page(HINSTANCE instance, int templateId, DLGPROC procedure, const wchar_t* title,
                    Wizard* wizard) {
    PROPSHEETPAGEW page{};
    page.dwSize = sizeof(page);
    page.dwFlags = PSP_USEHEADERTITLE;
    page.hInstance = instance;
    page.pszTemplate = MAKEINTRESOURCEW(templateId);
    page.pfnDlgProc = procedure;
    page.pszHeaderTitle = title;
    page.lParam = (LPARAM)wizard;
    return page;
}

} // namespace

bool IsGameFolder(const std::wstring& folder) {
    return !folder.empty() && FileExists(folder + L"\\SPEED2.EXE") && DirectoryExists(folder + L"\\TRACKS");
}

std::wstring DetectGameFolder() {
    // The installer's own record, in either view of the registry.
    const wchar_t* keys[] = { L"SOFTWARE\\EA GAMES\\Need for Speed Underground 2",
                              L"SOFTWARE\\Electronic Arts\\Need for Speed Underground 2" };
    const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const DWORD views[] = { KEY_WOW64_32KEY, KEY_WOW64_64KEY };
    for (HKEY root : roots)
        for (const wchar_t* key : keys)
            for (DWORD view : views)
                for (const wchar_t* value : { L"Install Dir", L"InstallDir", L"Install Path" }) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD size = sizeof(path);
                    if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, path, &size) ==
                        ERROR_SUCCESS) {
                        const std::wstring folder = Trimmed(path);
                        if (IsGameFolder(folder)) return folder;
                    }
                }

    // The usual places.
    std::vector<std::wstring> candidates;
    wchar_t buffer[MAX_PATH] = {};
    for (const wchar_t* variable : { L"ProgramFiles(x86)", L"ProgramFiles" })
        if (::GetEnvironmentVariableW(variable, buffer, MAX_PATH))
            for (const wchar_t* tail : { L"\\EA GAMES\\Need for Speed Underground 2",
                                         L"\\Need for Speed Underground 2" })
                candidates.push_back(std::wstring(buffer) + tail);
    for (wchar_t drive = L'C'; drive <= L'H'; ++drive)
        for (const wchar_t* tail : { L":\\Games\\NFSU2", L":\\Games\\Need for Speed Underground 2",
                                     L":\\NFSU2" })
            candidates.push_back(std::wstring(1, drive) + tail);
    for (const std::wstring& folder : candidates)
        if (IsGameFolder(folder)) return folder;
    return std::wstring();
}

bool RunSetupWizard(HINSTANCE instance, HWND owner, std::wstring& folder) {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LINK_CLASS | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&controls);

    Wizard wizard;
    wizard.choices.folder = IsGameFolder(folder) ? folder : DetectGameFolder();

    PROPSHEETPAGEW pages[] = {
        Page(instance, IDD_SETUP_FOLDER, FolderPage, L"Where is Need for Speed Underground 2?", &wizard),
        Page(instance, IDD_SETUP_PARTS, PartsPage, L"Plugins for the game", &wizard),
        Page(instance, IDD_SETUP_DONE, DonePage, L"Setup finished", &wizard),
    };
    HPROPSHEETPAGE handles[3] = {};
    for (int i = 0; i < 3; ++i) handles[i] = ::CreatePropertySheetPageW(&pages[i]);

    PROPSHEETHEADERW header{};
    header.dwSize = sizeof(header);
    header.dwFlags = PSH_WIZARD | PSH_AEROWIZARD | PSH_USEHICON;
    header.hwndParent = owner;
    header.hInstance = instance;
    header.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    header.pszCaption = kTitle;
    header.nPages = 3;
    header.phpage = handles;
    const INT_PTR result = ::PropertySheetW(&header);

    if (result <= 0 || !IsGameFolder(wizard.choices.folder)) return false;
    folder = wizard.choices.folder;
    return true;
}

bool RunInstallCommand(int& exitCode) {
    int count = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (!arguments) return false;
    bool install = false;
    SetupChoices choices;
    std::wstring reportPath;
    for (int i = 1; i < count; ++i) {
        const std::wstring name = arguments[i];
        const wchar_t* value = i + 1 < count ? arguments[i + 1] : L"";
        if (name == L"--install") { install = true; choices.folder = Trimmed(value); ++i; }
        else if (name == L"--report") { reportPath = value; ++i; }
        else if (name == L"--asi-loader") { choices.asiLoader = std::wcstol(value, nullptr, 10) != 0; ++i; }
        else if (name == L"--fastboot") { choices.fastBoot = std::wcstol(value, nullptr, 10) != 0; ++i; }
        else if (name == L"--fastboot-always") { choices.fastBootAlways = std::wcstol(value, nullptr, 10) != 0; ++i; }
        else if (name == L"--disable-old") { choices.disableOld = std::wcstol(value, nullptr, 10) != 0; ++i; }
    }
    ::LocalFree(arguments);
    if (!install) return false;

    std::wstring report;
    const InstallStatus status = IsGameFolder(choices.folder) ? Install(choices, report) : InstallStatus::Failed;
    if (!IsGameFolder(choices.folder)) report = L"FAILED: " + choices.folder + L" is not the game's folder\r\n";
    if (!reportPath.empty()) {
        FILE* file = nullptr;
        if (::_wfopen_s(&file, reportPath.c_str(), L"wt, ccs=UTF-8") == 0 && file) {
            std::wstring text = report;
            for (size_t at = 0; (at = text.find(L"\r\n", at)) != std::wstring::npos;) text.erase(at, 1);
            std::fputws(text.c_str(), file);
            std::fclose(file);
        }
    }
    exitCode = status == InstallStatus::Ok ? 0 : 1;
    return true;
}

std::wstring FastBootIni(const std::wstring& gameFolder) { return gameFolder + L"\\scripts\\FRSFastBoot.ini"; }

bool FastBootInstalled(const std::wstring& gameFolder) {
    return FileExists(gameFolder + L"\\scripts\\FRSFastBoot.asi");
}

bool WorldLoaderInstalled(const std::wstring& gameFolder) {
    return FileExists(gameFolder + L"\\scripts\\FRSWorldLoader.asi");
}

bool FastBootEnabled(const std::wstring& gameFolder) {
    return ::GetPrivateProfileIntW(L"FastBoot", L"Enabled", 1, FastBootIni(gameFolder).c_str()) != 0;
}

bool SetFastBootEnabled(const std::wstring& gameFolder, bool enabled) {
    return ::WritePrivateProfileStringW(L"FastBoot", L"Enabled", enabled ? L"1" : L"0",
                                        FastBootIni(gameFolder).c_str()) != FALSE;
}

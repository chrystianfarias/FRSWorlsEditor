#include "io/WorldProfiles.h"

#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <utility>

namespace io {

namespace {

const wchar_t kDefaultWorld[] = L"Default";
const wchar_t kStoreFolder[]  = L"worlds";
const wchar_t kNameFile[]     = L"world.txt";

// Belong to the game or to the editor rather than to a world: the store
// itself, the open world's name, the pose Launch Game hands the loader, and
// whatever an extension adds (AddSharedWorldFile).
std::vector<std::wstring>& ExtraShared() {
    static std::vector<std::wstring> names;
    return names;
}

bool IsShared(const wchar_t* name) {
    for (const wchar_t* shared : { kStoreFolder, kNameFile, L"launch-pose.txt" })
        if (::_wcsicmp(name, shared) == 0) return true;
    for (const std::wstring& shared : ExtraShared())
        if (::_wcsicmp(name, shared.c_str()) == 0) return true;
    return false;
}

bool Exists(const std::wstring& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

using Moves = std::vector<std::pair<std::wstring, std::wstring>>;

} // namespace

bool IsSharedWorldFile(const wchar_t* name) { return IsShared(name); }

void AddSharedWorldFile(const std::wstring& name) {
    if (!IsShared(name.c_str())) ExtraShared().push_back(name);
}

namespace {

void Undo(Moves& moves) {
    for (auto it = moves.rbegin(); it != moves.rend(); ++it)
        if (!::MoveFileExW(it->second.c_str(), it->first.c_str(), 0))
            LOG_WARN("worlds: could not move %S back to %S", it->second.c_str(), it->first.c_str());
    moves.clear();
}

// Every file and folder of `from` but the shared ones, into `to`.
bool MoveContents(const std::wstring& from, const std::wstring& to, Moves& moves, std::wstring& error) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((from + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!::wcscmp(found.cFileName, L".") || !::wcscmp(found.cFileName, L"..") || IsShared(found.cFileName))
            continue;
        const std::wstring source = from + L"\\" + found.cFileName;
        const std::wstring target = to + L"\\" + found.cFileName;
        if (!::MoveFileExW(source.c_str(), target.c_str(), 0)) {
            error = L"Could not move " + source + L" (error " + std::to_wstring(::GetLastError()) +
                    L"). Is the game or another program using it?";
            ok = false;
            break;
        }
        moves.emplace_back(source, target);
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
    return ok;
}

bool WriteName(const std::wstring& modelsFolder, const std::wstring& name) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, (modelsFolder + L"\\" + kNameFile).c_str(), L"w, ccs=UTF-8") != 0 || !file)
        return false;
    std::fputws(name.c_str(), file);
    std::fclose(file);
    return true;
}

} // namespace

std::wstring ActiveWorldName(const std::wstring& modelsFolder) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, (modelsFolder + L"\\" + kNameFile).c_str(), L"r, ccs=UTF-8") != 0 || !file)
        return kDefaultWorld;
    wchar_t line[128] = {};
    std::fgetws(line, 128, file);
    std::fclose(file);
    std::wstring name = line;
    if (!name.empty() && name.front() == L'\xFEFF') name.erase(name.begin());   // written with a BOM
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    return IsValidWorldName(name) ? name : kDefaultWorld;
}

std::vector<std::wstring> ListStoredWorlds(const std::wstring& modelsFolder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((modelsFolder + L"\\" + kStoreFolder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsValidWorldName(found.cFileName))
            names.push_back(found.cFileName);
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return ::_wcsicmp(a.c_str(), b.c_str()) < 0; });
    return names;
}

bool IsValidWorldName(const std::wstring& name) {
    if (name.empty() || name.size() > 64 || name.front() == L'.' || name.back() == L' ') return false;
    for (wchar_t c : name)
        if (!std::iswalnum(c) && c != L' ' && c != L'-' && c != L'_' && c != L'.') return false;
    return true;
}

bool SwitchWorld(const std::wstring& modelsFolder, const std::wstring& target, bool createNew,
                 std::wstring& error) {
    if (!IsValidWorldName(target)) {
        error = L"\"" + target + L"\" is not a usable world name.";
        return false;
    }
    const std::wstring active = ActiveWorldName(modelsFolder);
    const std::wstring store = modelsFolder + L"\\" + kStoreFolder;
    const std::wstring stash = store + L"\\" + active;
    const std::wstring source = store + L"\\" + target;

    if (::_wcsicmp(active.c_str(), target.c_str()) == 0) {
        error = L"The world \"" + target + L"\" is already open.";
        return false;
    }
    if (createNew == Exists(source)) {
        error = createNew ? L"There is already a world called \"" + target + L"\"."
                          : L"There is no world called \"" + target + L"\".";
        return false;
    }
    if (Exists(stash)) {
        error = L"The open world cannot be put away: " + stash + L" is already there.";
        return false;
    }

    ::CreateDirectoryW(modelsFolder.c_str(), nullptr);
    ::CreateDirectoryW(store.c_str(), nullptr);
    if (!::CreateDirectoryW(stash.c_str(), nullptr)) {
        error = L"Could not create " + stash + L".";
        return false;
    }

    Moves away, in;
    if (!MoveContents(modelsFolder, stash, away, error)) {
        Undo(away);
        ::RemoveDirectoryW(stash.c_str());
        return false;
    }
    if (!createNew) {
        if (!MoveContents(source, modelsFolder, in, error)) {
            Undo(in);
            Undo(away);
            ::RemoveDirectoryW(stash.c_str());
            return false;
        }
        // Left behind only if it held shared files of its own, which are dropped.
        if (!::RemoveDirectoryW(source.c_str())) {
            LOG_WARN("worlds: %S is not empty after opening it", source.c_str());
            const std::wstring leftover = store + L"\\." + target + L"-leftover";
            ::MoveFileExW(source.c_str(), leftover.c_str(), 0);
        }
    }
    if (!WriteName(modelsFolder, target)) LOG_WARN("worlds: could not write the open world's name");

    LOG_INFO("worlds: \"%S\" put away (%zu entries), \"%S\" %s (%zu entries)", active.c_str(), away.size(),
             target.c_str(), createNew ? "created" : "opened", in.size());
    return true;
}

} // namespace io

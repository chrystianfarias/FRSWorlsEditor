#include "io/WorldFile.h"

#include "core/Log.h"
#include "io/WorldProfiles.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace io {

namespace {

constexpr u32 kMagic = 0x57535246;   // "FRSW"
constexpr u32 kVersion = 1;
const char kModelsPrefix[] = "models/";

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

bool ReadWhole(const std::wstring& path, std::vector<u8>& out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    ::GetFileSizeEx(file, &size);
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    size_t at = 0;
    bool ok = true;
    while (at < out.size()) {
        const DWORD want = (DWORD)std::min<size_t>(out.size() - at, 64u << 20);
        if (!::ReadFile(file, out.data() + at, want, &read, nullptr) || !read) { ok = false; break; }
        at += read;
    }
    ::CloseHandle(file);
    return ok;
}

bool WriteWhole(const std::wstring& path, const u8* data, size_t size) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t at = 0;
    bool ok = true;
    while (at < size) {
        DWORD written = 0;
        const DWORD want = (DWORD)std::min<size_t>(size - at, 64u << 20);
        if (!::WriteFile(file, data + at, want, &written, nullptr) || !written) { ok = false; break; }
        at += written;
    }
    ::CloseHandle(file);
    return ok;
}

// The world's own files under `folder`, relative paths with '/'. Only the
// top level knows shared names.
void ListFiles(const std::wstring& root, const std::wstring& relative, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW found{};
    const std::wstring folder = relative.empty() ? root : root + L"\\" + relative;
    HANDLE search = ::FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!::wcscmp(found.cFileName, L".") || !::wcscmp(found.cFileName, L"..")) continue;
        if (relative.empty() && IsSharedWorldFile(found.cFileName)) continue;
        const std::wstring path = relative.empty() ? std::wstring(found.cFileName) : relative + L"\\" + found.cFileName;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(root, path, out);
        else out.push_back(path);
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
}

void MakeFolders(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        ::CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}

} // namespace

bool WorldFile::Get(const std::string& name, std::vector<u8>& bytes) const {
    const auto found = entries_.find(name);
    if (found == entries_.end()) return false;
    bytes = found->second;
    return true;
}

std::vector<std::string> WorldFile::Names() const {
    std::vector<std::string> names;
    for (const auto& [name, bytes] : entries_) names.push_back(name);
    return names;
}

bool WorldFile::Load(const std::wstring& path, std::wstring& error) {
    entries_.clear();
    std::vector<u8> data;
    if (!ReadWhole(path, data)) { error = L"Could not read " + path; return false; }
    const auto u32At = [&](size_t at) { u32 v = 0; if (at + 4 <= data.size()) std::memcpy(&v, data.data() + at, 4); return v; };
    if (data.size() < 12 || u32At(0) != kMagic) { error = path + L" is not a world file."; return false; }
    if (u32At(4) > kVersion) { error = L"That world was written by a newer editor."; return false; }
    const u32 count = u32At(8);
    size_t at = 12;
    for (u32 i = 0; i < count; ++i) {
        const u32 nameLength = u32At(at);
        at += 4;
        if (at + nameLength + 8 > data.size()) { error = L"The world file is cut short."; return false; }
        std::string name((const char*)data.data() + at, nameLength);
        at += nameLength;
        u64 size = 0;
        std::memcpy(&size, data.data() + at, 8);
        at += 8;
        if (at + size > data.size()) { error = L"The world file is cut short."; return false; }
        entries_[name].assign(data.data() + at, data.data() + at + size);
        at += (size_t)size;
    }
    return true;
}

bool WorldFile::Save(const std::wstring& path, std::wstring& error) const {
    std::vector<u8> out;
    const auto put32 = [&](u32 v) { const u8* b = (const u8*)&v; out.insert(out.end(), b, b + 4); };
    put32(kMagic);
    put32(kVersion);
    put32((u32)entries_.size());
    for (const auto& [name, bytes] : entries_) {
        put32((u32)name.size());
        out.insert(out.end(), name.begin(), name.end());
        const u64 size = bytes.size();
        const u8* b = (const u8*)&size;
        out.insert(out.end(), b, b + 8);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    const std::wstring temporary = path + L".saving";
    if (!WriteWhole(temporary, out.data(), out.size())) { error = L"Could not write " + temporary; return false; }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        error = L"Could not replace " + path + L" (error " + std::to_wstring(::GetLastError()) + L")";
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool PackWorld(const std::wstring& modelsFolder, WorldFile& file, size_t& files, std::wstring& error) {
    files = 0;
    for (const std::string& name : file.Names())
        if (name.rfind(kModelsPrefix, 0) == 0) file.Remove(name);
    std::vector<std::wstring> paths;
    ListFiles(modelsFolder, L"", paths);
    for (const std::wstring& relative : paths) {
        std::vector<u8> bytes;
        if (!ReadWhole(modelsFolder + L"\\" + relative, bytes)) {
            error = L"Could not read " + modelsFolder + L"\\" + relative;
            return false;
        }
        std::string name = kModelsPrefix + Utf8(relative);
        for (char& c : name) if (c == '\\') c = '/';
        file.Put(name, std::move(bytes));
        ++files;
    }
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char meta[160];
    std::snprintf(meta, sizeof(meta), "FRS World Editor world\nsaved %04u-%02u-%02u %02u:%02u\nfiles %zu\n", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, files);
    file.Put("meta", std::vector<u8>(meta, meta + std::strlen(meta)));
    return true;
}

bool UnpackWorld(const WorldFile& file, const std::wstring& modelsFolder, const std::wstring& backupFolder,
                 size_t& files, std::wstring& error) {
    files = 0;
    // The world that was open, put aside whole.
    std::vector<std::wstring> current;
    ListFiles(modelsFolder, L"", current);
    if (!current.empty()) {
        for (const std::wstring& relative : current) {
            const std::wstring target = backupFolder + L"\\" + relative;
            MakeFolders(target);
            if (!::MoveFileExW((modelsFolder + L"\\" + relative).c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) {
                error = L"Could not move " + relative + L" aside (error " + std::to_wstring(::GetLastError()) +
                        L"). Is the game or another program using it?";
                return false;
            }
        }
        LOG_INFO("world: %zu files of the previous world kept in %S", current.size(), backupFolder.c_str());
    }
    for (const std::string& name : file.Names()) {
        if (name.rfind(kModelsPrefix, 0) != 0) continue;
        std::wstring relative = Wide(name.substr(sizeof(kModelsPrefix) - 1));
        for (wchar_t& c : relative) if (c == L'/') c = L'\\';
        if (relative.empty() || relative.find(L"..") != std::wstring::npos || relative[0] == L'\\' ||
            relative.find(L':') != std::wstring::npos)
            continue;   // never outside the models folder
        std::vector<u8> bytes;
        file.Get(name, bytes);
        const std::wstring path = modelsFolder + L"\\" + relative;
        MakeFolders(path);
        if (!WriteWhole(path, bytes.data(), bytes.size())) {
            error = L"Could not write " + path;
            return false;
        }
        ++files;
    }
    return true;
}

bool WorldHasFiles(const std::wstring& modelsFolder) {
    std::vector<std::wstring> paths;
    ListFiles(modelsFolder, L"", paths);
    return !paths.empty();
}

std::string WorldSignature(const std::wstring& modelsFolder) {
    std::vector<std::wstring> paths;
    ListFiles(modelsFolder, L"", paths);
    std::sort(paths.begin(), paths.end());
    u64 hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) { hash ^= ((const u8*)data)[i]; hash *= 1099511628211ull; }
    };
    for (const std::wstring& relative : paths) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        ::GetFileAttributesExW((modelsFolder + L"\\" + relative).c_str(), GetFileExInfoStandard, &info);
        mix(relative.data(), relative.size() * sizeof(wchar_t));
        mix(&info.nFileSizeLow, 4);
        mix(&info.nFileSizeHigh, 4);
        mix(&info.ftLastWriteTime, sizeof(info.ftLastWriteTime));
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    return text;
}

} // namespace io

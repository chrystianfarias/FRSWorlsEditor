#include "io/WorldPublish.h"

#include "core/FileFingerprint.h"
#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>

#pragma comment(lib, "version.lib")

namespace io {

namespace {

struct Pattern {
    std::wstring glob;   // upper case
    std::wstring note;
};

std::vector<Pattern>& Patterns() {
    static std::vector<Pattern> patterns = {
        { L"*.FRSMESH", L"" },
        { L"*.FRSSECTION", L"" },
        { L"FILES\\**", L"" },
        // Not read by the game, but the licences of placed Sketchfab models
        // ask for their authors to be credited wherever the models go.
        { L"CREDITS.TXT", L"" },
    };
    return patterns;
}

std::wstring Upper(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)::towupper(c);
    return text;
}

// '*' within one folder, '**' across folders, '?' one character.
bool Matches(const wchar_t* glob, const wchar_t* name) {
    for (; *glob; ++glob, ++name) {
        if (glob[0] == L'*' && glob[1] == L'*') {
            for (const wchar_t* rest = name;; ++rest) {
                if (Matches(glob + 2, rest)) return true;
                if (!*rest) return false;
            }
        }
        if (*glob == L'*') {
            for (const wchar_t* rest = name;; ++rest) {
                if (Matches(glob + 1, rest)) return true;
                if (!*rest || *rest == L'\\') return false;
            }
        }
        if (!*name) return false;
        if (*glob != L'?' && *glob != *name) return false;
        if (*glob == L'?' && *name == L'\\') return false;
    }
    return *name == 0;
}

const Pattern* MatchingPattern(const std::wstring& relative) {
    const std::wstring upper = Upper(relative);
    for (const Pattern& pattern : Patterns())
        if (Matches(pattern.glob.c_str(), upper.c_str())) return &pattern;
    return nullptr;
}

void Walk(const std::wstring& folder, const std::wstring& relative, std::vector<PublishedFile>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring key = relative.empty() ? name : relative + L"\\" + name;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // The worlds put away, and the backups opening one leaves.
            if (relative.empty() && ::_wcsicmp(name.c_str(), L"worlds") == 0) continue;
            Walk(folder + L"\\" + name, key, out);
            continue;
        }
        if (!MatchingPattern(key)) continue;
        PublishedFile file;
        file.relative = key;
        file.path = folder + L"\\" + name;
        file.size = ((u64)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        out.push_back(std::move(file));
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

bool ReadWhole(const std::wstring& path, std::vector<u8>& out) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

// The region an overlay names (both kinds keep it at byte 8, eight characters).
std::wstring OverlayRegion(const std::wstring& path) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return {};
    char header[16]{};
    const bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    std::fclose(file);
    if (!ok || (std::memcmp(header, "FRSMESH1", 8) != 0 && std::memcmp(header, "FRSSECT1", 8) != 0)) return {};
    std::wstring region;
    for (int i = 8; i < 16 && header[i]; ++i) region += (wchar_t)(unsigned char)header[i];
    return Upper(region);
}

u64 VersionNumber(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return 0;
    std::vector<u8> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return 0;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(data.data(), L"\\", (void**)&info, &length) || !info) return 0;
    return ((u64)info->dwFileVersionMS << 32) | info->dwFileVersionLS;
}

// ---- a zip, stored (the overlays are mostly floats, which deflate little) ----

u32 Crc32(const u8* data, size_t size) {
    static u32 table[256];
    static bool ready = false;
    if (!ready) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

class ZipWriter {
public:
    explicit ZipWriter(FILE* file) : file_(file) {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        time_ = (u16)((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
        date_ = (u16)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
    }

    bool Add(const std::wstring& name, const std::vector<u8>& data) {
        std::string utf8 = Utf8(name);
        std::replace(utf8.begin(), utf8.end(), '\\', '/');
        if (offset_ + data.size() + 1024 > 0xFFFFFFF0ull) return false;   // no zip64 here

        Entry entry;
        entry.name = utf8;
        entry.crc = Crc32(data.data(), data.size());
        entry.size = (u32)data.size();
        entry.offset = (u32)offset_;

        std::vector<u8> header;
        Put32(header, 0x04034B50);
        Put16(header, 20);          // version needed
        Put16(header, 0x0800);      // names are UTF-8
        Put16(header, 0);           // stored
        Put16(header, time_);
        Put16(header, date_);
        Put32(header, entry.crc);
        Put32(header, entry.size);
        Put32(header, entry.size);
        Put16(header, (u16)utf8.size());
        Put16(header, 0);
        header.insert(header.end(), utf8.begin(), utf8.end());

        if (!Write(header) || !Write(data)) return false;
        entries_.push_back(std::move(entry));
        return true;
    }

    bool Finish() {
        const u64 start = offset_;
        std::vector<u8> central;
        for (const Entry& entry : entries_) {
            Put32(central, 0x02014B50);
            Put16(central, 20);         // made by
            Put16(central, 20);
            Put16(central, 0x0800);
            Put16(central, 0);
            Put16(central, time_);
            Put16(central, date_);
            Put32(central, entry.crc);
            Put32(central, entry.size);
            Put32(central, entry.size);
            Put16(central, (u16)entry.name.size());
            Put16(central, 0);
            Put16(central, 0);
            Put16(central, 0);
            Put16(central, 0);
            Put32(central, 0);
            Put32(central, entry.offset);
            central.insert(central.end(), entry.name.begin(), entry.name.end());
        }
        if (!Write(central)) return false;
        std::vector<u8> end;
        Put32(end, 0x06054B50);
        Put16(end, 0);
        Put16(end, 0);
        Put16(end, (u16)entries_.size());
        Put16(end, (u16)entries_.size());
        Put32(end, (u32)central.size());
        Put32(end, (u32)start);
        Put16(end, 0);
        return Write(end) && entries_.size() < 0xFFFF;
    }

    u64 Size() const { return offset_; }

private:
    struct Entry {
        std::string name;
        u32 crc = 0, size = 0, offset = 0;
    };

    static void Put16(std::vector<u8>& out, u16 value) {
        out.push_back((u8)value);
        out.push_back((u8)(value >> 8));
    }
    static void Put32(std::vector<u8>& out, u32 value) {
        for (int i = 0; i < 4; ++i) out.push_back((u8)(value >> (i * 8)));
    }
    bool Write(const std::vector<u8>& bytes) {
        if (bytes.empty()) return true;
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) return false;
        offset_ += bytes.size();
        return true;
    }

    FILE* file_;
    u64 offset_ = 0;
    u16 time_ = 0, date_ = 0;
    std::vector<Entry> entries_;
};

std::vector<u8> Bytes(const std::string& text) { return std::vector<u8>(text.begin(), text.end()); }

// A name that is safe as a file name on its own.
std::wstring FileSafe(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) out += (c < 32 || std::wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out.empty() ? L"World" : out;
}

// One line of text for the manifest: no line breaks inside a value.
std::string OneLine(const std::wstring& text) {
    std::string out = Utf8(text);
    for (char& c : out)
        if (c == '\r' || c == '\n') c = ' ';
    return out;
}

} // namespace

void AddPublishedWorldFiles(const std::wstring& pattern, const std::wstring& note) {
    const std::wstring glob = Upper(pattern);
    for (Pattern& known : Patterns())
        if (known.glob == glob) {
            if (!note.empty()) known.note = note;
            return;
        }
    Patterns().push_back({ glob, note });
}

std::vector<PublishedFile> CollectPublishedFiles(const std::wstring& modelsFolder) {
    std::vector<PublishedFile> files;
    Walk(modelsFolder, L"", files);
    std::sort(files.begin(), files.end(), [](const PublishedFile& a, const PublishedFile& b) {
        return ::_wcsicmp(a.relative.c_str(), b.relative.c_str()) < 0;
    });
    return files;
}

std::wstring FileVersionText(const std::wstring& path) {
    const u64 version = VersionNumber(path);
    if (!version) return {};
    wchar_t text[64];
    ::swprintf_s(text, L"%u.%u.%u.%u", (unsigned)(version >> 48), (unsigned)((version >> 32) & 0xFFFF),
                 (unsigned)((version >> 16) & 0xFFFF), (unsigned)(version & 0xFFFF));
    return text;
}

bool PublishWorld(const PublishRequest& request, PublishResult& result, std::wstring& error) {
    result = {};
    const std::wstring models = request.gameFolder + L"\\models";
    const std::vector<PublishedFile> files = CollectPublishedFiles(models);
    if (files.empty()) {
        error = L"The world has nothing the game reads yet: save some edits first.";
        return false;
    }

    // The loader: the newest of the copies there are.
    std::wstring loader;
    u64 best = 0;
    for (const std::wstring& candidate : request.loaderCandidates) {
        if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const u64 version = VersionNumber(candidate);
        if (loader.empty() || version > best) {
            loader = candidate;
            best = version;
        }
    }
    if (loader.empty()) {
        error = L"FRSWorldLoader.asi was not found beside the editor or in the game's scripts folder. "
                L"Run File > Game Folder and Plugins once, then publish again.";
        return false;
    }
    result.loaderUsed = loader;
    result.loaderVersion = FileVersionText(loader);

    // The regions the overlays edit, and the track files those edits are
    // byte ranges of.
    std::set<std::wstring> regions;
    for (const PublishedFile& file : files) {
        const std::wstring region = OverlayRegion(file.path);
        if (!region.empty()) regions.insert(region);
    }
    result.regions.assign(regions.begin(), regions.end());

    std::string targets;
    for (const std::wstring& region : regions)
        for (const std::wstring relative : { L"TRACKS\\STREAM" + region + L".BUN", L"TRACKS\\" + region + L".BUN" }) {
            const frs::FileFingerprint print = frs::FingerprintFile(request.gameFolder + L"\\" + relative);
            if (!print.valid) continue;
            char line[512];
            std::snprintf(line, sizeof(line), "target=%s|%llu|%016llx\r\n", Utf8(relative).c_str(), print.size,
                          print.hash);
            targets += line;
        }

    const std::wstring safe = FileSafe(request.name);
    char date[32];
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        ::localtime_s(&local, &now);
        std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
    }

    // models\frs-world.txt
    std::string manifest;
    manifest += "; A world made with FRS World Editor, published for playing.\r\n";
    manifest += "; FRSWorldLoader.asi reads the target lines: a region whose track file is not the\r\n";
    manifest += "; one the world was made for keeps the game's own city instead of crashing.\r\n";
    manifest += "name=" + OneLine(request.name) + "\r\n";
    manifest += "author=" + OneLine(request.author) + "\r\n";
    manifest += "published=" + std::string(date) + "\r\n";
    manifest += "editor=" + OneLine(request.editorVersion) + "\r\n";
    manifest += targets;
    for (const PublishedFile& file : files) manifest += "file=models\\" + Utf8(file.relative) + "\r\n";

    // The notes extensions asked for, once each.
    std::vector<std::wstring> notes;
    for (const PublishedFile& file : files)
        if (const Pattern* pattern = MatchingPattern(file.relative))
            if (!pattern->note.empty() && std::find(notes.begin(), notes.end(), pattern->note) == notes.end())
                notes.push_back(pattern->note);

    std::string regionList;
    for (const std::wstring& region : regions) regionList += (regionList.empty() ? "" : ", ") + Utf8(region);

    const std::string name = OneLine(request.name);
    std::string readme;
    readme += name + "\r\n" + std::string(name.size(), '=') + "\r\n\r\n";
    if (!request.author.empty()) readme += "By " + OneLine(request.author) + ", " + date + ".\r\n";
    readme += "A world for Need for Speed Underground 2, made with FRS World Editor " +
              OneLine(request.editorVersion) + ".\r\n\r\n";
    if (!request.description.empty()) {
        std::string description = Utf8(request.description);
        std::string crlf;
        for (size_t i = 0; i < description.size(); ++i) {
            if (description[i] == '\n' && (i == 0 || description[i - 1] != '\r')) crlf += '\r';
            crlf += description[i];
        }
        readme += crlf + "\r\n\r\n";
    }
    readme +=
        "INSTALLING\r\n"
        "----------\r\n"
        "1. Unpack everything in this zip into the game folder, the one with SPEED2.EXE,\r\n"
        "   and let it replace files when asked.\r\n"
        "2. The game needs an ASI loader to load scripts\\FRSWorldLoader.asi. Most modded\r\n"
        "   games have one already (dinput8.dll or another). If yours has none, move\r\n"
        "   dinput8.dll from the \"ASI loader (only if needed)\" folder next to SPEED2.EXE.\r\n"
        "3. Play. The FRS World Editor is not needed.\r\n\r\n"
        "One FRS world at a time: uninstall the previous one before installing another.\r\n"
        "The world edits the game's own track files as it reads them (nothing on disk is\r\n"
        "changed), so it fits the track files its author had" +
        std::string(regionList.empty() ? "" : " (" + regionList + ")") +
        ", normally\r\n"
        "those of the game as shipped. Where another map mod changed them, the loader\r\n"
        "leaves the world's edits out and the city looks as before;\r\n"
        "scripts\\FRSWorldLoader.log says so when that happens.\r\n\r\n";
    if (!notes.empty()) {
        readme += "ALSO NEEDED\r\n-----------\r\n";
        for (const std::wstring& note : notes) readme += "- " + OneLine(note) + "\r\n";
        readme += "\r\n";
    }
    readme +=
        "UNINSTALLING\r\n"
        "------------\r\n"
        "Run \"Uninstall " + Utf8(safe) + ".bat\" in the game folder. It deletes the world's files\r\n"
        "and nothing else; FRSWorldLoader.asi does nothing without a world.\r\n";

    // The uninstaller: the world's files, by name, and the manifest.
    std::string uninstall;
    uninstall += "@echo off\r\nchcp 65001 >nul\r\ncd /d \"%~dp0\"\r\n";
    uninstall += "echo Removing the world \"" + name + "\" from this game.\r\n";
    for (const PublishedFile& file : files) uninstall += "del /q \"models\\" + Utf8(file.relative) + "\" 2>nul\r\n";
    uninstall += "del /q \"models\\frs-world.txt\" 2>nul\r\n";
    uninstall += "echo Done.\r\npause\r\n";

    // Written beside the target and renamed over it, so a failure leaves no
    // half a zip under the chosen name.
    const std::wstring temp = request.zipPath + L".part";
    FILE* out = nullptr;
    if (::_wfopen_s(&out, temp.c_str(), L"wb") != 0 || !out) {
        error = L"Could not write " + request.zipPath;
        return false;
    }

    ZipWriter zip(out);
    bool ok = true;
    std::vector<u8> data;
    for (const PublishedFile& file : files) {
        if (!ReadWhole(file.path, data)) {
            error = L"Could not read models\\" + file.relative;
            ok = false;
            break;
        }
        if (!zip.Add(L"models\\" + file.relative, data)) {
            error = L"The world is too big for one zip (4 GB).";
            ok = false;
            break;
        }
        ++result.files;
    }
    ok = ok && zip.Add(L"models\\frs-world.txt", Bytes(manifest));
    if (ok && ReadWhole(loader, data)) ok = zip.Add(L"scripts\\FRSWorldLoader.asi", data);
    if (ok) {
        // Its settings only switch on research tools, so a copy beside the
        // chosen loader, or none, is as good as any.
        const std::wstring ini = loader.substr(0, loader.size() - 4) + L".ini";
        if (ReadWhole(ini, data)) ok = zip.Add(L"scripts\\FRSWorldLoader.ini", data);
    }
    if (ok && !request.asiLoader.empty() && ReadWhole(request.asiLoader, data)) {
        ok = zip.Add(L"ASI loader (only if needed)\\dinput8.dll", data);
        const std::wstring folder = request.asiLoader.substr(0, request.asiLoader.find_last_of(L"\\/") + 1);
        if (ok && ReadWhole(folder + L"LICENSE.txt", data))
            ok = zip.Add(L"ASI loader (only if needed)\\LICENSE.txt", data);
    }
    ok = ok && zip.Add(safe + L" - README.txt", Bytes(readme)) &&
         zip.Add(L"Uninstall " + safe + L".bat", Bytes(uninstall)) && zip.Finish();
    result.bytes = zip.Size();
    const bool closed = std::fclose(out) == 0;

    if (!ok || !closed) {
        if (error.empty()) error = L"Could not write " + request.zipPath;
        ::DeleteFileW(temp.c_str());
        return false;
    }
    if (!::MoveFileExW(temp.c_str(), request.zipPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temp.c_str());
        error = L"Could not write " + request.zipPath + L" (is it open somewhere?)";
        return false;
    }
    LOG_INFO("world published: %zu files, %llu bytes, loader %ls", result.files, result.bytes,
             result.loaderVersion.c_str());
    return true;
}

} // namespace io

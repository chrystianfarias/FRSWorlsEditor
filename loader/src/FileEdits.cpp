// FRS World Loader -- the file edits
//
// Puts edited meshes into Need for Speed Underground 2 without changing a byte
// of the game's own files.
//
// The game reads its geometry out of TRACKS\STREAM<region>.BUN. This plugin sits
// on ReadFile, and when a read covers a piece of that stream an edit applies to,
// it rewrites the bytes in the buffer after the real read has filled it. The
// game parses what it is handed and never knows the difference; the file on disk
// stays exactly as it shipped, and an edit is undone by deleting a file.
//
// Working at the file level rather than on the loaded structures is deliberate.
// It needs no knowledge of how the game lays a mesh out in memory, no addresses
// that a patch or a different build would move, and it covers every section,
// including ones streamed in long after the level has started.
//
// Two kinds of edit are applied, both from <game>\models:
//
//   .frsmesh     new vertex positions for a mesh, written at the stride the
//                file already uses, so nothing changes size
//   .frssection  a span of raw bytes replacing the same span in the stream, for
//                edits that change a mesh's vertex count and so have to move
//                everything around it. The editor builds those bytes, because
//                that is where the chunk format is understood; here they are
//                just a range to splice in
//
// Both end up as the same operation: this range of the file reads differently.
//
// A third kind replaces a whole file: anything under <game>\models\files is
// opened in place of the game file at the same relative path, so
// models\files\TRACKS\ROUTESL4RA\PathsFreeRoam.bin stands in for
// TRACKS\ROUTESL4RA\PathsFreeRoam.bin. That is for the small files the editor
// rebuilds whole -- the free-roam zones the shop markers live in -- where
// patching ranges would gain nothing and a file that grows would need its
// size, its reads and its mappings all corrected.


#include "Loader.h"

#include "../../src/core/FileFingerprint.h"

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace frs {
namespace {

// ---------------------------------------------------------------------------
// The edits
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct MeshOverlayHeader {
    char     magic[8];        // "FRSMESH1"
    char     region[8];
    unsigned hash;
    unsigned stride;          // bytes between vertices in the stream
    unsigned vertexCount;
    unsigned reserved;
    unsigned __int64 fileOffset;
    char     name[64];
};

struct SectionOverlayHeader {
    char     magic[8];        // "FRSSECT1"
    char     region[8];
    unsigned byteCount;
    unsigned target;          // 0 = the stream, 1 = the layout file beside it
    unsigned __int64 fileOffset;
    char     name[64];
};
#pragma pack(pop)

// One edit, reduced to what applying it needs.
//
// A mesh edit is not one contiguous range: positions sit at the start of each
// vertex and the rest of the vertex -- colour, texture coordinates, a normal
// when the layout has one -- has to be left alone. Keeping the stride here lets
// the same code serve both kinds without a mesh edit having to invent the bytes
// in between, which it does not know.
struct Edit {
    std::wstring       file;
    std::string        name;
    std::string        region;
    unsigned __int64   begin = 0;     // first byte of the stream this touches
    unsigned __int64   end   = 0;     // one past the last
    unsigned           stride = 0;    // 0 for a raw span
    bool               layout = false;   // true when it patches the layout file
    std::vector<float> positions;     // three floats per vertex, for a mesh edit
    std::vector<char>  bytes;         // the replacement, for a raw span
};

std::vector<Edit> g_edits;

// Whole-file replacements: the game's path relative to its folder, upper case
// with backslashes, and the file to open instead.
std::vector<std::pair<std::wstring, std::wstring>> g_redirects;
volatile LONG g_redirected = 0;

// Counted so a silent failure can be told apart from a wrong edit. Without
// these the only evidence is the picture, which says nothing about whether the
// bytes were ever seen.
// Every call that reaches the hook, counted before anything is decided.
//
// The counters below it all sit behind a test that can send a read back
// early, so a zero in them says nothing about whether the hook is on the
// path at all. This one separates a hook that never fires from a game that
// simply has not read a track yet.
volatile LONG g_allReads = 0;
volatile LONG g_supplied = 0;   // reads answered from beyond the file's end
volatile LONG g_streamReads = 0;
volatile LONG g_editsApplied = 0;
volatile LONG g_otherRegion = 0;   // edits held back from another region's file
volatile LONG g_otherReads = 0;
volatile LONG g_layoutReads = 0;
volatile unsigned __int64 g_firstStreamAt = ~0ull;
volatile unsigned __int64 g_firstLayoutAt = ~0ull;

// The handles that are reading a track stream, so the cost of deciding lands on
// opening a file rather than on every read.
CRITICAL_SECTION g_lock;
enum class TrackFile { Other, Stream, Layout };

// Which region a track file belongs to: "L4RA" for TRACKS\STREAML4RA.BUN and
// TRACKS\L4RA.BUN alike.
//
// An edit is a byte range of one region's files, and the same range of another
// region's stream is some other mesh entirely. Without this an L4RA edit was
// spliced into STREAML4RD.BUN the moment the game loaded a drag track, and the
// game crashed on the bytes it was handed. Kept as a fixed array so the read
// hook never allocates to find out.
struct Region {
    char name[8]{};
};

Region RegionFromName(const std::wstring& raw) {
    Region region;
    size_t start = raw.find_last_of(L"\\/");
    start = start == std::wstring::npos ? 0 : start + 1;
    std::wstring file = raw.substr(start);
    for (wchar_t& c : file) c = (wchar_t)::towupper(c);
    if (file.size() > 4 && file.compare(file.size() - 4, 4, L".BUN") == 0)
        file.resize(file.size() - 4);
    if (file.compare(0, 6, L"STREAM") == 0) file.erase(0, 6);
    for (size_t i = 0; i < file.size() && i < sizeof(region.name) - 1; ++i)
        region.name[i] = (char)file[i];
    return region;
}

// An edit made before overlays named their region applies anywhere, as it
// always did; a file whose region could not be told takes nothing.
bool EditFits(const Edit& edit, const Region& region) {
    if (edit.region.empty()) return true;
    return region.name[0] && ::_stricmp(edit.region.c_str(), region.name) == 0;
}

struct KnownHandle {
    HANDLE    handle;
    TrackFile kind;
    Region    region;
};
std::vector<KnownHandle> g_handles;
std::vector<std::wstring> g_seen;      // every distinct path read, for the log
size_t g_reported = 0;                 // how many of them the watcher has printed

std::string TrimmedField(const char* field, size_t size) {
    return std::string(field, ::strnlen(field, size));
}

void LoadEdits() {
    const std::wstring folder = GameFolder() + L"\\models";

    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*.frs*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        Log("no models folder at %S, nothing to apply", folder.c_str());
        return;
    }

    do {
        const std::wstring path = folder + L"\\" + found.cFileName;

        FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) continue;

        char magic[8]{};
        if (std::fread(magic, 1, sizeof(magic), file) != sizeof(magic)) {
            std::fclose(file);
            continue;
        }
        std::fseek(file, 0, SEEK_SET);

        Edit edit;
        edit.file = path;

        if (std::memcmp(magic, "FRSMESH1", 8) == 0) {
            MeshOverlayHeader header{};
            if (std::fread(&header, sizeof(header), 1, file) != 1) { std::fclose(file); continue; }

            edit.positions.resize((size_t)header.vertexCount * 3);
            const size_t wanted = edit.positions.size();
            if (std::fread(edit.positions.data(), sizeof(float), wanted, file) != wanted) {
                std::fclose(file);
                Log("%S is short, skipped", found.cFileName);
                continue;
            }

            edit.name   = TrimmedField(header.name, sizeof(header.name));
            edit.region = TrimmedField(header.region, sizeof(header.region));
            edit.stride = header.stride;
            edit.begin  = header.fileOffset;
            edit.end    = header.fileOffset +
                          (unsigned __int64)header.vertexCount * header.stride;

        } else if (std::memcmp(magic, "FRSSECT1", 8) == 0) {
            SectionOverlayHeader header{};
            if (std::fread(&header, sizeof(header), 1, file) != 1) { std::fclose(file); continue; }

            edit.bytes.resize(header.byteCount);
            if (std::fread(edit.bytes.data(), 1, header.byteCount, file) != header.byteCount) {
                std::fclose(file);
                Log("%S is short, skipped", found.cFileName);
                continue;
            }

            edit.name   = TrimmedField(header.name, sizeof(header.name));
            edit.region = TrimmedField(header.region, sizeof(header.region));
            edit.begin  = header.fileOffset;
            edit.end    = header.fileOffset + header.byteCount;
            edit.layout = header.target != 0;

        } else {
            std::fclose(file);
            continue;
        }

        std::fclose(file);
        Log("loaded %s '%s' for region %s: bytes %llu..%llu",
            edit.stride ? "mesh" : "section", edit.name.c_str(), edit.region.c_str(),
            edit.begin, edit.end);
        g_edits.push_back(std::move(edit));

    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    Log("%zu edits ready", g_edits.size());
}

// A published world names the game files it was made against.
//
// models\frs-world.txt is written by File > Publish World, never by the editor
// at work, so a world being edited is applied as it always was. In a published
// one each "target=" line holds a track file's path, size and fingerprint as
// the publisher had them; where this game's file differs, every edit for that
// region is left out. The city then looks as shipped, which is a great deal
// better than splicing a stranger's byte ranges into it and crashing.
void CheckPublishedTargets() {
    const std::wstring manifest = GameFolder() + L"\\models\\frs-world.txt";
    FILE* file = nullptr;
    if (::_wfopen_s(&file, manifest.c_str(), L"rb") != 0 || !file) return;

    std::string text;
    char chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, got);
    std::fclose(file);

    std::string worldName;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        if (line.compare(0, 5, "name=") == 0) {
            worldName = line.substr(5);
            continue;
        }
        if (line.compare(0, 7, "target=") != 0) continue;

        // target=TRACKS\STREAML4RA.BUN|118489088|0123456789abcdef
        const size_t bar1 = line.find('|', 7);
        const size_t bar2 = bar1 == std::string::npos ? std::string::npos : line.find('|', bar1 + 1);
        if (bar2 == std::string::npos) continue;
        const std::string relative = line.substr(7, bar1 - 7);
        const unsigned long long size = std::strtoull(line.substr(bar1 + 1, bar2 - bar1 - 1).c_str(), nullptr, 10);
        const unsigned long long hash = std::strtoull(line.substr(bar2 + 1).c_str(), nullptr, 16);

        wchar_t wide[MAX_PATH]{};
        ::MultiByteToWideChar(CP_UTF8, 0, relative.c_str(), -1, wide, MAX_PATH);
        const frs::FileFingerprint here = frs::FingerprintFile(GameFolder() + L"\\" + wide);
        if (here.valid && here.size == size && here.hash == hash) {
            Log("published world '%s': %s matches the one it was made for", worldName.c_str(), relative.c_str());
            continue;
        }

        // Only edits that name this region go; ones that name none are older
        // than published worlds and cannot come from one.
        const Region region = RegionFromName(wide);
        const size_t before = g_edits.size();
        g_edits.erase(std::remove_if(g_edits.begin(), g_edits.end(),
                                     [&](const Edit& edit) {
                                         return !edit.region.empty() && EditFits(edit, region);
                                     }),
                      g_edits.end());
        Log("published world '%s': %s is not the file it was made for (%s; size %llu here, %llu there). "
            "Its %zu edits for %s are left out so the game does not crash. Another map mod or another copy "
            "of the game changes this file; install the world on a game without it.",
            worldName.c_str(), relative.c_str(), here.valid ? "it differs" : "it is missing",
            here.size, size, before - g_edits.size(), region.name);
    }
}

// Every file under models\files, keyed by its path below that folder.
void LoadRedirects(const std::wstring& folder, const std::wstring& relative) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = folder + L"\\" + name;
        const std::wstring key = relative.empty() ? name : relative + L"\\" + name;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            LoadRedirects(path, key);
            continue;
        }
        std::wstring upper = key;
        for (wchar_t& c : upper) c = (wchar_t)::towupper(c);
        g_redirects.emplace_back(upper, path);

        char narrow[MAX_PATH]{};
        ::WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
        Log("replacing %s with the copy in models\\files", narrow);
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
}

// The replacement for a path the game is opening, or null. The game builds
// its paths as "<folder>/Tracks\RoutesL4RA\..." -- mixed separators, its own
// casing -- so both sides are reduced to one spelling before comparing.
const std::wstring* RedirectFor(const wchar_t* name) {
    if (g_redirects.empty() || !name) return nullptr;

    std::wstring path = name;
    for (wchar_t& c : path) c = c == L'/' ? L'\\' : (wchar_t)::towupper(c);
    std::wstring clean;
    for (wchar_t c : path)
        if (!(c == L'\\' && !clean.empty() && clean.back() == L'\\')) clean += c;

    static std::wstring folder;
    if (folder.empty()) {
        folder = GameFolder();
        for (wchar_t& c : folder) c = c == L'/' ? L'\\' : (wchar_t)::towupper(c);
        folder += L'\\';
    }
    if (clean.compare(0, folder.size(), folder) == 0) clean.erase(0, folder.size());
    else if (clean.compare(0, 2, L".\\") == 0)        clean.erase(0, 2);
    else if (clean.find(L':') != std::wstring::npos) return nullptr;   // elsewhere on disk

    for (const auto& redirect : g_redirects)
        if (redirect.first == clean) return &redirect.second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Applying an edit to a buffer the game has just read
// ---------------------------------------------------------------------------

// `readAt` is where in the file the buffer starts.
void ApplyEdits(unsigned __int64 readAt, char* buffer, DWORD length, bool layout,
                const Region& region) {
    const unsigned __int64 readEnd = readAt + length;

    for (const Edit& edit : g_edits) {
        if (edit.layout != layout) continue;
        if (edit.end <= readAt || edit.begin >= readEnd) continue;
        if (!EditFits(edit, region)) {
            ::InterlockedIncrement(&g_otherRegion);
            continue;
        }

        ::InterlockedIncrement(&g_editsApplied);

        if (edit.stride == 0) {
            // A raw span: copy in whatever part of it this read covers.
            const unsigned __int64 from = std::max(readAt, edit.begin);
            const unsigned __int64 to   = std::min(readEnd, edit.end);
            std::memcpy(buffer + (from - readAt),
                        edit.bytes.data() + (from - edit.begin),
                        (size_t)(to - from));
            continue;
        }

        // A mesh: each vertex's first twelve bytes, and nothing else. A read can
        // land in the middle of a vertex, so each one is clipped on its own
        // rather than assuming the buffer holds whole vertices.
        const size_t vertices = edit.positions.size() / 3;
        for (size_t v = 0; v < vertices; ++v) {
            const unsigned __int64 at = edit.begin + (unsigned __int64)v * edit.stride;
            if (at + 12 <= readAt || at >= readEnd) continue;

            const char* source = (const char*)&edit.positions[v * 3];
            for (int b = 0; b < 12; ++b) {
                const unsigned __int64 byteAt = at + b;
                if (byteAt < readAt || byteAt >= readEnd) continue;
                buffer[byteAt - readAt] = source[b];
            }
        }
    }
}

// Fills the part of a read the file itself could not answer.
//
// A rebuilt section that no longer fits where it was gets a new home past the
// end of the stream, and the section table is pointed at it. Nothing is written
// to disk, so the real read comes back short or empty; the bytes come from here
// instead. Returns how much of the buffer is now valid.
// Every read of a track stream, kept for the watcher to print.
//
// Which bytes the game asks for, and how many it got, is the one thing that
// decides whether an edit can reach it -- and it cannot be logged from the hook,
// where writing a file re-enters the call being serviced. So the reads are
// recorded as plain numbers here and printed from the watcher thread.
struct ReadNote {
    unsigned __int64 at;
    unsigned         wanted;
    unsigned         fromFile;    // what the real read returned
    unsigned         served;      // what this plugin made of it
};

ReadNote g_notes[128];
volatile LONG g_noteCount = 0;
size_t g_notesPrinted = 0;

void Note(unsigned __int64 at, DWORD wanted, DWORD fromFile, DWORD served) {
    const LONG index = ::InterlockedIncrement(&g_noteCount) - 1;
    if (index < 0 || index >= (LONG)(sizeof(g_notes) / sizeof(g_notes[0]))) return;

    g_notes[index].at       = at;
    g_notes[index].wanted   = wanted;
    g_notes[index].fromFile = fromFile;
    g_notes[index].served   = served;
}

DWORD SupplyBeyondFile(unsigned __int64 readAt, char* buffer, DWORD wanted,
                       DWORD alreadyRead, bool layout, const Region& region) {
    DWORD have = alreadyRead;
    const unsigned __int64 readEnd = readAt + wanted;

    for (const Edit& edit : g_edits) {
        if (edit.stride != 0) continue;            // only a raw span can stand in
        if (edit.layout != layout) continue;
        if (!EditFits(edit, region)) continue;
        if (edit.end <= readAt || edit.begin >= readEnd) continue;

        const unsigned __int64 from = readAt + have < edit.begin ? edit.begin
                                                                 : readAt + have;
        const unsigned __int64 to   = readEnd < edit.end ? readEnd : edit.end;
        if (to <= from) continue;

        std::memcpy(buffer + (from - readAt),
                    edit.bytes.data() + (from - edit.begin),
                    (size_t)(to - from));

        const DWORD reach = (DWORD)(to - readAt);
        if (reach > have) have = reach;
        ::InterlockedIncrement(&g_supplied);
    }
    return have;
}

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------

using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
ReadFileFn g_originalReadFile = nullptr;

// True when this handle is one of the track files.
//
// Both the stream and the layout file beside it count. A mesh that gains or
// loses vertices changes the size of its section, and that size is recorded in
// the layout file's section table -- so that read has to be corrected as well,
// or the game would be given the new bytes and the old length.
//
// The name is asked for once per handle and remembered. Asking on every read
// would put a kernel call in front of every byte the game loads, and the game
// loads a great deal.
TrackFile ClassifyName(const std::wstring& raw) {
    std::wstring name = raw;
    for (wchar_t& c : name) c = (wchar_t)::towupper(c);

    const bool inTracks = name.find(L"TRACKS") != std::wstring::npos;
    const bool isBun    = name.size() > 4 &&
                          name.compare(name.size() - 4, 4, L".BUN") == 0;
    if (!inTracks || !isBun) return TrackFile::Other;

    return name.find(L"STREAM") != std::wstring::npos ? TrackFile::Stream
                                                      : TrackFile::Layout;
}

// Remembered the moment a file is opened, rather than worked out from the
// handle on every read.
//
// Asking a handle for its path is the expensive part of the hook, and it only
// ever runs once a read has already happened. Catching the open instead records
// the name the game actually asked for -- which is also the only way to tell a
// file the game opens but never reads through ReadFile from one it does not
// open at all.
void Remember(HANDLE handle, const std::wstring& name) {
    if (handle == INVALID_HANDLE_VALUE || t_inside) return;

    const TrackFile kind = ClassifyName(name);
    const Region region = kind == TrackFile::Other ? Region{} : RegionFromName(name);

    ::EnterCriticalSection(&g_lock);
    for (auto& known : g_handles)
        if (known.handle == handle) { known.kind = kind; known.region = region; goto note; }

    if (g_handles.size() > 256) g_handles.clear();
    g_handles.push_back({ handle, kind, region });

note:
    // A track is always worth recording; anything else only until the list is
    // full. With every module hooked the browser the launcher runs would fill
    // it on its own and bury the one line this is here to catch.
    if (kind != TrackFile::Other || g_seen.size() < 200) {
        const std::wstring label =
            (kind == TrackFile::Stream ? L"[stream] " :
             kind == TrackFile::Layout ? L"[table]  " : L"         ") + name;
        if (std::find(g_seen.begin(), g_seen.end(), label) == g_seen.end())
            g_seen.push_back(label);
    }
    ::LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// The mapped path
//
// A track loaded from a 118 MB stream does not pass a few hundred reads through
// ReadFile, which is all the log ever showed. The file is mapped into memory
// and the game walks it as bytes, so there is no read to intercept: the pages
// arrive from the file the moment they are touched.
//
// Copy-on-write is what makes that tractable. A section created PAGE_WRITECOPY
// and viewed with FILE_MAP_COPY hands back pages that can be written without
// the file changing -- the edit lives in this process only, which is the same
// promise the ReadFile path already makes. So a mapping of a track file has its
// protection upgraded on the way through, and the edits are spliced into each
// view as it is handed out.
TrackFile Classify(HANDLE handle, Region* region = nullptr);   // beside the read hook

std::vector<KnownHandle> g_mappings;
volatile LONG g_mappedViews = 0;

TrackFile MappingKind(HANDLE mapping, Region* region = nullptr) {
    ::EnterCriticalSection(&g_lock);
    TrackFile kind = TrackFile::Other;
    for (const auto& known : g_mappings)
        if (known.handle == mapping) {
            kind = known.kind;
            if (region) *region = known.region;
            break;
        }
    ::LeaveCriticalSection(&g_lock);
    return kind;
}

using CreateFileMappingWFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD,
                                             DWORD, DWORD, LPCWSTR);
using CreateFileMappingAFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD,
                                             DWORD, DWORD, LPCSTR);
using MapViewOfFileFn   = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
using MapViewOfFileExFn = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID);

CreateFileMappingWFn g_originalCreateFileMappingW = nullptr;
CreateFileMappingAFn g_originalCreateFileMappingA = nullptr;
MapViewOfFileFn      g_originalMapViewOfFile = nullptr;
MapViewOfFileExFn    g_originalMapViewOfFileEx = nullptr;

// A track mapping has to be created writable-on-copy or no view of it could
// ever be written. The file handle itself is untouched and stays read-only.
DWORD MappingProtection(HANDLE file, DWORD protection) {
    if (Classify(file) == TrackFile::Other) return protection;
    if ((protection & 0xFF) == PAGE_READONLY) return PAGE_WRITECOPY;
    return protection;
}

void RememberMapping(HANDLE file, HANDLE mapping) {
    if (!mapping) return;
    Region region;
    const TrackFile kind = Classify(file, &region);

    ::EnterCriticalSection(&g_lock);
    if (g_mappings.size() > 128) g_mappings.clear();
    g_mappings.push_back({ mapping, kind, region });
    ::LeaveCriticalSection(&g_lock);
}

HANDLE WINAPI HookedCreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES security,
                                       DWORD protection, DWORD high, DWORD low,
                                       LPCWSTR name) {
    const HANDLE mapping = g_originalCreateFileMappingW(
        file, security, MappingProtection(file, protection), high, low, name);
    const DWORD error = ::GetLastError();
    RememberMapping(file, mapping);
    ::SetLastError(error);
    return mapping;
}

HANDLE WINAPI HookedCreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES security,
                                       DWORD protection, DWORD high, DWORD low,
                                       LPCSTR name) {
    const HANDLE mapping = g_originalCreateFileMappingA(
        file, security, MappingProtection(file, protection), high, low, name);
    const DWORD error = ::GetLastError();
    RememberMapping(file, mapping);
    ::SetLastError(error);
    return mapping;
}

// Splices the edits into a view the moment it is handed out.
//
// The view covers a window of the file starting at `offset`, so an edit lands
// in it on exactly the same test the read path uses -- the two share ApplyEdits
// and therefore cannot drift apart.
LPVOID FinishView(HANDLE mapping, LPVOID view, unsigned __int64 offset,
                  SIZE_T length) {
    if (!view) return view;

    Region region;
    const TrackFile kind = MappingKind(mapping, &region);
    if (kind == TrackFile::Other || g_edits.empty()) return view;

    ::InterlockedIncrement(&g_mappedViews);

    if (!length) {
        MEMORY_BASIC_INFORMATION info{};
        if (!::VirtualQuery(view, &info, sizeof(info))) return view;
        length = info.RegionSize;
    }

    if (kind == TrackFile::Stream && g_firstStreamAt == ~0ull) g_firstStreamAt = offset;
    if (kind == TrackFile::Layout && g_firstLayoutAt == ~0ull) g_firstLayoutAt = offset;

    // Copy-on-write pages are writable already; a view handed out read-only in
    // spite of the upgrade is turned over here instead.
    DWORD previous = 0;
    const bool guarded =
        ::VirtualProtect(view, length, PAGE_WRITECOPY, &previous) != FALSE;

    ApplyEdits(offset, (char*)view, (DWORD)length, kind == TrackFile::Layout, region);

    if (guarded) ::VirtualProtect(view, length, previous, &previous);
    return view;
}

LPVOID WINAPI HookedMapViewOfFile(HANDLE mapping, DWORD access, DWORD high,
                                  DWORD low, SIZE_T length) {
    if (MappingKind(mapping) != TrackFile::Other && access == FILE_MAP_READ)
        access = FILE_MAP_COPY;

    LPVOID view = g_originalMapViewOfFile(mapping, access, high, low, length);
    return FinishView(mapping, view, ((unsigned __int64)high << 32) | low, length);
}

LPVOID WINAPI HookedMapViewOfFileEx(HANDLE mapping, DWORD access, DWORD high,
                                    DWORD low, SIZE_T length, LPVOID at) {
    if (MappingKind(mapping) != TrackFile::Other && access == FILE_MAP_READ)
        access = FILE_MAP_COPY;

    LPVOID view = g_originalMapViewOfFileEx(mapping, access, high, low, length, at);
    return FinishView(mapping, view, ((unsigned __int64)high << 32) | low, length);
}

// --- how big the game thinks a track file is --------------------------------
//
// A section that outgrew the room around it is served from beyond the end of
// the file. Supplying those bytes is only half of it: the game has to ask for
// them first, and it will not ask for bytes it believes are not there. The
// stream file ends at 118 489 088 and the relocated section starts after that,
// so every read of it is a read past the end -- which the game either never
// issues or discards.
//
// So the file is reported as reaching past the last edit. Nothing is written to
// disk and the real file is untouched; this only changes the answer to "how far
// does this file go", which is the question standing between the game and bytes
// this plugin is holding ready.
unsigned __int64 ReportedSize(HANDLE file, unsigned __int64 real) {
    // The stream only. The file beside it holding the region's tables is a few
    // hundred kilobytes, and telling the game it is a hundred megabytes long --
    // which the first version of this did, because both count as track files --
    // has it sizing a buffer for a file that is not there.
    Region region;
    if (Classify(file, &region) != TrackFile::Stream) return real;

    unsigned __int64 furthest = real;
    for (const Edit& edit : g_edits)
        if (!edit.layout && EditFits(edit, region) && edit.end > furthest)
            furthest = edit.end;

    return furthest;
}

using GetFileSizeFn   = DWORD(WINAPI*)(HANDLE, LPDWORD);
using GetFileSizeExFn = BOOL(WINAPI*)(HANDLE, PLARGE_INTEGER);

GetFileSizeFn   g_originalGetFileSize = nullptr;
GetFileSizeExFn g_originalGetFileSizeEx = nullptr;

volatile LONG g_sizeAnswers = 0;

DWORD WINAPI HookedGetFileSize(HANDLE file, LPDWORD high) {
    const DWORD low = g_originalGetFileSize(file, high);
    if (low == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR) return low;

    unsigned __int64 real = low;
    if (high) real |= (unsigned __int64)*high << 32;

    const unsigned __int64 reported = ReportedSize(file, real);
    if (reported == real) return low;

    ::InterlockedIncrement(&g_sizeAnswers);
    if (high) *high = (DWORD)(reported >> 32);
    return (DWORD)(reported & 0xFFFFFFFFu);
}

BOOL WINAPI HookedGetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
    const BOOL ok = g_originalGetFileSizeEx(file, size);
    if (!ok || !size) return ok;

    const unsigned __int64 reported =
        ReportedSize(file, (unsigned __int64)size->QuadPart);

    if (reported != (unsigned __int64)size->QuadPart) {
        ::InterlockedIncrement(&g_sizeAnswers);
        size->QuadPart = (LONGLONG)reported;
    }
    return ok;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
CreateFileWFn g_originalCreateFileW = nullptr;
CreateFileAFn g_originalCreateFileA = nullptr;

// Only reads are redirected: a game that writes a file -- a save, a setting --
// must reach its own.
bool ReadOnlyOpen(DWORD access, DWORD creation) {
    return !(access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
           creation == OPEN_EXISTING;
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD creation,
                                DWORD flags, HANDLE templateFile) {
    if (!t_inside && ReadOnlyOpen(access, creation)) {
        if (const std::wstring* replacement = RedirectFor(name)) {
            ::InterlockedIncrement(&g_redirected);
            const HANDLE handle = g_originalCreateFileW(replacement->c_str(), access, share,
                                                        security, creation, flags, templateFile);
            const DWORD error = ::GetLastError();
            Remember(handle, L"[replaced] " + std::wstring(name));
            ::SetLastError(error);
            return handle;
        }
    }
    const HANDLE handle = g_originalCreateFileW(name, access, share, security,
                                                creation, flags, templateFile);
    const DWORD error = ::GetLastError();
    if (name) Remember(handle, name);
    ::SetLastError(error);
    return handle;
}

HANDLE WINAPI HookedCreateFileA(LPCSTR name, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD creation,
                                DWORD flags, HANDLE templateFile) {
    if (name && !t_inside && !g_redirects.empty() && ReadOnlyOpen(access, creation)) {
        wchar_t wide[MAX_PATH]{};
        ::MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);
        if (const std::wstring* replacement = RedirectFor(wide)) {
            ::InterlockedIncrement(&g_redirected);
            const HANDLE handle = g_originalCreateFileW(replacement->c_str(), access, share,
                                                        security, creation, flags, templateFile);
            const DWORD error = ::GetLastError();
            Remember(handle, L"[replaced] " + std::wstring(wide));
            ::SetLastError(error);
            return handle;
        }
    }
    const HANDLE handle = g_originalCreateFileA(name, access, share, security,
                                                creation, flags, templateFile);
    const DWORD error = ::GetLastError();
    if (name) {
        wchar_t wide[MAX_PATH]{};
        ::MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);
        Remember(handle, wide);
    }
    ::SetLastError(error);
    return handle;
}

TrackFile Classify(HANDLE handle, Region* regionOut) {
    ::EnterCriticalSection(&g_lock);

    for (const auto& known : g_handles)
        if (known.handle == handle) {
            const TrackFile kind = known.kind;
            if (regionOut) *regionOut = known.region;
            ::LeaveCriticalSection(&g_lock);
            return kind;
        }

    // Only a file on disk has a path worth asking for. Consoles, pipes and
    // sockets go through ReadFile too, and asking those for a final path is
    // both pointless and the sort of call that can block inside a hook.
    wchar_t path[MAX_PATH]{};
    DWORD length = 0;
    if (::GetFileType(handle) == FILE_TYPE_DISK)
        length = ::GetFinalPathNameByHandleW(handle, path, MAX_PATH,
                                             FILE_NAME_NORMALIZED);

    TrackFile kind = TrackFile::Other;
    Region region;
    if (length > 0 && length < MAX_PATH) {
        std::wstring name = path;
        for (wchar_t& c : name) c = (wchar_t)::towupper(c);

        const bool inTracks = name.find(L"\\TRACKS\\") != std::wstring::npos;
        const bool isBun    = name.size() > 4 &&
                              name.compare(name.size() - 4, 4, L".BUN") == 0;
        if (inTracks && isBun) {
            kind = name.find(L"\\STREAM") != std::wstring::npos ? TrackFile::Stream
                                                                : TrackFile::Layout;
            region = RegionFromName(name);
        }
    }

    // Handles are reused after a file is closed, so the list cannot grow without
    // bound or a stale entry would misjudge a later file.
    if (g_handles.size() > 256) g_handles.clear();
    g_handles.push_back({ handle, kind, region });
    if (regionOut) *regionOut = region;

    ::LeaveCriticalSection(&g_lock);
    return kind;
}

BOOL WINAPI HookedReadFile(HANDLE file, LPVOID buffer, DWORD toRead,
                           LPDWORD readOut, LPOVERLAPPED overlapped) {
    // Where the read starts has to be taken before the call, because the call
    // moves the file pointer. An overlapped read carries its own offset.
    ::InterlockedIncrement(&g_allReads);

    unsigned __int64 readAt = 0;
    bool knowOffset = false;

    if (overlapped) {
        readAt = ((unsigned __int64)overlapped->OffsetHigh << 32) | overlapped->Offset;
        knowOffset = true;
    } else if (!g_edits.empty() && Classify(file) != TrackFile::Other) {
        LARGE_INTEGER zero{}, here{};
        if (::SetFilePointerEx(file, zero, &here, FILE_CURRENT)) {
            readAt = (unsigned __int64)here.QuadPart;
            knowOffset = true;
        }
    }

    const BOOL ok = g_originalReadFile(file, buffer, toRead, readOut, overlapped);

    // The caller reads this after the call, and everything below -- the path
    // lookup above all -- overwrites it. A hook that quietly changes the error
    // code turns a successful read into whatever the last thing it did says.
    const DWORD error = ::GetLastError();
    struct Restore {
        DWORD value;
        ~Restore() { ::SetLastError(value); }
    } restore{ error };

    if (!ok || g_edits.empty() || !knowOffset) return ok;

    DWORD got = readOut ? *readOut : 0;

    // A read of nothing is still worth looking at. A section that outgrew the
    // room around it is moved past the end of the file, where the real read
    // returns zero bytes -- and then this is the only thing that can answer it.
    Region region;
    const TrackFile kind = Classify(file, &region);
    if (kind == TrackFile::Other) {
        if (got) ::InterlockedIncrement(&g_otherReads);
        return ok;
    }

    ::InterlockedIncrement(&g_streamReads);
    if (kind == TrackFile::Layout) ::InterlockedIncrement(&g_layoutReads);

    // Where the first read of each file landed, kept as numbers rather than
    // written out here. Opening a log file from inside a ReadFile hook is
    // asking the C runtime to re-enter the very call it is servicing, and the
    // process does not survive it. A watcher thread reports these instead.
    if (kind == TrackFile::Layout) {
        if (g_firstLayoutAt == ~0ull) g_firstLayoutAt = readAt;
    } else {
        if (g_firstStreamAt == ~0ull) g_firstStreamAt = readAt;
    }

    ApplyEdits(readAt, (char*)buffer, got, kind == TrackFile::Layout, region);

    // Now the part the file could not supply.
    //
    // Serving bytes the file does not contain is what makes the size of an edit
    // unbounded: the section no longer has to fit in the gap the original layout
    // happened to leave, because it does not have to be in the file at all.
    const DWORD fromFile = got;
    const DWORD supplied = SupplyBeyondFile(readAt, (char*)buffer, toRead, got,
                                            kind == TrackFile::Layout, region);
    if (supplied > got) {
        got = supplied;
        if (readOut) *readOut = got;
    }

    if (kind == TrackFile::Stream) Note(readAt, toRead, fromFile, got);
    return ok;
}

// Redirects every import of one function in a module's table.
//
// Going through the import table rather than writing into kernel32 keeps this
// out of other plugins' way and undoes cleanly. But it has to find them all:
// a module can import the same function through more than one descriptor, and
// patching the first one found leaves the game calling the real thing through
// the second -- a hook that reports success and never fires.
int PatchImport(HMODULE module, const char* function,
                void* replacement, void** original) {
    auto* base = (char*)module;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;

    auto* imports = (IMAGE_IMPORT_DESCRIPTOR*)(base + directory.VirtualAddress);

    // Resolved from kernel32 and from the API set it also lives behind, because
    // a module may import either and they are not always the same address.
    void* targets[2] = { nullptr, nullptr };
    if (HMODULE k32 = ::GetModuleHandleA("kernel32.dll"))
        targets[0] = (void*)::GetProcAddress(k32, function);
    if (HMODULE base2 = ::GetModuleHandleA("KernelBase.dll"))
        targets[1] = (void*)::GetProcAddress(base2, function);
    if (!targets[0] && !targets[1]) return 0;

    int patched = 0;
    for (; imports->Name; ++imports) {
        auto* thunk = (IMAGE_THUNK_DATA*)(base + imports->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            void* current = (void*)thunk->u1.Function;
            if (current != targets[0] && current != targets[1]) continue;

            DWORD protection = 0;
            if (!::VirtualProtect(&thunk->u1.Function, sizeof(void*),
                                  PAGE_READWRITE, &protection))
                continue;

            if (original && !*original) *original = current;
            thunk->u1.Function = (ULONG_PTR)replacement;
            ::VirtualProtect(&thunk->u1.Function, sizeof(void*), protection, &protection);
            ++patched;
        }
    }
    return patched;
}


// Every module in the process, not only the executable.
//
// The log made the case for this plainly: reads arrived through the game's own
// import table while not a single open did, so whatever opens the files is a
// library the game loads, reading through its own table. Patching only the
// executable leaves that library talking to the real ReadFile, which is why the
// stream was never seen.
//
// Safe to run again at any time: an entry already redirected no longer matches
// the real address, so a second pass skips it and cannot capture our own hook
// as the original.
// Whether a module is part of the game, as opposed to Windows, the C runtime
// or whatever the launcher brought with it.
//
// Scope is not caution for its own sake here. Redirecting the C runtime's own
// imports means this plugin's logging re-enters its own hooks, and the browser
// the launcher runs opens files constantly on threads of its own; neither has
// anything to do with reading a track. The DLL that does is the game's, so the
// game's folder is the boundary -- minus the scripts folder, which is where the
// launcher and this plugin live.
bool GameModule(HMODULE module) {
    if (module == g_module) return false;             // never ourselves

    wchar_t path[MAX_PATH]{};
    if (!::GetModuleFileNameW(module, path, MAX_PATH)) return false;

    std::wstring name = path;
    for (wchar_t& c : name) c = (wchar_t)::towupper(c);

    static std::wstring folder;
    if (folder.empty()) {
        folder = GameFolder();
        for (wchar_t& c : folder) c = (wchar_t)::towupper(c);
    }

    if (name.size() < folder.size()) return false;
    if (name.compare(0, folder.size(), folder) != 0) return false;

    return name.find(L"\\SCRIPTS\\") == std::wstring::npos;
}

int PatchEverywhere(const char* function, void* replacement, void** original) {
    HMODULE modules[512];
    DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed))
        return PatchImport(::GetModuleHandleW(nullptr), function, replacement, original);

    const size_t count = needed / sizeof(HMODULE);
    int patched = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!GameModule(modules[i])) continue;
        patched += PatchImport(modules[i], function, replacement, original);
    }
    return patched;
}

// Hooks a function where it lives rather than in anyone's import table.
//
// The executable resolves its imports only once it is running, so when this
// plugin loads its table has no CreateFile entry to redirect -- and the game
// reads GLOBAL\GLOBALB, the file the shop list is in, before the worker thread
// gets to patch anything. So the file replacements hook KernelBase itself, in
// the slot Windows leaves for exactly this: five bytes of padding before the
// function and a two-byte `mov edi, edi` at its start. The padding becomes a
// jump to the hook and the `mov` a short jump back onto it; the original is
// then the function two bytes in. Returns that, or null when the function does
// not have the slot (someone else got there first).
void* HotPatch(void* function, void* replacement) {
    if (!function) return nullptr;
    auto* start = (unsigned char*)function;
    if (start[0] != 0x8B || start[1] != 0xFF) return nullptr;
    for (int i = 1; i <= 5; ++i)
        if (start[-i] != 0xCC && start[-i] != 0x90) return nullptr;

    DWORD protection = 0;
    if (!::VirtualProtect(start - 5, 7, PAGE_EXECUTE_READWRITE, &protection)) return nullptr;
    start[-5] = 0xE9;                                            // jmp rel32
    const LONG offset = (LONG)((unsigned char*)replacement - start);
    std::memcpy(start - 4, &offset, 4);
    *(volatile unsigned short*)start = 0xF9EB;                   // jmp short -7
    ::VirtualProtect(start - 5, 7, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), start - 5, 7);
    return start + 2;
}

// Set once CreateFile is hot-patched; the import-table pass must then leave
// those entries alone, since what it would record as the original is the
// patched function and the hook would call itself.
bool g_createFileHotPatched = false;

// Installs, or reinstalls, every hook across whatever is loaded right now.
//
// Called again on each watcher tick because a module loaded later arrives with
// an untouched table of its own, and the track files are opened long after the
// plugin starts.
int InstallHooks() {
    void* slot = nullptr;
    int patched = 0;

    slot = nullptr;
    patched += PatchEverywhere("ReadFile", (void*)&HookedReadFile, &slot);
    if (slot) g_originalReadFile = (ReadFileFn)slot;

    if (!g_createFileHotPatched) {
        slot = nullptr;
        patched += PatchEverywhere("CreateFileW", (void*)&HookedCreateFileW, &slot);
        if (slot) g_originalCreateFileW = (CreateFileWFn)slot;

        slot = nullptr;
        patched += PatchEverywhere("CreateFileA", (void*)&HookedCreateFileA, &slot);
        if (slot) g_originalCreateFileA = (CreateFileAFn)slot;
    }

    slot = nullptr;
    patched += PatchEverywhere("CreateFileMappingW", (void*)&HookedCreateFileMappingW, &slot);
    if (slot) g_originalCreateFileMappingW = (CreateFileMappingWFn)slot;

    slot = nullptr;
    patched += PatchEverywhere("CreateFileMappingA", (void*)&HookedCreateFileMappingA, &slot);
    if (slot) g_originalCreateFileMappingA = (CreateFileMappingAFn)slot;

    slot = nullptr;
    patched += PatchEverywhere("MapViewOfFile", (void*)&HookedMapViewOfFile, &slot);
    if (slot) g_originalMapViewOfFile = (MapViewOfFileFn)slot;

    slot = nullptr;
    patched += PatchEverywhere("MapViewOfFileEx", (void*)&HookedMapViewOfFileEx, &slot);
    if (slot) g_originalMapViewOfFileEx = (MapViewOfFileExFn)slot;

    slot = nullptr;
    patched += PatchEverywhere("GetFileSize", (void*)&HookedGetFileSize, &slot);
    if (slot) g_originalGetFileSize = (GetFileSizeFn)slot;

    slot = nullptr;
    patched += PatchEverywhere("GetFileSizeEx", (void*)&HookedGetFileSizeEx, &slot);
    if (slot) g_originalGetFileSizeEx = (GetFileSizeExFn)slot;

    return patched;
}

DWORD WINAPI Start(LPVOID) {
    // The log, the lock and the file replacements were set up in DllMain.
    LoadEdits();
    CheckPublishedTargets();

    if (g_edits.empty() && g_redirects.empty()) {
        Log("nothing to apply; the game will read its own files unchanged");
        return 0;
    }

    g_originalReadFile = (ReadFileFn)::ReadFile;

    void* previous = nullptr;
    const int hooked = PatchImport(::GetModuleHandleW(nullptr), "ReadFile",
                                   (void*)&HookedReadFile, &previous);
    if (hooked && previous) g_originalReadFile = (ReadFileFn)previous;

    // Set before the patch, never after: if the table entry is redirected and
    // the original is not recorded, the hook calls through a null pointer.
    if (!g_createFileHotPatched) {
        g_originalCreateFileW = (CreateFileWFn)::CreateFileW;
        g_originalCreateFileA = (CreateFileAFn)::CreateFileA;

        void* previousW = nullptr;
        if (PatchImport(::GetModuleHandleW(nullptr), "CreateFileW",
                        (void*)&HookedCreateFileW, &previousW) && previousW)
            g_originalCreateFileW = (CreateFileWFn)previousW;

        void* previousA = nullptr;
        if (PatchImport(::GetModuleHandleW(nullptr), "CreateFileA",
                        (void*)&HookedCreateFileA, &previousA) && previousA)
            g_originalCreateFileA = (CreateFileAFn)previousA;

        g_originalCreateFileW = (CreateFileWFn)::CreateFileW;
        g_originalCreateFileA = (CreateFileAFn)::CreateFileA;
    }

    g_originalReadFile           = (ReadFileFn)::ReadFile;
    g_originalCreateFileMappingW = (CreateFileMappingWFn)::CreateFileMappingW;
    g_originalCreateFileMappingA = (CreateFileMappingAFn)::CreateFileMappingA;
    g_originalMapViewOfFile      = (MapViewOfFileFn)::MapViewOfFile;
    g_originalMapViewOfFileEx    = (MapViewOfFileExFn)::MapViewOfFileEx;
    g_originalGetFileSize        = (GetFileSizeFn)::GetFileSize;
    g_originalGetFileSizeEx      = (GetFileSizeExFn)::GetFileSizeEx;

    Log("hooks installed in %d import entries across the loaded modules",
        InstallHooks());

    // Reporting from out here keeps the hook free of anything that could
    // re-enter it, and a run that ends badly still leaves a record.
    LONG lastAll = -1, lastStream = -1, lastOther = -1, lastApplied = -1,
         lastViews = -1, lastSupplied = -1;
    int installed = 0;
    for (;;) {
        ::Sleep(2000);

        // A module loaded since the last pass brings an untouched table.
        if (const int more = InstallHooks()) {
            installed += more;
            Log("hooks installed in %d further import entries (%d since start)",
                more, installed);
        }

        // Every stream read since the last look, with what came of it.
        while (true) {
            ReadNote note{};
            bool have = false;

            ::EnterCriticalSection(&g_lock);
            const LONG count = g_noteCount;
            if (g_notesPrinted < (size_t)count &&
                g_notesPrinted < sizeof(g_notes) / sizeof(g_notes[0])) {
                note = g_notes[g_notesPrinted++];
                have = true;
            }
            ::LeaveCriticalSection(&g_lock);

            if (!have) break;

            Log("stream read: %llu..%llu (%u wanted), %u from the file, %u served",
                note.at, note.at + note.wanted, note.wanted, note.fromFile,
                note.served);
        }

        // Whatever the game has opened since the last look. Printed from
        // out here rather than from the hook, which must stay free of the
        // file writing that would re-enter the call it is servicing.
        for (;;) {
            std::wstring next;
            ::EnterCriticalSection(&g_lock);
            if (g_reported < g_seen.size()) next = g_seen[g_reported++];
            ::LeaveCriticalSection(&g_lock);
            if (next.empty()) break;

            char narrow[MAX_PATH]{};
            ::WideCharToMultiByte(CP_UTF8, 0, next.c_str(), -1, narrow,
                                  MAX_PATH, nullptr, nullptr);
            Log("opened: %s", narrow);
        }
        if (g_allReads == lastAll && g_streamReads == lastStream &&
            g_otherReads == lastOther && g_editsApplied == lastApplied &&
            g_mappedViews == lastViews && g_supplied == lastSupplied)
            continue;

        lastAll = g_allReads;
        lastViews = g_mappedViews;
        lastSupplied = g_supplied;

        lastStream = g_streamReads;
        lastOther = g_otherReads;
        lastApplied = g_editsApplied;

        Log("reads: %ld through the hook, %ld of a track file (%ld of the table), "
            "%ld of anything else with a known offset; %ld edits applied, "
            "%ld held back as another region's; "
            "%ld mapped views of a track; %ld reads answered from beyond the "
            "file's end, %ld sizes reported larger; first track byte at %lld, "
            "first table byte at %lld",
            lastAll, lastStream, g_layoutReads, lastOther, lastApplied,
            g_otherRegion, lastViews, lastSupplied, g_sizeAnswers,
            g_firstStreamAt == ~0ull ? -1ll : (long long)g_firstStreamAt,
            g_firstLayoutAt == ~0ull ? -1ll : (long long)g_firstLayoutAt);
    }
}

} // namespace

void StartFileEdits() {
    // The file replacements go in before DllMain returns, on the loading
    // thread. The game reads GLOBAL\GLOBALB while a thread would still be
    // starting, so a hook installed from there arrives after the one file the
    // shop list lives in. Only KernelBase's CreateFile is patched here -- the
    // import walk over every module stays on the thread -- and nothing here
    // waits on anything.
    ::InitializeCriticalSection(&g_lock);
    LoadRedirects(GameFolder() + L"\\models\\files", L"");
    // Patched whether or not anything is replaced. The region's table file is
    // opened before the import walk on the thread reaches the module that
    // opens it, and a handle this plugin never saw opened is never known to be
    // that file -- so a section whose size changed was read at its shipped
    // length, cut short, and the game crashed on it. This only ever worked
    // while a world happened to carry a file replacement as well.
    {
        HMODULE kernelBase = ::GetModuleHandleW(L"KernelBase.dll");
        void* w = kernelBase ? (void*)::GetProcAddress(kernelBase, "CreateFileW") : nullptr;
        void* a = kernelBase ? (void*)::GetProcAddress(kernelBase, "CreateFileA") : nullptr;
        // Each original is recorded before its function is patched: from the
        // moment the jump is written, any thread may land in the hook, and the
        // hook calls straight through it. The original of a hot-patched
        // function is that function two bytes in.
        g_originalCreateFileW = (CreateFileWFn)::CreateFileW;
        g_originalCreateFileA = (CreateFileAFn)::CreateFileA;
        bool hookedW = false, hookedA = false;
        if (w) {
            const CreateFileWFn previous = g_originalCreateFileW;
            g_originalCreateFileW = (CreateFileWFn)((unsigned char*)w + 2);
            hookedW = HotPatch(w, (void*)&HookedCreateFileW) != nullptr;
            if (!hookedW) g_originalCreateFileW = previous;
        }
        if (a) {
            const CreateFileAFn previous = g_originalCreateFileA;
            g_originalCreateFileA = (CreateFileAFn)((unsigned char*)a + 2);
            hookedA = HotPatch(a, (void*)&HookedCreateFileA) != nullptr;
            if (!hookedA) g_originalCreateFileA = previous;
        }
        // With either one in place the import-table pass must stay off
        // CreateFile: the address it would record as the original is the
        // patched function, and the hook would call itself.
        g_createFileHotPatched = hookedW || hookedA;
        if (hookedW || hookedA) {
            Log("file replacements: hooked in KernelBase -- CreateFileW %s, CreateFileA %s",
                hookedW ? "yes" : "no", hookedA ? "yes" : "no");
        } else {
            Log("file replacements: KernelBase CreateFile has no hot-patch slot (%p %p); "
                "only files opened after the import tables are patched are replaced", w, a);
        }
    }

    // Off the loader lock: opening files and walking the import table from
    // inside DllMain is asking for a deadlock.
    if (HANDLE thread = ::CreateThread(nullptr, 0, &Start, nullptr, 0, nullptr))
        ::CloseHandle(thread);
}

void LogFileEditTotals() {
    Log("on the way out: %ld reads through the hook, %ld of a track stream, "
        "%ld of other files, %ld edits applied, %ld held back as another region's",
        g_allReads, g_streamReads, g_otherReads, g_editsApplied, g_otherRegion);
}

} // namespace frs

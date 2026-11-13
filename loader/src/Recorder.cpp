// FRS World Loader -- the memory recorder
//
// A tool for working out what the game does, not for playing: off unless the
// .ini beside the plugin says [Recorder] Enabled=1. tools/memrec.py reads the
// recordings.

#include "Loader.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace frs {
namespace {

// ---------------------------------------------------------------------------
// Memory recorder
//
// Finding the switch the game flips when a shop's prompt comes up -- and the
// one it flips when the player takes it -- by reading code alone is slow. A
// recording of what changes in the game's global data while someone does it
// is fast: F9 starts and stops, F8 marks a moment ("the
// prompt is up", "I pressed Enter"), and the file is compared offline.
//
// Ten times a second the executable's writable sections are compared with the
// last look and every 32-bit word that changed is written with its new value,
// so a recording costs what changes rather than what exists. The keys are read
// with GetAsyncKeyState on the recorder's own thread: this is the physical
// keyboard of the person playing, which is exactly what is wanted here.
// ---------------------------------------------------------------------------

struct WritableRange { unsigned begin, end; };

std::vector<WritableRange> WritableSections() {
    std::vector<WritableRange> out;
    auto* base = (char*)::GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const unsigned begin = (unsigned)(size_t)base + section->VirtualAddress;
        const unsigned size = std::max(section->Misc.VirtualSize, section->SizeOfRawData) & ~3u;
        // Only what is committed and readable: a page that is not would take
        // the game down with the first read.
        for (unsigned at = begin; at < begin + size;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!::VirtualQuery((void*)(size_t)at, &info, sizeof(info))) break;
            const unsigned regionEnd = std::min(begin + size,
                                                (unsigned)((size_t)info.BaseAddress + info.RegionSize));
            const DWORD p = info.Protect & 0xFF;
            const bool readable = info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
                                  (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_READONLY ||
                                   p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_READ);
            if (readable) out.push_back({ at, regionEnd });
            at = regionEnd;
        }
    }
    return out;
}

bool Held(int key) { return (::GetAsyncKeyState(key) & 0x8000) != 0; }

// The keys, from the .ini beside the plugin ([Recorder] RecordKey,
// MarkKey, as virtual-key numbers). F9 and F8 by default: the game reads the
// keyboard through DirectInput and ignores modifiers, so a letter held with
// Ctrl+Shift still reloads the car (R) or opens the map (M), and F1, F5, F6,
// Home and Pause belong to other plugins (Extra Options among them).
int RecorderKey(const wchar_t* name, int fallback) {
    return Setting(L"Recorder", name, fallback);
}

DWORD WINAPI Recorder(LPVOID) {
    const int recordKey = RecorderKey(L"RecordKey", VK_F9);
    const int markKey = RecorderKey(L"MarkKey", VK_F8);
    const std::vector<WritableRange> ranges = WritableSections();
    size_t words = 0;
    for (const WritableRange& r : ranges) words += (r.end - r.begin) / 4;

    std::vector<unsigned> last(words);
    FILE* file = nullptr;
    bool recording = false, wasR = false, wasM = false;
    DWORD started = 0, nextSample = 0;
    int take = 0, marks = 0;
    unsigned long long changes = 0;

    const auto snapshot = [&](bool write) {
        size_t at = 0;
        std::vector<unsigned> frame;   // address, value pairs
        for (const WritableRange& r : ranges) {
            for (unsigned a = r.begin; a < r.end; a += 4, ++at) {
                const unsigned value = *(const volatile unsigned*)(size_t)a;
                if (write && value != last[at]) { frame.push_back(a); frame.push_back(value); }
                last[at] = value;
            }
        }
        if (!write || !file) return;
        const unsigned header[3] = { 0x454D4152u /* "RAME" */, ::GetTickCount() - started,
                                     (unsigned)(frame.size() / 2) };
        std::fwrite(header, 4, 3, file);
        if (!frame.empty()) std::fwrite(frame.data(), 4, frame.size(), file);
        changes += frame.size() / 2;
    };

    for (;;) {
        ::Sleep(15);
        const bool r = Held(recordKey), m = Held(markKey);

        if (r && !wasR) {
            if (!recording) {
                wchar_t path[MAX_PATH];
                std::wstring folder = GameFolder() + L"\\scripts";
                do {
                    ::swprintf_s(path, L"%s\\FRS-memory-%03d.bin", folder.c_str(), ++take);
                } while (::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && take < 999);
                {
                    const Inside inside;
                    if (::_wfopen_s(&file, path, L"wb") != 0) file = nullptr;
                }
                if (file) {
                    // The file opens with where everything is: the ranges, then
                    // a full copy, so a value can be read at any moment.
                    const unsigned head[3] = { 0x314D5246u /* "FRM1" */, (unsigned)ranges.size(), 0 };
                    std::fwrite(head, 4, 3, file);
                    for (const WritableRange& range : ranges) std::fwrite(&range, 4, 2, file);
                    snapshot(false);
                    std::fwrite(last.data(), 4, last.size(), file);
                    started = ::GetTickCount();
                    nextSample = started;
                    recording = true;
                    marks = 0;
                    changes = 0;
                    ::MessageBeep(MB_OK);
                    Log("memory recording %d started: %zu words in %zu sections", take, words, ranges.size());
                }
            } else {
                recording = false;
                if (file) { std::fclose(file); file = nullptr; }
                ::MessageBeep(MB_ICONHAND);
                Log("memory recording %d stopped after %lu ms: %llu changes, %d marks", take,
                    ::GetTickCount() - started, changes, marks);
            }
        }
        if (recording && m && !wasM && file) {
            const unsigned mark[3] = { 0x4B52414Du /* "MARK" */, ::GetTickCount() - started,
                                       (unsigned)++marks };
            std::fwrite(mark, 4, 3, file);
            ::MessageBeep(MB_ICONASTERISK);
        }
        wasR = r;
        wasM = m;

        if (recording && ::GetTickCount() >= nextSample) {
            nextSample += 100;
            snapshot(true);
        }
    }
}

} // namespace

void StartRecorder() {
    if (!Setting(L"Recorder", L"Enabled", 0)) return;
    if (HANDLE thread = ::CreateThread(nullptr, 0, &Recorder, nullptr, 0, nullptr))
        ::CloseHandle(thread);
    Log("memory recorder on: F9 starts and stops, F8 marks (keys in the .ini)");
}

} // namespace frs

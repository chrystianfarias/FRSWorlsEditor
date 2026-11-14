// FRSStreamLog.asi -- what the game's track streamer is doing, for checking
// docs/STREAMING-STUDY.md in the game (experiments E1..E7, see README.md).
//
// Reads only. Nothing in the game is patched or hooked and no game function
// is called: a thread of its own polls the fields below every few ms and logs
// what changed, plus one status line per second, to scripts\FRSStreamLog.log
// (the previous session's log is kept as FRSStreamLog.prev.log). Every read
// goes through SEH, so a field caught mid-update can only give a bad number,
// never a crash. F12 writes a numbered marker and a full snapshot.
//
// SPEED2.EXE v1.2 (checked from UG2.txt; the study's tags in brackets):
//   0x88EB00  TheTrackStreamer
//     +0x00/+0x04   section table (stride 0x50) / count             [C]
//     +0x20 +0x24 +0x28  reads in flight / activated / did not fit  [C, +0x28 I]
//     +0xB0         streaming position 0: +0 car index (-1 = camera),
//                   +4 pos xy, +0xC vel xy, +0x16 zone, +0x18 last zone [C]
//     +0x168        loading phase 0..6                               [C]
//     +0x16F        debug text "C5" / "C5 - C9" (SwitchZones)        [C]
//     +0x184/+0x188 sections dropped for memory: count / pointers    [C]
//     +0x298/+0x29C wanted list: count / section pointers            [C]
//     +0x6A0/+0x6A4 pool size / pool object                          [C]
//     +0x6A8        user memory held (pauses loading)                [C]
//   section entry: +0x08 i16 number, +0x0B wanted bits, +0x0C status
//     (0 none 1 allocated 2 reading 3 loaded 4 activated), +0x18 size,
//     +0x44 memory, +0x4C current size (set by activation 0x5DAA40)  [C]
//   pool object ([+0x6A4], built by 0x5E4B70): block list at +0x14,
//     node +0 next, +0xC size, +0x10 byte in use -- the same walk as the
//     game's free (0x5DEED0) and largest (0x5DEF00) callbacks         [C]
//   0x890080  pCurrentWorld: car = [world + 0x1C + 4 * car index], the
//     object whose +0x60/+0x70 the streamer reads (0x5E4F31)          [C]
//   car +0x5C4  TopologyCoordinate (wheels and body):                [C/I]
//     +0x08 tree (+0x30 i16 pack header number), +0x14 boundary
//     (+0x08 i16 zone)  -- 0x5E68D0 SetSectionPosition
//   0x88F768  TheTopologyManager: tree slots at +8, 520 of them,
//     slot(n) = 0x5D3C10                                              [C]
//   0x800DFC  section base (20 in L4RA)                               [C]
//   0x800DF0  camera zone of view 1, -1 when none (0x5E98E0)          [C]
//   0x7A47A4 1.5 s, 0x7A47A8 100 m, 0x7A52F4 178.81 m/s: prediction   [C]
//   0x7FB708  RealTimeFrames; 0x8654A4 game flow (6 = gameplay)       [C]
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- Addresses -------------------------------------------------------------

constexpr uintptr_t kStreamer      = 0x88EB00;
constexpr uintptr_t kWorld         = 0x890080;
constexpr uintptr_t kTopology      = 0x88F768;
constexpr uintptr_t kBase          = 0x800DFC;
constexpr uintptr_t kCameraZone    = 0x800DF0;
constexpr uintptr_t kFrames        = 0x7FB708;
constexpr uintptr_t kGameFlow      = 0x8654A4;
constexpr uintptr_t kPlayer        = 0x8900AC;   // +4 the player's car (FRSCarTrace)
constexpr uintptr_t kPredSeconds   = 0x7A47A4;
constexpr uintptr_t kPredMax       = 0x7A47A8;
constexpr uintptr_t kPredMaxSpeed  = 0x7A52F4;
constexpr uintptr_t kPoolSizes     = 0x79DC54;   // 3 ints, KiB

constexpr size_t kSectionStride = 0x50;
constexpr int    kTopologySlots = 520;

// ---- Safe reads --------------------------------------------------------------

bool Copy(void* to, uintptr_t from, size_t n) {
    if (from < 0x10000) return false;
    __try {
        std::memcpy(to, (const void*)from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T> T Get(uintptr_t a, T fallback = T{}) {
    T v;
    return Copy(&v, a, sizeof(T)) ? v : fallback;
}

// ---- Log -------------------------------------------------------------------

char g_folder[MAX_PATH];
FILE* g_log = nullptr;
int g_frame = 0;

void Log(const char* format, ...) {
    if (!g_log) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::fprintf(g_log, "[%02u:%02u:%02u.%03u f%d] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, g_frame);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// "C25" for 325, "-" for none.
std::string Name(int n) {
    if (n <= 0) return n == 0 ? "0" : "-";
    char text[16];
    std::snprintf(text, sizeof(text), "%c%d", (char)('@' + n / 100), n % 100);
    return text;
}

std::string Kib(uint32_t bytes) {
    char text[24];
    std::snprintf(text, sizeof(text), "%uK", (bytes + 512) / 1024);
    return text;
}

const char* StatusName(int s) {
    switch (s) {
    case 0: return "none";
    case 1: return "alloc";
    case 2: return "reading";
    case 3: return "loaded";
    case 4: return "active";
    default: return "?";
    }
}

// Lines of names, wrapped so the log stays readable.
void LogList(const char* head, const std::vector<std::string>& items) {
    if (items.empty()) return;
    std::string line;
    bool first = true;
    for (const std::string& item : items) {
        if (line.size() + item.size() > 150) {
            Log("%-8s %s", first ? head : "", line.c_str());
            line.clear();
            first = false;
        }
        if (!line.empty()) line += ' ';
        line += item;
    }
    if (!line.empty()) Log("%-8s %s", first ? head : "", line.c_str());
}

// ---- State -----------------------------------------------------------------

struct Section {
    int16_t number = 0;
    uint8_t wanted = 0;
    int status = 0;
    uint32_t size = 0;      // +0x18
    uint32_t current = 0;   // +0x4C
};

struct Coord {
    bool valid = false;
    uintptr_t tree = 0;
    int treeNumber = 0;     // pack header number, e.g. 325
    int zone = 0;           // boundary zone, e.g. 305
};

struct Pool {
    bool valid = false;
    uint32_t size = 0, used = 0, free = 0, largest = 0;
    int blocks = 0;
};

struct State {
    uintptr_t table = 0;
    int count = 0;
    std::vector<Section> sections;
    int phase = 0, reads = 0, activated = 0, noFit = 0, userMem = 0;
    int carIndex = -1;
    float pos[2] = {}, vel[2] = {};
    int loadZone = 0, lastZone = 0;
    int cameraZone = -1;
    char debugText[24] = {};
    std::vector<int> wanted;      // numbers, list order
    std::vector<int> dropped;
    uintptr_t car = 0;
    float carPos[3] = {};
    Coord coord;
    int base = 20;
};

float g_predSeconds = 1.5f, g_predMax = 100.0f, g_predMaxSpeed = 178.81f;

int NumberAt(uintptr_t table, int count, uintptr_t entry) {
    if (!table || entry < table) return 0;
    const uintptr_t index = (entry - table) / kSectionStride;
    if ((entry - table) % kSectionStride || (int)index >= count) return 0;
    return Get<int16_t>(entry + 8);
}

uintptr_t FindCar(int index, float tsPosX, float tsPosY) {
    // The streamer's own car first (what it reads its position from).
    if (index >= 0 && index < 16) {
        const uintptr_t world = Get<uintptr_t>(kWorld);
        if (world) {
            const uintptr_t car = Get<uintptr_t>(world + 0x1C + 4 * index);
            if (car) {
                const float x = Get<float>(car + 0x60, NAN), y = Get<float>(car + 0x64, NAN);
                if (std::fabs(x - tsPosX) < 50.0f && std::fabs(y - tsPosY) < 50.0f) return car;
            }
        }
    }
    // Else the player's car, as FRSCarTrace finds it.
    const uintptr_t player = Get<uintptr_t>(kPlayer);
    return player ? Get<uintptr_t>(player + 4) : 0;
}

bool Read(State& s) {
    s.table = Get<uintptr_t>(kStreamer + 0x00);
    s.count = Get<int>(kStreamer + 0x04);
    if (!s.table || s.count <= 0 || s.count > 4096) return false;

    std::vector<uint8_t> raw((size_t)s.count * kSectionStride);
    if (!Copy(raw.data(), s.table, raw.size())) return false;
    s.sections.resize((size_t)s.count);
    for (int i = 0; i < s.count; ++i) {
        const uint8_t* e = raw.data() + (size_t)i * kSectionStride;
        Section& sec = s.sections[(size_t)i];
        std::memcpy(&sec.number, e + 0x08, 2);
        sec.wanted = e[0x0B];
        std::memcpy(&sec.status, e + 0x0C, 4);
        std::memcpy(&sec.size, e + 0x18, 4);
        std::memcpy(&sec.current, e + 0x4C, 4);
    }

    s.reads     = Get<int>(kStreamer + 0x20);
    s.activated = Get<int>(kStreamer + 0x24);
    s.noFit     = Get<int>(kStreamer + 0x28);
    s.carIndex  = Get<int>(kStreamer + 0xB0, -1);
    s.pos[0]    = Get<float>(kStreamer + 0xB4);
    s.pos[1]    = Get<float>(kStreamer + 0xB8);
    s.vel[0]    = Get<float>(kStreamer + 0xBC);
    s.vel[1]    = Get<float>(kStreamer + 0xC0);
    s.loadZone  = Get<int16_t>(kStreamer + 0xC6);
    s.lastZone  = Get<int16_t>(kStreamer + 0xC8);
    s.phase     = Get<int>(kStreamer + 0x168);
    s.userMem   = Get<int>(kStreamer + 0x6A8);
    Copy(s.debugText, kStreamer + 0x16F, 20);
    s.debugText[20] = 0;
    for (char& c : s.debugText)
        if (c && (c < 32 || c > 126)) c = '?';
    s.cameraZone = Get<int>(kCameraZone, -1);
    s.base = Get<int>(kBase, 20);

    s.wanted.clear();
    const int wantedCount = Get<int>(kStreamer + 0x298);
    if (wantedCount > 0 && wantedCount <= 256) {
        uintptr_t ptrs[256];
        if (Copy(ptrs, kStreamer + 0x29C, (size_t)wantedCount * 4))
            for (int i = 0; i < wantedCount; ++i)
                if (int n = NumberAt(s.table, s.count, ptrs[i])) s.wanted.push_back(n);
    }
    s.dropped.clear();
    const int droppedCount = Get<int>(kStreamer + 0x184);
    if (droppedCount > 0 && droppedCount <= 64) {
        uintptr_t ptrs[64];
        if (Copy(ptrs, kStreamer + 0x188, (size_t)droppedCount * 4))
            for (int i = 0; i < droppedCount; ++i)
                if (int n = NumberAt(s.table, s.count, ptrs[i])) s.dropped.push_back(n);
    }

    s.car = 0;
    s.coord = Coord{};
    if (Get<int>(kGameFlow) == 6) {
        s.car = FindCar(s.carIndex, s.pos[0], s.pos[1]);
        if (s.car && Copy(s.carPos, s.car + 0x60, 12)) {
            const uintptr_t c = s.car + 0x5C4;
            s.coord.tree = Get<uintptr_t>(c + 0x08);
            s.coord.treeNumber = s.coord.tree ? Get<int16_t>(s.coord.tree + 0x30) : 0;
            const uintptr_t boundary = Get<uintptr_t>(c + 0x14);
            s.coord.zone = boundary ? Get<int16_t>(boundary + 0x08) : 0;
            s.coord.valid = true;
        } else {
            s.car = 0;
        }
    }
    return true;
}

Pool ReadPool() {
    Pool p;
    p.size = Get<uint32_t>(kStreamer + 0x6A0);
    const uintptr_t pool = Get<uintptr_t>(kStreamer + 0x6A4);
    if (!pool) return p;
    const uintptr_t head = pool + 0x14;
    uintptr_t node = Get<uintptr_t>(head);
    for (int guard = 0; node && node != head && guard < 100000; ++guard) {
        uint8_t raw[0x14];
        if (!Copy(raw, node, sizeof(raw))) return p;
        uint32_t next, size;
        std::memcpy(&next, raw, 4);
        std::memcpy(&size, raw + 0x0C, 4);
        if (raw[0x10]) p.used += size;
        else {
            p.free += size;
            if (size > p.largest) p.largest = size;
        }
        ++p.blocks;
        node = next;
    }
    p.valid = node == head;
    return p;
}

// Slot(n) of 0x5D3C10.
int Slot(int n, int base) {
    if (n == 0) return 0;
    int m = n % 100;
    if (m >= base && m < 2 * base) n -= base;
    return (n / 100 - 1) * 20 + n % 100;
}

uintptr_t SlotTree(int zone, int base) {
    const int slot = Slot(zone, base);
    if (slot < 0 || slot >= kTopologySlots) return 0;
    return Get<uintptr_t>(kTopology + 8 + 4 * (uintptr_t)slot);
}

void Predict(const State& s, float out[2], float* reach) {
    const float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
    float scale = 0.0f;
    if (speed <= g_predMaxSpeed) {
        scale = g_predSeconds;
        if (g_predSeconds * speed > g_predMax && speed > 0.0f) scale = g_predMax / speed;
    }
    out[0] = s.pos[0] + s.vel[0] * scale;
    out[1] = s.pos[1] + s.vel[1] * scale;
    *reach = speed * scale;
}

const Section* FindSection(const State& s, int number) {
    for (const Section& sec : s.sections)
        if (sec.number == number) return &sec;
    return nullptr;
}

std::string SectionState(const State& s, int number) {
    const Section* sec = FindSection(s, number);
    if (!sec) return Name(number) + " (no table entry)";
    return Name(number) + " " + StatusName(sec->status);
}

void LogStatus(const State& s, const char* tag) {
    const Pool pool = ReadPool();
    int loaded = 0, active = 0;
    for (const Section& sec : s.sections) {
        if (sec.status >= 1) ++loaded;
        if (sec.status == 4) ++active;
    }
    const float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
    float pred[2], reach;
    Predict(s, pred, &reach);
    char poolText[160];
    if (pool.valid)
        std::snprintf(poolText, sizeof(poolText), "pool used %s free %s largest %s of %s (%d blocks)", Kib(pool.used).c_str(),
                      Kib(pool.free).c_str(), Kib(pool.largest).c_str(), Kib(pool.size).c_str(), pool.blocks);
    else
        std::snprintf(poolText, sizeof(poolText), "pool ? (size %s)", Kib(pool.size).c_str());
    char carText[200];
    if (s.coord.valid)
        std::snprintf(carText, sizeof(carText), "car (%.1f %.1f %.1f) %.0f km/h | coll %s tree %s",
                      s.carPos[0], s.carPos[1], s.carPos[2], speed * 3.6f, Name(s.coord.zone).c_str(),
                      s.coord.tree ? Name(s.coord.treeNumber).c_str() : "NULL");
    else
        std::snprintf(carText, sizeof(carText), "streampos (%.1f %.1f) %.0f km/h | no car", s.pos[0], s.pos[1], speed * 3.6f);
    const bool predicted = s.coord.valid && s.coord.zone > 0 && s.loadZone != s.coord.zone;
    Log("%s %s | load %s cam %s | pred %s (%.1f %.1f) +%.0fm | phase %d reads %d act %d nofit %d user %d | wanted %d alloc %d active %d dropped %d | %s",
        tag, carText, Name(s.loadZone).c_str(), Name(s.cameraZone).c_str(), predicted ? "ON" : "off", pred[0], pred[1], reach,
        s.phase, s.reads, s.activated, s.noFit, s.userMem, (int)s.wanted.size(), loaded, active, (int)s.dropped.size(), poolText);
}

void LogSnapshot(const State& s) {
    LogStatus(s, "SNAP");
    Log("SNAP     base %d, debug text \"%s\", last zone %s, car index %d", s.base, s.debugText, Name(s.lastZone).c_str(), s.carIndex);
    if (s.coord.valid) {
        const uintptr_t slotTree = SlotTree(s.coord.zone, s.base);
        Log("SNAP     coll zone %s slot %d: slot tree %s, car tree %s; zone's geometry section %s", Name(s.coord.zone).c_str(),
            Slot(s.coord.zone, s.base), slotTree ? Name(Get<int16_t>(slotTree + 0x30)).c_str() : "NULL",
            s.coord.tree ? Name(s.coord.treeNumber).c_str() : "NULL",
            s.coord.zone > 0 ? SectionState(s, s.coord.zone + s.base).c_str() : "-");
    }
    std::vector<std::string> items;
    for (int n : s.wanted) {
        const Section* sec = FindSection(s, n);
        if (!sec) continue;
        items.push_back(Name(n) + ":" + StatusName(sec->status) + ":" + Kib(sec->size) + "/" + Kib(sec->current));
    }
    LogList("WANTED", items);
    items.clear();
    for (const Section& sec : s.sections)
        if (sec.status >= 1 && !sec.wanted)
            items.push_back(Name(sec.number) + ":" + StatusName(sec.status) + ":" + Kib(sec.current));
    LogList("UNWANTED", items);
    items.clear();
    for (int n : s.dropped) items.push_back(Name(n));
    LogList("DROPPED", items);
}

bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI Watch(LPVOID) {
    char path[MAX_PATH], prev[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\FRSStreamLog.log", g_folder);
    std::snprintf(prev, sizeof(prev), "%s\\FRSStreamLog.prev.log", g_folder);
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = std::fopen(path, "w");
    if (!g_log) return 0;

    g_predSeconds = Get<float>(kPredSeconds);
    g_predMax = Get<float>(kPredMax);
    g_predMaxSpeed = Get<float>(kPredMaxSpeed);
    if (g_predSeconds != 1.5f || g_predMax != 100.0f || std::fabs(g_predMaxSpeed - 178.81f) > 0.01f) {
        Log("FRSStreamLog: this is not SPEED2.EXE v1.2 (prediction constants %g %g %g); nothing will be logged", g_predSeconds,
            g_predMax, g_predMaxSpeed);
        std::fclose(g_log);
        g_log = nullptr;
        return 0;
    }
    Log("FRSStreamLog: polling TheTrackStreamer (read only). F12 = marker + snapshot. Pool sizes %d/%d/%d KiB.",
        Get<int>(kPoolSizes), Get<int>(kPoolSizes + 4), Get<int>(kPoolSizes + 8));
    Log("Legend: load = loading zone (+0xC6), cam = camera zone, coll = car's collision zone, tree = collision pack bound to the car;");
    Log("        section status none/alloc/reading/loaded/active; sizes full(+0x18)/current(+0x4C) in KiB.");

    State last, s;
    bool haveLast = false;
    bool gap = false;
    int gapFrame = 0;
    DWORD gapTick = 0, lastStatus = 0;
    bool f12 = false, lastPredicted = false;
    int marks = 0;
    std::string lastDebug;

    for (;;) {
        Sleep(4);
        g_frame = Get<int>(kFrames);
        if (!Read(s)) {
            if (haveLast) Log("TABLE    the streamer's section table is gone (region unloaded)");
            haveLast = false;
            continue;
        }
        const DWORD tick = GetTickCount();

        if (!haveLast || s.table != last.table || s.count != last.count) {
            Log("TABLE    %d sections at 0x%08X, base %d, pool %s", s.count, (unsigned)s.table, s.base,
                Kib(Get<uint32_t>(kStreamer + 0x6A0)).c_str());
            last = s;
            last.loadZone = -999;   // log everything below once
            last.cameraZone = -999;
            last.phase = -1;
            last.coord.valid = false;
            last.wanted.clear();
            last.dropped.clear();
            for (Section& sec : last.sections) sec.status = 0;
            haveLast = true;
        }

        // Zones.
        if (s.loadZone != last.loadZone) {
            float pred[2], reach;
            Predict(s, pred, &reach);
            const float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]);
            Log("ZONE     load %s -> %s at (%.1f %.1f) vel (%.1f %.1f) %.0f km/h, pred (%.1f %.1f) +%.0fm, coll %s, cam %s",
                Name(last.loadZone).c_str(), Name(s.loadZone).c_str(), s.pos[0], s.pos[1], s.vel[0], s.vel[1], speed * 3.6f,
                pred[0], pred[1], reach, s.coord.valid ? Name(s.coord.zone).c_str() : "-", Name(s.cameraZone).c_str());
        }
        if (s.cameraZone != last.cameraZone && last.cameraZone != -999)
            Log("CAMZONE  %s -> %s", Name(last.cameraZone).c_str(), Name(s.cameraZone).c_str());
        if (lastDebug != s.debugText) {
            lastDebug = s.debugText;
            Log("SWITCH   debug text \"%s\"", s.debugText);
        }
        if (s.phase != last.phase) {
            if (s.phase == 6 && last.phase >= 0) LogStatus(s, "PHASE6 ");
            else Log("PHASE    %d -> %d", last.phase, s.phase);
        }
        if (s.userMem != last.userMem) Log("USERMEM  %d -> %d (loading paused while > 0)", last.userMem, s.userMem);

        // Prediction (inferred: the loading zone differs from the car's own zone).
        const bool predicted = s.coord.valid && s.coord.zone > 0 && s.loadZone != s.coord.zone;
        if (predicted != lastPredicted) {
            float pred[2], reach;
            Predict(s, pred, &reach);
            Log("PRED     %s: load %s, car in %s, pred (%.1f %.1f) +%.0fm", predicted ? "ON" : "off", Name(s.loadZone).c_str(),
                Name(s.coord.zone).c_str(), pred[0], pred[1], reach);
            lastPredicted = predicted;
        }

        // Collision binding.
        if (s.coord.valid) {
            if (!last.coord.valid || s.coord.zone != last.coord.zone || s.coord.tree != last.coord.tree ||
                s.coord.treeNumber != last.coord.treeNumber)
                Log("COLL     zone %s tree %s at (%.1f %.1f %.1f)", Name(s.coord.zone).c_str(),
                    s.coord.tree ? Name(s.coord.treeNumber).c_str() : "NULL", s.carPos[0], s.carPos[1], s.carPos[2]);
            const bool bad = s.coord.zone > 0 && (!s.coord.tree || s.coord.treeNumber - s.base != s.coord.zone);
            if (bad && !gap) {
                gap = true;
                gapFrame = g_frame;
                gapTick = tick;
                Log("GAP      start: car in %s, tree %s; geometry section %s; load zone %s", Name(s.coord.zone).c_str(),
                    s.coord.tree ? Name(s.coord.treeNumber).c_str() : "NULL", SectionState(s, s.coord.zone + s.base).c_str(),
                    Name(s.loadZone).c_str());
            } else if (!bad && gap) {
                gap = false;
                Log("GAP      end after %d frames, %u ms: car in %s, tree %s", g_frame - gapFrame, (unsigned)(tick - gapTick),
                    Name(s.coord.zone).c_str(), s.coord.tree ? Name(s.coord.treeNumber).c_str() : "NULL");
            }
        } else if (last.coord.valid) {
            Log("COLL     no car");
            if (gap) {
                gap = false;
                Log("GAP      end (no car) after %d frames", g_frame - gapFrame);
            }
        }

        // Wanted list changes.
        if (s.wanted != last.wanted) {
            std::vector<std::string> added, removed;
            for (int n : s.wanted)
                if (std::find(last.wanted.begin(), last.wanted.end(), n) == last.wanted.end()) added.push_back(Name(n));
            for (int n : last.wanted)
                if (std::find(s.wanted.begin(), s.wanted.end(), n) == s.wanted.end()) removed.push_back(Name(n));
            if (!added.empty() || !removed.empty())
                Log("WANTED   now %d (+%d -%d)", (int)s.wanted.size(), (int)added.size(), (int)removed.size());
            LogList("WANT+", added);
            LogList("WANT-", removed);
        }
        if (s.dropped != last.dropped) {
            std::vector<std::string> items;
            for (int n : s.dropped) {
                const Section* sec = FindSection(s, n);
                items.push_back(Name(n) + (sec ? ":" + Kib(sec->size) : std::string()));
            }
            if (items.empty()) Log("DROPPED  (empty)");
            else LogList("DROPPED", items);
        }

        // Section status changes, grouped by new status.
        if (s.sections.size() == last.sections.size()) {
            std::vector<std::string> unload, alloc, reading, loaded, active, other;
            for (size_t i = 0; i < s.sections.size(); ++i) {
                const Section& a = last.sections[i];
                const Section& b = s.sections[i];
                if (a.status == b.status) continue;
                const std::string n = Name(b.number);
                switch (b.status) {
                case 0: unload.push_back(n + ":" + StatusName(a.status)); break;
                case 1: alloc.push_back(n + ":" + Kib(b.size)); break;
                case 2: reading.push_back(n + ":" + Kib(b.size)); break;
                case 3: loaded.push_back(n); break;
                case 4: active.push_back(n + ":" + Kib(b.size) + "/" + Kib(b.current)); break;
                default: other.push_back(n + ":" + std::to_string(a.status) + ">" + std::to_string(b.status)); break;
                }
            }
            LogList("UNLOAD", unload);
            LogList("ALLOC", alloc);
            LogList("READ", reading);
            LogList("LOADED", loaded);
            LogList("ACTIVE", active);
            LogList("STATUS?", other);
        }

        // Marker.
        const bool pressed = (GetAsyncKeyState(VK_F12) & 0x8000) != 0 && OurWindowFocused();
        if (pressed && !f12) {
            ++marks;
            Log("==================== MARK %d ====================", marks);
            LogSnapshot(s);
        }
        f12 = pressed;

        // Status: each second in gameplay, each 10 s otherwise.
        const DWORD every = Get<int>(kGameFlow) == 6 ? 1000 : 10000;
        if (tick - lastStatus >= every) {
            LogStatus(s, "STAT   ");
            lastStatus = tick;
        }

        std::swap(last, s);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        GetModuleFileNameA(module, g_folder, MAX_PATH);
        if (char* slash = std::strrchr(g_folder, '\\')) *slash = 0;
        if (HANDLE thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}

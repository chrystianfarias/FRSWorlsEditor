// FRS World Loader -- shops placed in the editor
//
// The editor places a shop as a working copy of a shipped one: a career shop
// record, a free-roam zone, a beacon position and its garage door, written to
// <game>\models\files (see the editor's io/MarkerWrite). Its record carries
// "FRSP" at +0x54, and that is all this file looks for. What the game still
// needs from here, found with the memory recorder and the code:
//
// - Being discovered. 0x533310 lets a shop into the career's event table --
//   which the prompt, the minimap and the world map all read -- only once the
//   save lists it as discovered: 0x5003B0 looks the shop's name hash up in the
//   save's list ([0x85AD40+0x7AFC] count, +0x7B00 entries of 8 bytes: hash,
//   state) and wants state 1. A save made before the shop existed has no entry
//   for it, so for a placed shop's hash the lookup answers with an entry of the
//   plugin's own, already discovered. The save itself is not touched.
// - The GPS point. A zone names it at +0x34/+0x38/+0x3C, x, y, z in whole units
//   (0x529480), and the route search only ends on a road segment that holds it:
//   inside its width and length, within 4 units of its height (0x413250). The
//   editor cannot tell where the GPS roads run, so the point moves onto the
//   centre of the segment nearest the shop, in the game's road network:
//   [0x88D218] blocks at [0x88D21C + 4 i], plan box at +0x60 / +0x68, a segment
//   count (i16) at +0x34, segments from +0x8C, 0x38 bytes each (centre x, y at
//   +0, height at +0x12 in eighths). The zone is the one the event table
//   (0x85AD40, 64 entries of 0x18: zone at +0, shop at +8) pairs with the shop.
// - Room on the minimap. Its icons come from a pool the HUD package lays out
//   beforehand: 0x4DB900 looks up <KIND>INDICATOR_0..7 for each of 17 kinds
//   (list 0x836560). The package has six of a shop kind and the shipped shops
//   use them all; the editor adds more to HUD_SingleRace.fng, and the eight
//   here becomes sixteen so those up to _15 are found.
//
// Once a session it also writes to the log every test 0x533310 puts each
// placed shop through, so a shop that does not show can be told apart.

#include "Loader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace frs {
namespace {

constexpr unsigned kPlacedMagic   = 0x50535246;   // "FRSP" at +0x54 of the shop record
constexpr size_t   kPlacedMagicAt = 0x54;

constexpr unsigned kShopList = 0x8383F0, kShopCount = 0x8383F4, kShopRecordSize = 160;

constexpr uintptr_t kEventTable = 0x85AD40;
constexpr int       kEventSlots = 64;
constexpr size_t    kEventSlot = 0x18;

constexpr uintptr_t kRoadBlocks = 0x88D218, kRoadTable = 0x88D21C;
constexpr size_t    kRoadSegmentsAt = 0x8C, kRoadSegment = 0x38;
constexpr float     kRoadReach = 200.0f;
constexpr size_t    kZoneCentre = 0x04, kZoneHeight = 0x14, kZoneGps = 0x34;

constexpr DWORD kCheckEveryMs = 200;
constexpr DWORD kReportAfterMs = 4000;

// ---- discovered ----------------------------------------------------------------

constexpr unsigned kSaveLookup         = 0x5003B0;   // push esi / mov esi, [ecx+0x7AFC]
constexpr unsigned kSaveLookupContinue = 0x5003B7;

struct SaveEntry { unsigned hash; unsigned char state, pad[3]; };
SaveEntry g_entries[32];

bool IsPlaced(const unsigned char* record) {
    return *(const unsigned*)(record + kPlacedMagicAt) == kPlacedMagic;
}

extern "C" SaveEntry* __cdecl PlacedShopSaveEntry(unsigned hash) {
    auto* list = *(unsigned char**)(size_t)kShopList;
    const unsigned count = *(unsigned*)(size_t)kShopCount;
    if (!list || count > 512 || ::IsBadReadPtr(list, count * kShopRecordSize)) return nullptr;
    bool placed = false;
    for (unsigned i = 0; i < count && !placed; ++i) {
        const unsigned char* record = list + i * kShopRecordSize;
        placed = *(const unsigned*)(record + 0x38) == hash && IsPlaced(record);
    }
    if (!placed) return nullptr;
    for (SaveEntry& entry : g_entries) {
        if (entry.hash == hash) return &entry;
        if (entry.hash == 0) {
            entry.hash = hash;
            entry.state = 1;
            return &entry;
        }
    }
    return nullptr;
}

// Replaces the seven bytes `push esi / mov esi, [ecx+0x7AFC]` that open
// 0x5003B0 (thiscall, the hash on the stack, `ret 4`):
//
//        push ecx / push edx / push dword [esp+0xC] / call PlacedShopSaveEntry
//        add esp, 4 / pop edx / pop ecx / test eax, eax / jz go / ret 4
//   go:  push esi / mov esi, [ecx+0x7AFC] / jmp 0x5003B7
bool HookSaveLookup() {
    auto* target = (unsigned char*)(size_t)kSaveLookup;
    const unsigned char expected[7] = { 0x56, 0x8B, 0xB1, 0xFC, 0x7A, 0x00, 0x00 };
    if (::IsBadReadPtr(target, 7) || std::memcmp(target, expected, 7) != 0) return false;

    unsigned char* stub = NewCode(64);
    if (!stub) return false;
    size_t n = 0;
    stub[n++] = 0x51; stub[n++] = 0x52;                                       // push ecx / push edx
    stub[n++] = 0xFF; stub[n++] = 0x74; stub[n++] = 0x24; stub[n++] = 0x0C;   // push dword [esp+0xC]
    stub[n] = 0xE8;                                                           // call PlacedShopSaveEntry
    Put32(stub + n + 1, (unsigned)((unsigned char*)&PlacedShopSaveEntry - (stub + n + 5)));
    n += 5;
    stub[n++] = 0x83; stub[n++] = 0xC4; stub[n++] = 0x04;                     // add esp, 4
    stub[n++] = 0x5A; stub[n++] = 0x59;                                       // pop edx / pop ecx
    stub[n++] = 0x85; stub[n++] = 0xC0;                                       // test eax, eax
    stub[n++] = 0x74; stub[n++] = 0x03;                                       // jz go (+3)
    stub[n++] = 0xC2; stub[n++] = 0x04; stub[n++] = 0x00;                     // ret 4
    std::memcpy(stub + n, expected, 7); n += 7;                               // go:
    PutJump(stub + n, (void*)(size_t)kSaveLookupContinue);

    PatchJump(target, 7, stub);
    return true;
}

// ---- the minimap's pool ----------------------------------------------------------

constexpr unsigned kMinimapPoolCount = 0x4DB97A;   // cmp ebx, 8 / jl 0x4DB912

bool WidenMinimapPool() {
    auto* target = (unsigned char*)(size_t)kMinimapPoolCount;
    if (::IsBadReadPtr(target, 3)) return false;
    if (target[0] == 0x83 && target[1] == 0xFB && target[2] == 0x10) return true;   // done already
    const unsigned char expected[3] = { 0x83, 0xFB, 0x08 };
    if (std::memcmp(target, expected, 3) != 0) return false;
    const unsigned char sixteen = 0x10;
    Patch(target + 2, &sixteen, 1);
    return true;
}

// ---- the GPS point ----------------------------------------------------------------

uintptr_t ZoneOf(uintptr_t record) {
    for (int i = 0; i < kEventSlots; ++i) {
        const uintptr_t at = kEventTable + i * kEventSlot;
        if (Peek<uintptr_t>(at + 8) == record) return Peek<uintptr_t>(at);
    }
    return 0;
}

bool NearestRoad(float x, float y, float z, float out[3], float& distance) {
    const unsigned blocks = Peek<unsigned>(kRoadBlocks);
    if (!blocks || blocks > 4096) return false;
    bool found = false;
    float best = 0.0f;
    for (unsigned b = 0; b < blocks; ++b) {
        const uintptr_t block = Peek<uintptr_t>(kRoadTable + 4 * b);
        if (!block) continue;
        const float x0 = Peek<float>(block + 0x60), y0 = Peek<float>(block + 0x64);
        const float x1 = Peek<float>(block + 0x68), y1 = Peek<float>(block + 0x6C);
        if (x < x0 - kRoadReach || x > x1 + kRoadReach || y < y0 - kRoadReach || y > y1 + kRoadReach)
            continue;
        const int count = Peek<short>(block + 0x34);
        for (int s = 0; s < count && s < 4096; ++s) {
            const uintptr_t segment = block + kRoadSegmentsAt + s * kRoadSegment;
            const float sx = Peek<float>(segment), sy = Peek<float>(segment + 4);
            const float sz = Peek<short>(segment + 0x12) / 8.0f;
            // Height counts too: a road on a bridge overhead is not the one here.
            const float d = (sx - x) * (sx - x) + (sy - y) * (sy - y) + 4 * (sz - z) * (sz - z);
            if (found && d >= best) continue;
            found = true;
            best = d;
            out[0] = sx; out[1] = sy; out[2] = sz;
        }
    }
    distance = found ? std::sqrt(best) : 0.0f;
    return found;
}

bool SnapGpsTarget(uintptr_t record) {
    const uintptr_t zone = ZoneOf(record);
    if (!zone || ::IsBadWritePtr((void*)(zone + kZoneGps), 12)) return false;
    const float x = Peek<float>(zone + kZoneCentre), y = Peek<float>(zone + kZoneCentre + 4);
    const float z = Peek<float>(zone + kZoneHeight);
    float road[3] = {};
    float distance = 0.0f;
    if (!NearestRoad(x, y, z, road, distance)) return false;
    int* gps = (int*)(zone + kZoneGps);
    for (int k = 0; k < 3; ++k) gps[k] = (int)std::lround(road[k]);
    Log("markers: GPS target of the shop at (%.1f, %.1f, %.1f) moved onto the road at (%d, %d, %d), %.1f away",
        x, y, z, gps[0], gps[1], gps[2], distance);
    return true;
}

// ---- the report -------------------------------------------------------------------

// Read only; the one call (0x512870) is a pure test of the career's progress,
// made on the game thread like the game's own.
void ReportPlacedShops() {
    const uintptr_t list = Peek<uintptr_t>(kShopList);
    const unsigned count = Peek<unsigned>(kShopCount);
    int used = 0;
    for (int i = 0; i < kEventSlots; ++i) used += Peek<uintptr_t>(kEventTable + i * kEventSlot) != 0;
    const int stage = Peek<int>(kEventTable + 0x7AF8);
    const bool allOpen = Peek<unsigned char>(0x838441) != 0;
    Log("markers: report -- %u shops in the career, event table %d of %d used, save stage %d%s", count, used,
        kEventSlots, stage, allOpen ? ", everything unlocked" : "");
    // The zone hash of every event in the table (+0x30 of its zone record):
    // a race start the editor placed shows here as its FRS_ZONE_ hash.
    std::string zones;
    for (int i = 0; i < kEventSlots; ++i) {
        const uintptr_t zone = Peek<uintptr_t>(kEventTable + i * kEventSlot);
        if (!zone) continue;
        char text[24];
        std::snprintf(text, sizeof(text), " %08X%s", Peek<unsigned>(zone + 0x30),
                      Peek<uintptr_t>(kEventTable + i * kEventSlot + 8) ? "s" : "");
        zones += text;
    }
    Log("markers: report -- event zones (s = shop):%s", zones.c_str());
    if (!list || !count || count > 512) return;
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t record = list + i * kShopRecordSize;
        if (Peek<unsigned>(record + kPlacedMagicAt) != kPlacedMagic) continue;
        int slot = -1;
        for (int s = 0; s < kEventSlots && slot < 0; ++s)
            if (Peek<uintptr_t>(kEventTable + s * kEventSlot + 8) == record) slot = s;
        const SaveEntry* entry = PlacedShopSaveEntry(Peek<unsigned>(record + 0x38));
        using TestFn = bool(__thiscall*)(uintptr_t);
        const bool progress = ((TestFn)0x512870)(record);
        Log("markers: report -- %.56s: in the event table %s; stage %d (save %d); needs discovery %d, "
            "discovered %s; career test 0x512870 %s; zone hash %08X",
            (const char*)record, slot >= 0 ? "YES" : "no", (int)Peek<signed char>(record + 0x9D), stage,
            (int)Peek<unsigned char>(record + 0x51), entry && entry->state == 1 ? "yes" : "NO",
            progress ? "passes" : "FAILS", Peek<unsigned>(record + 0x3C));
    }
}

std::vector<uintptr_t> g_snapped;   // placed shops whose GPS point is on the road
DWORD g_nextCheck = 0;
DWORD g_reportAt = 0;
bool  g_reported = false;
bool  g_wasInGameplay = false;

} // namespace

void InstallPlacedShopHooks() {
    Log(HookSaveLookup() ? "markers: the save's shop lookup hooked at 0x5003B0"
                         : "markers: 0x5003B0 is not the expected code; placed shops stay off the map");
    Log(WidenMinimapPool() ? "markers: the minimap takes up to 16 icons of a kind (0x4DB97A)"
                           : "markers: 0x4DB97A is not the expected code; the minimap keeps 8 icons of a kind");
}

void PlacedShopsFrame() {
    const bool inGameplay = InGameplay();
    if (g_wasInGameplay && !inGameplay) {
        // The world was torn down; the records and zones come back elsewhere.
        g_snapped.clear();
        g_reportAt = 0;
        g_reported = false;
    }
    g_wasInGameplay = inGameplay;
    if (!inGameplay) return;

    const DWORD now = ::GetTickCount();
    if (!g_reportAt) g_reportAt = now + kReportAfterMs;
    if (!g_reported && (LONG)(now - g_reportAt) >= 0) {
        g_reported = true;
        ReportPlacedShops();
    }
    if ((LONG)(now - g_nextCheck) < 0) return;
    g_nextCheck = now + kCheckEveryMs;

    const uintptr_t list = Peek<uintptr_t>(kShopList);
    const unsigned count = Peek<unsigned>(kShopCount);
    if (!list || !count || count > 256) return;
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t record = list + i * kShopRecordSize;
        if (Peek<unsigned>(record + kPlacedMagicAt) != kPlacedMagic) continue;
        bool done = false;
        for (const uintptr_t snapped : g_snapped) done |= snapped == record;
        if (!done && SnapGpsTarget(record)) {
            g_snapped.push_back(record);
            Log("markers: placed shop %.56s at 0x%08X", (const char*)record, (unsigned)record);
        }
    }
}

} // namespace frs

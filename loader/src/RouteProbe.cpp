// FRS World Loader -- the road graph probe
//
// A tool for checking an edited road graph (RoutesFreeRoam.bin) in the game,
// not for playing: off unless the .ini beside the plugin says
// [RouteProbe] Enabled=1. Once the game is in free roam it writes to the log
// what the game made of the graph it loaded -- how many routes and signposts,
// and for the routes named in the .ini, their nodes, the permissions the game
// worked out for them (traffic, racers and the player's GPS) and every
// signpost into and out of them as the game holds it after its own load-time
// pruning. That answers whether a new road was taken in and joined up without
// anyone having to drive there. Layout: docs/research/road-paths.md.

#include "Loader.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace frs {
namespace {

constexpr uintptr_t kRouteTable = 0x88A334;      // route*[1500], by id
constexpr int       kRouteSlots = 1500;
constexpr uintptr_t kSignposts = 0x883DB0;       // -> the signpost array
constexpr uintptr_t kSignpostsLive = 0x883DB4;
constexpr uintptr_t kSignpostsFile = 0x883DB8;
constexpr size_t    kNodesAt = 0x8C, kNode = 0x38, kSignpost = 0x20;

struct Probe {
    bool enabled = false;
    std::vector<int> routes;
    DWORD readySince = 0;
    bool done = false;
} g_probe;

std::wstring IniPath() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring ini = path;
    const size_t dot = ini.rfind(L'.');
    if (dot != std::wstring::npos) ini.resize(dot);
    return ini + L".ini";
}

// The permission bits of a node: mode 0 (the player and GPS), 1 (traffic)
// and 2 (racers), each forward and backward.
void DescribeNode(char* out, size_t size, unsigned flags) {
    std::snprintf(out, size, "gps %c%c traffic %c%c racers %c%c", (flags >> 11) & 1 ? 'F' : '-',
                  (flags >> 18) & 1 ? 'B' : '-', (flags >> 25) & 1 ? 'F' : '-', (flags >> 26) & 1 ? 'B' : '-',
                  (flags >> 27) & 1 ? 'F' : '-', (flags >> 28) & 1 ? 'B' : '-');
}

void Report() {
    const uintptr_t* table = (const uintptr_t*)kRouteTable;
    int routes = 0, highest = 0;
    for (int id = 0; id < kRouteSlots; ++id)
        if (Peek<uintptr_t>((uintptr_t)&table[id])) { ++routes; highest = id; }
    const uintptr_t signposts = Peek<uintptr_t>(kSignposts);
    const int live = Peek<int>(kSignpostsLive), file = Peek<int>(kSignpostsFile);
    Log("route probe: %d routes in the table (highest id %d); signposts %d live of %d in the file", routes,
        highest, live, file);

    for (int id : g_probe.routes) {
        if (id <= 0 || id >= kRouteSlots) continue;
        const uintptr_t route = Peek<uintptr_t>((uintptr_t)&table[id]);
        if (!route) {
            Log("route probe: route %d is NOT in the game's table", id);
            continue;
        }
        const int nodes = Peek<short>(route + 0x34);
        const unsigned flags = Peek<unsigned>(route + 0x70);
        unsigned char lanes[8] = {};
        for (int k = 0; k < 8; ++k) lanes[k] = Peek<unsigned char>(route + 0x84 + k);
        Log("route probe: route %d: %d nodes, length %.1f, flags 0x%08X (active %d), lanes %d%d%d%d/%d%d%d%d", id,
            nodes, Peek<float>(route + 0x40), flags, (flags >> 6) & 1, lanes[0], lanes[1], lanes[2], lanes[3],
            lanes[4], lanes[5], lanes[6], lanes[7]);
        for (int k = 0; k < nodes && k < 64; ++k) {
            const uintptr_t node = route + kNodesAt + (size_t)k * kNode;
            char permissions[64];
            DescribeNode(permissions, sizeof(permissions), Peek<unsigned>(node + 0x0C));
            Log("route probe:   node %d at (%.1f, %.1f) %s, slots %d %d", k, Peek<float>(node), Peek<float>(node + 4),
                permissions, (int)Peek<unsigned short>(node + 0x1C), (int)Peek<unsigned short>(node + 0x1E));
        }
        // Every signpost into or out of it, as the game holds them now.
        if (!signposts) continue;
        for (int s = 0; s < live && s < 8000; ++s) {
            const uintptr_t at = signposts + (size_t)s * kSignpost;
            const int dstNode = Peek<unsigned short>(at + 4), srcNode = Peek<unsigned short>(at + 6);
            const int dst = Peek<unsigned short>(at + 8), src = Peek<unsigned short>(at + 0x0A);
            if (dst != id && src != id) continue;
            const unsigned type = Peek<unsigned>(at + 0x0C);
            Log("route probe:   signpost %d: route %d node %d -> route %d node %d, gps/racers %u traffic %u, %s%s", s,
                src, srcNode, dst, dstNode, type & 0xF, (type >> 4) & 0xF, (type >> 18) & 1 ? "backward" : "forward",
                (type >> 23) & 1 ? ", flips" : "");
        }
    }
}

} // namespace

void StartRouteProbe() {
    const std::wstring ini = IniPath();
    g_probe.enabled = ::GetPrivateProfileIntW(L"RouteProbe", L"Enabled", 0, ini.c_str()) != 0;
    if (!g_probe.enabled) return;
    wchar_t list[512] = {};
    ::GetPrivateProfileStringW(L"RouteProbe", L"Routes", L"", list, 512, ini.c_str());
    for (wchar_t* at = list; *at;) {
        wchar_t* end = nullptr;
        const long id = std::wcstol(at, &end, 10);
        if (end == at) { ++at; continue; }
        g_probe.routes.push_back((int)id);
        at = end;
    }
    Log("route probe: on, %zu routes to describe once in free roam", g_probe.routes.size());
}

// On the game thread: once the game has been in gameplay for a few seconds.
void RouteProbeFrame() {
    if (!g_probe.enabled || g_probe.done) return;
    if (!InGameplay()) { g_probe.readySince = 0; return; }
    if (!g_probe.readySince) { g_probe.readySince = ::GetTickCount() | 1; return; }
    if (::GetTickCount() - g_probe.readySince < 4000) return;
    g_probe.done = true;
    Report();
}

} // namespace frs

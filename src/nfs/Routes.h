#pragma once

#include "core/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

// The road graph traffic, AI racers and the free-roam GPS drive on:
// TRACKS/ROUTES<region>/RoutesFreeRoam.bin. Layout, meanings and the rules a
// writer has to keep: docs/research/road-paths.md. A port of
// tools/paths/nfsroutes.py, which rebuilds every shipped Routes file byte for
// byte; the editor writes the graph again at every bake, always starting from
// the game's own file.
namespace nfs::routes {

#pragma pack(push, 1)
struct Node {                       // 0x38
    float x = 0, y = 0;
    float raceDist = 0;
    u32   flags = 0;                // route id 0..10, permissions, intersection 19..24
    u16   segLen256 = 0;
    i16   height8 = 0;
    u16   halfLeft256 = 0, halfRight256 = 0;
    i16   dirX = 0, dirY = 0;
    u16   slots[2] = { 0xFFFF, 0xFFFF };
    i8    crossSlope = 0, slope = 0;
    i8    racing[2] = { 0, 0 };
    i8    lanes[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    u8    speeds[10] = { 100, 100, 6, 6, 6, 6, 6, 6, 6, 6 };
    u8    pad[2] = { 0, 0 };
};
static_assert(sizeof(Node) == 0x38, "route node layout");

struct Header {                     // 0x8C
    u32   linkNext = 0xB, linkPrev = 0xB;
    u16   one = 1;
    u16   id = 0;
    u32   kind = 0;
    char  name[32] = "TrackRoutesA32";
    float unk30 = 1.0f;
    i16   count = 0, count2 = 0;
    u32   rt38 = 0, rt3C = 0;
    float length = 0;
    u16   excludedEvents[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    u8    unk4C[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    float layerZ = 0;
    u8    zero58[8] = {};
    float bbox[4] = {};
    u32   flags = 0x007FE141;
    u32   unk74 = 0x100;
    float unk78 = 999.0f;
    u32   unk7C = 0x100;
    float unk80 = 999.0f;
    u8    lanePresent[8] = {};
};
static_assert(sizeof(Header) == 0x8C, "route header layout");

struct Signpost {                   // 0x20
    float runtimeDist = 0;
    u16   dstNode = 0, srcNode = 0, dstRoute = 0, srcRoute = 0;
    u32   flags = 0;
    i32   unk10 = -1;
    u32   unk14 = 0;
    u16   ext[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
};
static_assert(sizeof(Signpost) == 0x20, "signpost layout");
#pragma pack(pop)

constexpr u16 kNoSlot = 0xFFFF;
constexpr u32 kNoIntersection = 0x3F;
constexpr int kMaxRouteId = 1499;
constexpr size_t kMaxSignposts = 4000;

// Node permission bits: mode 0 (the player and the GPS), 1 (traffic), 2 (racers),
// forward and backward along the route.
constexpr u32 kGpsForward = 1u << 11, kGpsBackward = 1u << 18;
constexpr u32 kTrafficForward = 1u << 25, kTrafficBackward = 1u << 26;
constexpr u32 kRacersForward = 1u << 27, kRacersBackward = 1u << 28;
constexpr u32 kBit12 = 1u << 12;

u32 SignpostFlags(int a, int b, bool backward, bool flip);

struct Route {
    Header header;
    std::vector<Node> nodes;
    u16 Id() const { return header.id; }
};

struct Intersection {
    std::vector<u8> raw;            // 0x114
    u16 Route(int k) const;
    u16 NodeIndex(int k) const;
    void SetNodeIndex(int k, u16 node);
};

class Graph {
public:
    bool Parse(const std::vector<u8>& bytes, std::wstring& error);
    std::vector<u8> Write() const;

    std::vector<Route>& Routes() { return routes_; }
    const std::vector<Route>& Routes() const { return routes_; }
    std::vector<Signpost>& Signposts() { return signposts_; }
    Route* Find(u16 id);
    const Route* Find(u16 id) const;

    u16  FreeRouteId(u16 start = 101) const;
    // Ids FreeRouteId never hands out: a race's graph keeps links to routes of
    // the free-roam graph it leaves out, and a new route must not answer them.
    void Reserve(u16 id) { reserved_.push_back(id); }
    void AddRoute(Route route);                 // kept sorted by id
    // Lengths, directions, slopes, header length, count, box and lane flags,
    // from the nodes' positions, heights, widths and lanes.
    static void Finish(Route& route);
    // Clears node slots past the end of the signpost table (the shipped L4RA
    // file has 232): appended signposts would otherwise start answering them.
    size_t DropDanglingSlots();
    // Appends a signpost and hooks it into the source node's first free slot.
    bool AddSignpost(u16 srcRoute, u16 srcNode, u16 dstRoute, u16 dstNode, bool backward, bool flip, int a,
                     int b, std::wstring& error);
    // Marks a route end that leads nowhere the way the shipped routes do: a
    // link from the end node to itself that nothing may take (A = B = 7),
    // backward at a route's first node. A route end with no link at all is
    // dropped by the game at load, and the route with it.
    bool AddDeadEnd(u16 route, u16 node, std::wstring& error);
    // Inserts a node before `index` (1..count-1), copying the one before, and
    // renumbers every reference to the route's later nodes.
    bool InsertNode(u16 route, u16 index, float x, float y, float z, std::wstring& error);
    // The closest point of a route to (x, y): segment k, fraction t, distance.
    bool Project(const Route& route, float x, float y, int& k, float& t, float& distance) const;
    // A node of `route` at (x, y) with both slots free: an inner node within
    // `snap` units, or a new one splitting the nearest segment there.
    int JunctionNode(u16 route, float x, float y, float z, std::wstring& error, float snap = 4.0f);
    // A two-way T between one end of `newRoute` and `oldNode` of `oldRoute`:
    // both slots of each, entering and leaving in both directions.
    bool ConnectEnd(u16 newRoute, bool atStart, u16 oldRoute, u16 oldNode, bool traffic, std::wstring& error);
    // Problems with the cross-references the game relies on; empty when fine.
    std::vector<std::string> Check() const;

private:
    std::vector<Route> routes_;
    std::vector<Signpost> signposts_;
    std::vector<Intersection> intersections_;
    std::vector<u32> order_;                    // chunk ids as they came
    std::vector<u16> reserved_;
    u32 signHeader_ = 0;
    std::unordered_map<u32, std::vector<u8>> extra_;
};

} // namespace nfs::routes

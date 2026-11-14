// Zoning: the game's buildings stood along the sides of roads, the way a city
// builder fills the lots of the streets it is given.
//
// Each zoned side of a road is walked from end to end. At every step a model
// of the side's kind is drawn (from the road's seed, so the same road always
// gets the same street) and stood behind the sidewalk, facing the road, as
// deep as the zone allows; it is kept if it clears every road and every
// building already stood, and the walk goes on past it. Where nothing fits,
// the walk moves on a little and tries again. Grid streets so become blocks
// with buildings round every edge, corners included.

#include "roads/RoadNetwork.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace roads {

const wchar_t* ZoneName(int kind) {
    switch (kind) {
        case kZoneHouses:     return L"houses";
        case kZoneShops:      return L"shops";
        case kZoneApartments: return L"apartments";
        case kZoneTowers:     return L"towers";
        case kZoneIndustry:   return L"industry";
        case kZoneMixed:      return L"mixed";
        default:              return L"none";
    }
}

namespace {

bool Has(const std::string& name, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (name.find(w) != std::string::npos) return true;
    return false;
}

// splitmix64: the same numbers for the same road, whatever else changes.
u64 Mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A rectangle on the ground: centre, unit axes, half sizes.
struct Box {
    Vec3  centre;
    Vec3  u, v;
    float hu = 0.0f, hv = 0.0f;

    Vec3 Corner(int k) const {
        const float su = (k == 1 || k == 2) ? 1.0f : -1.0f;
        const float sv = (k >= 2) ? 1.0f : -1.0f;
        return centre + u * (hu * su) + v * (hv * sv);
    }
};

float Dot2(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y; }

// Whether two rectangles overlap, grown by `margin` each (separating axes).
bool Overlap(const Box& a, const Box& b, float margin) {
    const Vec3 d = b.centre - a.centre;
    const Vec3 axes[4] = { a.u, a.v, b.u, b.v };
    for (const Vec3& axis : axes) {
        const float ra = std::fabs(Dot2(a.u, axis)) * a.hu + std::fabs(Dot2(a.v, axis)) * a.hv + margin;
        const float rb = std::fabs(Dot2(b.u, axis)) * b.hu + std::fabs(Dot2(b.v, axis)) * b.hv + margin;
        if (std::fabs(Dot2(d, axis)) > ra + rb) return false;
    }
    return true;
}

// The distance on the ground from a point to a rectangle (0 inside).
float PointBox(const Vec3& p, const Box& box) {
    const Vec3 d = p - box.centre;
    const float x = std::max(0.0f, std::fabs(Dot2(d, box.u)) - box.hu);
    const float y = std::max(0.0f, std::fabs(Dot2(d, box.v)) - box.hv);
    return std::sqrt(x * x + y * y);
}

float PointSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab{ b.x - a.x, b.y - a.y, 0.0f };
    const float len2 = Dot2(ab, ab);
    float t = len2 > 1e-9f ? Dot2(Vec3{ p.x - a.x, p.y - a.y, 0.0f }, ab) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Whether a line from a to b crosses the rectangle.
bool SegmentCrossesBox(const Vec3& a, const Vec3& b, const Box& box) {
    // In the box's own frame, clipped against its slab on each axis.
    const Vec3 da = a - box.centre, db = b - box.centre;
    float p0[2] = { Dot2(da, box.u), Dot2(da, box.v) };
    float p1[2] = { Dot2(db, box.u), Dot2(db, box.v) };
    const float h[2] = { box.hu, box.hv };
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 2; ++k) {
        const float d = p1[k] - p0[k];
        if (std::fabs(d) < 1e-9f) {
            if (std::fabs(p0[k]) > h[k]) return false;
            continue;
        }
        float ta = (-h[k] - p0[k]) / d, tb = (h[k] - p0[k]) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

float SegmentBox(const Vec3& a, const Vec3& b, const Box& box) {
    if (SegmentCrossesBox(a, b, box)) return 0.0f;
    float best = std::min(PointBox(a, box), PointBox(b, box));
    for (int k = 0; k < 4; ++k) best = std::min(best, PointSegment(box.Corner(k), a, b));
    return best;
}

// Where a road runs: its middle line, sampled, and how far out it reaches.
struct RoadLine {
    u32 segment = 0;
    std::vector<Vec3> points;
    float half = 0.0f;
    float walkLeft = 0.0f, walkRight = 0.0f;   // Edge per side
    Vec3 lo, hi;                               // bounds on the ground, reach included
};

} // namespace

int ClassifyBuilding(const std::string& name, const Vec3& min, const Vec3& max) {
    if (name.rfind("XB_", 0) != 0 && name.rfind("XBU_", 0) != 0) return kZoneNone;
    // The most detailed of a building's levels of detail; the game swaps in
    // the others itself.
    if (name.find("_1A") == std::string::npos) return kZoneNone;
    // Parts of buildings, landmarks and things that are not buildings.
    if (Has(name, { "WINDOW", "BRIDGE", "SUPPORT", "PILLAR", "AIRPORT", "AMPHITHEATRE", "BASE", "TOP", "WALL",
                    "ROOF", "HALLWAY", "WALKWAY", "FOUTAIN", "FOUNTAIN", "TANK", "ELEMENT", "LOBBY", "BOTTOM",
                    "SIGN", "DOOR", "STAIR", "PART", "GATE", "FENCE", "CLIFF", "LIGHTHOUSE", "DOGHOUSE", "PIER",
                    "DOCK", "TUNNEL", "RAMP", "STADIUM", "CASTLE", "MUSEUM", "TRACK", "CRANE", "SHIP" }))
        return kZoneNone;
    const float w = max.x - min.x, d = max.y - min.y, h = max.z - min.z;
    // Standing on flat ground at its own origin's height.
    if (min.z < -1.0f || min.z > 0.6f) return kZoneNone;
    if (w < 5.0f || d < 5.0f || w > 80.0f || d > 80.0f || h < 4.0f) return kZoneNone;
    if (Has(name, { "WAREHOUSE", "WHOUSE", "WH_", "FACTORY", "INDUST", "HANGER", "HANGAR", "SHIPYARD", "PLANT",
                    "ABANDON", "GARAGE", "DEPOT", "STORAGE", "POWER", "MILL", "REFINERY" }))
        return kZoneIndustry;
    if (h > 45.0f) return kZoneTowers;
    if (Has(name, { "HOUSE", "HOME", "COTTAGE" }) || (h <= 14.0f && std::max(w, d) <= 26.0f)) return kZoneHouses;
    if (Has(name, { "CONDO", "APART", "HOTEL", "TOWER" }) || h > 20.0f) return kZoneApartments;
    return kZoneShops;
}

void Network::PlaceBuildings(std::vector<Pole>& poles, std::vector<MeshPiece>& pieces) const {
    bool any = false;
    for (const Segment& s : segments_)
        if (!s.profile.bridge && (s.profile.zoneLeft || s.profile.zoneRight)) any = true;
    if (!any || buildings_.empty()) return;

    // The models of each kind; a kind the region has none of takes any.
    std::vector<const BuildingModel*> byKind[kZoneKinds];
    for (const BuildingModel& m : buildings_) {
        if (m.kind <= kZoneNone || m.kind >= kZoneKinds) continue;
        byKind[m.kind].push_back(&m);
        if (m.kind == kZoneHouses || m.kind == kZoneShops || m.kind == kZoneApartments)
            byKind[kZoneMixed].push_back(&m);
    }
    std::vector<const BuildingModel*> all;
    for (const BuildingModel& m : buildings_)
        if (m.kind > kZoneNone) all.push_back(&m);
    for (int k = 1; k < kZoneKinds; ++k)
        if (byKind[k].empty()) byKind[k] = all;

    // Every road, as the line of its middle and its reach either side.
    const auto middleOf = [](const Curve& curve, float t, float offset, Vec3& point, Vec3& forward, Vec3& right) {
        Vec3 tangent = curve.Tangent(t);
        tangent.z = 0.0f;
        const float len = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
        forward = len > 1e-6f ? tangent * (1.0f / len) : Vec3{ 1.0f, 0.0f, 0.0f };
        right = { forward.y, -forward.x, 0.0f };
        point = curve.At(t) + right * offset;
    };
    std::vector<RoadLine> lines;
    for (const Segment& s : segments_) {
        const Curve curve = CurveOf(s);
        RoadLine line;
        line.segment = s.id;
        line.walkLeft = s.profile.Edge(-1);
        line.walkRight = s.profile.Edge(1);
        line.half = std::max(line.walkLeft, line.walkRight) + 0.3f;
        const float rough = Length(curve.p1 - curve.p0) + Length(curve.p2 - curve.p1) + Length(curve.p3 - curve.p2);
        const int n = std::clamp((int)(rough / 3.0f), 4, 400);
        line.lo = { 1e30f, 1e30f, 0.0f };
        line.hi = { -1e30f, -1e30f, 0.0f };
        for (int k = 0; k <= n; ++k) {
            Vec3 p, f, r;
            middleOf(curve, (float)k / (float)n, s.Offset(), p, f, r);
            line.points.push_back(p);
            line.lo = { std::min(line.lo.x, p.x), std::min(line.lo.y, p.y), 0.0f };
            line.hi = { std::max(line.hi.x, p.x), std::max(line.hi.y, p.y), 0.0f };
        }
        line.lo = line.lo - Vec3{ line.half, line.half, 0.0f };
        line.hi = line.hi + Vec3{ line.half, line.half, 0.0f };
        lines.push_back(std::move(line));
    }
    // Junctions spread wider than the roads into them, round their curves.
    struct Disc { Vec3 centre; float radius; };
    std::vector<Disc> discs;
    for (const Node& node : nodes_) {
        const std::vector<u32> around = SegmentsAt(node.id);
        if (around.size() < 3) continue;
        float widest = 0.0f;
        for (u32 id : around)
            if (const Segment* s = FindSegment(id)) widest = std::max(widest, s->profile.HalfWidth());
        discs.push_back({ node.position, widest * 1.5f + 0.5f });
    }

    std::vector<Box> stood;
    const auto clear = [&](const Box& box, u32 own, int side, float ownHalf) {
        const float reach = std::max(box.hu, box.hv) * 1.5f;
        for (const RoadLine& line : lines) {
            if (box.centre.x + reach < line.lo.x || box.centre.x - reach > line.hi.x ||
                box.centre.y + reach < line.lo.y || box.centre.y - reach > line.hi.y)
                continue;
            float half = line.half;
            if (line.segment == own) half = ownHalf;
            (void)side;
            for (size_t k = 0; k + 1 < line.points.size(); ++k)
                if (SegmentBox(line.points[k], line.points[k + 1], box) < half) return false;
        }
        for (const Disc& disc : discs)
            if (PointBox(disc.centre, box) < disc.radius) return false;
        for (const Box& other : stood)
            if (Overlap(box, other, 0.4f)) return false;
        return true;
    };

    constexpr size_t kMostBuildings = 4000;
    size_t placed = 0;
    for (const Segment& s : segments_) {
        const RoadProfile& p = s.profile;
        if (p.bridge || (!p.zoneLeft && !p.zoneRight)) continue;
        const Curve curve = CurveOf(s);
        // Length along the road's middle, for walking it.
        const int n = 200;
        std::vector<float> along(n + 1, 0.0f);
        Vec3 last, f0, r0;
        middleOf(curve, 0.0f, s.Offset(), last, f0, r0);
        for (int k = 1; k <= n; ++k) {
            Vec3 pt, f, r;
            middleOf(curve, (float)k / n, s.Offset(), pt, f, r);
            along[k] = along[k - 1] + std::sqrt((pt.x - last.x) * (pt.x - last.x) + (pt.y - last.y) * (pt.y - last.y));
            last = pt;
        }
        const float total = along[n];
        if (total < 4.0f) continue;
        const auto tAt = [&](float d) {
            d = std::clamp(d, 0.0f, total);
            const auto it = std::lower_bound(along.begin(), along.end(), d);
            const int k = std::clamp((int)(it - along.begin()), 1, n);
            const float span = along[k] - along[k - 1];
            const float f = span > 1e-6f ? (d - along[k - 1]) / span : 0.0f;
            return ((float)(k - 1) + f) / (float)n;
        };

        MeshPiece walls;
        char name[64];
        std::snprintf(name, sizeof(name), "ROAD_B%u", s.id);
        walls.name = name;
        walls.segment = s.id;

        for (int side = -1; side <= 1; side += 2) {
            const int kind = p.Zone(side);
            if (kind <= kZoneNone || kind >= kZoneKinds) continue;
            const std::vector<const BuildingModel*>& choices = byKind[kind];
            if (choices.empty()) continue;
            const float edge = p.Edge(side) + p.zoneSetback;
            const float ownHalf = std::max(0.5f, p.Edge(side) + std::min(0.0f, p.zoneSetback) - 0.05f);
            const bool turned = p.zoneTurn == 90 || p.zoneTurn == 270;
            // The left side walks the other way, so its street reads the same
            // from either end -- and so it starts at the road's start too.
            float d = 0.0f;
            u64 slot = 0;
            while (d < total && placed < kMostBuildings) {
                bool done = false;
                for (u64 attempt = 0; attempt < 8 && !done; ++attempt) {
                    const u64 r = Mix(((u64)(u32)p.zoneSeed << 40) ^ ((u64)s.id << 20) ^ ((u64)(side + 1) << 18) ^
                                      (slot << 4) ^ attempt);
                    const BuildingModel& m = *choices[(size_t)(r % choices.size())];
                    float frontage = m.max.x - m.min.x, depth = m.max.y - m.min.y;
                    if (turned) std::swap(frontage, depth);
                    if (depth > p.zoneDepth) continue;
                    const float mid = d + frontage * 0.5f;
                    if (mid > total) continue;
                    Vec3 at, forward, right;
                    middleOf(curve, tAt(mid), s.Offset(), at, forward, right);
                    const Vec3 away = right * (float)side;
                    Box box;
                    box.centre = at + away * (edge + depth * 0.5f);
                    box.u = forward;
                    box.v = away;
                    box.hu = frontage * 0.5f;
                    box.hv = depth * 0.5f;
                    if (!clear(box, s.id, side, ownHalf)) continue;

                    // The model's +Y points away from the road (its front
                    // faces the road), then the zone's turn.
                    const float base = std::atan2(-away.x, away.y);
                    const float heading = base + (float)p.zoneTurn * 3.14159265f / 180.0f;
                    const float ch = std::cos(heading), sh = std::sin(heading);
                    const float cx = (m.min.x + m.max.x) * 0.5f, cy = (m.min.y + m.max.y) * 0.5f;
                    // Standing on the lowest ground under it, or the road's height.
                    float z = 1e30f;
                    if (ground_) {
                        const float top = at.z + 8.0f;
                        for (int k = 0; k < 5; ++k) {
                            const Vec3 q = k < 4 ? box.Corner(k) : box.centre;
                            float h = 0.0f;
                            if (ground_(q.x, q.y, top, h) && h > at.z - 30.0f) z = std::min(z, h);
                        }
                    }
                    if (z > 1e29f) z = at.z + (p.Walk(side) ? p.sidewalkHeight : 0.0f);
                    z -= 0.2f;

                    Pole building;
                    building.model = m.name;
                    building.building = true;
                    building.segment = s.id;
                    building.heading = heading * 180.0f / 3.14159265f;
                    building.position = { box.centre.x - (cx * ch - cy * sh), box.centre.y - (cx * sh + cy * ch), z };
                    building.lamp = building.position;
                    for (int k = 0; k < 4; ++k) {
                        building.corners[k] = box.Corner(k);
                        building.corners[k].z = z;
                    }
                    poles.push_back(building);
                    stood.push_back(box);
                    ++placed;

                    // Solid: the game's own collision does not come with a placed model.
                    Box inner = box;
                    inner.hu = std::max(0.5f, box.hu - 0.3f);
                    inner.hv = std::max(0.5f, box.hv - 0.3f);
                    CollisionWall wall;
                    wall.barrier = true;
                    wall.surface = kSurfaceWall;
                    for (int k = 0; k <= 4; ++k) {
                        Vec3 c = inner.Corner(k % 4);
                        c.z = z;
                        wall.points.push_back(c);
                    }
                    walls.walls.push_back(std::move(wall));

                    d = mid + frontage * 0.5f + p.zoneGap;
                    done = true;
                }
                if (!done) d += 3.0f;
                ++slot;
            }
        }
        if (!walls.walls.empty()) pieces.push_back(std::move(walls));
    }
}

// ---------------------------------------------------------------------------
// Ground between roads: every area roads close all round (a face of the
// road graph, as drawn on the map) is filled with the region's grass, a
// little under the roads so they lie on it.
//
// The faces are found by walking the graph: from each road, at the node it
// reaches, on along the sharpest right turn. That walks every enclosed area
// clockwise and the open land round them all anticlockwise, so the
// clockwise loops are the ones to fill. Roads leading nowhere (a node with
// only them) are left out first: they close nothing.

namespace {

float SignedArea(const std::vector<Vec3>& ring) {
    float a = 0.0f;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        a += (ring[j].x - ring[i].x) * (ring[j].y + ring[i].y);
    return a * 0.5f;     // positive anticlockwise
}

// Ear clipping of an anticlockwise ring: index triples into it.
std::vector<u32> Triangulate(const std::vector<Vec3>& ring) {
    std::vector<u32> out;
    std::vector<u32> left(ring.size());
    for (u32 i = 0; i < (u32)ring.size(); ++i) left[i] = i;
    const auto cross = [&](u32 a, u32 b, u32 c) {
        return (ring[b].x - ring[a].x) * (ring[c].y - ring[a].y) - (ring[b].y - ring[a].y) * (ring[c].x - ring[a].x);
    };
    const auto inside = [&](u32 p, u32 a, u32 b, u32 c) {
        return cross(a, b, p) > 1e-6f && cross(b, c, p) > 1e-6f && cross(c, a, p) > 1e-6f;
    };
    size_t guard = 0;
    while (left.size() > 3 && guard++ < ring.size() * ring.size()) {
        bool clipped = false;
        for (size_t k = 0; k < left.size(); ++k) {
            const u32 a = left[(k + left.size() - 1) % left.size()], b = left[k], c = left[(k + 1) % left.size()];
            if (cross(a, b, c) <= 1e-6f) continue;          // not convex here
            bool ear = true;
            for (u32 p : left)
                if (p != a && p != b && p != c && inside(p, a, b, c)) { ear = false; break; }
            if (!ear) continue;
            out.insert(out.end(), { a, b, c });
            left.erase(left.begin() + (ptrdiff_t)k);
            clipped = true;
            break;
        }
        if (!clipped) break;      // not simple: the rest is left out
    }
    if (left.size() == 3 && cross(left[0], left[1], left[2]) > 0.0f) out.insert(out.end(), { left[0], left[1], left[2] });
    return out;
}

} // namespace

void Network::BuildFills(std::vector<MeshPiece>& pieces) const {
    if (!fillBetween_ || segments_.size() < 2) return;
    // Roads leading nowhere go first, again and again.
    std::set<u32> live;
    for (const Segment& s : segments_)
        if (s.a != s.b) live.insert(s.id);
    for (bool pruned = true; pruned;) {
        pruned = false;
        std::map<u32, int> degree;
        for (u32 id : live) {
            const Segment* s = FindSegment(id);
            ++degree[s->a];
            ++degree[s->b];
        }
        for (auto it = live.begin(); it != live.end();) {
            const Segment* s = FindSegment(*it);
            if (degree[s->a] < 2 || degree[s->b] < 2) { it = live.erase(it); pruned = true; }
            else ++it;
        }
    }
    if (live.size() < 2) return;

    // Each node's roads, by the angle they leave it at.
    struct Out { u32 segment; bool fromA; float angle; };
    std::map<u32, std::vector<Out>> around;
    for (u32 id : live) {
        const Segment* s = FindSegment(id);
        const Curve c = CurveOf(*s);
        const Vec3 ta = c.Tangent(0.0f), tb = c.Tangent(1.0f) * -1.0f;
        around[s->a].push_back({ id, true, std::atan2(ta.y, ta.x) });
        around[s->b].push_back({ id, false, std::atan2(tb.y, tb.x) });
    }
    for (auto& [node, list] : around)
        std::sort(list.begin(), list.end(), [](const Out& x, const Out& y) { return x.angle < y.angle; });

    std::set<std::pair<u32, bool>> walked;     // (road, leaving its node a)
    const std::string grass = groundTexture_;
    int filled = 0;
    for (u32 id : live)
        for (int dir = 0; dir < 2; ++dir) {
            const std::pair<u32, bool> first{ id, dir == 0 };
            if (walked.count(first)) continue;
            std::vector<Vec3> ring;
            std::pair<u32, bool> h = first;
            bool closed = false;
            for (size_t steps = 0; steps < live.size() * 2 + 2; ++steps) {
                walked.insert(h);
                const Segment* s = FindSegment(h.first);
                const Curve c = CurveOf(*s);
                const float length = Length(c.p1 - c.p0) + Length(c.p2 - c.p1) + Length(c.p3 - c.p2);
                const int n = std::clamp((int)(length / 4.0f), 2, 200);
                for (int k = 0; k < n; ++k) {
                    const float t = h.second ? (float)k / n : 1.0f - (float)k / n;
                    Vec3 tangent = c.Tangent(t);
                    tangent.z = 0.0f;
                    const float tl = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
                    const Vec3 right = tl > 1e-6f ? Vec3{ tangent.y / tl, -tangent.x / tl, 0.0f } : Vec3{};
                    ring.push_back(c.At(t) + right * s->Offset() - Vec3{ 0.0f, 0.0f, 0.3f });
                }
                // On at the far node: the sharpest right turn.
                const u32 at = h.second ? s->b : s->a;
                const std::vector<Out>& list = around[at];
                size_t back = 0;
                for (size_t k = 0; k < list.size(); ++k)
                    if (list[k].segment == h.first && list[k].fromA == !h.second) back = k;
                const Out& next = list[(back + 1) % list.size()];
                h = { next.segment, next.fromA };
                if (h == first) { closed = true; break; }
                if (walked.count(h)) break;
            }
            if (!closed || ring.size() < 3) continue;
            const float area = SignedArea(ring);
            if (area >= -1.0f) continue;            // the open land round it, or nothing
            std::reverse(ring.begin(), ring.end());
            const std::vector<u32> triangles = Triangulate(ring);
            if (triangles.empty()) continue;
            MeshPiece piece;
            char name[64];
            std::snprintf(name, sizeof(name), "ROAD_F%u_%d", id, filled++);
            piece.name = name;
            MeshGroup& group = piece.Group(grass);
            for (const Vec3& q : ring) {
                MeshVertex v;
                v.position = q;
                v.normal = { 0.0f, 0.0f, 1.0f };
                v.uv = { q.x / 8.0f, -q.y / 8.0f };
                group.vertices.push_back(v);
            }
            for (size_t k = 0; k + 2 < triangles.size(); k += 3) {
                const Vec3& a = ring[triangles[k]], & b = ring[triangles[k + 1]], & cc = ring[triangles[k + 2]];
                Vec3 n = Cross(b - a, cc - a);
                if (n.z < 0.0f) n = n * -1.0f;
                for (int i = 0; i < 3; ++i) {
                    MeshVertex& v = group.vertices[triangles[k + i]];
                    v.normal = Normalize(v.normal + Normalize(n));
                }
                group.indices.insert(group.indices.end(), { triangles[k], triangles[k + 1], triangles[k + 2] });
                CollisionArea collision;
                collision.surface = kSurfaceGrass;
                collision.outline = { a, b, cc };
                piece.areas.push_back(std::move(collision));
            }
            pieces.push_back(std::move(piece));
        }
}

} // namespace roads

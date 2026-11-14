// Road prefabs: roundabouts, an overpass, an interchange, crossroads --
// written out as nodes and roads at a point and a heading.

#include "roads/Prefabs.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace roads {

namespace {

constexpr float kPi = 3.14159265f;

// A road of the prefab like `base`, two-way, with nothing of its own beside
// it: no zoning, no ground beside it, no walls, not a bridge.
RoadProfile Street(const RoadProfile& base) {
    RoadProfile p = base;
    if (p.lanesForward < 1) p.lanesForward = 1;
    if (p.lanesBackward < 1) p.lanesBackward = p.lanesForward;
    p.zoneLeft = p.zoneRight = 0;
    p.terrain[0].on = p.terrain[1].on = false;
    p.wallLeft = p.wallRight = false;
    p.bridge = false;
    p.shoulderType = 0;
    p.medianType = 0;
    p.sidewalkLeft = p.sidewalkRight = true;
    return p;
}

RoadProfile Bridge(RoadProfile p) {
    p.bridge = true;
    p.wallLeft = p.wallRight = true;
    return p;
}

// Places the prefab's nodes and roads in its own frame: x along its heading,
// y to the left, lift over the ground (or over the middle's ground, for
// decks that stay level).
struct Builder {
    Network& network;
    Vec3 origin;
    float heading = 0.0f;     // degrees
    const PrefabGround& ground;
    PrefabResult& result;
    float baseZ = 0.0f;

    Vec3 World(float x, float y) const {
        const float h = heading * kPi / 180.0f, c = std::cos(h), s = std::sin(h);
        return { origin.x + x * c - y * s, origin.y + x * s + y * c, origin.z };
    }

    // A node on the ground there, or `lift` over the middle's ground (a deck).
    u32 Node(float x, float y, float lift = 0.0f) {
        Vec3 p = World(x, y);
        p.z = lift > 0.0f ? baseZ + lift : (ground ? ground(p.x, p.y, origin.z) : origin.z);
        const u32 id = network.AddNode(p);
        result.nodes.push_back(id);
        return id;
    }

    u32 End(float x, float y) {
        const u32 id = Node(x, y);
        result.ends.push_back(id);
        return id;
    }

    // A straight road.
    Segment* Straight(u32 a, u32 b, const RoadProfile& profile) {
        Segment* s = network.AddSegment(a, b, profile);
        if (!s) return nullptr;
        s->shape = 1;
        result.segments.push_back(s->id);
        return s;
    }

    // A road leaving each end in a direction of its own (degrees in the
    // prefab's frame, from that end into the road), with handles that long.
    Segment* Curved(u32 a, u32 b, const RoadProfile& profile, float dirA, float dirB, float handleA, float handleB) {
        Segment* s = network.AddSegment(a, b, profile);
        if (!s) return nullptr;
        s->shape = 2;
        s->dirA = dirA + heading;
        s->dirB = dirB + heading;
        s->handleA = std::max(0.5f, handleA);
        s->handleB = std::max(0.5f, handleB);
        result.segments.push_back(s->id);
        return s;
    }

    // A ramp between the ground and a deck: level where it meets each, so the
    // climb is an S rather than a kink.
    Segment* Ramp(u32 a, u32 b, const RoadProfile& profile) {
        Segment* s = Straight(a, b, profile);
        if (!s) return nullptr;
        s->liftSetA = s->liftSetB = true;
        s->liftA = s->liftB = 0.0f;
        return s;
    }
};

// A roundabout: a one-way ring (anticlockwise, the traffic keeping right),
// quarter-circle by quarter-circle between the arms, and an arm out of each.
void Roundabout(Builder& b, const RoadProfile& base, const std::vector<float>& arms, float radius, int ringLanes,
                float armLength) {
    RoadProfile ring = Street(base);
    ring.lanesForward = ringLanes;
    ring.lanesBackward = 0;
    ring.sidewalkLeft = false;      // the island side
    RoadProfile arm = Street(base);
    std::vector<u32> nodes;
    for (float a : arms) {
        const float r = a * kPi / 180.0f;
        nodes.push_back(b.Node(radius * std::cos(r), radius * std::sin(r)));
    }
    for (size_t i = 0; i < arms.size(); ++i) {
        const size_t j = (i + 1) % arms.size();
        float sweep = arms[j] - arms[i];
        while (sweep <= 0.0f) sweep += 360.0f;
        const float handle = 4.0f / 3.0f * std::tan(sweep * kPi / 180.0f / 4.0f) * radius;
        b.Curved(nodes[i], nodes[j], ring, arms[i] + 90.0f, arms[j] - 90.0f, handle, handle);
    }
    for (size_t i = 0; i < arms.size(); ++i) {
        const float r = arms[i] * kPi / 180.0f;
        const float reach = radius + armLength;
        b.Straight(nodes[i], b.End(reach * std::cos(r), reach * std::sin(r)), arm);
    }
}

// A crossing with the arms at these angles and crossings painted.
void Crossing(Builder& b, const RoadProfile& base, const std::vector<float>& arms, float armLength) {
    const u32 middle = b.Node(0.0f, 0.0f);
    if (Node* n = b.network.FindNode(middle)) n->crosswalks = true;
    const RoadProfile street = Street(base);
    for (float a : arms) {
        const float r = a * kPi / 180.0f;
        b.Straight(middle, b.End(armLength * std::cos(r), armLength * std::sin(r)), street);
    }
}

// One road over another: the lower one straight along x, the upper one
// along y, up a ramp, over a deck `height` high and down again.
struct OverpassLayout { float deckHalf, rampEnd, reach; };

OverpassLayout Overpass(Builder& b, const RoadProfile& lower, const RoadProfile& upper, float height,
                        bool lowerEnds, std::vector<u32>* upperGround) {
    OverpassLayout l;
    l.deckHalf = lower.HalfWidth() + 8.0f;
    l.rampEnd = l.deckHalf + std::max(40.0f, height * 8.0f);
    l.reach = l.rampEnd + 30.0f;
    if (lowerEnds) b.Straight(b.End(-l.reach, 0.0f), b.End(l.reach, 0.0f), lower);
    const RoadProfile deck = Bridge(upper);
    const u32 south = b.Node(0.0f, -l.rampEnd);
    const u32 north = b.Node(0.0f, l.rampEnd);
    const u32 deckSouth = b.Node(0.0f, -l.deckHalf, height);
    const u32 deckNorth = b.Node(0.0f, l.deckHalf, height);
    b.Ramp(south, deckSouth, deck);
    b.Straight(deckSouth, deckNorth, deck);
    b.Ramp(deckNorth, north, deck);
    b.Straight(b.End(0.0f, -l.reach), south, upper);
    b.Straight(north, b.End(0.0f, l.reach), upper);
    if (upperGround) *upperGround = { south, north };
    return l;
}

// A diamond interchange: a highway along x under a road along y, and four
// slip roads from the highway up to where the road comes back to the ground.
void Diamond(Builder& b, const RoadProfile& base) {
    RoadProfile highway = Street(base);
    highway.lanesForward = highway.lanesBackward = 2;
    highway.sidewalkWidth = 0.0f;
    highway.shoulderType = 1;
    highway.shoulderSides = 0;
    highway.speedLimit = 100.0f;
    const RoadProfile road = Street(base);
    RoadProfile slip = Street(base);
    slip.lanesForward = slip.lanesBackward = 1;
    slip.sidewalkWidth = 0.0f;

    std::vector<u32> landings;
    const OverpassLayout l = Overpass(b, highway, road, 8.0f, false, &landings);
    const float exit = l.rampEnd + 45.0f, reach = exit + 60.0f;
    const u32 west = b.Node(-exit, 0.0f), east = b.Node(exit, 0.0f);
    b.Straight(b.End(-reach, 0.0f), west, highway);
    b.Straight(west, east, highway);
    b.Straight(east, b.End(reach, 0.0f), highway);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = 0; sy < 2; ++sy) {
            const u32 from = sx < 0 ? west : east;
            const u32 to = landings[(size_t)sy];
            const float ys = sy ? 1.0f : -1.0f;
            const float dx = exit, dy = l.rampEnd;
            const float length = std::sqrt(dx * dx + dy * dy);
            // Off the highway towards the middle, bending to meet the road side on.
            const float leave = std::atan2(ys * 0.45f, (float)-sx) * 180.0f / kPi;
            const float arrive = sx < 0 ? 180.0f : 0.0f;
            b.Curved(from, to, slip, leave, arrive, length * 0.45f, length * 0.4f);
        }
}

struct Entry {
    PrefabInfo info;
    std::function<void(Builder&, const RoadProfile&)> build;
};

const std::vector<Entry>& Entries() {
    static const std::vector<Entry> entries = {
        { { "roundabout4", L"Roundabout, 4 arms",
            L"A one-lane roundabout 18 m across the ring, four two-way arms." },
          [](Builder& b, const RoadProfile& p) { Roundabout(b, p, { 0, 90, 180, 270 }, 18.0f, 1, 30.0f); } },
        { { "roundabout4big", L"Roundabout, 4 arms, two lanes",
            L"A two-lane roundabout 30 m across the ring, four two-way arms." },
          [](Builder& b, const RoadProfile& p) { Roundabout(b, p, { 0, 90, 180, 270 }, 30.0f, 2, 35.0f); } },
        { { "roundabout3", L"Roundabout, 3 arms", L"A one-lane roundabout with three arms." },
          [](Builder& b, const RoadProfile& p) { Roundabout(b, p, { 0, 120, 240 }, 18.0f, 1, 30.0f); } },
        { { "overpass", L"Overpass", L"One road over another on an 8 m deck, with ramps up and down." },
          [](Builder& b, const RoadProfile& p) {
              Overpass(b, Street(p), Street(p), 8.0f, true, nullptr);
          } },
        { { "diamond", L"Diamond interchange",
            L"A 2+2 highway under a road, with four slip roads between them." },
          [](Builder& b, const RoadProfile& p) { Diamond(b, p); } },
        { { "crossroads", L"Crossroads", L"Four arms meeting, with pedestrian crossings." },
          [](Builder& b, const RoadProfile& p) { Crossing(b, p, { 0, 90, 180, 270 }, 45.0f); } },
        { { "tjunction", L"T junction", L"Three arms meeting, with pedestrian crossings." },
          [](Builder& b, const RoadProfile& p) { Crossing(b, p, { 0, 90, 180 }, 45.0f); } },
    };
    return entries;
}

} // namespace

const std::vector<PrefabInfo>& Prefabs() {
    static const std::vector<PrefabInfo> list = [] {
        std::vector<PrefabInfo> out;
        for (const Entry& e : Entries()) out.push_back(e.info);
        return out;
    }();
    return list;
}

const PrefabInfo* FindPrefab(const std::string& key) {
    for (const PrefabInfo& info : Prefabs())
        if (info.key == key) return &info;
    return nullptr;
}

bool PlacePrefab(Network& network, const std::string& key, const Vec3& origin, float heading, const RoadProfile& base,
                 const PrefabGround& ground, PrefabResult& result, float joinReach) {
    const Entry* entry = nullptr;
    for (const Entry& e : Entries())
        if (e.info.key == key) entry = &e;
    if (!entry) return false;
    result = {};
    Builder b{ network, origin, heading, ground, result };
    b.baseZ = ground ? ground(origin.x, origin.y, origin.z) : origin.z;
    entry->build(b, base);

    // Open ends close to a node already there join it.
    if (joinReach > 0.0f) {
        const std::set<u32> own(result.nodes.begin(), result.nodes.end());
        std::vector<u32> stillOpen;
        for (u32 end : result.ends) {
            const Node* e = network.FindNode(end);
            if (!e) continue;
            u32 best = 0;
            float bestDistance = joinReach;
            for (const Node& other : network.Nodes()) {
                if (own.count(other.id) || other.follow) continue;
                const float dx = other.position.x - e->position.x, dy = other.position.y - e->position.y;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d < bestDistance) { bestDistance = d; best = other.id; }
            }
            if (best) {
                network.MergeNodes(end, best);
                ++result.joined;
            } else {
                stillOpen.push_back(end);
            }
        }
        result.ends = std::move(stillOpen);
    }
    network.SetDirty();
    return true;
}

bool DrawPrefabPicture(const std::string& key, const RoadProfile& base, int size, std::vector<u32>& bgra) {
    Network scratch;
    PrefabResult result;
    if (!PlacePrefab(scratch, key, Vec3{}, 0.0f, base, nullptr, result, 0.0f)) return false;
    struct Dab { float x, y, r, z; bool deck; };
    std::vector<Dab> dabs;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (const Segment& s : scratch.Segments()) {
        const Curve c = scratch.CurveOf(s);
        const float half = s.profile.HalfWidth();
        for (int i = 0; i <= 80; ++i) {
            const Vec3 p = c.At(i / 80.0f);
            dabs.push_back({ p.x, p.y, half, p.z, s.profile.bridge });
            lo[0] = std::min(lo[0], p.x - half); lo[1] = std::min(lo[1], p.y - half);
            hi[0] = std::max(hi[0], p.x + half); hi[1] = std::max(hi[1], p.y + half);
        }
    }
    if (dabs.empty()) return false;
    const float span = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 1.08f;
    const float scale = size / std::max(span, 1.0f);
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
    bgra.assign((size_t)size * size, 0xFF1B2333u);
    std::stable_sort(dabs.begin(), dabs.end(), [](const Dab& a, const Dab& b) { return a.z < b.z; });
    for (const Dab& d : dabs) {
        const float px = (d.x - cx) * scale + size * 0.5f, py = size * 0.5f - (d.y - cy) * scale;
        const float r = std::max(1.2f, d.r * scale);
        const u32 colour = d.deck ? (d.z > 4.0f ? 0xFF8FC8EEu : 0xFF6E9CC0u) : 0xFF8A8F99u;
        for (int y = (int)(py - r - 1); y <= (int)(py + r + 1); ++y)
            for (int x = (int)(px - r - 1); x <= (int)(px + r + 1); ++x) {
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                const float dx = x + 0.5f - px, dy = y + 0.5f - py;
                if (dx * dx + dy * dy <= r * r) bgra[(size_t)y * size + x] = colour;
            }
    }
    return true;
}

} // namespace roads

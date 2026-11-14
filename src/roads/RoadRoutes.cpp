#include "roads/RoadRoutes.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace roads {

namespace {

using nfs::routes::Graph;
using RNode = nfs::routes::Node;
using nfs::routes::Route;

constexpr float kSpacing = 15.0f;   // between route nodes along a road

// One segment as part of a chain, and which way the chain runs along it.
struct Leg {
    const Segment* segment = nullptr;
    bool reversed = false;          // the chain runs from b to a
};

struct Chain {
    std::vector<Leg> legs;
    u32 startNode = 0, endNode = 0; // leader ids
};

// A point of a route being made: where, and the road it lies on.
struct RoutePoint {
    Vec3 position;
    const Segment* segment = nullptr;
    bool reversed = false;
    u32 node = 0;                   // the network node it stands on, 0 between
};

Vec3 Flat(const Vec3& v) { return { v.x, v.y, 0.0f }; }

Vec3 Unit(const Vec3& v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y);
    return l > 1e-5f ? Vec3{ v.x / l, v.y / l, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
}

i8 LaneByte(float offset, float maxHalf) {
    const int v = std::clamp((int)std::lround(offset / std::max(0.5f, maxHalf) * 128.0f), -128, 127);
    return (i8)(v == -1 ? -2 : v);   // -1 means "no lane"
}

} // namespace

bool AddRoutes(const Network& network, Graph& graph, RouteReport& report, bool race) {
    // ---- the roads that count, and their ends by node --------------------------
    // A node that follows another (a split highway's carriageway start) stands
    // for it: the roads meet there.
    const auto leader = [&](u32 id) {
        for (int guard = 0; guard < 8; ++guard) {
            const roads::Node* n = network.FindNode(id);
            if (!n || !n->follow) break;
            id = n->follow;
        }
        return id;
    };
    std::vector<const Segment*> segments;
    for (const Segment& s : network.Segments()) {
        if (s.profile.keepGround) continue;            // stands in for a game road that has its own route
        if (!s.profile.traffic && !s.profile.gps) continue;
        if (leader(s.a) == leader(s.b)) continue;
        segments.push_back(&s);
    }
    if (segments.empty()) return true;

    struct End { const Segment* segment; bool atA; Vec3 out; };
    std::map<u32, std::vector<End>> ends;
    for (const Segment* s : segments) {
        const Curve c = network.CurveOf(*s);
        ends[leader(s->a)].push_back({ s, true, Unit(c.Tangent(0.0f)) });
        ends[leader(s->b)].push_back({ s, false, Unit(c.Tangent(1.0f) * -1.0f) });
    }

    // ---- through pairs: at each node, the two ends that run on straightest ----
    std::map<const Segment*, std::pair<const Segment*, const Segment*>> next;   // (at a, at b)
    std::map<u32, std::vector<End>> branches;      // ends that join a through route in a T
    for (auto& [node, list] : ends) {
        if (list.size() < 2) continue;
        size_t bi = 0, bj = 1;
        float best = 2.0f;
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                if (list[i].segment == list[j].segment) continue;
                const float d = Dot(list[i].out, list[j].out);
                if (d < best) { best = d; bi = i; bj = j; }
            }
        const End& e0 = list[bi];
        const End& e1 = list[bj];
        auto& n0 = next[e0.segment];
        auto& n1 = next[e1.segment];
        (e0.atA ? n0.first : n0.second) = e1.segment;
        (e1.atA ? n1.first : n1.second) = e0.segment;
        for (size_t k = 0; k < list.size(); ++k)
            if (k != bi && k != bj) branches[node].push_back(list[k]);
    }

    // ---- chains of segments --------------------------------------------------------
    std::set<const Segment*> used;
    std::vector<Chain> chains;
    for (const Segment* start : segments) {
        if (used.count(start)) continue;
        // Back up to the chain's first segment.
        const Segment* first = start;
        bool firstReversed = false;
        {
            const Segment* at = start;
            bool reversed = false;
            std::set<const Segment*> seen{ at };
            for (;;) {
                // The segment before `at`, entered through its start end.
                const u32 startNode = leader(reversed ? at->b : at->a);
                const auto link = next.find(at);
                const Segment* prev = link == next.end() ? nullptr : (reversed ? link->second.second : link->second.first);
                if (!prev || seen.count(prev) || used.count(prev)) break;
                const bool prevReversed = leader(prev->a) == startNode;   // it runs into startNode at its a
                at = prev;
                reversed = prevReversed;
                seen.insert(at);
            }
            first = at;
            firstReversed = reversed;
        }
        Chain chain;
        const Segment* at = first;
        bool reversed = firstReversed;
        while (at && !used.count(at)) {
            used.insert(at);
            chain.legs.push_back({ at, reversed });
            const u32 endNode = leader(reversed ? at->a : at->b);
            const auto link = next.find(at);
            const Segment* following = link == next.end() ? nullptr : (reversed ? link->second.first : link->second.second);
            if (!following || used.count(following)) break;
            reversed = leader(following->b) == endNode;
            at = following;
        }
        chain.startNode = leader(chain.legs.front().reversed ? chain.legs.front().segment->b : chain.legs.front().segment->a);
        chain.endNode = leader(chain.legs.back().reversed ? chain.legs.back().segment->a : chain.legs.back().segment->b);
        chains.push_back(std::move(chain));
    }

    // ---- a route for each chain --------------------------------------------------
    graph.DropDanglingSlots();
    std::map<const Segment*, std::pair<u16, bool>> routeOf;        // route id, chain runs reversed
    std::vector<std::pair<u16, const Chain*>> made;
    for (const Chain& chain : chains) {
        std::vector<RoutePoint> points;
        for (const Leg& leg : chain.legs) {
            const Curve c = network.CurveOf(*leg.segment);
            float length = 0.0f;
            Vec3 previous = c.At(0.0f);
            for (int i = 1; i <= 32; ++i) { const Vec3 p = c.At(i / 32.0f); length += Length(p - previous); previous = p; }
            const int steps = std::max(1, (int)std::ceil(length / kSpacing));
            for (int i = points.empty() ? 0 : 1; i <= steps; ++i) {
                const float t = (float)i / steps;
                RoutePoint p;
                p.position = c.At(leg.reversed ? 1.0f - t : t);
                p.segment = leg.segment;
                p.reversed = leg.reversed;
                if (i == 0) p.node = leader(leg.reversed ? leg.segment->b : leg.segment->a);
                if (i == steps) p.node = leader(leg.reversed ? leg.segment->a : leg.segment->b);
                points.push_back(p);
            }
        }
        if (points.size() < 2) continue;

        Route route;
        route.header.id = graph.FreeRouteId();
        if (!route.header.id) {
            report.problems += L"No free route id is left under 1500.\n";
            return false;
        }
        for (const RoutePoint& rp : points) {
            const RoadProfile& p = rp.segment->profile;
            RNode n;
            n.x = rp.position.x;
            n.y = rp.position.y;
            n.height8 = (i16)std::lround(rp.position.z * 8.0f);
            const float half = p.HalfRoad();
            n.halfLeft256 = n.halfRight256 = (u16)std::clamp((long)std::lround(half * 256.0f), 1l, 65535l);
            // The lanes, as offsets right of the direction each is driven.
            std::vector<float> forward, backward;
            const float lane = p.laneWidth;
            // Where the lanes sit off the line: a shoulder on one side only
            // moves the cross-section, not the lanes (Segment::Offset).
            const float centre = (p.ShoulderAt(1) - p.ShoulderAt(-1)) * 0.5f;
            for (int k = 0; k < p.lanesForward; ++k)
                forward.push_back(centre + p.MedianLeft() + p.MedianWidth() + (k + 0.5f) * lane);
            for (int k = 0; k < p.lanesBackward; ++k)
                backward.push_back(-(centre - half + p.ShoulderAt(-1) + (k + 0.5f) * lane));
            if (rp.reversed) std::swap(forward, backward);
            // Slots as the game's own routes fill them: left to right, the
            // outermost lane always in slot 2 -- one lane in slot 2, two in 1
            // and 2, three in 0 to 2. Slot 3 is never used by the shipped
            // routes, and a route with a lane only there is dropped at load.
            const auto place = [&](std::vector<float> offsets, int base) {
                std::sort(offsets.begin(), offsets.end());
                if (offsets.size() > 4) offsets.erase(offsets.begin(), offsets.end() - 4);
                const int first = offsets.size() >= 4 ? 0 : 3 - (int)offsets.size();
                for (size_t k = 0; k < offsets.size(); ++k) n.lanes[base + first + (int)k] = LaneByte(offsets[k], half);
            };
            place(forward, 0);
            place(backward, 4);
            u32 flags = route.header.id | nfs::routes::kBit12 | nfs::routes::kNoIntersection << 19;
            const bool fw = !forward.empty(), bw = !backward.empty();
            if (p.gps && !race) {
                if (fw) flags |= nfs::routes::kGpsForward | nfs::routes::kRacersForward;
                if (bw) flags |= nfs::routes::kGpsBackward | nfs::routes::kRacersBackward;
            }
            if (p.traffic) {
                if (fw) flags |= nfs::routes::kTrafficForward;
                if (bw) flags |= nfs::routes::kTrafficBackward;
            }
            n.flags = flags;
            route.nodes.push_back(n);
        }
        Graph::Finish(route);
        // Lane speed: the road's limit on straights, lower on bends, as on the game's own roads.
        for (size_t k = 0; k < route.nodes.size(); ++k) {
            RNode& n = route.nodes[k];
            const RoadProfile& p = points[k].segment->profile;
            const int limit = std::clamp((int)std::lround(p.speedLimit / 3.6f), 6, 40);
            float turn = 0.0f;
            if (k > 0 && k + 1 < route.nodes.size()) {
                const float a0 = std::atan2((float)route.nodes[k - 1].dirY, (float)route.nodes[k - 1].dirX);
                const float a1 = std::atan2((float)n.dirY, (float)n.dirX);
                float d = std::fabs(std::fmod(a1 - a0 + 3.0f * kPi, 2.0f * kPi) - kPi);
                turn = Degrees(d) / std::max(n.segLen256 / 256.0f, 1.0f);
            }
            const int v = std::clamp((int)std::lround(limit - 4.5f * turn), 6, limit);
            for (int j = 0; j < 8; ++j)
                if (n.lanes[j] != -1) n.speeds[2 + j] = (u8)v;
        }
        const u16 id = route.header.id;
        report.nodes += route.nodes.size();
        graph.AddRoute(std::move(route));
        for (const Leg& leg : chain.legs) routeOf[leg.segment] = { id, leg.reversed };
        made.push_back({ id, &chain });
        ++report.routes;
    }

    std::wstring error;
    const auto routeEndAt = [&](u16 id, u32 node, bool& atStart) {
        for (const auto& [rid, chain] : made)
            if (rid == id) {
                if (chain->startNode == node) { atStart = true; return true; }
                if (chain->endNode == node) { atStart = false; return true; }
            }
        return false;
    };

    // ---- T junctions inside the network -----------------------------------------
    for (const auto& [node, list] : branches) {
        // The through route at this node.
        u16 through = 0;
        for (const End& e : ends[node]) {
            const auto r = routeOf.find(e.segment);
            if (r == routeOf.end()) continue;
            bool isBranch = false;
            for (const End& b : list) isBranch |= b.segment == e.segment && b.atA == e.atA;
            if (!isBranch) { through = r->second.first; break; }
        }
        const roads::Node* n = network.FindNode(node);
        if (!through || !n) continue;
        for (const End& b : list) {
            const auto r = routeOf.find(b.segment);
            if (r == routeOf.end()) continue;
            bool atStart = false;
            if (!routeEndAt(r->second.first, node, atStart)) continue;
            const int at = graph.JunctionNode(through, n->position.x, n->position.y, n->position.z, error);
            if (at < 0 || !graph.ConnectEnd(r->second.first, atStart, through, (u16)at, b.segment->profile.traffic, error)) {
                report.problems += L"A junction could not be linked: " + error + L"\n";
                continue;
            }
            report.links += 4;
        }
    }

    // ---- ends that meet the game's own roads ---------------------------------------
    // The routes made here, to tell them from the game's.
    std::set<u16> ours;
    for (const auto& [id, chain] : made) ours.insert(id);
    struct Mouth { u32 node; Vec3 centre; float reach; };
    constexpr float kEndReach = 20.0f;   // how near a road's loose end must come to a game road to join it
    std::vector<Mouth> mouths;
    for (const GameJunction& j : network.GameJunctions()) mouths.push_back({ leader(j.node), j.centre, j.halfRoad + 4.0f });
    for (const GameCut& c : network.Cuts()) {
        if (!c.corners.empty()) continue;    // a face of a junction made of the game's faces
        bool junction = false;
        for (const GameJunction& j : network.GameJunctions()) junction |= j.node == c.node;
        if (!junction) mouths.push_back({ leader(c.node), c.centre, c.halfWidth + 4.0f });
    }
    // A junction made of the game's faces joins the game's route nearest its
    // road's end, where the road's line meets it.
    for (const FaceJunction& j : network.FaceJunctions())
        if (const Node* n = network.FindNode(j.node)) mouths.push_back({ leader(j.node), n->position, 30.0f });
    // Any other road that just ends near one of the game's roads joins it
    // too: an end left unjoined is a road the game does not count as one
    // (free roam puts a car on it back on the nearest road it knows).
    for (const Node& n : network.Nodes()) {
        if (network.SegmentsAt(n.id).size() != 1) continue;
        bool taken = false;
        for (const Mouth& m : mouths) taken |= m.node == leader(n.id);
        if (!taken) mouths.push_back({ leader(n.id), n.position, kEndReach });
    }
    for (const Mouth& m : mouths) {
        // The game's route under the junction: the nearest with lanes.
        const Route* best = nullptr;
        float bestDistance = m.reach;
        float bestZ = m.centre.z;     // the game's road height there
        for (const Route& r : graph.Routes()) {
            if (ours.count(r.header.id)) continue;
            bool lanes = false;
            for (u8 present : r.header.lanePresent) lanes |= present != 0;
            if (!lanes) continue;
            int k = 0;
            float t = 0.0f, d = 0.0f;
            if (graph.Project(r, m.centre.x, m.centre.y, k, t, d) && d < bestDistance) {
                bestDistance = d;
                best = &r;
                if (k >= 0 && (size_t)k < r.nodes.size()) {
                    const size_t next = std::min((size_t)k + 1, r.nodes.size() - 1);
                    bestZ = (r.nodes[(size_t)k].height8 + (r.nodes[next].height8 - r.nodes[(size_t)k].height8) * t) / 8.0f;
                }
            }
        }
        if (!best) {
            if (!race && m.reach != kEndReach)
                report.problems += L"A junction into the game's road found no route of the game's under it.\n";
            continue;
        }
        const u16 gameRoute = best->header.id;
        for (const auto& [id, chain] : made) {
            for (int side = 0; side < 2; ++side) {
                const u32 node = side ? chain->endNode : chain->startNode;
                if (node != m.node) continue;
                // Its own node of the game's route for every road joining here.
                const int at = graph.JunctionNode(gameRoute, m.centre.x, m.centre.y, bestZ, error);
                const Segment* segment = side ? chain->legs.back().segment : chain->legs.front().segment;
                if (at < 0 || !graph.ConnectEnd(id, side == 0, gameRoute, (u16)at, segment->profile.traffic, error)) {
                    report.problems += L"A road could not be joined to the game's: " + error + L"\n";
                    continue;
                }
                report.gameLinks += 4;
                LOG_INFO("routes: road end node %u (route %u) joined to the game's route %u at its node %d, %.1f away",
                         m.node, (unsigned)id, (unsigned)gameRoute, at, bestDistance);
            }
        }
    }

    // Every end that leads nowhere is marked so, as the game's own are.
    size_t deadEnds = 0;
    for (const auto& [id, chain] : made) {
        const Route* r = graph.Find(id);
        if (!r || r->nodes.empty()) continue;
        for (const u16 end : { (u16)0, (u16)(r->nodes.size() - 1) }) {
            const RNode& n = graph.Find(id)->nodes[end];
            if (n.slots[0] != nfs::routes::kNoSlot || n.slots[1] != nfs::routes::kNoSlot) continue;
            if (graph.AddDeadEnd(id, end, error)) ++deadEnds;
            else report.problems += L"A road's dead end could not be marked: " + error + L"\n";
        }
    }
    if (deadEnds) LOG_INFO("routes: %zu dead ends marked", deadEnds);
    for (const std::string& problem : graph.Check()) report.problems += std::wstring(problem.begin(), problem.end()) + L"\n";
    LOG_INFO("routes: %zu roads made routes (%zu nodes), %zu links between them, %zu into the game's", report.routes,
             report.nodes, report.links, report.gameLinks);
    return report.problems.empty();
}

} // namespace roads

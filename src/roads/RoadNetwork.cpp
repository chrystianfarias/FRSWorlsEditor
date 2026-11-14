#include "roads/RoadNetwork.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <sstream>

namespace roads {

namespace {

// Spacing of the cross-sections along a road, and the length of road one
// baked piece holds. Short pieces keep each in the section it stands over,
// which is the section the game streams in around it.
constexpr float kSampleStep  = 2.0f;
constexpr float kPieceLength = 96.0f;
// A fillet between two roads at a junction, in this many straight parts.
constexpr int kFilletSteps = 10;
constexpr float kJunctionTile = 16.0f;   // world units one repeat of the junction texture covers

Vec3 Horizontal(const Vec3& v) { return { v.x, v.y, 0.0f }; }

Vec3 HorizontalDirection(const Vec3& v, const Vec3& fallback = { 1.0f, 0.0f, 0.0f }) {
    const Vec3 h = Horizontal(v);
    const float length = Length(h);
    return length > 1e-5f ? h * (1.0f / length) : fallback;
}

// To the right of a horizontal direction, seen from above (Z up).
Vec3 RightOf(const Vec3& forward) { return { forward.y, -forward.x, 0.0f }; }

u32 Grey(float lighting) {
    const int level = std::clamp((int)std::lround(lighting * 255.0f), 0, 255);
    return 0xFF000000u | ((u32)level << 16) | ((u32)level << 8) | (u32)level;
}

// Adds the quad a0-b0-b1-a1 as two triangles facing `facing`: the winding is
// chosen so the face's normal points that way.
void AddQuad(MeshGroup& group, const MeshVertex& a0, const MeshVertex& b0, const MeshVertex& b1,
             const MeshVertex& a1, const Vec3& facing) {
    const u32 base = (u32)group.vertices.size();
    group.vertices.push_back(a0);
    group.vertices.push_back(b0);
    group.vertices.push_back(b1);
    group.vertices.push_back(a1);
    const Vec3 normal = Cross(b0.position - a0.position, a1.position - a0.position);
    const Vec3 diagonal = Cross(b1.position - a0.position, a1.position - b0.position);
    const float sense = Dot(Length(normal) > 1e-8f ? normal : diagonal, facing);
    if (sense >= 0.0f) {
        for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u }) group.indices.push_back(base + i);
    } else {
        for (u32 i : { 0u, 2u, 1u, 0u, 3u, 2u }) group.indices.push_back(base + i);
    }
}

MeshVertex Vertex(const Vec3& position, const Vec3& normal, float u, float v, u32 color) {
    MeshVertex out;
    out.position = position;
    out.normal = normal;
    out.uv = { u, v };
    out.color = color;
    return out;
}

// One cross-section of a road: where it is and which way it runs.
struct Station {
    Vec3  point;
    Vec3  forward;     // horizontal, unit
    Vec3  right;       // horizontal, unit
    float distance = 0.0f;
    float t = 0.0f;    // on the curve
    float bank = 0.0f; // radians, the right edge up

    // A point across the road, `lateral` from the middle (right positive)
    // and `height` above the surface there, following the bank.
    Vec3 At(float lateral, float height) const {
        return point + right * (lateral * std::cos(bank)) + Vec3{ 0.0f, 0.0f, lateral * std::sin(bank) + height };
    }
};

// How the ends of a segment are pulled back from junctions.
struct Trim {
    float startT = 0.0f;
    float endT   = 1.0f;
};

// Stations along the curve between two parameters, evenly spaced by length.
std::vector<Station> Stations(const Curve& curve, float t0, float t1) {
    std::vector<Station> out;
    if (t1 <= t0) return out;

    // A dense table of the curve, to measure along it.
    const int dense = 256;
    std::vector<float> ts(dense + 1), lengths(dense + 1, 0.0f);
    Vec3 previous = curve.At(t0);
    for (int i = 0; i <= dense; ++i) {
        ts[i] = t0 + (t1 - t0) * (float)i / dense;
        const Vec3 p = curve.At(ts[i]);
        if (i) lengths[i] = lengths[i - 1] + Length(p - previous);
        previous = p;
    }
    const float total = lengths[dense];
    const int steps = std::clamp((int)std::ceil(total / kSampleStep), 1, 4000);

    int cursor = 0;
    for (int s = 0; s <= steps; ++s) {
        const float want = total * (float)s / steps;
        while (cursor < dense - 1 && lengths[cursor + 1] < want) ++cursor;
        const float span = lengths[cursor + 1] - lengths[cursor];
        const float f = span > 1e-6f ? (want - lengths[cursor]) / span : 0.0f;
        const float t = ts[cursor] + (ts[cursor + 1] - ts[cursor]) * std::clamp(f, 0.0f, 1.0f);

        Station station;
        station.point = curve.At(t);
        station.forward = HorizontalDirection(curve.Tangent(t),
                                              HorizontalDirection(curve.p3 - curve.p0));
        station.right = RightOf(station.forward);
        station.distance = want;
        station.t = t;
        out.push_back(station);
    }
    return out;
}

// The lane textures of a profile, left to right, and whether each is mirrored
// (its marking, drawn on the texture's left edge, on the lane's right).
struct LanePaint { std::string texture; bool mirrored; };

std::vector<LanePaint> LanePaints(const RoadProfile& p) {
    std::vector<LanePaint> out;
    const int lanes = p.Lanes();
    for (int k = 0; k < lanes; ++k) {
        if (k < p.lanesBackward) {
            // Left of the centre line: the marking is on the side towards it.
            out.push_back({ k == p.lanesBackward - 1 ? p.laneCentre : p.laneInner, true });
        } else {
            out.push_back({ k == p.lanesBackward ? p.laneCentre : p.laneInner, false });
        }
    }
    return out;
}

// Builds the strips of a stretch of road between stations [first, last].
// Lanes a road has that the road it meets at an end does not: they narrow
// away to nothing towards that end, the outer edge drawing in and the whole
// cross-section sliding over so it meets the other road edge to edge.
// Counts per end (A, B) and side (0 left: backward lanes, 1 right: forward).
struct StretchTaper {
    int   lanes[2][2] = { { 0, 0 }, { 0, 0 } };
    float slide[2] = { 0.0f, 0.0f };   // sideways, at each end, to meet the other road edge to edge
    // How far from each end (A, B) each wall (0 left, 1 right) is left out:
    // where the road runs side by side with another split off at the same
    // node (an exit), until the two have parted.
    float wallSkip[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    float length = 60.0f;
    float total = 0.0f;
    // How far from each end the median narrows away to nothing (0: it does
    // not), where the road going on has none.
    float medianClose[2] = { 0.0f, 0.0f };
    // The same for the shoulders (both sides), where the road going on has none.
    float shoulderClose[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };   // per end and side
    bool Any() const {
        return lanes[0][0] || lanes[0][1] || lanes[1][0] || lanes[1][1] || slide[0] != 0.0f || slide[1] != 0.0f ||
               medianClose[0] > 0.0f || medianClose[1] > 0.0f || shoulderClose[0][0] > 0.0f || shoulderClose[0][1] > 0.0f ||
               shoulderClose[1][0] > 0.0f || shoulderClose[1][1] > 0.0f ||
               walkClose[0][0] > 0.0f || walkClose[0][1] > 0.0f || walkClose[1][0] > 0.0f || walkClose[1][1] > 0.0f ||
               laneStretch[0] != 0.0f || laneStretch[1] != 0.0f || outerStretch[0][0] != 0.0f ||
               outerStretch[0][1] != 0.0f || outerStretch[1][0] != 0.0f || outerStretch[1][1] != 0.0f ||
               walkSink[0][0] > 0.0f || walkSink[0][1] > 0.0f || walkSink[1][0] > 0.0f || walkSink[1][1] > 0.0f;
    }
    // And per end (A, B) and side (0 left, 1 right) the sidewalks and the
    // walls, where the road going on has none on that side: the sidewalk
    // narrows and sinks to the road, the wall lowers to nothing.
    float walkClose[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    float wallClose[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    // What is left once everything above has closed: per end, how far the
    // right kerb moves out (the outer lane or shoulder widening to meet the
    // other road's), and per end and side how much wider the sidewalk gets.
    float laneStretch[2] = { 0.0f, 0.0f };   // the outer right lane, per end
    float outerStretch[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };   // shoulder, else sidewalk
    // A sidewalk that stays as wide but sinks to the road, where the road
    // going on has a shoulder there instead: it becomes the shoulder.
    float walkSink[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    static float Closing(float d, float reach) {
        if (reach <= 0.0f) return 0.0f;
        const float t = std::clamp(d / reach, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }
    float ShoulderClosed(int side, float distance) const {
        const int k = side > 0 ? 1 : 0;
        return std::min(1.0f, Closing(distance, shoulderClose[0][k]) + Closing(total - distance, shoulderClose[1][k]));
    }
    float WalkClosed(int side, float distance) const {
        const int k = side > 0 ? 1 : 0;
        return std::min(1.0f, Closing(distance, walkClose[0][k]) + Closing(total - distance, walkClose[1][k]));
    }
    // How far down the sidewalk has come: narrowing away or sinking.
    float WalkLow(int side, float distance) const {
        const int k = side > 0 ? 1 : 0;
        const float sunk = std::min(1.0f, Closing(distance, walkSink[0][k]) + Closing(total - distance, walkSink[1][k]));
        return std::max(WalkClosed(side, distance), sunk);
    }
    float WallClosed(int side, float distance) const {
        const int k = side > 0 ? 1 : 0;
        return std::min(1.0f, Closing(distance, wallClose[0][k]) + Closing(total - distance, wallClose[1][k]));
    }
    // The shoulder's narrowing stretch nearest `distance`: v of the game's
    // hatching that starts from nothing (RDP_ROADSTRIP_MA), 0 at the node.
    bool InShoulderTaper(int side, float distance, float& v) const {
        const int k = side > 0 ? 1 : 0;
        const float a = shoulderClose[0][k], b = shoulderClose[1][k];
        if (a > 0.0f && distance <= a + 1e-3f) { v = distance / a; return true; }
        if (b > 0.0f && total - distance <= b + 1e-3f) { v = (total - distance) / b; return true; }
        return false;
    }
    // How closed the median is at `distance`: 0 full width, 1 gone.
    float MedianClosed(float distance) const {
        const auto closed = [](float d, float reach) {
            if (reach <= 0.0f) return 0.0f;
            const float t = std::clamp(d / reach, 0.0f, 1.0f);
            return 1.0f - t * t * (3.0f - 2.0f * t);
        };
        return std::min(1.0f, closed(distance, medianClose[0]) + closed(total - distance, medianClose[1]));
    }
    bool WallAt(int side, float from, float to) const {
        const int k = side > 0 ? 1 : 0;
        return from >= wallSkip[0][k] - 1e-3f && to <= total - wallSkip[1][k] + 1e-3f;
    }
};

void BuildStretch(const RoadProfile& p, const std::vector<Station>& stations, size_t first, size_t last,
                  float crossBefore, float crossAfter, float medianBefore, float medianAfter, MeshPiece& piece,
                  const StretchTaper& taper = StretchTaper{}) {
    // Where a point across the road lands once the tapers are applied.
    const auto remap = [&](float distance, float x) {
        if (!taper.Any()) return x;
        const float lane = p.laneWidth, half = p.HalfRoad();
        const auto open = [&](float d) {
            const float t = std::clamp(d / std::max(1.0f, taper.length), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float wa = open(distance), wb = open(taper.total - distance);
        float shrink[2], region[2];
        for (int side = 0; side < 2; ++side) {
            shrink[side] = lane * (taper.lanes[0][side] * (1.0f - wa) + taper.lanes[1][side] * (1.0f - wb));
            region[side] = lane * (float)std::max(taper.lanes[0][side], taper.lanes[1][side]);
        }
        float out = x;
        // A sidewalk narrowing away: what is beyond it closes in on the kerb.
        if (p.sidewalkWidth > 0.01f) {
            for (int side = -1; side <= 1; side += 2) {
                if (!p.Walk(side)) continue;
                const float c = taper.WalkClosed(side, distance);
                const float beyond = x * side - half;
                if (c > 0.0f && beyond > 0.0f) out -= std::min(beyond, p.sidewalkWidth) * c * side;
            }
        }
        const float shoulderL = p.ShoulderAt(-1), shoulderR = p.ShoulderAt(1);
        // The shoulders narrowing away: the edges close in on the lanes.
        if (shoulderR > 0.01f) {
            const float c = taper.ShoulderClosed(1, distance);
            if (c > 0.0f && x > half - shoulderR) out -= std::min(x - (half - shoulderR), shoulderR) * c;
        }
        if (shoulderL > 0.01f) {
            const float c = taper.ShoulderClosed(-1, distance);
            if (c > 0.0f && x < -half + shoulderL) out += std::min(-half + shoulderL - x, shoulderL) * c;
        }
        // The leftover: the outer right lane widening (or narrowing), and on
        // each side the shoulder -- or the sidewalk where there is none -- to
        // meet the other road.
        {
            const float laneMore = taper.laneStretch[0] * (1.0f - wa) + taper.laneStretch[1] * (1.0f - wb);
            if (laneMore != 0.0f) {
                const float edge = half - shoulderR, from = edge - lane;
                if (x >= edge) out += laneMore;
                else if (x > from) out += laneMore * (x - from) / std::max(0.01f, lane);
            }
            for (int side = -1; side <= 1; side += 2) {
                const int k = side > 0 ? 1 : 0;
                const float more = taper.outerStretch[0][k] * (1.0f - wa) + taper.outerStretch[1][k] * (1.0f - wb);
                if (more == 0.0f) continue;
                const float shoulderHere = side > 0 ? shoulderR : shoulderL;
                const float from = shoulderHere > 0.01f ? half - shoulderHere : half;
                const float width = shoulderHere > 0.01f ? shoulderHere : (p.Walk(side) ? p.sidewalkWidth : 0.0f);
                const float beyond = x * side - from;
                if (width > 0.01f && beyond > 0.0f) out += more * std::min(1.0f, beyond / width) * side;
            }
        }
        // The median narrowing away: the lanes either side close in on its
        // middle, which stays where it is.
        if (const float median = p.MedianWidth(); median > 0.01f) {
            const float c = taper.MedianClosed(distance);
            if (c > 0.0f) {
                const float m0 = p.MedianLeft(), m1 = m0 + median, mid = (m0 + m1) * 0.5f;
                if (x >= m1) out -= median * 0.5f * c;
                else if (x <= m0) out += median * 0.5f * c;
                else out = mid + (x - mid) * (1.0f - c);
            }
        }
        if (region[1] > 0.0f) {
            const float from = half - shoulderR - region[1];
            if (x >= half - shoulderR) out -= shrink[1];
            else if (x > from) out -= shrink[1] * (x - from) / region[1];
        }
        if (region[0] > 0.0f) {
            const float from = -half + shoulderL + region[0];
            if (x <= -half + shoulderL) out += shrink[0];
            else if (x < from) out += shrink[0] * (from - x) / region[0];
        }
        return out + taper.slide[0] * (1.0f - wa) + taper.slide[1] * (1.0f - wb);
    };
    // A bridge's deck: its underside and its two edges.
    if (p.bridge && p.deckThickness > 0.01f) {
        const u32 grey = Grey(0.5f);
        MeshGroup& g = piece.Group(p.deckTexture);
        for (size_t i = first; i < last; ++i) {
            const Station& s0 = stations[i];
            const Station& s1 = stations[i + 1];
            const auto at = [&](const Station& s, float lateral, float height) {
                return s.At(remap(s.distance, lateral), height);
            };
            const float v0 = s0.distance / 8.0f, v1 = s1.distance / 8.0f;
            const Vec3 down{ 0.0f, 0.0f, -1.0f };
            const float under = -p.deckThickness;
            const float l = -p.Edge(-1), r = p.Edge(1);
            AddQuad(g, Vertex(at(s0, l, under), down, 0, v0, grey), Vertex(at(s0, r, under), down, (r - l) / 8, v0, grey),
                    Vertex(at(s1, r, under), down, (r - l) / 8, v1, grey), Vertex(at(s1, l, under), down, 0, v1, grey),
                    down);
            for (int side = -1; side <= 1; side += 2) {
                const Vec3 n = s0.right * (float)side;
                const float x = p.Edge(side) * side;
                const float top = p.Walk(side) ? p.sidewalkHeight : 0.0f;
                AddQuad(g, Vertex(at(s0, x, under), n, 0, v0, grey), Vertex(at(s0, x, top), n, 0.3f, v0, grey),
                        Vertex(at(s1, x, top), n, 0.3f, v1, grey), Vertex(at(s1, x, under), n, 0, v1, grey), n);
            }
        }
    }
    const u32 color = Grey(0.5f);   // lit afterwards (LightPieces)
    const Vec3 up{ 0.0f, 0.0f, 1.0f };
    const float half = p.HalfRoad();
    const float lane = p.laneWidth;
    const float median = p.MedianWidth();
    const float medianLeft = p.MedianLeft();
    const std::vector<LanePaint> paints = LanePaints(p);

    auto at = [&](const Station& s, float lateral, float height) { return s.At(remap(s.distance, lateral), height); };
    // Where a lane starts across the road: the backward lanes from the left
    // edge, then the median, then the forward lanes.
    const auto laneStart = [&](size_t k) {
        return -half + p.ShoulderAt(-1) + lane * (float)k + ((int)k >= p.lanesBackward ? median : 0.0f);
    };
    // The median opens out before a junction: the road runs through.
    const auto medianOpen = [&](const Station& s0, const Station& s1) {
        return s1.distance <= medianBefore + 0.01f || s0.distance >= medianAfter - 0.01f;
    };

    for (size_t i = first; i < last; ++i) {
        const Station& s0 = stations[i];
        const Station& s1 = stations[i + 1];

        // A pedestrian crossing: the stripes across the whole road instead
        // of the lanes, over the last few units before a junction.
        const bool crossing = s1.distance <= crossBefore + 0.01f || s0.distance >= crossAfter - 0.01f;
        if (crossing) {
            const float depth0 = s0.distance <= crossBefore + 0.01f ? s0.distance : s0.distance - crossAfter;
            const float depth1 = s1.distance <= crossBefore + 0.01f ? s1.distance : s1.distance - crossAfter;
            const float v0 = depth0 / kCrosswalkDepth, v1 = depth1 / kCrosswalkDepth;
            const float u0 = -half / kCrosswalkTile, u1 = half / kCrosswalkTile;
            MeshGroup& g = piece.Group(kCrosswalkTexture);
            AddQuad(g, Vertex(at(s0, -half, 0), up, u0, v0, color), Vertex(at(s0, half, 0), up, u1, v0, color),
                    Vertex(at(s1, half, 0), up, u1, v1, color), Vertex(at(s1, -half, 0), up, u0, v1, color), up);
        }
        // Lanes.
        for (size_t k = 0; k < paints.size() && !crossing; ++k) {
            const float x0 = laneStart(k);
            const float x1 = x0 + lane;
            const float u0 = paints[k].mirrored ? 1.0f : 0.0f;
            const float u1 = 1.0f - u0;
            const float v0 = s0.distance / p.textureLength;
            const float v1 = s1.distance / p.textureLength;
            MeshGroup& g = piece.Group(paints[k].texture);
            AddQuad(g, Vertex(at(s0, x0, 0), up, u0, v0, color), Vertex(at(s0, x1, 0), up, u1, v0, color),
                    Vertex(at(s1, x1, 0), up, u1, v1, color), Vertex(at(s1, x0, 0), up, u0, v1, color), up);
        }

        // The shoulders: the lane texture across them from the lane's side,
        // so a line there keeps its width.
        if (p.AnyShoulder() && !crossing) {
            // A road strip across its width; striped, the hatching starts
            // from nothing where it narrows away (RDP_ROADSTRIP_MA).
            for (int side = -1; side <= 1; side += 2) {
                const float shoulder = p.ShoulderAt(side);
                if (shoulder <= 0.01f) continue;
                const float line = p.shoulderLine ? std::min(0.15f, shoulder * 0.3f) : 0.0f;
                float t0 = 0.0f, t1 = 0.0f;
                const bool tapering = p.shoulderType == 2 && p.shoulderTexture.empty() &&
                                      taper.InShoulderTaper(side, s0.distance, t0) &&
                                      taper.InShoulderTaper(side, s1.distance, t1);
                MeshGroup& g = piece.Group(tapering ? std::string("RDP_ROADSTRIP_MA") : p.ShoulderTexture());
                const float v0 = tapering ? t0 : s0.distance / p.textureLength;
                const float v1 = tapering ? t1 : s1.distance / p.textureLength;
                const float inner = (half - shoulder + line) * side, outer = half * side;
                const float x0 = side < 0 ? outer : inner, x1 = side < 0 ? inner : outer;
                const float u0 = side < 0 ? 1.0f : 0.0f, u1 = 1.0f - u0;
                AddQuad(g, Vertex(at(s0, x0, 0), up, u0, v0, color), Vertex(at(s0, x1, 0), up, u1, v0, color),
                        Vertex(at(s1, x1, 0), up, u1, v1, color), Vertex(at(s1, x0, 0), up, u0, v1, color), up);
                if (line > 0.0f) {
                    // The solid white line of the game's lane texture.
                    MeshGroup& lg = piece.Group("RDP_LANEB_WS_AA_KT");
                    const float a = (half - shoulder) * side, b = (half - shoulder + line) * side;
                    const float la0 = side < 0 ? b : a, la1 = side < 0 ? a : b;
                    const float lv0 = s0.distance / p.textureLength, lv1 = s1.distance / p.textureLength;
                    AddQuad(lg, Vertex(at(s0, la0, 0), up, 0.03f, lv0, color), Vertex(at(s0, la1, 0), up, 0.10f, lv0, color),
                            Vertex(at(s1, la1, 0), up, 0.10f, lv1, color), Vertex(at(s1, la0, 0), up, 0.03f, lv1, color),
                            up);
                }
            }
        }

        // The median.
        if (median > 0.01f && !crossing) {
            const float m0 = medianLeft, m1 = medianLeft + median;
            const float v0 = s0.distance / p.textureLength, v1 = s1.distance / p.textureLength;
            if (medianOpen(s0, s1) || p.medianType == 1) {
                // Painted, or opened up before a junction: flat on the road.
                MeshGroup& g = piece.Group(medianOpen(s0, s1) ? std::string("RDP_INTERSECTMIDTILE") : p.MedianTexture());
                const float w = medianOpen(s0, s1) ? median / 8.0f : 1.0f;
                AddQuad(g, Vertex(at(s0, m0, 0), up, 0, v0, color), Vertex(at(s0, m1, 0), up, w, v0, color),
                        Vertex(at(s1, m1, 0), up, w, v1, color), Vertex(at(s1, m0, 0), up, 0, v1, color), up);
            } else {
                // Raised: a top and a kerb or barrier face either side --
                // sinking into the road as it narrows away.
                const float h = p.medianType == 4 ? std::max(0.3f, p.medianHeight) : p.medianHeight;
                const float h0 = h * (1.0f - taper.MedianClosed(s0.distance));
                const float h1 = h * (1.0f - taper.MedianClosed(s1.distance));
                MeshGroup& g = piece.Group(p.MedianTexture());
                const float topW = p.medianType == 4 ? std::min(0.3f, median * 0.5f) : median;
                const float t0 = m0 + (median - topW) * 0.5f, t1 = t0 + topW;
                if (h0 > 0.005f || h1 > 0.005f) {
                    AddQuad(g, Vertex(at(s0, t0, h0), up, 0, v0, color), Vertex(at(s0, t1, h0), up, 1, v0, color),
                            Vertex(at(s1, t1, h1), up, 1, v1, color), Vertex(at(s1, t0, h1), up, 0, v1, color), up);
                    for (int side = -1; side <= 1; side += 2) {
                        const float foot = side < 0 ? m0 : m1;
                        const float top = side < 0 ? t0 : t1;
                        const Vec3 n = s0.right * (float)side;
                        const float uTop =
                            p.medianType == 4 ? 1.0f : std::clamp(h / std::max(median, 0.01f), 0.0f, 1.0f);
                        AddQuad(g, Vertex(at(s0, foot, 0), n, 0, v0, color), Vertex(at(s0, top, h0), n, uTop, v0, color),
                                Vertex(at(s1, top, h1), n, uTop, v1, color), Vertex(at(s1, foot, 0), n, 0, v1, color),
                                n);
                    }
                }
            }
        }

        // Sidewalks: the top, and the kerb face towards the road.
        for (int side = -1; side <= 1; side += 2) {
            if (!p.Walk(side)) continue;
            MeshGroup& g = piece.Group(p.sidewalkTexture);
            const float h = p.sidewalkHeight;
            // Sinking to the road where it narrows away.
            const float h0 = h * (1.0f - taper.WalkLow(side, s0.distance));
            const float h1 = h * (1.0f - taper.WalkLow(side, s1.distance));
            if (taper.WalkClosed(side, s0.distance) > 0.999f && taper.WalkClosed(side, s1.distance) > 0.999f) continue;
            const float v0 = s0.distance / p.textureLength;
            const float v1 = s1.distance / p.textureLength;
            const float kerbU = std::clamp(h / std::max(p.sidewalkWidth, 0.01f), 0.0f, 1.0f);
            const float inner = half * side;
            const float outer = (half + p.sidewalkWidth) * side;
            const float uIn = side < 0 ? 1.0f : 0.0f;
            const float uOut = 1.0f - uIn;
            AddQuad(g, Vertex(at(s0, inner, h0), up, uIn, v0, color), Vertex(at(s0, outer, h0), up, uOut, v0, color),
                    Vertex(at(s1, outer, h1), up, uOut, v1, color), Vertex(at(s1, inner, h1), up, uIn, v1, color), up);
            const Vec3 face0 = s0.right * (float)-side;
            if (h0 > 0.005f || h1 > 0.005f)
                AddQuad(g, Vertex(at(s0, inner, 0), face0, 0, v0, color), Vertex(at(s0, inner, h0), face0, kerbU, v0, color),
                        Vertex(at(s1, inner, h1), face0, kerbU, v1, color), Vertex(at(s1, inner, 0), face0, 0, v1, color),
                        face0);
        }

        // Walls on the outer edges: flat (faced both ways), a guardrail, or
        // -- for a model -- nothing here (the model is stood along the edge).
        for (int side = -1; side <= 1; side += 2) {
            if (!p.Wall(side) || p.wallHeight < 0.05f || p.wallStyle == 2) continue;
            if (!taper.WallAt(side, s0.distance, s1.distance)) continue;
            MeshGroup& g = piece.Group(p.wallTexture);
            const float x = p.Edge(side) * side;
            const float base = p.Walk(side) ? p.sidewalkHeight : 0.0f;
            // Where the sidewalk under it sinks and the wall itself lowers
            // away towards a road without one.
            const float base0 = p.Walk(side) ? p.sidewalkHeight * (1.0f - taper.WalkLow(side, s0.distance)) : 0.0f;
            const float base1 = p.Walk(side) ? p.sidewalkHeight * (1.0f - taper.WalkLow(side, s1.distance)) : 0.0f;
            const float rise0 = p.wallHeight * (1.0f - taper.WallClosed(side, s0.distance));
            const float rise1 = p.wallHeight * (1.0f - taper.WallClosed(side, s1.distance));
            const float v0 = s0.distance / p.wallTextureLength;
            const float v1 = s1.distance / p.wallTextureLength;
            if (p.wallStyle == 0) {
                if (rise0 < 0.01f && rise1 < 0.01f) continue;
                const float top0 = base0 + rise0, top1 = base1 + rise1;
                const float depth = std::clamp(p.wallThickness, 0.0f, 5.0f);
                const float uh0 = rise0 / std::max(0.01f, p.wallHeight + depth);
                const float uh1 = rise1 / std::max(0.01f, p.wallHeight + depth);
                if (depth < 0.01f) {
                    for (int face = -1; face <= 1; face += 2) {
                        const Vec3 n = s0.right * (float)face;
                        AddQuad(g, Vertex(at(s0, x, base0), n, 0, v0, color), Vertex(at(s0, x, top0), n, rise0 / std::max(0.01f, p.wallHeight), v0, color),
                                Vertex(at(s1, x, top1), n, rise1 / std::max(0.01f, p.wallHeight), v1, color),
                                Vertex(at(s1, x, base1), n, 0, v1, color), n);
                    }
                    continue;
                }
                // With depth: the face towards the road, the one away from
                // it, the top, and an end where the wall starts or stops.
                const float out = x + depth * (float)side;
                const float ud = depth / std::max(0.01f, p.wallHeight + depth);
                const Vec3 inward = s0.right * (float)-side, outward = s0.right * (float)side;
                AddQuad(g, Vertex(at(s0, x, base0), inward, 0, v0, color), Vertex(at(s0, x, top0), inward, uh0, v0, color),
                        Vertex(at(s1, x, top1), inward, uh1, v1, color), Vertex(at(s1, x, base1), inward, 0, v1, color),
                        inward);
                AddQuad(g, Vertex(at(s0, out, base0), outward, 0, v0, color),
                        Vertex(at(s0, out, top0), outward, uh0, v0, color), Vertex(at(s1, out, top1), outward, uh1, v1, color),
                        Vertex(at(s1, out, base1), outward, 0, v1, color), outward);
                AddQuad(g, Vertex(at(s0, x, top0), up, uh0, v0, color), Vertex(at(s0, out, top0), up, uh0 + ud, v0, color),
                        Vertex(at(s1, out, top1), up, uh1 + ud, v1, color), Vertex(at(s1, x, top1), up, uh1, v1, color), up);
                const float last = stations.back().distance;
                const auto cap = [&](const Station& s, float facing, float b, float t, float uh) {
                    const Vec3 n = s.forward * facing;
                    AddQuad(g, Vertex(at(s, x, b), n, 0, 0, color), Vertex(at(s, x, t), n, 0, uh, color),
                            Vertex(at(s, out, t), n, ud, uh, color), Vertex(at(s, out, b), n, ud, 0, color), n);
                };
                const bool startsHere = s0.distance <= 1e-3f || (i > 0 && !taper.WallAt(side, stations[i - 1].distance,
                                                                                          s0.distance));
                const bool stopsHere = s1.distance >= last - 1e-3f ||
                                       (i + 2 < stations.size() && !taper.WallAt(side, s1.distance,
                                                                                  stations[i + 2].distance));
                if (startsHere && rise0 > 0.01f) cap(s0, -1.0f, base0, top0, uh0);
                if (stopsHere && rise1 > 0.01f) cap(s1, 1.0f, base1, top1, uh1);
                continue;
            }
            // A guardrail ends where the wall has mostly lowered away.
            if (taper.WallClosed(side, s0.distance) > 0.5f || taper.WallClosed(side, s1.distance) > 0.5f) continue;
            (void)base;
            // A guardrail: a W-shaped steel beam, its profile scaled to the
            // wall height, on posts every two units.
            const float scale = p.wallHeight / 0.8f;
            static const float profile[][2] = { { 0.00f, 0.42f }, { 0.07f, 0.50f }, { 0.02f, 0.60f },
                                                { 0.07f, 0.70f }, { 0.00f, 0.80f } };
            const float in = (float)-side;     // towards the road
            for (int k = 0; k + 1 < 5; ++k) {
                const float xa = x + in * profile[k][0] * scale, ha = base + profile[k][1] * scale;
                const float xb = x + in * profile[k + 1][0] * scale, hb = base + profile[k + 1][1] * scale;
                const float ua = (float)k / 4.0f, ub = (float)(k + 1) / 4.0f;
                for (int face = -1; face <= 1; face += 2) {
                    const Vec3 n = s0.right * (in * (float)face);
                    AddQuad(g, Vertex(at(s0, xa, ha), n, ua, v0, color), Vertex(at(s0, xb, hb), n, ub, v0, color),
                            Vertex(at(s1, xb, hb), n, ub, v1, color), Vertex(at(s1, xa, ha), n, ua, v1, color), n);
                }
            }
            // A post where a multiple of two units falls in this step.
            const float post = std::ceil(s0.distance / 2.0f) * 2.0f;
            if (post < s1.distance) {
                const float f = (post - s0.distance) / std::max(1e-4f, s1.distance - s0.distance);
                Station s = s0;
                s.point = s0.point + (s1.point - s0.point) * f;
                const float w = 0.06f * scale, px = x - in * 0.08f * scale;
                const Vec3 c[4] = { at(s, px - w, 0), at(s, px + w, 0), at(s, px + w, 0), at(s, px - w, 0) };
                const Vec3 along = s0.forward * w;
                const Vec3 corners[4] = { c[0] - along, c[1] - along, c[2] + along, c[3] + along };
                const float top = base + 0.78f * scale;
                for (int k = 0; k < 4; ++k) {
                    const Vec3 a = corners[k] + Vec3{ 0, 0, base }, b = corners[(k + 1) % 4] + Vec3{ 0, 0, base };
                    const Vec3 n = HorizontalDirection(Cross(b - a, up), s0.right);
                    AddQuad(g, Vertex(a, n, 0, 0, color), Vertex({ a.x, a.y, a.z - base + top }, n, 0, 1, color),
                            Vertex({ b.x, b.y, b.z - base + top }, n, 1, 1, color), Vertex(b, n, 1, 0, color), n);
                }
            }
        }
    }

    // Collision, on every other cross-section: a quad every few units is
    // plenty for a car and keeps the packs small.
    std::vector<size_t> picks;
    for (size_t i = first; i <= last; i += 2) picks.push_back(i);
    if (picks.back() != last) picks.push_back(last);
    const auto edge = [&](float lateral, float height) {
        std::vector<Vec3> line;
        for (size_t i : picks) line.push_back(at(stations[i], lateral, height));
        return line;
    };
    CollisionStrip road;
    road.left = edge(-half, 0.0f);
    road.right = edge(half, 0.0f);
    road.surface = kSurfaceRoad;
    piece.strips.push_back(road);
    for (int side = -1; side <= 1; side += 2) {
        if (!p.Walk(side)) continue;
        // Its height where it sinks away towards a road without one.
        const auto sunk = [&](float lateral) {
            std::vector<Vec3> line;
            for (size_t i : picks)
                line.push_back(at(stations[i], lateral,
                                  p.sidewalkHeight * (1.0f - taper.WalkLow(side, stations[i].distance))));
            return line;
        };
        CollisionStrip walk;
        walk.left = sunk(side < 0 ? -p.Edge(-1) : half);
        walk.right = sunk(side < 0 ? -half : p.Edge(1));
        walk.surface = kSurfaceSidewalk;
        piece.strips.push_back(walk);
        // The kerb, in runs where it still stands.
        CollisionWall kerb;
        kerb.barrier = false;
        kerb.height = p.sidewalkHeight;
        kerb.surface = kSurfaceSidewalk;
        for (size_t i : picks) {
            if (taper.WalkLow(side, stations[i].distance) > 0.5f) {
                if (kerb.points.size() >= 2 && p.sidewalkHeight > 0.02f) piece.walls.push_back(kerb);
                kerb.points.clear();
                continue;
            }
            kerb.points.push_back(at(stations[i], half * side, 0.0f));
        }
        if (kerb.points.size() >= 2 && p.sidewalkHeight > 0.02f) piece.walls.push_back(kerb);
    }
    for (int side = -1; side <= 1; side += 2) {
        if (!p.Wall(side) || p.wallHeight < 0.05f) continue;
        // In runs, leaving out where the wall is (see StretchTaper).
        CollisionWall wall;
        wall.barrier = true;
        for (size_t i : picks) {
            const float d = stations[i].distance;
            if (!taper.WallAt(side, d, d) || taper.WallClosed(side, d) > 0.6f) {
                if (wall.points.size() >= 2) piece.walls.push_back(wall);
                wall.points.clear();
                continue;
            }
            wall.points.push_back(at(stations[i], p.Edge(side) * side,
                                     p.Walk(side) ? p.sidewalkHeight * (1.0f - taper.WalkLow(side, d)) : 0.0f));
        }
        if (wall.points.size() >= 2) piece.walls.push_back(wall);
    }
    // A raised median: its top, and a kerb -- or for a barrier, a wall nothing
    // gets over -- along both sides, where it is not opened for a junction.
    if (median > 0.01f && p.medianType >= 2) {
        std::vector<std::vector<size_t>> runs(1);
        for (size_t i : picks) {
            const Station& s = stations[i];
            const bool open = s.distance < medianBefore - 0.01f || s.distance > medianAfter + 0.01f ||
                              taper.MedianClosed(s.distance) > 0.5f;
            if (open) {
                if (!runs.back().empty()) runs.emplace_back();
            } else {
                runs.back().push_back(i);
            }
        }
        for (const auto& run : runs) {
            if (run.size() < 2) continue;
            const auto line = [&](float lateral, float height) {
                std::vector<Vec3> out;
                for (size_t i : run) out.push_back(at(stations[i], lateral, height));
                return out;
            };
            const float h = p.medianType == 4 ? std::max(0.3f, p.medianHeight) : p.medianHeight;
            if (p.medianType == 4) {
                for (float x : { medianLeft, medianLeft + median }) {
                    CollisionWall barrier;
                    barrier.points = line(x, 0.0f);
                    barrier.barrier = true;
                    piece.walls.push_back(barrier);
                }
                continue;
            }
            CollisionStrip top;
            top.left = line(medianLeft, h);
            top.right = line(medianLeft + median, h);
            top.surface = p.medianType == 3 ? kSurfaceGrass : kSurfaceSidewalk;
            piece.strips.push_back(top);
            for (float x : { medianLeft, medianLeft + median }) {
                CollisionWall kerb;
                kerb.points = line(x, 0.0f);
                kerb.barrier = false;
                kerb.height = h;
                kerb.surface = kSurfaceSidewalk;
                piece.walls.push_back(kerb);
            }
        }
    }
}


// The bank of every station: from the curvature in auto mode, smoothed along
// the road, and eased to flat over the last stretch at ends that need it.
void Bank(const RoadProfile& p, const Curve& curve, std::vector<Station>& stations, bool easeStart, bool easeEnd) {
    if (stations.size() < 2) return;
    const float limit = Radians(std::clamp(p.bankMax, 0.0f, 45.0f));
    std::vector<float> raw(stations.size(), 0.0f);
    if (p.bankMode == 2) {
        for (float& b : raw) b = Radians(std::clamp(p.bankAngle, -45.0f, 45.0f));
    } else {
        const float v = p.designSpeed / 3.6f;   // km/h to m/s
        for (size_t i = 0; i < stations.size(); ++i) {
            const Vec3 d1 = curve.Tangent(stations[i].t), d2 = curve.Second(stations[i].t);
            const float speed2 = d1.x * d1.x + d1.y * d1.y;
            if (speed2 < 1e-8f) continue;
            // Signed curvature in plan: positive turning left, where the
            // right edge is the outside and goes up.
            const float k = (d1.x * d2.y - d1.y * d2.x) / (speed2 * std::sqrt(speed2));
            raw[i] = std::clamp(std::atan(v * v * k / 9.81f), -limit, limit);
        }
        // Smoothed over about 16 units either way.
        std::vector<float> smooth(raw.size(), 0.0f);
        const int window = 8;
        for (size_t i = 0; i < raw.size(); ++i) {
            float sum = 0.0f;
            int count = 0;
            for (int k = -window; k <= window; ++k) {
                const long j = (long)i + k;
                if (j < 0 || j >= (long)raw.size()) continue;
                sum += raw[(size_t)j];
                ++count;
            }
            smooth[i] = count ? sum / count : 0.0f;
        }
        raw = std::move(smooth);
    }
    const float total = stations.back().distance;
    const float taper = std::min(25.0f, total * 0.35f);
    for (size_t i = 0; i < stations.size(); ++i) {
        float ease = 1.0f;
        const float d = stations[i].distance;
        if (easeStart && taper > 0.01f) {
            const float t = std::clamp(d / taper, 0.0f, 1.0f);
            ease = std::min(ease, t * t * (3.0f - 2.0f * t));
        }
        if (easeEnd && taper > 0.01f) {
            const float t = std::clamp((total - d) / taper, 0.0f, 1.0f);
            ease = std::min(ease, t * t * (3.0f - 2.0f * t));
        }
        stations[i].bank = raw[i] * ease;
    }
}

// A wall built of a game model: the model stood along the edge every
// `wallModelSpacing`, each turned along the road there. Returned as poles
// without a lamp, so they draw and bake as the street lights do.
void PlaceWallModels(const Segment& segment, const std::vector<Station>& stations, std::vector<Pole>& out) {
    const RoadProfile& p = segment.profile;
    if (p.wallStyle != 2 || p.wallModel.empty() || p.wallModelSpacing < 0.5f || stations.size() < 2) return;
    const float total = stations.back().distance;
    for (int side = -1; side <= 1; side += 2) {
        if (!p.Wall(side)) continue;
        size_t cursor = 0;
        for (float at = p.wallModelSpacing * 0.5f; at < total; at += p.wallModelSpacing) {
            while (cursor + 1 < stations.size() - 1 && stations[cursor + 1].distance < at) ++cursor;
            const Station& s0 = stations[cursor];
            const Station& s1 = stations[cursor + 1];
            const float f = std::clamp((at - s0.distance) / std::max(1e-4f, s1.distance - s0.distance), 0.0f, 1.0f);
            Station s = s0;
            s.point = s0.point + (s1.point - s0.point) * f;
            s.bank = s0.bank + (s1.bank - s0.bank) * f;
            const Vec3 forward = HorizontalDirection(s0.forward + (s1.forward - s0.forward) * f, s0.forward);
            s.right = RightOf(forward);
            Pole piece;
            piece.model = p.wallModel;
            piece.segment = segment.id;
            piece.position = s.At(p.Edge(side) * side, p.Walk(side) ? p.sidewalkHeight : 0.0f);
            piece.heading = Degrees(std::atan2(forward.y, forward.x)) + p.wallModelTurn + (side < 0 ? 180.0f : 0.0f);
            piece.lamp = piece.position;
            piece.radius = 1.0f;
            piece.strength = 0.0f;
            out.push_back(piece);
        }
    }
}

// Pillars under a bridge: a square column every `pillarSpacing`, from the
// deck's underside to the ground, two side by side under a wide deck.
void PlacePillars(const Segment& segment, const std::vector<Station>& stations,
                  const Network::GroundQuery& ground, const Network::ModelBounds& bounds, MeshPiece& piece,
                  std::vector<Pole>& models) {
    const RoadProfile& p = segment.profile;
    if (!p.bridge || p.pillarSpacing < 2.0f || stations.size() < 2) return;
    const float total = stations.back().distance;
    const float w = std::max(0.3f, p.pillarWidth) * 0.5f;
    const u32 grey = Grey(0.5f);
    // Across the deck: as many as asked, spread evenly over its width.
    const int count = p.pillarCount > 0 ? std::min(p.pillarCount, 16) : (p.HalfWidth() > 7.0f ? 2 : 1);
    std::vector<float> laterals;
    for (int i = 0; i < count; ++i) laterals.push_back(-p.HalfWidth() + 2.0f * p.HalfWidth() * (i + 0.5f) / count);
    // The game catches a car on a vertical wall well above the wall's top (2-3
    // m), so a pillar's collision only rises a few metres from the ground,
    // enough for cars driving under the bridge, and none at all where that
    // would come near the deck the cars drive on.
    constexpr float kPillarCollision = 4.0f;   // from the ground up
    constexpr float kDeckClearance = 5.0f;     // between its top and the deck
    const auto collides = [&](float bottom, float deck, float& height) {
        height = std::min(height, kPillarCollision);
        return deck - (bottom + height) >= kDeckClearance;
    };
    size_t cursor = 0;
    for (float at = p.pillarSpacing * 0.5f; at < total - 2.0f; at += p.pillarSpacing) {
        while (cursor + 1 < stations.size() - 1 && stations[cursor + 1].distance < at) ++cursor;
        const Station& s0 = stations[cursor];
        const Station& s1 = stations[cursor + 1];
        const float f = std::clamp((at - s0.distance) / std::max(1e-4f, s1.distance - s0.distance), 0.0f, 1.0f);
        const Vec3 point = s0.point + (s1.point - s0.point) * f;
        const Vec3 forward = HorizontalDirection(s0.forward + (s1.forward - s0.forward) * f, s0.forward);
        const Vec3 right = RightOf(forward);
        for (float lateral : laterals) {
            const float bank = s0.bank + (s1.bank - s0.bank) * f;
            const Vec3 centre = point + right * (lateral * std::cos(bank)) + Vec3{ 0.0f, 0.0f, lateral * std::sin(bank) };
            float top = centre.z - p.deckThickness;
            float bottom = top - 30.0f;
            float found = 0.0f;
            const bool grounded = ground && ground(centre.x, centre.y, top - 0.5f, found);
            if (grounded) bottom = found - 1.0f;
            if (top - bottom < 0.5f) continue;
            const float deck = centre.z;
            // Collision from the ground it stands on (none without one).
            const float floor = bottom + 1.0f;

            // A game model: its top under the deck; the column fills what it
            // leaves below, down to the ground.
            Vec3 lo, hi;
            if (!p.pillarModel.empty() && bounds && bounds(p.pillarModel, lo, hi) && hi.z - lo.z > 0.1f) {
                const float heading = Degrees(std::atan2(forward.y, forward.x)) + p.pillarModelTurn;
                Pole model;
                model.model = p.pillarModel;
                model.segment = segment.id;
                model.position = { centre.x, centre.y, top - hi.z };
                model.heading = heading;
                model.lamp = model.position;
                model.radius = 1.0f;
                model.strength = 0.0f;
                models.push_back(model);
                // Its footprint, turned, collides as a wall from the ground up.
                const float turn = heading * 3.14159265f / 180.0f;
                const Vec3 ax{ std::cos(turn), std::sin(turn), 0.0f }, ay{ -ax.y, ax.x, 0.0f };
                CollisionWall wall;
                wall.barrier = false;
                wall.height = top - floor;
                wall.surface = kSurfaceWall;
                if (grounded && collides(floor, deck, wall.height)) {
                    const float xs[4] = { lo.x, hi.x, hi.x, lo.x }, ys[4] = { lo.y, lo.y, hi.y, hi.y };
                    for (int k = 0; k <= 4; ++k) {
                        const Vec3 at = model.position + ax * xs[k % 4] + ay * ys[k % 4];
                        wall.points.push_back({ at.x, at.y, floor });
                    }
                    piece.walls.push_back(std::move(wall));
                }
                const float under = model.position.z + lo.z;
                if (under - bottom < 0.3f) continue;
                top = under + 0.1f;   // the column below it, into the model a little
            }
            const Vec3 c[4] = { centre - forward * w - right * w, centre + forward * w - right * w,
                                centre + forward * w + right * w, centre - forward * w + right * w };
            MeshGroup& g = piece.Group(p.pillarTexture);
            CollisionWall wall;
            wall.barrier = false;
            wall.height = top - floor;
            wall.surface = kSurfaceWall;
            const bool solid = grounded && top > floor + 0.3f && collides(floor, deck, wall.height);
            for (int k = 0; k < 4; ++k) {
                const Vec3 a = c[k], b = c[(k + 1) % 4];
                // Outwards, from the column's middle through this side.
                const Vec3 n = HorizontalDirection((a + b) * 0.5f - centre, right);
                const float height = (top - bottom) / 4.0f;
                AddQuad(g, Vertex({ a.x, a.y, bottom }, n, 0, 0, grey), Vertex({ a.x, a.y, top }, n, 0, height, grey),
                        Vertex({ b.x, b.y, top }, n, 1, height, grey), Vertex({ b.x, b.y, bottom }, n, 1, 0, grey), n);
                wall.points.push_back({ a.x, a.y, floor });
            }
            wall.points.push_back(wall.points.front());
            if (solid) piece.walls.push_back(std::move(wall));
        }
    }
}

// Street lights along a stretch of road, every `poleSpacing` from half a
// spacing in, on the sides the profile asks for.
// Smooth random height in [-1, 1] at a point on the ground: value noise on a
// lattice, two octaves, the same for the same seed wherever it is asked.
float ReliefNoise(float x, float y, int seed) {
    const auto lattice = [seed](int i, int j) {
        u32 h = (u32)i * 73856093u ^ (u32)j * 19349663u ^ (u32)seed * 83492791u;
        h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
        return (float)(h & 0xFFFF) / 32767.5f - 1.0f;
    };
    const auto octave = [&](float px, float py) {
        const float fx = std::floor(px), fy = std::floor(py);
        const int i = (int)fx, j = (int)fy;
        float u = px - fx, v = py - fy;
        u = u * u * (3.0f - 2.0f * u);
        v = v * v * (3.0f - 2.0f * v);
        const float a = lattice(i, j), b = lattice(i + 1, j), c = lattice(i, j + 1), d = lattice(i + 1, j + 1);
        return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
    };
    return octave(x / 18.0f, y / 18.0f) * 0.7f + octave(x / 6.0f + 31.7f, y / 6.0f - 12.3f) * 0.3f;
}

// What the ground beside a road needs from round it: the settings of the
// road going on at each end (already turned to this road's sides, null where
// there is none or it has no ground that side), the ground to meet, and --
// out -- the row it ends with at each end and side, for junction corners.
struct SideContext {
    const RoadProfile* next[2][2] = {};          // [end][side: 0 left, 1 right]
    const Network::GroundQuery* ground = nullptr;
    std::vector<Vec3> ends[2][2];                // [end][side], from the road outwards
};

// A side's cross-section: the height at each offset out from the road.
struct CrossSection {
    std::vector<float> offset{ 0.0f }, height{ 0.0f };
    float width = 0.0f;
    float At(float x) const {
        if (x <= 0.0f) return 0.0f;
        for (size_t i = 1; i < offset.size(); ++i)
            if (x <= offset[i]) {
                const float f = (x - offset[i - 1]) / std::max(1e-4f, offset[i] - offset[i - 1]);
                return height[i - 1] + (height[i] - height[i - 1]) * f;
            }
        return height.back();
    }
};

CrossSection SectionOf(const RoadProfile::SideTerrain& t) {
    CrossSection c;
    for (const RoadProfile::TerrainBand& band : t.bands) {
        if (band.width < 0.1f) continue;
        const int steps = std::clamp((int)std::ceil(band.width / 1.5f), 2, 80);
        const float o0 = c.offset.back(), h0 = c.height.back();
        for (int i = 1; i <= steps; ++i) {
            const float u = (float)i / steps;
            c.offset.push_back(o0 + band.width * u);
            c.height.push_back(h0 + band.rise * u * u * (3.0f - 2.0f * u));
        }
    }
    c.width = c.offset.back();
    return c;
}

// Texture coordinates in the world's own space, so neighbouring pieces carry
// on seamlessly: from above for gentle ground, from the side (along the axis
// the face looks least along) for steep ground.
int Projection(const Vec3& normal) {
    if (std::fabs(normal.z) > 0.72f) return 0;
    return std::fabs(normal.x) > std::fabs(normal.y) ? 1 : 2;
}
Vec2 Project(const Vec3& q, int projection, float tile) {
    switch (projection) {
        case 1:  return { q.y / tile, -q.z / tile };
        case 2:  return { q.x / tile, -q.z / tile };
        default: return { q.x / tile, -q.y / tile };
    }
}

// The ground beside a road (RoadProfile::terrain): rows across each side at
// every station, from the road's outer edge (past a wall's thickness) at its
// height, band after band outwards, the relief growing away from the road.
// Over the last stretch before a node where another road with ground beside
// it goes on, the two ease to halfway between their settings, so they meet
// edge to edge. The outer edge is brought to the ground there when asked.
// Steep, high ground turns to rock triangle by triangle -- grass, then the
// grass-and-rock texture, then rock -- the thresholds jittered so the edge is
// ragged. Grass collision, row by row.
void BuildSideTerrain(const RoadProfile& p, const std::vector<Station>& stations, const std::string& grass,
                      const std::string& rockTexture, const std::string& mixTexture, SideContext& context,
                      MeshPiece& piece) {
    if (stations.size() < 2) return;
    constexpr float kGrassTile = 24.0f, kRockTile = 14.0f;
    const auto smooth = [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float total = stations.back().distance;
    const float blend = std::min(30.0f, total * 0.45f);
    for (int side = -1; side <= 1; side += 2) {
        const int si = side < 0 ? 0 : 1;
        const RoadProfile::SideTerrain& t = p.terrain[si];
        if (!t.on || t.bands.empty() || t.Width() < 0.5f) continue;
        // The settings this side mixes in: its own, and each neighbour's.
        struct Source { const RoadProfile* profile; CrossSection section; float start, lift; };
        const auto sourceOf = [&](const RoadProfile& q) {
            Source src{ &q, SectionOf(q.terrain[si]), 0.0f, 0.0f };
            const bool wall = side < 0 ? q.wallLeft : q.wallRight;
            src.start = q.Edge(side) + (wall ? std::max(q.wallThickness, 0.05f) : 0.0f);
            src.lift = q.Walk(side) ? q.sidewalkHeight : 0.0f;
            return src;
        };
        Source own = sourceOf(p);
        Source next[2];
        bool hasNext[2] = { false, false };
        for (int e = 0; e < 2; ++e)
            if (const RoadProfile* q = context.next[e][si]) {
                next[e] = sourceOf(*q);
                hasNext[e] = next[e].section.width > 0.4f;
            }
        float widest = own.section.width;
        for (int e = 0; e < 2; ++e)
            if (hasNext[e]) widest = std::max(widest, next[e].section.width);
        const size_t across = (size_t)std::clamp((int)std::ceil(widest / 2.5f), 2, 120) + 1;
        const size_t count = stations.size();
        std::vector<Vec3> pos(count * across);
        std::vector<float> above(count * across);       // over the road's edge
        const auto at = [across](size_t k, size_t i) { return k * across + i; };
        for (size_t k = 0; k < count; ++k) {
            const Station& st = stations[k];
            // How much of each neighbour: half at the node, none `blend` away.
            float w[2] = { 0.0f, 0.0f };
            if (hasNext[0]) w[0] = 0.5f * (1.0f - smooth(0.0f, blend, st.distance));
            if (hasNext[1]) w[1] = 0.5f * (1.0f - smooth(0.0f, blend, total - st.distance));
            const float wo = 1.0f - w[0] - w[1];
            const auto mix = [&](auto get) {
                float v = get(own) * wo;
                for (int e = 0; e < 2; ++e)
                    if (w[e] > 0.0f) v += get(next[e]) * w[e];
                return v;
            };
            const float width = mix([](const Source& src) { return src.section.width; });
            const float start = mix([](const Source& src) { return src.start; });
            const float lift = mix([](const Source& src) { return src.lift; });
            const Vec3 edge = st.At(side * start, lift);
            for (size_t i = 0; i < across; ++i) {
                const float u = (float)i / (float)(across - 1);
                Vec3 q = edge + st.right * (side * width * u);
                const float x = width * u;
                const float h = mix([&](const Source& src) {
                    const RoadProfile::SideTerrain& st2 = src.profile->terrain[si];
                    const float grow = std::min(1.0f, x / 10.0f);
                    return src.section.At(src.section.width * u) +
                           st2.relief * grow * ReliefNoise(q.x, q.y, st2.seed);
                });
                q.z = edge.z + h;
                pos[at(k, i)] = q;
            }
            // The outer edge onto the ground there, the rest leaning with it.
            const bool meet = mix([si](const Source& src) {
                return src.profile->terrain[si].meetGround ? 1.0f : 0.0f; }) > 0.5f;
            if (meet && context.ground && *context.ground) {
                const Vec3 outer = pos[at(k, across - 1)];
                float ground = 0.0f;
                if ((*context.ground)(outer.x, outer.y, outer.z + 80.0f, ground) && ground > outer.z - 120.0f) {
                    const float delta = ground - 0.05f - outer.z;
                    for (size_t i = 1; i < across; ++i) {
                        const float u = (float)i / (float)(across - 1);
                        pos[at(k, i)].z += delta * smooth(0.0f, 1.0f, u);
                    }
                }
            }
            for (size_t i = 0; i < across; ++i) above[at(k, i)] = pos[at(k, i)].z - edge.z;
        }
        context.ends[0][si].assign(pos.begin(), pos.begin() + (ptrdiff_t)across);
        context.ends[1][si].assign(pos.end() - (ptrdiff_t)across, pos.end());

        // Smooth normals, and where along the road and how far up the
        // ground's own surface each point is, for the rock's texture.
        std::vector<Vec3> nrm(count * across);
        std::vector<float> alongAt(count * across), upAt(count * across);
        for (size_t k = 0; k < count; ++k) {
            float up = 0.0f;
            for (size_t i = 0; i < across; ++i) {
                const Vec3 along = pos[at(std::min(k + 1, count - 1), i)] - pos[at(k > 0 ? k - 1 : 0, i)];
                const Vec3 out = pos[at(k, std::min(i + 1, across - 1))] - pos[at(k, i > 0 ? i - 1 : 0)];
                Vec3 n = Cross(along, out);
                if (n.z < 0.0f) n = n * -1.0f;
                nrm[at(k, i)] = Normalize(n);
                if (i) up += Length(pos[at(k, i)] - pos[at(k, i - 1)]);
                alongAt[at(k, i)] = stations[k].distance;
                upAt[at(k, i)] = up;
            }
        }
        // How much rock each point wants: steep, high, and a ragged edge from
        // noise. The ground is cut exactly where this crosses each threshold,
        // so grass turns to grass-and-rock and then to rock along smooth,
        // irregular lines instead of the triangles' edges.
        std::vector<float> want(count * across, 0.0f);
        const bool rocky = t.rock && !rockTexture.empty();
        if (rocky)
            for (size_t k = 0; k < count; ++k)
                for (size_t i = 0; i < across; ++i) {
                    const size_t v = at(k, i);
                    const float slope = Degrees(std::acos(std::clamp(nrm[v].z, 0.0f, 1.0f)));
                    const float steep = smooth(t.rockAngle - 12.0f, t.rockAngle + 12.0f, slope);
                    const float lifted = smooth(t.rockHeight - 2.0f, t.rockHeight + 5.0f, std::fabs(above[v]));
                    want[v] = steep * (0.45f + 0.55f * lifted) +
                              0.2f * ReliefNoise(pos[v].x * 1.4f, pos[v].y * 1.4f, t.seed + 977);
                }
        // The grass-and-rock texture is a strip: grass along one long edge,
        // rock along the other, a ragged line between. It is laid across the
        // band between the two limits -- grass edge against the grass, rock
        // edge against the rock -- and runs on along the road.
        const float limits[2] = { mixTexture.empty() ? 0.5f : 0.43f, mixTexture.empty() ? 0.5f : 0.57f };

        const std::string names[3] = { t.texture.empty() ? grass : t.texture, mixTexture, rockTexture };
        struct Point { Vec3 p, n; float w, along, up; };
        const auto pointAt = [&](size_t v) { return Point{ pos[v], nrm[v], want[v], alongAt[v], upAt[v] }; };
        const auto lerp = [](const Point& a, const Point& b, float f) {
            return Point{ a.p + (b.p - a.p) * f, Normalize(a.n + (b.n - a.n) * f), a.w + (b.w - a.w) * f,
                          a.along + (b.along - a.along) * f, a.up + (b.up - a.up) * f };
        };
        // The part of a polygon on one side of a threshold.
        const auto clip = [&](const std::vector<Point>& in, float limit, bool keepAbove) {
            std::vector<Point> out;
            for (size_t i = 0; i < in.size(); ++i) {
                const Point& a = in[i];
                const Point& b = in[(i + 1) % in.size()];
                const bool ina = keepAbove ? a.w >= limit : a.w < limit;
                const bool inb = keepAbove ? b.w >= limit : b.w < limit;
                if (ina) out.push_back(a);
                if (ina != inb) out.push_back(lerp(a, b, (limit - a.w) / (b.w - a.w)));
            }
            return out;
        };
        // Grid points keep one vertex per texture; cut points get their own.
        std::vector<int> slot[3];
        for (auto& g : slot) g.assign(count * across, -1);
        const auto add = [&](int g, const Point& q) -> u32 {
            MeshGroup& group = piece.Group(names[g]);
            MeshVertex v;
            v.position = q.p;
            v.normal = q.n;
            if (g == 0) v.uv = Project(q.p, 0, kGrassTile);
            else if (g == 1) {
                const float across = std::clamp((q.w - limits[0]) / std::max(1e-4f, limits[1] - limits[0]), 0.0f, 1.0f);
                v.uv = { 0.03f + 0.94f * across, -q.along / 20.0f };
            } else {
                // The game's cliff tiles only along its length (V): that runs
                // along the road, and up the slope it is mirrored back and
                // forth instead of wrapping, so no seam shows.
                const float f = q.up / 20.0f;
                const float mirrored = 1.0f - std::fabs((f - std::floor(f)) * 2.0f - 1.0f);
                v.uv = { 0.02f + 0.96f * mirrored, -q.along / 24.0f };
            }
            group.vertices.push_back(v);
            return (u32)(group.vertices.size() - 1);
        };
        const auto gridVertex = [&](int g, size_t v) -> u32 {
            if (slot[g][v] < 0) slot[g][v] = (int)add(g, pointAt(v));
            return (u32)slot[g][v];
        };
        const auto emit = [&](size_t i0, size_t i1, size_t i2) {
            // Counter-clockwise from above.
            if (Cross(pos[i1] - pos[i0], pos[i2] - pos[i0]).z < 0.0f) std::swap(i1, i2);
            const size_t ids[3] = { i0, i1, i2 };
            for (int g = 0; g < 3; ++g) {
                if (g > 0 && !rocky) break;
                const float lo = g == 0 ? -1e9f : limits[g - 1], hi = g == 2 ? 1e9f : limits[g];
                if (lo >= hi) continue;
                // Whole inside: the grid's shared vertices.
                bool whole = true;
                for (size_t id : ids) whole = whole && want[id] >= lo && want[id] < hi;
                MeshGroup* group = nullptr;
                if (whole) {
                    const u32 a = gridVertex(g, i0), b = gridVertex(g, i1), c = gridVertex(g, i2);
                    group = &piece.Group(names[g]);
                    group->indices.insert(group->indices.end(), { a, b, c });
                    continue;
                }
                std::vector<Point> poly{ pointAt(i0), pointAt(i1), pointAt(i2) };
                if (g > 0) poly = clip(poly, lo, true);
                if (g < 2 && poly.size() >= 3) poly = clip(poly, hi, false);
                if (poly.size() < 3) continue;
                std::vector<u32> fan;
                for (const Point& q : poly) fan.push_back(add(g, q));
                group = &piece.Group(names[g]);
                for (size_t j = 1; j + 1 < fan.size(); ++j)
                    group->indices.insert(group->indices.end(), { fan[0], fan[j], fan[j + 1] });
            }
        };
        for (size_t k = 0; k + 1 < count; ++k)
            for (size_t i = 0; i + 1 < across; ++i) {
                const size_t a = at(k, i), b = at(k, i + 1), c = at(k + 1, i), d = at(k + 1, i + 1);
                emit(a, b, c);
                emit(b, d, c);
            }
        for (size_t i = 0; i + 1 < across; ++i) {
            CollisionStrip strip;
            strip.surface = kSurfaceGrass;
            for (size_t k = 0; k < count; ++k) {
                strip.left.push_back(pos[at(k, side < 0 ? i + 1 : i)]);
                strip.right.push_back(pos[at(k, side < 0 ? i : i + 1)]);
            }
            piece.strips.push_back(std::move(strip));
        }
    }
    // Groups a side left empty (no rock there) go.
    piece.groups.erase(std::remove_if(piece.groups.begin(), piece.groups.end(),
                                      [](const MeshGroup& g) { return g.indices.empty(); }),
                       piece.groups.end());
}

// The gap between the ground beside two roads round a junction corner: the
// two end rows joined, point for point along their length.
void LoftCorner(const std::vector<Vec3>& a, const std::vector<Vec3>& b, const std::string& grass, MeshPiece& piece) {
    if (a.size() < 2 || b.size() < 2) return;
    const size_t n = std::max(a.size(), b.size());
    const auto resample = [n](const std::vector<Vec3>& row) {
        std::vector<float> along{ 0.0f };
        for (size_t i = 1; i < row.size(); ++i) along.push_back(along.back() + Length(row[i] - row[i - 1]));
        std::vector<Vec3> out;
        for (size_t j = 0; j < n; ++j) {
            const float d = along.back() * (float)j / (float)(n - 1);
            size_t i = 1;
            while (i + 1 < row.size() && along[i] < d) ++i;
            const float f = std::clamp((d - along[i - 1]) / std::max(1e-4f, along[i] - along[i - 1]), 0.0f, 1.0f);
            out.push_back(row[i - 1] + (row[i] - row[i - 1]) * f);
        }
        return out;
    };
    const std::vector<Vec3> ra = resample(a), rb = resample(b);
    MeshGroup& group = piece.Group(grass);
    const u32 base = (u32)group.vertices.size();
    for (int r = 0; r < 2; ++r)
        for (const Vec3& q : r ? rb : ra) {
            MeshVertex v;
            v.position = q;
            v.normal = { 0.0f, 0.0f, 1.0f };
            v.uv = Project(q, 0, 24.0f);
            group.vertices.push_back(v);
        }
    const u32 m = (u32)n;
    for (u32 j = 0; j + 1 < m; ++j) {
        const u32 a0 = base + j, a1 = a0 + 1, b0 = base + m + j, b1 = b0 + 1;
        for (const auto& tri : { std::array<u32, 3>{ a0, a1, b0 }, std::array<u32, 3>{ a1, b1, b0 } }) {
            const Vec3& p0 = group.vertices[tri[0]].position, & p1 = group.vertices[tri[1]].position,
                      & p2 = group.vertices[tri[2]].position;
            if (Cross(p1 - p0, p2 - p0).z >= 0.0f) group.indices.insert(group.indices.end(), { tri[0], tri[1], tri[2] });
            else group.indices.insert(group.indices.end(), { tri[0], tri[2], tri[1] });
        }
    }
    CollisionStrip strip;
    strip.surface = kSurfaceGrass;
    // Its left on the left of the way the rows run out from the junction.
    const Vec3 out = ra.back() - ra.front();
    const bool bLeft = Cross(out, rb.front() - ra.front()).z > 0.0f;
    strip.left = bLeft ? rb : ra;
    strip.right = bLeft ? ra : rb;
    piece.strips.push_back(std::move(strip));
}

void PlacePoles(const Segment& segment, const std::vector<Station>& stations, std::vector<Pole>& out) {
    const RoadProfile& p = segment.profile;
    if (p.poleModel.empty() || p.poleSpacing < 2.0f || stations.size() < 2) return;
    const float total = stations.back().distance;
    const bool walks = p.sidewalkWidth > 0.01f;
    (void)walks;
    size_t cursor = 0;
    int index = 0;
    for (float at = p.poleSpacing * 0.5f; at < total; at += p.poleSpacing, ++index) {
        while (cursor + 1 < stations.size() - 1 && stations[cursor + 1].distance < at) ++cursor;
        const Station& s0 = stations[cursor];
        const Station& s1 = stations[cursor + 1];
        const float span = std::max(1e-4f, s1.distance - s0.distance);
        const float f = std::clamp((at - s0.distance) / span, 0.0f, 1.0f);
        const Vec3 point = s0.point + (s1.point - s0.point) * f;
        const Vec3 right = HorizontalDirection(s0.right + (s1.right - s0.right) * f, s0.right);
        const float bank = s0.bank + (s1.bank - s0.bank) * f;

        if (p.poleSides == 4 && p.MedianWidth() > 0.01f) {
            Pole pole;
            pole.model = p.poleModel;
            pole.segment = segment.id;
            const float x = p.MedianLeft() + p.MedianWidth() * 0.5f;
            const float raised = p.medianType >= 2 && p.medianType <= 3 ? p.medianHeight : 0.0f;
            pole.position = point + right * (x * std::cos(bank)) + Vec3{ 0.0f, 0.0f, x * std::sin(bank) + raised };
            pole.heading = Degrees(std::atan2(right.y, right.x)) + p.poleTurn;
            pole.lamp = pole.position + Vec3{ 0.0f, 0.0f, 7.0f };
            pole.color = p.lampColor;
            pole.radius = p.lampRadius * 1.2f;   // a double-armed pole lights both ways
            pole.strength = p.lampStrength;
            out.push_back(pole);
            continue;
        }
        std::vector<int> sides;
        switch (p.poleSides) {
            case 1: sides = { -1 }; break;
            case 2: sides = { -1, 1 }; break;
            case 3: sides = { (index % 2) ? -1 : 1 }; break;
            default: sides = { 1 }; break;
        }
        for (int side : sides) {
            const float lateral = p.Walk(side) ? std::max(p.HalfRoad(), p.Edge(side) - p.poleInset)
                                               : p.HalfRoad() + p.poleInset;
            Pole pole;
            pole.model = p.poleModel;
            pole.segment = segment.id;
            pole.position = point + right * (lateral * (float)side * std::cos(bank)) +
                            Vec3{ 0.0f, 0.0f, lateral * (float)side * std::sin(bank) + (p.Walk(side) ? p.sidewalkHeight : 0.0f) };
            // Facing the road: towards its middle.
            const Vec3 inward = right * (float)-side;
            pole.heading = Degrees(std::atan2(inward.y, inward.x)) + p.poleTurn;
            // The lamp hangs over the road's edge, up high.
            pole.lamp = pole.position + inward * 2.0f + Vec3{ 0.0f, 0.0f, 7.0f };
            pole.color = p.lampColor;
            pole.radius = p.lampRadius;
            pole.strength = p.lampStrength;
            out.push_back(pole);
        }
    }
}

// The vertex colours: the ambient of the piece's road, plus the light of
// every lamp in reach, falling off smoothly to nothing at its radius. Alpha
// is 0x82, as on every shipped road surface.
void LightPieces(std::vector<MeshPiece>& pieces, const std::vector<Pole>& poles,
                 const std::function<const RoadProfile*(const MeshPiece&)>& profileOf,
                 const Network::LightSources* sources) {
    for (MeshPiece& piece : pieces) {
        const RoadProfile* profile = profileOf(piece);
        const u32 ambient = profile ? profile->ambient : 0x383C42;
        const Aabb box = piece.Bounds();
        const auto reaching = [&](const std::vector<Pole>& list) {
            std::vector<const Pole*> out;
            for (const Pole& pole : list) {
                const float r = pole.radius;
                if (pole.strength <= 0.0f || pole.lamp.x + r < box.min.x || pole.lamp.x - r > box.max.x ||
                    pole.lamp.y + r < box.min.y || pole.lamp.y - r > box.max.y)
                    continue;
                out.push_back(&pole);
            }
            return out;
        };
        const std::vector<const Pole*> near = reaching(poles);
        const std::vector<const Pole*> gameLamps = sources ? reaching(sources->lamps) : std::vector<const Pole*>{};
        for (MeshGroup& group : piece.groups)
            for (MeshVertex& v : group.vertices) {
                // The light the game baked into its own ground under this
                // point, when there is ground of its there: its street lights
                // are in it already. Elsewhere the night ambient, and the
                // game's street lights in reach.
                u32 base = ambient;
                const bool sampled = sources && sources->sample && sources->sample(v.position, base);
                float c[3] = { (float)((base >> 16) & 0xFF), (float)((base >> 8) & 0xFF), (float)(base & 0xFF) };
                std::vector<const Pole*> lights = near;
                if (!sampled) lights.insert(lights.end(), gameLamps.begin(), gameLamps.end());
                // Lamps combine like light on a screen, never past their own
                // colour: under one the road reaches about 0xB0 as the game's
                // do, and overlapping pools do not burn out to white.
                for (const Pole* pole : lights) {
                    const float dx = v.position.x - pole->lamp.x, dy = v.position.y - pole->lamp.y;
                    const float d2 = (dx * dx + dy * dy) / (pole->radius * pole->radius);
                    if (d2 >= 1.0f) continue;
                    const float fall = 1.0f - d2;
                    const float f = std::min(1.0f, fall * fall * fall * pole->strength * 0.6f);
                    const float lamp[3] = { (float)((pole->color >> 16) & 0xFF), (float)((pole->color >> 8) & 0xFF),
                                            (float)(pole->color & 0xFF) };
                    for (int k = 0; k < 3; ++k)
                        if (lamp[k] > c[k]) c[k] += (lamp[k] - c[k]) * f;
                }
                u32 out = 0x82000000u;
                for (int k = 0; k < 3; ++k)
                    out |= (u32)std::clamp((int)std::lround(c[k]), 0, 255) << (16 - 8 * k);
                v.color = out;
            }
    }
}

// ---- a junction made of the game's faces ------------------------------------

namespace {

struct FaceCorner {
    Vec3 p;
    Vec2 uv;
};

bool IsRoadSurface(const std::string& t) { return t.rfind("RDP_", 0) == 0; }
bool IsWalkSurface(const std::string& t) {
    return t.find("SIDEWALK") != std::string::npos || t.find("CURB") != std::string::npos ||
           t.find("KERB") != std::string::npos;
}

float PlanArea(const Vec3& a, const Vec3& b, const Vec3& c) {
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

// A line in plan that pieces are cut along: f(p) = (p - a) . n. Only pieces
// overlapping `box` (when set) are cut by it.
struct SplitLine {
    Vec2 a, n;
    bool bounded = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float At(const Vec3& p) const { return (p.x - a.x) * n.x + (p.y - a.y) * n.y; }
};

SplitLine LineThrough(const Vec3& p, const Vec3& q) {
    SplitLine l;
    l.a = { p.x, p.y };
    const Vec3 d = HorizontalDirection(q - p);
    l.n = { d.y, -d.x };
    return l;
}

void SplitBy(const std::vector<FaceCorner>& poly, const SplitLine& line, std::vector<std::vector<FaceCorner>>& out) {
    constexpr float kEpsilon = 1e-4f;
    if (line.bounded) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const FaceCorner& c : poly) {
            x0 = std::min(x0, c.p.x); y0 = std::min(y0, c.p.y);
            x1 = std::max(x1, c.p.x); y1 = std::max(y1, c.p.y);
        }
        if (x1 < line.x0 || x0 > line.x1 || y1 < line.y0 || y0 > line.y1) { out.push_back(poly); return; }
    }
    std::vector<float> f(poly.size());
    bool plus = false, minus = false;
    for (size_t i = 0; i < poly.size(); ++i) {
        f[i] = line.At(poly[i].p);
        if (std::fabs(f[i]) < kEpsilon) f[i] = 0.0f;
        plus |= f[i] > 0.0f;
        minus |= f[i] < 0.0f;
    }
    if (!plus || !minus) { out.push_back(poly); return; }
    std::vector<FaceCorner> a, b;
    for (size_t i = 0; i < poly.size(); ++i) {
        const size_t k = (i + 1) % poly.size();
        if (f[i] >= 0.0f) a.push_back(poly[i]);
        if (f[i] <= 0.0f) b.push_back(poly[i]);
        if ((f[i] > 0.0f && f[k] < 0.0f) || (f[i] < 0.0f && f[k] > 0.0f)) {
            const float t = f[i] / (f[i] - f[k]);
            FaceCorner c;
            c.p = poly[i].p + (poly[k].p - poly[i].p) * t;
            c.uv = { poly[i].uv.x + (poly[k].uv.x - poly[i].uv.x) * t, poly[i].uv.y + (poly[k].uv.y - poly[i].uv.y) * t };
            a.push_back(c);
            b.push_back(c);
        }
    }
    if (a.size() >= 3) out.push_back(std::move(a));
    if (b.size() >= 3) out.push_back(std::move(b));
}

bool InTriangle2(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, float slack = 1e-4f) {
    const float d1 = PlanArea(a, b, p), d2 = PlanArea(b, c, p), d3 = PlanArea(c, a, p);
    const bool negative = d1 < -slack || d2 < -slack || d3 < -slack;
    const bool positive = d1 > slack || d2 > slack || d3 > slack;
    return !(negative && positive);
}

} // namespace

// `endSign` and `endShift`: how the road's own across (right of the way it
// was drawn) runs in this junction's terms, and the road's sideways shift.
void BuildFaceJunction(const FaceJunction& j, const Vec3& node, const Vec3& into, const RoadProfile& profile,
                       float endSign, float endShift, MeshPiece& piece) {
    if (j.faces.empty()) return;
    const Vec3 u = HorizontalDirection(into);
    const Vec3 r = RightOf(u);
    const float hr = profile.HalfRoad(), hw = profile.HalfWidth();
    const auto along = [&](const Vec3& q) { return (q.x - node.x) * u.x + (q.y - node.y) * u.y; };
    const auto lat = [&](const Vec3& q) { return (q.x - node.x) * r.x + (q.y - node.y) * r.y; };
    const u32 color = Grey(0.5f);   // lit afterwards
    const Vec3 up{ 0.0f, 0.0f, 1.0f };

    // The chosen face under a point, and the game's height there.
    const auto faceAt = [&](const Vec3& q, float* height) -> const GameFace* {
        const GameFace* best = nullptr;
        float gap = 1e30f;
        for (const GameFace& f : j.faces) {
            const float d = PlanArea(f.p[0], f.p[1], f.p[2]);
            if (std::fabs(d) < 1e-3f || !InTriangle2(q, f.p[0], f.p[1], f.p[2])) continue;
            const float l1 = PlanArea(f.p[1], f.p[2], q) / d, l2 = PlanArea(f.p[2], f.p[0], q) / d;
            const float z = f.p[0].z * l1 + f.p[1].z * l2 + f.p[2].z * (1.0f - l1 - l2);
            if (std::fabs(z - q.z) < gap) { gap = std::fabs(z - q.z); best = &f; if (height) *height = z; }
        }
        return best;
    };

    // A face is road when marked so, or by its texture when not marked.
    const auto isRoad = [](const GameFace& f) { return f.kind == 1 || (f.kind == 0 && IsRoadSurface(f.texture)); };

    // The game's road height: a plane through the corners of its road faces.
    double s[4][4] = {};
    size_t samples = 0;
    float meanZ = 0.0f;
    for (const GameFace& f : j.faces) {
        if (!isRoad(f) || std::fabs(PlanArea(f.p[0], f.p[1], f.p[2])) < 1e-3f) continue;
        for (const Vec3& q : f.p) {
            const double row[4] = { 1.0, q.x - node.x, q.y - node.y, q.z };
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 4; ++b) s[a][b] += row[a] * row[b];
            meanZ += q.z;
            ++samples;
        }
    }
    double plane[3] = { samples ? meanZ / samples : node.z, 0.0, 0.0 };
    if (samples >= 3) {
        // Gauss-Jordan on the 3x3 normal equations.
        double m[3][4];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 4; ++b) m[a][b] = s[a][b];
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) {
            int pivot = c;
            for (int a = c + 1; a < 3; ++a)
                if (std::fabs(m[a][c]) > std::fabs(m[pivot][c])) pivot = a;
            if (std::fabs(m[pivot][c]) < 1e-9) { ok = false; break; }
            for (int b = 0; b < 4; ++b) std::swap(m[c][b], m[pivot][b]);
            for (int a = 0; a < 3; ++a) {
                if (a == c) continue;
                const double k = m[a][c] / m[c][c];
                for (int b = 0; b < 4; ++b) m[a][b] -= k * m[c][b];
            }
        }
        if (ok)
            for (int a = 0; a < 3; ++a) plane[a] = m[a][3] / m[a][a];
    }
    const auto fitZ = [&](const Vec3& q) {
        return (float)(plane[0] + plane[1] * (q.x - node.x) + plane[2] * (q.y - node.y));
    };
    // The game's road height at a point: on the road face under it, or
    // carried on from the nearest one -- so the new surface follows the
    // road's own crown and slopes and meets its faces without a step.
    struct RoadPlane { Vec3 a, b, c; float z0, zx, zy; };
    std::vector<RoadPlane> roadPlanes;
    for (const GameFace& f : j.faces) {
        if (!isRoad(f)) continue;
        const float d = PlanArea(f.p[0], f.p[1], f.p[2]);
        if (std::fabs(d) < 1e-3f) continue;
        const Vec3 e1 = f.p[1] - f.p[0], e2 = f.p[2] - f.p[0];
        const Vec3 n = Cross(e1, e2);
        if (std::fabs(n.z) < 1e-6f) continue;
        RoadPlane rp;
        rp.a = f.p[0]; rp.b = f.p[1]; rp.c = f.p[2];
        rp.zx = -n.x / n.z;
        rp.zy = -n.y / n.z;
        rp.z0 = f.p[0].z - rp.zx * f.p[0].x - rp.zy * f.p[0].y;
        roadPlanes.push_back(rp);
    }
    const auto planDistance = [](const Vec3& q, const Vec3& a, const Vec3& b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float l = dx * dx + dy * dy;
        const float t = l > 1e-9f ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / l, 0.0f, 1.0f) : 0.0f;
        const float ex = a.x + dx * t - q.x, ey = a.y + dy * t - q.y;
        return ex * ex + ey * ey;
    };
    const auto gameZ = [&](const Vec3& q) {
        if (roadPlanes.empty()) return fitZ(q);
        const RoadPlane* best = nullptr;
        float bestDistance = 1e30f;
        for (const RoadPlane& rp : roadPlanes) {
            float d = 0.0f;
            if (!InTriangle2(q, rp.a, rp.b, rp.c, 1e-5f))
                d = std::min({ planDistance(q, rp.a, rp.b), planDistance(q, rp.b, rp.c), planDistance(q, rp.c, rp.a) });
            if (d < bestDistance) { bestDistance = d; best = &rp; }
        }
        // Carried on a little way only, then the road's overall plane.
        const float own = best->z0 + best->zx * q.x + best->zy * q.y;
        const float w = std::clamp(std::sqrt(bestDistance) / 12.0f, 0.0f, 1.0f);
        return own * (1.0f - w) + fitZ(q) * w;
    };

    // Where each of the new road's kerb lines reaches the game's road.
    float reach[2] = { -1.0f, -1.0f };
    float furthest = 0.0f;
    for (const GameFace& f : j.faces)
        for (const Vec3& q : f.p) furthest = std::max(furthest, along(q));
    for (int k = 0; k < 2; ++k) {
        const float side = k ? 1.0f : -1.0f;
        for (float a = 0.0f; a <= std::max(furthest, 1.0f); a += 0.1f) {
            Vec3 q = node + u * a + r * (side * hr);
            q.z = gameZ(q);
            const GameFace* f = faceAt(q, nullptr);
            if (f && isRoad(*f)) { reach[k] = a; break; }
        }
        if (reach[k] < 0.0f) reach[k] = furthest;
    }
    const float depth = std::max(reach[0], reach[1]) + 0.5f;

    // From the game's road up to the node, the road surface eases to the
    // node's own height, so the new road starts where its end is.
    // It is the game's own height by where the game's road begins, so the
    // new surface meets the game's road faces without a step.
    const float offset = node.z - gameZ(node);
    const float ease = std::min(reach[0], reach[1]);
    const auto roadZ = [&](const Vec3& q) {
        const float w = ease > 0.01f ? std::clamp(1.0f - along(q) / ease, 0.0f, 1.0f) : 0.0f;
        return gameZ(q) + offset * w;
    };

    // ---- rebuilt on the game's own mesh -----------------------------------
    //
    // The chosen faces keep the game's vertices and triangles: what each one
    // becomes (road or sidewalk) is the choice made in the wizard, and only
    // the faces reaching in under the new road are cut -- at the node's line
    // and at the road end's own lane edges, so its vertices meet the road's.

    // The road end as the procedural road leaves it: its edges across (lat,
    // in this junction's terms) and what lies between them.
    const float sign = endSign;
    const auto latOfX = [&](float x) { return sign * (x + endShift); };
    const float half = profile.HalfRoad();
    std::vector<float> roadLats;
    {
        const float sl = profile.ShoulderAt(-1);
        const float lane = profile.laneWidth;
        const float median = profile.MedianWidth();
        std::vector<float> xs = { -half, half };
        if (sl > 0.01f) xs.push_back(-half + sl);
        if (profile.ShoulderAt(1) > 0.01f) xs.push_back(half - profile.ShoulderAt(1));
        for (int k = 0; k <= profile.Lanes(); ++k) {
            const float x = -half + sl + lane * (float)k + (k > profile.lanesBackward ? median : 0.0f);
            xs.push_back(x);
            if (k == profile.lanesBackward && median > 0.01f) xs.push_back(x + median);
        }
        for (float x : xs) roadLats.push_back(latOfX(x));
        std::sort(roadLats.begin(), roadLats.end());
        roadLats.erase(std::unique(roadLats.begin(), roadLats.end(),
                                   [](float a, float b) { return std::fabs(a - b) < 0.02f; }),
                       roadLats.end());
    }
    const float roadLo = roadLats.front(), roadHi = roadLats.back();
    // The sidewalks either side: [kerb, outer] in lat, absent when width 0.
    float walkLo = roadLo, walkHi = roadHi;
    for (int side = -1; side <= 1; side += 2) {
        if (!profile.Walk(side)) continue;
        const float outer = latOfX((half + profile.sidewalkWidth) * side);
        if (outer < roadLo) walkLo = outer;
        if (outer > roadHi) walkHi = outer;
    }
    const float walkLift = profile.sidewalkHeight;
    const auto inRoadZone = [&](float l) { return l >= roadLo - 1e-3f && l <= roadHi + 1e-3f; };
    const auto inWidth = [&](float l) { return l >= walkLo - 1e-3f && l <= walkHi + 1e-3f; };

    // Faces reaching in under the new road behind the node are cut there.
    std::vector<SplitLine> cutLines;
    {
        SplitLine l;
        l.a = { node.x, node.y };
        l.n = { u.x, u.y };
        cutLines.push_back(l);
        std::vector<float> lats = roadLats;
        if (walkLo < roadLo - 0.01f) lats.push_back(walkLo);
        if (walkHi > roadHi + 0.01f) lats.push_back(walkHi);
        for (float value : lats) {
            SplitLine side;
            const Vec3 p = node + r * value;
            side.a = { p.x, p.y };
            side.n = { r.x, r.y };
            cutLines.push_back(side);
        }
    }

    // Vertices on the edge of the chosen faces stay where they are: the
    // game's faces beyond them share them.
    const auto key3 = [](const Vec3& p) {
        return std::make_tuple(std::lround(p.x * 100.0f), std::lround(p.y * 100.0f), std::lround(p.z * 100.0f));
    };
    std::set<std::tuple<long, long, long>> pinned;
    {
        std::map<std::pair<std::tuple<long, long, long>, std::tuple<long, long, long>>, int> edges;
        for (const GameFace& f : j.faces)
            for (int k = 0; k < 3; ++k) {
                auto a = key3(f.p[k]), b = key3(f.p[(k + 1) % 3]);
                if (b < a) std::swap(a, b);
                ++edges[{ a, b }];
            }
        for (const auto& [edge, count] : edges)
            if (count == 1) { pinned.insert(edge.first); pinned.insert(edge.second); }
    }

    // The crossing on the game's road: where the new road's kerb lines meet
    // it, widened by the corner radius along it.
    Vec3 g = HorizontalDirection(j.gameForward, r);
    const bool crossing = std::fabs(Dot(g, u)) < 0.9f;
    if (Dot(g, r) < 0.0f) g = g * -1.0f;
    const Vec3 gn{ -g.y, g.x, 0.0f };
    const auto ga = [&](const Vec3& q) { return (q.x - node.x) * g.x + (q.y - node.y) * g.y; };
    const auto gx = [&](const Vec3& q) { return (q.x - node.x) * gn.x + (q.y - node.y) * gn.y; };
    float boxMin = 1e30f, boxMax = -1e30f;
    for (int k = 0; k < 2; ++k) {
        const Vec3 corner = node + u * reach[k] + r * (k ? roadHi : roadLo);
        boxMin = std::min(boxMin, ga(corner) - j.fillet);
        boxMax = std::max(boxMax, ga(corner) + j.fillet);
    }
    const auto crossingPart = [&](const Vec3& q) {
        if (!crossing) return 0;
        const float a = ga(q);
        if (a >= boxMin && a <= boxMax) return 1;
        if (j.crosswalks && ((a >= boxMax && a <= boxMax + kCrosswalkDepth) ||
                             (a <= boxMin && a >= boxMin - kCrosswalkDepth)))
            return 2;
        return 0;
    };

    const std::string asphalt = "RDP_INTERSECTMIDTILE";
    const auto asphaltUv = [&](const Vec3& q) { return Vec2{ q.x / kJunctionTile, q.y / kJunctionTile }; };
    const auto fanUp = [&](MeshGroup& gr, const std::vector<MeshVertex>& ring) {
        const u32 base = (u32)gr.vertices.size();
        for (const MeshVertex& v : ring) gr.vertices.push_back(v);
        for (size_t k = 1; k + 1 < ring.size(); ++k) {
            const Vec3 n = Cross(ring[k].position - ring[0].position, ring[k + 1].position - ring[0].position);
            if (Length(n) < 1e-7f) continue;
            gr.indices.push_back(base);
            if (n.z >= 0.0f) { gr.indices.push_back(base + (u32)k); gr.indices.push_back(base + (u32)k + 1); }
            else { gr.indices.push_back(base + (u32)k + 1); gr.indices.push_back(base + (u32)k); }
        }
    };
    const float easeW = ease;
    const auto easing = [&](const Vec3& q) {
        return easeW > 0.01f ? std::clamp(1.0f - along(q) / easeW, 0.0f, 1.0f) : 0.0f;
    };

    // ---- the flat faces: cut where they reach under the road, then each
    // part what the choice makes it.
    struct Part {
        std::vector<FaceCorner> corners;
        std::vector<float> z;          // each corner's height once rebuilt
        const GameFace* face = nullptr;
        int role = 0;                  // 1 road, 2 sidewalk
        bool converted = false;        // made road where the game had something else
    };
    std::vector<Part> parts;
    for (const GameFace& f : j.faces) {
        if (std::fabs(PlanArea(f.p[0], f.p[1], f.p[2])) < 1e-3f) continue;   // standing: below
        std::vector<std::vector<FaceCorner>> polys{ { { f.p[0], f.uv[0] }, { f.p[1], f.uv[1] }, { f.p[2], f.uv[2] } } };
        bool reachesIn = false;
        for (const Vec3& q : f.p) reachesIn |= along(q) < 0.02f && lat(q) > walkLo - 0.5f && lat(q) < walkHi + 0.5f;
        if (reachesIn)
            for (const SplitLine& line : cutLines) {
                std::vector<std::vector<FaceCorner>> next;
                for (const auto& poly : polys) SplitBy(poly, line, next);
                polys = std::move(next);
            }
        // The game's road cut where the crossing and its stripes begin and
        // end, so they run straight across it.
        if (crossing && isRoad(f) && f.kind != 2) {
            std::vector<float> at = { boxMin, boxMax };
            if (j.crosswalks) { at.push_back(boxMin - kCrosswalkDepth); at.push_back(boxMax + kCrosswalkDepth); }
            for (float value : at) {
                SplitLine line;
                const Vec3 p = node + g * value;
                line.a = { p.x, p.y };
                line.n = { g.x, g.y };
                std::vector<std::vector<FaceCorner>> next;
                for (const auto& poly : polys) SplitBy(poly, line, next);
                polys = std::move(next);
            }
        }
        for (auto& poly : polys) {
            Vec3 c{};
            for (const FaceCorner& k : poly) c = c + k.p;
            c = c * (1.0f / poly.size());
            if (along(c) < 0.0f && inWidth(lat(c))) continue;     // the new road lies there
            Part part;
            part.face = &f;
            part.corners = std::move(poly);
            if (f.kind == 1) part.role = 1;
            else if (f.kind == 2) part.role = 2;
            else if (isRoad(f)) part.role = 1;
            else part.role = along(c) >= 0.0f && inRoadZone(lat(c)) ? 1 : 2;   // in the new road's way
            part.converted = part.role == 1 && !isRoad(f);
            parts.push_back(std::move(part));
        }
    }
    // Heights: road made where there was sidewalk drops to the road; the
    // sidewalk beside the new road eases to the node's height with it; the
    // node's own line is exactly the road end's.
    for (Part& part : parts) {
        for (const FaceCorner& k : part.corners) {
            float z = k.p.z;
            const bool fixed = pinned.count(key3(k.p)) != 0;
            const float a = along(k.p), l = lat(k.p);
            if (std::fabs(a) < 0.02f && inWidth(l)) {
                z = node.z + (inRoadZone(l) && part.role == 1 ? 0.0f : walkLift);
            } else if (!fixed) {
                if (part.converted) z = roadZ(k.p);
                else if (part.role == 2 && inWidth(l)) z = k.p.z + offset * easing(k.p);
            }
            part.z.push_back(z);
        }
    }
    // Drawn, and driven on.
    for (const Part& part : parts) {
        const GameFace& f = *part.face;
        Vec3 c{};
        for (const FaceCorner& k : part.corners) c = c + k.p;
        c = c * (1.0f / part.corners.size());
        const int crossPart = part.role == 1 && !part.converted ? crossingPart(c) : 0;
        const bool asphaltHere = part.converted || crossPart == 1;
        std::string texture = f.texture;
        if (asphaltHere) texture = asphalt;
        else if (crossPart == 2) texture = kCrosswalkTexture;
        std::vector<MeshVertex> ring;
        CollisionArea area;
        for (size_t i = 0; i < part.corners.size(); ++i) {
            const Vec3 q{ part.corners[i].p.x, part.corners[i].p.y, part.z[i] };
            Vec2 t = part.corners[i].uv;
            if (asphaltHere) t = asphaltUv(q);
            else if (crossPart == 2) {
                const float a = ga(q);
                const float d = a > boxMax - 1e-3f ? a - boxMax : boxMin - a;
                t = { gx(q) / kCrosswalkTile, d / kCrosswalkDepth };
            }
            ring.push_back(Vertex(q, up, t.x, t.y, color));
            area.outline.push_back(q);
        }
        fanUp(piece.Group(texture), ring);
        area.surface = part.role == 1 ? kSurfaceRoad : kSurfaceSidewalk;
        piece.areas.push_back(std::move(area));
    }

    // Kerbs: along every edge where a part meets another at another height.
    {
        const auto key2 = [](const Vec3& p) { return std::make_pair(std::lround(p.x * 100.0f), std::lround(p.y * 100.0f)); };
        struct Side { size_t part; Vec3 a, b; };   // a, b at the part's own heights
        std::map<std::pair<std::pair<long, long>, std::pair<long, long>>, std::vector<Side>> edges;
        for (size_t i = 0; i < parts.size(); ++i) {
            const Part& part = parts[i];
            for (size_t k = 0; k < part.corners.size(); ++k) {
                const size_t n = (k + 1) % part.corners.size();
                Vec3 a{ part.corners[k].p.x, part.corners[k].p.y, part.z[k] };
                Vec3 b{ part.corners[n].p.x, part.corners[n].p.y, part.z[n] };
                auto ka = key2(a), kb = key2(b);
                if (kb < ka) { std::swap(ka, kb); std::swap(a, b); }
                edges[{ ka, kb }].push_back({ i, a, b });
            }
        }
        for (const auto& [edge, sides] : edges) {
            if (sides.size() != 2) continue;
            const Side& s0 = sides[0];
            const Side& s1 = sides[1];
            const float da = s1.a.z - s0.a.z, db = s1.b.z - s0.b.z;
            if (std::max(std::fabs(da), std::fabs(db)) < 0.03f) continue;
            // The low side faces the kerb; the high side is the sidewalk on top.
            const bool firstLow = da + db > 0.0f;
            const Side& low = firstLow ? s0 : s1;
            const Side& high = firstLow ? s1 : s0;
            Vec3 lowMiddle{};
            for (const FaceCorner& k : parts[low.part].corners) lowMiddle = lowMiddle + k.p;
            lowMiddle = lowMiddle * (1.0f / parts[low.part].corners.size());
            const Vec3 m = (low.a + low.b) * 0.5f;
            Vec3 face = HorizontalDirection(Vec3{ lowMiddle.x - m.x, lowMiddle.y - m.y, 0.0f }, r);
            MeshGroup& gr = piece.Group(profile.sidewalkTexture);
            const float h = std::max(high.a.z - low.a.z, high.b.z - low.b.z);
            const float kerbU = std::clamp(h / std::max(0.5f, profile.sidewalkWidth), 0.0f, 1.0f);
            const float va = Length(Vec3{ low.a.x - node.x, low.a.y - node.y, 0.0f }) / std::max(0.5f, profile.textureLength);
            const float vb = Length(Vec3{ low.b.x - node.x, low.b.y - node.y, 0.0f }) / std::max(0.5f, profile.textureLength);
            AddQuad(gr, Vertex(low.a, face, 0, va, color), Vertex(high.a, face, kerbU, va, color),
                    Vertex(high.b, face, kerbU, vb, color), Vertex(low.b, face, 0, vb, color), face);
            CollisionWall kerb;
            kerb.points = { low.a, low.b };
            kerb.barrier = false;
            kerb.height = h;
            kerb.surface = kSurfaceSidewalk;
            piece.walls.push_back(std::move(kerb));
        }
    }

    // ---- the standing faces (the game's kerbs): as they were, unless the
    // sidewalk on top of them has become road, or the new road lies there.
    {
        const auto key2 = [](const Vec3& p) { return std::make_pair(std::lround(p.x * 100.0f), std::lround(p.y * 100.0f)); };
        std::set<std::pair<long, long>> lowered;    // plan points of road made from sidewalk
        for (const Part& part : parts)
            if (part.converted)
                for (const FaceCorner& k : part.corners) lowered.insert(key2(k.p));
        for (const GameFace& f : j.faces) {
            if (std::fabs(PlanArea(f.p[0], f.p[1], f.p[2])) >= 1e-3f) continue;
            const Vec3 middle = (f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f);
            if (along(middle) < 0.0f && inWidth(lat(middle))) continue;
            // Its top corners: the higher two.
            Vec3 c[3] = { f.p[0], f.p[1], f.p[2] };
            std::sort(c, c + 3, [](const Vec3& a, const Vec3& b) { return a.z > b.z; });
            if (lowered.count(key2(c[0])) && lowered.count(key2(c[1]))) continue;
            const Vec3 n = HorizontalDirection(Cross(f.p[1] - f.p[0], f.p[2] - f.p[0]), r);
            MeshGroup& gr = piece.Group(f.texture);
            const u32 base = (u32)gr.vertices.size();
            for (int k = 0; k < 3; ++k) gr.vertices.push_back(Vertex(f.p[k], n, f.uv[k].x, f.uv[k].y, color));
            for (u32 i : { 0u, 1u, 2u, 0u, 2u, 1u }) gr.indices.push_back(base + i);
            const float high = c[0].z, low = c[2].z;
            if (high - low <= 0.03f) continue;
            const Vec3 foot0 = c[2], foot1 = c[1].z - low < 0.03f ? c[1] : c[0];
            if (Length(Vec3{ foot1.x - foot0.x, foot1.y - foot0.y, 0.0f }) < 0.05f) continue;
            CollisionWall wall;
            wall.points = { foot0, { foot1.x, foot1.y, low } };
            wall.barrier = false;
            wall.height = high - low;
            wall.surface = kSurfaceSidewalk;
            piece.walls.push_back(std::move(wall));
        }
    }

    // ---- from the road end to the faces, where they do not reach the node:
    // its edges zipped to the nearest edge of the faces, lane edge by lane
    // edge, with the sidewalks and their kerbs.
    {
        bool reachesNode = false;
        for (const Part& part : parts)
            for (const FaceCorner& k : part.corners)
                reachesNode |= std::fabs(along(k.p)) < 0.03f && inWidth(lat(k.p));
        if (!reachesNode && !parts.empty()) {
            // The faces' open edges facing the node.
            struct Edge { Vec3 a, b; };
            std::vector<Edge> open;
            {
                const auto key2 = [](const Vec3& p) { return std::make_pair(std::lround(p.x * 100.0f), std::lround(p.y * 100.0f)); };
                std::map<std::pair<std::pair<long, long>, std::pair<long, long>>, int> count;
                for (const Part& part : parts)
                    for (size_t k = 0; k < part.corners.size(); ++k) {
                        auto a = key2(part.corners[k].p), b = key2(part.corners[(k + 1) % part.corners.size()].p);
                        if (b < a) std::swap(a, b);
                        ++count[{ a, b }];
                    }
                for (const Part& part : parts) {
                    Vec3 mid{};
                    for (const FaceCorner& k : part.corners) mid = mid + k.p;
                    mid = mid * (1.0f / part.corners.size());
                    for (size_t k = 0; k < part.corners.size(); ++k) {
                        const size_t n = (k + 1) % part.corners.size();
                        auto a = key2(part.corners[k].p), b = key2(part.corners[n].p);
                        if (b < a) std::swap(a, b);
                        if (count[{ a, b }] != 1) continue;
                        const Vec3 pa{ part.corners[k].p.x, part.corners[k].p.y, part.z[k] };
                        const Vec3 pb{ part.corners[n].p.x, part.corners[n].p.y, part.z[n] };
                        const Vec3 m = (pa + pb) * 0.5f;
                        if (along(m) > along(mid)) continue;          // faces away from the node
                        open.push_back({ pa, pb });
                    }
                }
            }
            // Across the road's width, the nearest open edge at each lat.
            std::vector<Vec3> chain;
            const auto onEdge = [&](const Edge& e, float l, Vec3& at) {
                const float la = lat(e.a), lb = lat(e.b);
                if ((l < std::min(la, lb) - 1e-4f) || (l > std::max(la, lb) + 1e-4f) || std::fabs(la - lb) < 1e-5f)
                    return false;
                const float t = (l - la) / (lb - la);
                at = e.a + (e.b - e.a) * t;
                return true;
            };
            const auto nearestAt = [&](float l, Vec3& at) {
                float best = 1e30f;
                for (const Edge& e : open) {
                    Vec3 q;
                    if (onEdge(e, l, q) && along(q) > -0.05f && along(q) < best) { best = along(q); at = q; }
                }
                return best < 1e29f;
            };
            std::vector<float> stops;
            for (const Edge& e : open)
                for (const Vec3& q : { e.a, e.b })
                    if (lat(q) > walkLo && lat(q) < walkHi) stops.push_back(lat(q));
            for (float l : roadLats) stops.push_back(l);
            stops.push_back(walkLo);
            stops.push_back(walkHi);
            std::sort(stops.begin(), stops.end());
            stops.erase(std::unique(stops.begin(), stops.end(), [](float a, float b) { return std::fabs(a - b) < 0.02f; }),
                        stops.end());
            for (float l : stops) {
                Vec3 q;
                if (nearestAt(std::clamp(l, walkLo + 1e-3f, walkHi - 1e-3f), q)) chain.push_back(q);
            }
            if (chain.size() >= 2) {
                // The road end's own points, and the zip by zone.
                const auto endPoint = [&](float l, bool walk) {
                    Vec3 p = node + r * l;
                    p.z = node.z + (walk ? walkLift : 0.0f);
                    return p;
                };
                struct Zone { float lo, hi; bool walk; };
                std::vector<Zone> zones;
                if (walkLo < roadLo - 0.01f) zones.push_back({ walkLo, roadLo, true });
                zones.push_back({ roadLo, roadHi, false });
                if (walkHi > roadHi + 0.01f) zones.push_back({ roadHi, walkHi, true });
                for (const Zone& zone : zones) {
                    std::vector<Vec3> ends, near;
                    if (zone.walk) { ends = { endPoint(zone.lo, true), endPoint(zone.hi, true) }; }
                    else for (float l : roadLats) ends.push_back(endPoint(l, false));
                    for (const Vec3& q : chain)
                        if (lat(q) >= zone.lo - 0.03f && lat(q) <= zone.hi + 0.03f) near.push_back(q);
                    if (near.size() < 2 || ends.size() < 2) continue;
                    MeshGroup& gr = piece.Group(zone.walk ? profile.sidewalkTexture : asphalt);
                    const auto uvOf = [&](const Vec3& q) {
                        if (!zone.walk) return asphaltUv(q);
                        const float across = std::min(std::fabs(lat(q) - roadLo), std::fabs(lat(q) - roadHi));
                        return Vec2{ std::clamp(across / std::max(0.01f, profile.sidewalkWidth), 0.0f, 1.0f),
                                     along(q) / std::max(0.5f, profile.textureLength) };
                    };
                    const auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                        std::vector<MeshVertex> ring;
                        for (const Vec3& q : { a, b, c }) {
                            const Vec2 t = uvOf(q);
                            ring.push_back(Vertex(q, up, t.x, t.y, color));
                        }
                        fanUp(gr, ring);
                        CollisionArea area;
                        area.outline = { a, b, c };
                        area.surface = zone.walk ? kSurfaceSidewalk : kSurfaceRoad;
                        piece.areas.push_back(std::move(area));
                    };
                    size_t i = 0, k = 0;
                    while (i + 1 < ends.size() || k + 1 < near.size()) {
                        const bool advanceEnd =
                            k + 1 >= near.size() || (i + 1 < ends.size() && lat(ends[i + 1]) <= lat(near[k + 1]));
                        if (advanceEnd) { tri(ends[i], ends[i + 1], near[k]); ++i; }
                        else { tri(ends[i], near[k + 1], near[k]); ++k; }
                    }
                    // The kerb between this sidewalk and the road, along the zip.
                    if (zone.walk && walkLift > 0.02f) {
                        const float l = zone.lo < roadLo ? zone.hi : zone.lo;
                        Vec3 farTop{}, farLow{};
                        bool found = false;
                        for (const Vec3& q : chain)
                            if (std::fabs(lat(q) - l) < 0.03f) { farTop = q; found = true; }
                        if (found) {
                            farLow = farTop;
                            farLow.z = roadZ(farTop);
                            const Vec3 nearLow = endPoint(l, false), nearTop = endPoint(l, true);
                            const Vec3 face = r * (zone.lo < roadLo ? 1.0f : -1.0f);
                            MeshGroup& kg = piece.Group(profile.sidewalkTexture);
                            AddQuad(kg, Vertex(nearLow, face, 0, 0, color), Vertex(nearTop, face, 0.1f, 0, color),
                                    Vertex(farTop, face, 0.1f, 1, color), Vertex(farLow, face, 0, 1, color), face);
                            CollisionWall kerb;
                            kerb.points = { nearLow, farLow };
                            kerb.barrier = false;
                            kerb.height = walkLift;
                            kerb.surface = kSurfaceSidewalk;
                            piece.walls.push_back(std::move(kerb));
                        }
                    }
                }
            }
        }
    }
}

// The box of a junction made into one of the game's roads, in the box's own
// terms: `a` along the game's road, `y` across it towards the new road.
void BuildGameJunction(const GameJunction& j, float mouthHalf, float branchWalk, MeshPiece& piece) {
    const float L = j.HalfLength(mouthHalf, branchWalk);
    const float half = j.halfRoad, wn = j.walkNear, wf = j.walkFar, h = j.walkHeight;
    const u32 color = Grey(0.5f);   // lit afterwards
    const Vec3 up{ 0.0f, 0.0f, 1.0f };
    const auto P = [&](float a, float y, float lift = 0.0f) { return j.At(a, y, L, lift); };
    const auto walkUv = [&](const UvMap& map, const Vec3& p, float a, float y) {
        if (map.valid) return map.At(p);
        return Vec2{ y / std::max(0.5f, wn + wf) , a / 8.0f };
    };
    const auto faceUp = [&](MeshGroup& g, const std::vector<Vec3>& fan, const std::vector<Vec2>& uvs) {
        // A fan from fan[0], each triangle turned to face up.
        const u32 base = (u32)g.vertices.size();
        for (size_t k = 0; k < fan.size(); ++k) g.vertices.push_back(Vertex(fan[k], up, uvs[k].x, uvs[k].y, color));
        for (size_t k = 1; k + 1 < fan.size(); ++k) {
            const Vec3 n = Cross(fan[k] - fan[0], fan[k + 1] - fan[0]);
            if (Length(n) < 1e-5f) continue;
            g.indices.push_back(base);
            if (n.z >= 0.0f) { g.indices.push_back(base + (u32)k); g.indices.push_back(base + (u32)k + 1); }
            else { g.indices.push_back(base + (u32)k + 1); g.indices.push_back(base + (u32)k); }
        }
    };

    // The junction surface: kerb to kerb across the game's road, and out
    // into the mouth of the new road between the curving kerbs.
    std::vector<Vec2> outline;   // (a, y)
    outline.push_back({ -L, -half });
    outline.push_back({ L, -half });
    outline.push_back({ L, half });
    std::vector<std::vector<Vec2>> corners(2);
    for (int s = 1; s >= -1; s -= 2) {
        // The kerb from the game's road edge round to the new road's edge.
        const Vec2 start{ s * L, half }, end{ s * mouthHalf, half + wn }, control{ s * mouthHalf, half };
        std::vector<Vec2>& curve = corners[s > 0 ? 0 : 1];
        const int steps = 10;
        for (int k = 0; k <= steps; ++k) {
            const float t = (float)k / steps, u = 1.0f - t;
            curve.push_back({ start.x * u * u + control.x * 2 * u * t + end.x * t * t,
                              start.y * u * u + control.y * 2 * u * t + end.y * t * t });
        }
        if (s > 0) {
            for (size_t k = 1; k < curve.size(); ++k) outline.push_back(curve[k]);
        } else {
            for (size_t k = 0; k < curve.size(); ++k) outline.push_back(curve[curve.size() - 1 - k]);
        }
    }
    {
        MeshGroup& g = piece.Group("RDP_INTERSECTMIDTILE");
        std::vector<Vec3> fan{ P(0.0f, 0.0f) };
        std::vector<Vec2> uvs{ { 0.0f, 0.0f } };
        for (const Vec2& o : outline) {
            fan.push_back(P(o.x, o.y));
            uvs.push_back({ o.y / 16.0f, o.x / 16.0f });
        }
        fan.push_back(fan[1]);
        uvs.push_back(uvs[1]);
        faceUp(g, fan, uvs);
        CollisionArea area;
        for (const Vec2& o : outline) area.outline.push_back(P(o.x, o.y));
        area.surface = kSurfaceRoad;
        piece.areas.push_back(std::move(area));
    }

    // Pedestrian crossings across the game's road, either side of the junction:
    // the game's stripes, laid a hair over the junction surface.
    if (j.crosswalks) {
        MeshGroup& g = piece.Group(kCrosswalkTexture);
        const float depth = std::min(kCrosswalkDepth, L - mouthHalf - 0.5f);
        for (int s = -1; s <= 1; s += 2) {
            if (depth < 1.0f) break;
            const float a0 = s * (L - depth), a1 = s * L;
            const Vec3 p00 = P(a0, -half, 0.03f), p01 = P(a0, half, 0.03f);
            const Vec3 p11 = P(a1, half, 0.03f), p10 = P(a1, -half, 0.03f);
            const float u0 = -half / kCrosswalkTile, u1 = half / kCrosswalkTile;
            AddQuad(g, Vertex(p00, up, u0, 0, color), Vertex(p01, up, u1, 0, color), Vertex(p11, up, u1, 1, color),
                    Vertex(p10, up, u0, 1, color), up);
        }
    }

    // The far sidewalk, straight on, and its kerb.
    if (wf > 0.01f && !j.walkTextureFar.empty()) {
        MeshGroup& g = piece.Group(j.walkTextureFar);
        const int steps = std::max(2, (int)std::ceil(2.0f * L / 2.0f));
        CollisionStrip top;
        std::vector<Vec3> kerbLine;
        for (int k = 0; k < steps; ++k) {
            const float a0 = -L + 2.0f * L * k / steps, a1 = -L + 2.0f * L * (k + 1) / steps;
            const Vec3 i0 = P(a0, -half, h), o0 = P(a0, -half - wf, h), o1 = P(a1, -half - wf, h), i1 = P(a1, -half, h);
            const Vec2 ui0 = walkUv(j.uvFar, i0, a0, 0), uo0 = walkUv(j.uvFar, o0, a0, wf);
            const Vec2 uo1 = walkUv(j.uvFar, o1, a1, wf), ui1 = walkUv(j.uvFar, i1, a1, 0);
            AddQuad(g, Vertex(i0, up, ui0.x, ui0.y, color), Vertex(o0, up, uo0.x, uo0.y, color),
                    Vertex(o1, up, uo1.x, uo1.y, color), Vertex(i1, up, ui1.x, ui1.y, color), up);
            const Vec3 face = j.Out();     // the kerb faces the road
            const Vec3 b0 = P(a0, -half), b1 = P(a1, -half);
            AddQuad(g, Vertex(b0, face, ui0.x, ui0.y, color), Vertex(i0, face, ui0.x, ui0.y + 0.02f, color),
                    Vertex(i1, face, ui1.x, ui1.y + 0.02f, color), Vertex(b1, face, ui1.x, ui1.y, color), face);
            if (k == 0) { top.left.push_back(o0); top.right.push_back(i0); kerbLine.push_back(b0); }
            top.left.push_back(o1);
            top.right.push_back(i1);
            kerbLine.push_back(b1);
        }
        top.surface = kSurfaceSidewalk;
        piece.strips.push_back(std::move(top));
        CollisionWall kerb;
        kerb.points = std::move(kerbLine);
        kerb.barrier = false;
        kerb.height = h;
        kerb.surface = kSurfaceSidewalk;
        piece.walls.push_back(std::move(kerb));
    }

    // The near corners: the sidewalk between the curving kerb and the cut's
    // straight edges, fanned from the corner of the box.
    if (wn > 0.01f && !j.walkTextureNear.empty()) {
        MeshGroup& g = piece.Group(j.walkTextureNear);
        for (int s = 1; s >= -1; s -= 2) {
            const std::vector<Vec2>& curve = corners[s > 0 ? 0 : 1];
            std::vector<Vec2> ring{ { s * L, half + wn } };            // the box's corner
            for (const Vec2& c : curve) ring.push_back(c);
            std::vector<Vec3> fan;
            std::vector<Vec2> uvs;
            CollisionArea area;
            for (const Vec2& r : ring) {
                const Vec3 p = P(r.x, r.y, h);
                fan.push_back(p);
                uvs.push_back(walkUv(j.uvNear, p, r.x, r.y - half));
                area.outline.push_back(p);
            }
            faceUp(g, fan, uvs);
            area.surface = kSurfaceSidewalk;
            piece.areas.push_back(std::move(area));
            // The kerb face along the curve, facing the road.
            CollisionWall kerb;
            kerb.barrier = false;
            kerb.height = h;
            kerb.surface = kSurfaceSidewalk;
            for (size_t k = 0; k + 1 < curve.size(); ++k) {
                const Vec3 a0 = P(curve[k].x, curve[k].y), a1 = P(curve[k + 1].x, curve[k + 1].y);
                const Vec3 t0 = P(curve[k].x, curve[k].y, h), t1 = P(curve[k + 1].x, curve[k + 1].y, h);
                const Vec3 mid = (a0 + a1) * 0.5f;
                Vec3 n = HorizontalDirection(Cross(a1 - a0, up), up);
                if (Dot(n, P(0.0f, 0.0f) - mid) < 0.0f) n = n * -1.0f;
                const Vec2 u0 = walkUv(j.uvNear, t0, curve[k].x, 0), u1 = walkUv(j.uvNear, t1, curve[k + 1].x, 0);
                AddQuad(g, Vertex(a0, n, u0.x, u0.y, color), Vertex(t0, n, u0.x, u0.y + 0.02f, color),
                        Vertex(t1, n, u1.x, u1.y + 0.02f, color), Vertex(a1, n, u1.x, u1.y, color), n);
                if (k == 0) kerb.points.push_back(a0);
                kerb.points.push_back(a1);
            }
            piece.walls.push_back(std::move(kerb));
        }
    }
}

Vec3 Quadratic(const Vec3& a, const Vec3& control, const Vec3& b, float t) {
    const float s = 1.0f - t;
    return a * (s * s) + control * (2.0f * s * t) + b * (t * t);
}

// Where two horizontal lines cross: a + da*s = b + db*u. False when parallel.
bool CrossLines(const Vec3& a, const Vec3& da, const Vec3& b, const Vec3& db, float& s, float& u) {
    const float det = da.x * (-db.y) - da.y * (-db.x);
    if (std::fabs(det) < 1e-4f) return false;
    const Vec3 d = b - a;
    s = (d.x * (-db.y) - d.y * (-db.x)) / det;
    u = (da.x * d.y - da.y * d.x) / det;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

Vec3 Curve::At(float t) const {
    const float s = 1.0f - t;
    return p0 * (s * s * s) + p1 * (3.0f * s * s * t) + p2 * (3.0f * s * t * t) + p3 * (t * t * t);
}

Vec3 Curve::Second(float t) const {
    return (p2 - p1 * 2.0f + p0) * (6.0f * (1.0f - t)) + (p3 - p2 * 2.0f + p1) * (6.0f * t);
}

Vec3 Curve::Tangent(float t) const {
    const float s = 1.0f - t;
    return (p1 - p0) * (3.0f * s * s) + (p2 - p1) * (6.0f * s * t) + (p3 - p2) * (3.0f * t * t);
}

MeshGroup& MeshPiece::Group(const std::string& texture) {
    for (MeshGroup& g : groups)
        if (g.texture == texture) return g;
    groups.push_back({});
    groups.back().texture = texture;
    return groups.back();
}

size_t MeshPiece::VertexCount() const {
    size_t n = 0;
    for (const MeshGroup& g : groups) n += g.vertices.size();
    return n;
}

Aabb MeshPiece::Bounds() const {
    Aabb box;
    for (const MeshGroup& g : groups)
        for (const MeshVertex& v : g.vertices) box.Add(v.position);
    return box;
}

// ---------------------------------------------------------------------------

const Node* Network::FindNode(u32 id) const {
    const int i = NodeIndex(id);
    return i < 0 ? nullptr : &nodes_[(size_t)i];
}
Node* Network::FindNode(u32 id) {
    const int i = NodeIndex(id);
    return i < 0 ? nullptr : &nodes_[(size_t)i];
}
const Segment* Network::FindSegment(u32 id) const {
    const int i = SegmentIndex(id);
    return i < 0 ? nullptr : &segments_[(size_t)i];
}
Segment* Network::FindSegment(u32 id) {
    const int i = SegmentIndex(id);
    return i < 0 ? nullptr : &segments_[(size_t)i];
}
int Network::NodeIndex(u32 id) const {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].id == id) return (int)i;
    return -1;
}
int Network::SegmentIndex(u32 id) const {
    for (size_t i = 0; i < segments_.size(); ++i)
        if (segments_[i].id == id) return (int)i;
    return -1;
}

u32 Network::AddNode(const Vec3& position) {
    Node node;
    node.id = nextId_++;
    node.position = position;
    nodes_.push_back(node);
    SetDirty();
    return node.id;
}

Segment* Network::AddSegment(u32 a, u32 b, const RoadProfile& profile) {
    if (a == b || !FindNode(a) || !FindNode(b)) return nullptr;
    for (const Segment& s : segments_)
        if ((s.a == a && s.b == b) || (s.a == b && s.b == a)) return nullptr;
    Segment segment;
    segment.id = nextId_++;
    segment.a = a;
    segment.b = b;
    segment.profile = profile;
    segments_.push_back(segment);
    SetDirty();
    return &segments_.back();
}

std::vector<u32> Network::SegmentsAt(u32 node) const {
    std::vector<u32> out;
    for (const Segment& s : segments_)
        if (s.a == node || s.b == node) out.push_back(s.id);
    return out;
}

void Network::RemoveNode(u32 id) {
    std::vector<u32> ends;
    for (const Segment& s : segments_)
        if (s.a == id || s.b == id) ends.push_back(s.a == id ? s.b : s.a);
    segments_.erase(std::remove_if(segments_.begin(), segments_.end(),
                                   [&](const Segment& s) { return s.a == id || s.b == id; }),
                    segments_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == id; }),
                 nodes_.end());
    cuts_.erase(std::remove_if(cuts_.begin(), cuts_.end(), [&](const GameCut& c) { return c.node == id; }),
                cuts_.end());
    gameJunctions_.erase(std::remove_if(gameJunctions_.begin(), gameJunctions_.end(),
                                        [&](const GameJunction& j) { return j.node == id; }),
                         gameJunctions_.end());
    for (u32 end : ends)
        if (SegmentsAt(end).empty())
            nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == end; }),
                         nodes_.end());
    SetDirty();
}

bool Network::EndDirection(u32 node, Vec3& into, const Segment*& segment) const {
    for (u32 id : SegmentsAt(node)) {
        const Segment* s = FindSegment(id);
        if (!s) continue;
        const Curve c = CurveOf(*s);
        const Vec3 t = s->a == node ? c.Tangent(0.0f) * -1.0f : c.Tangent(1.0f);
        into = HorizontalDirection(t);
        segment = s;
        return true;
    }
    return false;
}

bool Network::BuildFaceJunctionPiece(const FaceJunction& j, MeshPiece& piece) const {
    const Node* n = FindNode(j.node);
    Vec3 into;
    const Segment* s = nullptr;
    if (!n || !EndDirection(j.node, into, s)) return false;
    char name[64];
    std::snprintf(name, sizeof(name), "ROAD_F%u", j.node);
    piece.name = name;
    piece.node = j.node;
    if (j.refined.empty()) {
        // Leaving the node at its start, the road's right is this junction's left.
        BuildFaceJunction(j, n->position, into, s->profile, s->a == j.node ? -1.0f : 1.0f, s->Offset(), piece);
        return true;
    }
    // Refined by hand: its triangles as they came back; the flat ones are
    // driven on (road or sidewalk by texture), the standing ones are kerbs.
    for (const RefinedTriangle& t : j.refined) {
        MeshGroup& g = piece.Group(t.texture);
        const u32 base = (u32)g.vertices.size();
        for (int k = 0; k < 3; ++k) g.vertices.push_back(Vertex(t.p[k], t.n[k], t.uv[k].x, t.uv[k].y, Grey(0.5f)));
        for (u32 k = 0; k < 3; ++k) g.indices.push_back(base + k);
        const Vec3 normal = Cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        const float length = Length(normal);
        if (length < 1e-6f) continue;
        if (std::fabs(normal.z) / length > 0.3f) {
            CollisionArea area;
            area.outline = { t.p[0], t.p[1], t.p[2] };
            area.surface = IsRoadSurface(t.texture) ? kSurfaceRoad : kSurfaceSidewalk;
            piece.areas.push_back(std::move(area));
            continue;
        }
        Vec3 low = t.p[0], other = t.p[1];
        float high = std::max({ t.p[0].z, t.p[1].z, t.p[2].z });
        // The foot: the two lowest corners.
        Vec3 c[3] = { t.p[0], t.p[1], t.p[2] };
        std::sort(c, c + 3, [](const Vec3& a, const Vec3& b) { return a.z < b.z; });
        low = c[0];
        other = c[1];
        if (Length(Vec3{ other.x - low.x, other.y - low.y, 0.0f }) < 0.05f || high - low.z < 0.03f) continue;
        CollisionWall wall;
        wall.points = { low, { other.x, other.y, low.z } };
        wall.barrier = false;
        wall.height = high - low.z;
        wall.surface = kSurfaceSidewalk;
        piece.walls.push_back(std::move(wall));
    }
    return true;
}

std::vector<GameCut> Network::Cuts() const {
    std::vector<GameCut> out = cuts_;
    for (const FaceJunction& j : faceJunctions_) {
        if (!FindNode(j.node)) continue;
        for (GameCut& c : j.Cuts()) out.push_back(std::move(c));
    }
    for (const GameJunction& j : gameJunctions_) {
        float mouthHalf = 0.0f, branchWalk = 0.0f;
        MouthOf(j, mouthHalf, branchWalk);
        out.push_back(j.Cut(j.HalfLength(mouthHalf, branchWalk)));
    }
    return out;
}

void Network::MouthOf(const GameJunction& j, float& half, float& walk) const {
    half = 4.0f;
    walk = j.walkNear;
    for (u32 id : SegmentsAt(j.node)) {
        const Segment* s = FindSegment(id);
        if (!s) continue;
        half = s->profile.HalfRoad();
        walk = s->profile.sidewalkWidth > 0.01f ? s->profile.sidewalkWidth : 0.0f;
        break;
    }
}

bool Network::UpdateFollowers() {
    bool moved = false;
    // A new road leaving one of the game's roads starts at the junction's
    // mouth, square to the road.
    for (const GameJunction& j : gameJunctions_) {
        Node* node = FindNode(j.node);
        if (!node) continue;
        const Vec3 out = j.Out();
        const Vec3 at = j.At(0.0f, j.halfRoad + j.walkNear, 1.0f);
        const float heading = Degrees(std::atan2(out.y, out.x));
        if (Length(at - node->position) > 1e-4f || !node->hasHeading || std::fabs(node->heading - heading) > 1e-3f)
            moved = true;
        node->position = at;
        node->hasHeading = true;
        node->heading = heading;
    }
    for (Node& node : nodes_) {
        if (!node.follow) continue;
        const Node* leader = FindNode(node.follow);
        if (!leader) { node.follow = 0; continue; }
        const float h = Radians(leader->heading);
        const Vec3 right{ std::sin(h), -std::cos(h), 0.0f };
        const Vec3 at = leader->position + right * node.followOffset;
        if (Length(at - node.position) > 1e-4f || !node.hasHeading || node.heading != leader->heading) moved = true;
        node.position = at;
        node.hasHeading = true;
        node.heading = leader->heading;
    }
    if (moved) SetDirty();
    return moved;
}

void Network::RemoveSegment(u32 id) {
    const Segment* segment = FindSegment(id);
    if (!segment) return;
    const u32 a = segment->a, b = segment->b;
    segments_.erase(segments_.begin() + SegmentIndex(id));
    for (u32 end : { a, b })
        if (SegmentsAt(end).empty())
            nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == end; }),
                         nodes_.end());
    SetDirty();
}

void Network::MergeNodes(u32 from, u32 into) {
    if (from == into || !FindNode(from) || !FindNode(into)) return;
    for (Segment& s : segments_) {
        if (s.a == from) s.a = into;
        if (s.b == from) s.b = into;
    }
    for (GameCut& c : cuts_)
        if (c.node == from) c.node = into;
    for (GameJunction& j : gameJunctions_)
        if (j.node == from) j.node = into;
    // Loops, and second segments between the same two nodes, go.
    std::vector<Segment> kept;
    for (const Segment& s : segments_) {
        if (s.a == s.b) continue;
        const bool twin = std::any_of(kept.begin(), kept.end(), [&](const Segment& k) {
            return (k.a == s.a && k.b == s.b) || (k.a == s.b && k.b == s.a);
        });
        if (!twin) kept.push_back(s);
    }
    segments_ = std::move(kept);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == from; }),
                 nodes_.end());
    SetDirty();
}

u32 Network::SplitSegment(u32 segmentId, float t) {
    const Segment* segment = FindSegment(segmentId);
    if (!segment || t <= 0.02f || t >= 0.98f) return 0;
    const Segment copy = *segment;
    const Curve curve = CurveOf(copy);
    const Vec3 point = curve.At(t);
    const Vec3 direction = HorizontalDirection(curve.Tangent(t));

    segments_.erase(segments_.begin() + SegmentIndex(segmentId));
    const u32 middle = AddNode(point);
    Node* node = FindNode(middle);
    // The new node keeps the curve's direction, so the road does not bend.
    node->hasHeading = true;
    node->heading = Degrees(std::atan2(direction.y, direction.x));
    Segment* added = AddSegment(copy.a, middle, copy.profile);
    const u32 firstId = added ? added->id : 0;
    added = AddSegment(middle, copy.b, copy.profile);
    const u32 secondId = added ? added->id : 0;
    // Found again: adding the second may have moved the first.
    Segment* first = FindSegment(firstId);
    Segment* second = FindSegment(secondId);
    node = FindNode(middle);
    if (first) first->shift = copy.shift;
    if (second) second->shift = copy.shift;
    if (copy.shape != 0 && first && second) {
        // The two halves of the Bezier (de Casteljau), each with its own
        // fixed directions and handles: the road keeps its exact line.
        const Vec3 p01 = curve.p0 + (curve.p1 - curve.p0) * t, p12 = curve.p1 + (curve.p2 - curve.p1) * t;
        const Vec3 p23 = curve.p2 + (curve.p3 - curve.p2) * t;
        const Vec3 q0 = p01 + (p12 - p01) * t, q1 = p12 + (p23 - p12) * t;
        const auto set = [](Segment& s, const Vec3& from, const Vec3& h0, const Vec3& h1, const Vec3& to) {
            const Vec3 d0 = Horizontal(h0 - from), d1 = Horizontal(h1 - to);
            s.shape = 2;
            s.dirA = Degrees(std::atan2(d0.y, d0.x));
            s.dirB = Degrees(std::atan2(d1.y, d1.x));
            s.handleA = std::max(0.01f, Length(d0));
            s.handleB = std::max(0.01f, Length(d1));
        };
        set(*first, curve.p0, p01, q0, point);
        set(*second, point, q1, p23, curve.p3);
        node->hasHeading = false;
    }
    SetDirty();
    return middle;
}

Curve Network::CurveOf(const Segment& segment) const {
    const Node* a = FindNode(segment.a);
    const Node* b = FindNode(segment.b);
    Curve curve;
    if (!a || !b) return curve;

    // The direction a road with a fixed shape leaves `node` in.
    const auto fixedLeaving = [&](const Segment& s, const Node& node, Vec3& out) {
        const bool atA = s.a == node.id;
        if (s.shape == 1) {
            const Node* far = FindNode(atA ? s.b : s.a);
            if (!far) return false;
            out = HorizontalDirection(far->position - node.position);
            return true;
        }
        if (s.shape == 2) {
            const float d = Radians(atA ? s.dirA : s.dirB);
            out = { std::cos(d), std::sin(d), 0.0f };
            return true;
        }
        return false;
    };

    // The direction a segment leaves `node` in, towards `other`.
    auto leaving = [&](const Node& node, const Node& other) {
        const Vec3 chord = HorizontalDirection(other.position - node.position);
        Vec3 fixed;
        if (fixedLeaving(segment, node, fixed)) return fixed;
        // On from a road drawn with a fixed shape: the same tangent.
        if (!node.hasHeading) {
            const std::vector<u32> around = SegmentsAt(node.id);
            if (around.size() == 2) {
                const Segment* before = FindSegment(around[0] == segment.id ? around[1] : around[0]);
                if (before && before->shape != 0 && fixedLeaving(*before, node, fixed) && Dot(fixed, chord) < 0.2f)
                    return fixed * -1.0f;
            }
        }
        if (node.hasHeading) {
            const Vec3 h{ std::cos(Radians(node.heading)), std::sin(Radians(node.heading)), 0.0f };
            return Dot(h, chord) >= 0.0f ? h : h * -1.0f;
        }
        const std::vector<u32> around = SegmentsAt(node.id);
        if (around.size() == 2) {
            // Smooth through the node: the direction from the far end of the
            // other segment to the far end of this one.
            const Segment* otherSegment = FindSegment(around[0] == segment.id ? around[1] : around[0]);
            const Node* behind = otherSegment
                ? FindNode(otherSegment->a == node.id ? otherSegment->b : otherSegment->a) : nullptr;
            if (behind) {
                const Vec3 through = HorizontalDirection(other.position - behind->position, chord);
                if (Dot(through, chord) > -0.2f) return through;
            }
        }
        return chord;
    };

    const Vec3 da = leaving(*a, *b);
    const Vec3 db = leaving(*b, *a);
    const float span = Length(Horizontal(b->position - a->position));
    const float handle = span / 3.0f;
    const float handleA = segment.handleA > 0.0f ? segment.handleA : handle;
    const float handleB = segment.handleB > 0.0f ? segment.handleB : handle;
    const float rise = b->position.z - a->position.z;

    curve.p0 = a->position;
    curve.p3 = b->position;
    curve.p1 = a->position + da * handleA + Vec3{ 0.0f, 0.0f, segment.liftSetA ? segment.liftA : rise / 3.0f };
    curve.p2 = b->position + db * handleB + Vec3{ 0.0f, 0.0f, segment.liftSetB ? segment.liftB : -rise / 3.0f };
    return curve;
}

bool Network::NearestOnSegment(const Segment& segment, const Vec3& point, float& t, float& distance) const {
    const Curve curve = CurveOf(segment);
    distance = 1e30f;
    const int steps = 128;
    for (int i = 0; i <= steps; ++i) {
        const float ti = (float)i / steps;
        const float d = Length(Horizontal(curve.At(ti) - point));
        if (d < distance) { distance = d; t = ti; }
    }
    // Refine around the best sample.
    float lo = std::max(0.0f, t - 1.0f / steps), hi = std::min(1.0f, t + 1.0f / steps);
    for (int k = 0; k < 20; ++k) {
        const float m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
        const float d1 = Length(Horizontal(curve.At(m1) - point));
        const float d2 = Length(Horizontal(curve.At(m2) - point));
        if (d1 < d2) hi = m2; else lo = m1;
    }
    t = (lo + hi) * 0.5f;
    distance = Length(Horizontal(curve.At(t) - point));
    return true;
}

// ---------------------------------------------------------------------------

// How far `point` is outside road `o`'s cross-section (negative inside):
// across it from its nearest point, against its own span -- lanes and
// shoulders, and the sidewalks when `outer`.
float Network::SideGap(const Segment& o, const Vec3& point, bool outer) const {
    float t = 0.0f, distance = 0.0f;
    NearestOnSegment(o, point, t, distance);
    const Curve oc = CurveOf(o);
    const Vec3 q = oc.At(t);
    const Vec3 right = RightOf(HorizontalDirection(oc.Tangent(t)));
    const float l = Dot(Horizontal(point - q), right) - o.Offset();
    const float half = o.profile.HalfRoad();
    const float lo = -(outer ? o.profile.Edge(-1) : half), hi = outer ? o.profile.Edge(1) : half;
    return l > hi ? l - hi : l < lo ? lo - l : -std::min(l - lo, hi - l);
}

std::vector<MeshPiece> Network::BuildMesh(std::vector<Pole>* polesOut) const {
    std::vector<MeshPiece> pieces;
    std::vector<Pole> poles;

    // ---- junctions: how far each road is pulled back from its node --------
    struct End {
        u32   segment = 0;
        bool  atStart = true;     // the segment's node a is the junction
        Vec3  direction;          // leaving the junction, horizontal
        float angle = 0.0f;
        float trimT = 0.0f;
        Station station;          // the cross-section where the road stops
        bool  hasStation = false;
    };
    std::map<u32, Trim> trims;
    std::map<u32, std::vector<End>> junctions;

    for (const Node& node : nodes_) {
        const std::vector<u32> around = SegmentsAt(node.id);
        if (around.size() < 3) continue;

        std::vector<End> ends;
        float widest = 0.0f;
        for (u32 id : around) {
            const Segment& s = *FindSegment(id);
            const Curve curve = CurveOf(s);
            End end;
            end.segment = id;
            end.atStart = s.a == node.id;
            end.direction = HorizontalDirection(end.atStart ? curve.Tangent(0.0f) : curve.Tangent(1.0f) * -1.0f);
            end.angle = std::atan2(end.direction.y, end.direction.x);
            widest = std::max(widest, s.profile.HalfWidth());
            ends.push_back(end);
        }
        std::sort(ends.begin(), ends.end(), [](const End& x, const End& y) { return x.angle < y.angle; });

        // Far enough that neighbouring roads clear each other.
        float radius = widest * 1.1f;
        for (size_t i = 0; i < ends.size(); ++i) {
            const End& e0 = ends[i];
            const End& e1 = ends[(i + 1) % ends.size()];
            float gap = e1.angle - e0.angle;
            if (gap <= 0.0f) gap += 2.0f * kPi;
            gap = std::clamp(gap, Radians(20.0f), kPi);
            const float w = std::max(FindSegment(e0.segment)->profile.HalfWidth(),
                                     FindSegment(e1.segment)->profile.HalfWidth());
            radius = std::max(radius, w / std::tan(gap * 0.5f));
        }
        radius += 1.0f;

        for (End& end : ends) {
            const Segment& s = *FindSegment(end.segment);
            const Curve curve = CurveOf(s);
            const float span = Length(Horizontal(curve.p3 - curve.p0));
            const float r = std::min(radius, span * 0.45f);
            // The parameter where the road is `r` from the node.
            float t = 0.0f;
            const int steps = 200;
            for (int k = 1; k <= steps; ++k) {
                const float tk = (float)k / steps;
                const Vec3 p = curve.At(end.atStart ? tk : 1.0f - tk);
                if (Length(Horizontal(p - node.position)) >= r) { t = tk; break; }
                t = tk;
            }
            Trim& trim = trims[end.segment];
            if (end.atStart) trim.startT = std::max(trim.startT, t);
            else trim.endT = std::min(trim.endT, 1.0f - t);
            end.trimT = end.atStart ? t : 1.0f - t;
        }
        junctions[node.id] = std::move(ends);
    }

    // A road's shoulders end at a junction of three or more roads where one of
    // the others has none: the junction meets it at its lanes' edge.
    const auto shoulderEndsAt = [&](const Segment& s, u32 nodeId) {
        if (!s.profile.AnyShoulder() || !junctions.count(nodeId)) return false;
        for (u32 id : SegmentsAt(nodeId))
            if (const Segment* o = FindSegment(id); o && o->id != s.id && !o->profile.AnyShoulder()) return true;
        return false;
    };
    // A road's end at a through node, once it has closed what the road going
    // on there does not have: its kerbs and outer edges across its own way.
    struct EndShape { float laneLeft, laneRight, outerLeft, outerRight; };
    // On one side, what each road keeps against the other's: a shoulder goes
    // where the other has neither a shoulder nor a sidewalk there, a
    // sidewalk likewise (one meeting a shoulder stays, sinking to the road).
    struct SideRule { bool shoulderCloses, walkCloses, walkSinks; };
    const auto sideRule = [](const RoadProfile& a, int side, const RoadProfile& b, int theirs) {
        const bool bShoulder = b.ShoulderAt(theirs) > 0.01f, bWalk = b.Walk(theirs);
        SideRule rule;
        rule.shoulderCloses = a.ShoulderAt(side) > 0.01f && !bShoulder && !bWalk;
        rule.walkCloses = a.Walk(side) && !bWalk && !bShoulder;
        rule.walkSinks = a.Walk(side) && !bWalk && bShoulder;
        return rule;
    };
    const auto endShape = [&](const Segment& q, const Segment& r, bool sameWay) {
        const RoadProfile& a = q.profile;
        const RoadProfile& b = r.profile;
        const int bForward = sameWay ? b.lanesForward : b.lanesBackward;
        const int bBackward = sameWay ? b.lanesBackward : b.lanesForward;
        const float lanesLeft = (float)std::max(0, a.lanesBackward - bBackward) * a.laneWidth;
        const float lanesRight = (float)std::max(0, a.lanesForward - bForward) * a.laneWidth;
        const float median = a.MedianWidth() > 0.01f && b.MedianWidth() <= 0.01f && a.medianTaper > 0.01f
                                 ? a.MedianWidth() * 0.5f : 0.0f;
        const float half = a.HalfRoad();
        EndShape e;
        e.laneLeft = -half + a.ShoulderAt(-1) + q.Offset() + lanesLeft + median;
        e.laneRight = half - a.ShoulderAt(1) + q.Offset() - lanesRight - median;
        float outer[2];
        for (int side = -1; side <= 1; side += 2) {
            const SideRule rule = sideRule(a, side, b, sameWay ? side : -side);
            float width = 0.0f;
            if (a.ShoulderAt(side) > 0.01f && !rule.shoulderCloses) width += a.ShoulderAt(side);
            if (a.Walk(side) && !rule.walkCloses) width += a.sidewalkWidth;
            outer[side > 0 ? 1 : 0] = width;
        }
        e.outerLeft = e.laneLeft - outer[0];
        e.outerRight = e.laneRight + outer[1];
        return e;
    };

    // Where each road's walls start, per end and side (A left, A right, B
    // left, B right), for the gores between roads split apart.
    std::map<u32, std::array<float, 4>> wallSkips;

    // The ground beside the roads: each road's end rows, for the corners of
    // junctions, and the neighbours' settings it eases into.
    std::map<std::pair<u32, int>, std::vector<Vec3>> sideEnds;      // (road, end * 2 + side)
    std::map<std::pair<u32, int>, RoadProfile> sideNeighbours;

    // ---- segments ----------------------------------------------------------
    for (const Segment& s : segments_) {
        const Curve curve = CurveOf(s);
        const Trim trim = trims.count(s.id) ? trims[s.id] : Trim{};
        if (trim.endT - trim.startT < 0.01f) continue;
        std::vector<Station> stations = Stations(curve, trim.startT, trim.endT);
        if (std::fabs(s.Offset()) > 1e-4f)
            for (Station& st : stations) st.point = st.point + st.right * s.Offset();
        if (stations.size() < 2) continue;

        // The junction ends take their cross-section from here, so they meet.
        for (auto& [nodeId, ends] : junctions)
            for (End& end : ends)
                if (end.segment == s.id) {
                    end.station = end.atStart ? stations.front() : stations.back();
                    end.hasStation = true;
                }

        const float total = stations.back().distance;
        if (s.profile.bankMode != 0) {
            // Flat where the road meets a junction, ends, or runs on into a road
            // that is not banked.
            const auto eases = [&](u32 nodeId) {
                const std::vector<u32> around = SegmentsAt(nodeId);
                if (around.size() != 2) return true;
                const Segment* other = FindSegment(around[0] == s.id ? around[1] : around[0]);
                return !other || other->profile.bankMode == 0;
            };
            Bank(s.profile, curve, stations, eases(s.a), eases(s.b));
        }
        const size_t polesBefore = poles.size();
        PlacePoles(s, stations, poles);
        // A pole is solid: a post of collision round each, which the game's
        // own model does not bring when it is placed.
        {
            MeshPiece posts;
            char postsName[64];
            std::snprintf(postsName, sizeof(postsName), "ROAD_L%u", s.id);
            posts.name = postsName;
            posts.segment = s.id;
            for (size_t k = polesBefore; k < poles.size(); ++k) {
                const Vec3 c = poles[k].position;
                const float w = 0.25f;
                CollisionWall post;
                post.barrier = false;
                post.height = 6.0f;
                post.surface = kSurfaceWall;
                post.points = { { c.x - w, c.y - w, c.z }, { c.x + w, c.y - w, c.z }, { c.x + w, c.y + w, c.z },
                                { c.x - w, c.y + w, c.z }, { c.x - w, c.y - w, c.z } };
                posts.walls.push_back(std::move(post));
            }
            if (!posts.walls.empty()) pieces.push_back(std::move(posts));
        }
        PlaceWallModels(s, stations, poles);
        if (!s.profile.bridge && (s.profile.Terrain(-1) || s.profile.Terrain(1))) {
            MeshPiece side;
            char sideName[64];
            std::snprintf(sideName, sizeof(sideName), "ROAD_G%u", s.id);
            side.name = sideName;
            side.segment = s.id;
            SideContext context;
            context.ground = &ground_;
            for (int end = 0; end < 2; ++end) {
                const u32 nodeId = end ? s.b : s.a;
                const std::vector<u32> around = SegmentsAt(nodeId);
                if (around.size() != 2) continue;
                const Segment* o = FindSegment(around[0] == s.id ? around[1] : around[0]);
                if (!o || o->profile.bridge) continue;
                // Its sides as this road's: swapped when it runs the other way.
                const bool sameWay = (s.a == nodeId) != (o->a == nodeId);
                for (int si = 0; si < 2; ++si) {
                    const int theirs = sameWay ? si : 1 - si;
                    if (!o->profile.terrain[theirs].on) continue;
                    // Their settings with that side put where this road has it.
                    RoadProfile& turned = sideNeighbours[{ s.id, end * 2 + si }];
                    turned = o->profile;
                    if (!sameWay) {
                        std::swap(turned.terrain[0], turned.terrain[1]);
                        std::swap(turned.wallLeft, turned.wallRight);
                        std::swap(turned.sidewalkLeft, turned.sidewalkRight);
                    }
                    context.next[end][si] = &turned;
                }
            }
            BuildSideTerrain(s.profile, stations, groundTexture_, rockTexture_, rockMixTexture_, context, side);
            for (int end = 0; end < 2; ++end)
                for (int si = 0; si < 2; ++si) sideEnds[{ s.id, end * 2 + si }] = context.ends[end][si];
            if (side.VertexCount()) pieces.push_back(std::move(side));
        }
        if (s.profile.bridge) {
            MeshPiece pillars;
            char pillarName[64];
            std::snprintf(pillarName, sizeof(pillarName), "ROAD_P%u", s.id);
            pillars.name = pillarName;
            pillars.segment = s.id;
            PlacePillars(s, stations, ground_, modelBounds_, pillars, poles);
            if (pillars.VertexCount() || !pillars.walls.empty()) pieces.push_back(std::move(pillars));
        }
        // Crossings at the ends that meet a junction which asks for them.
        const auto crosses = [&](u32 nodeId) {
            const Node* n = FindNode(nodeId);
            if (const GameJunction* gj = FindGameJunction(nodeId)) return gj->crosswalks;   // across the mouth
            if (const FaceJunction* fj = FindFaceJunction(nodeId)) return fj->crosswalks;
            return n && n->crosswalks && junctions.count(nodeId);
        };
        const float reach = std::min(kCrosswalkDepth, total * 0.4f);
        const float crossBefore = crosses(s.a) ? reach : -1.0f;
        const float crossAfter = crosses(s.b) ? total - reach : total + 1.0f;
        // The median opens out over the last stretch before a junction.
        const float open = std::min(kCrosswalkDepth + 6.0f, total * 0.4f);
        const float medianBefore = junctions.count(s.a) ? open : -1.0f;
        const float medianAfter = junctions.count(s.b) ? total - open : total + 1.0f;
        // Where it meets a road with fewer lanes at a through node, its
        // extra lanes taper away towards that node.
        StretchTaper taper;
        taper.total = total;
        taper.length = std::min(std::max(5.0f, s.profile.taperLength), total * 0.9f);
        for (int end = 0; end < 2; ++end) {
            const u32 nodeId = end ? s.b : s.a;
            const std::vector<u32> around = SegmentsAt(nodeId);
            if (around.size() != 2) continue;
            const Segment* o = FindSegment(around[0] == s.id ? around[1] : around[0]);
            if (!o || o->id == s.id) continue;
            // The other road's lanes in this road's direction of travel.
            const bool sameWay = (s.a == nodeId) != (o->a == nodeId);
            const int oForward = sameWay ? o->profile.lanesForward : o->profile.lanesBackward;
            const int oBackward = sameWay ? o->profile.lanesBackward : o->profile.lanesForward;
            taper.lanes[end][1] = std::max(0, s.profile.lanesForward - oForward);
            taper.lanes[end][0] = std::max(0, s.profile.lanesBackward - oBackward);
            // A median the road going on does not have narrows away.
            const bool closes = s.profile.MedianWidth() > 0.01f && o->profile.MedianWidth() <= 0.01f &&
                                s.profile.medianTaper > 0.01f;
            if (closes)
                taper.medianClose[end] = std::clamp(total * s.profile.medianTaper / 100.0f, 1.0f, total);
            // Each side's shoulder and sidewalk against what the other road
            // has on the same side.
            for (int side = -1; side <= 1; side += 2) {
                const SideRule rule = sideRule(s.profile, side, o->profile, sameWay ? side : -side);
                const int k = side > 0 ? 1 : 0;
                if (rule.shoulderCloses) taper.shoulderClose[end][k] = taper.length;
                if (rule.walkCloses) taper.walkClose[end][k] = taper.length;
                if (rule.walkSinks) taper.walkSink[end][k] = taper.length;
            }
            // Each side's sidewalk and wall against the other road's on the
            // same side (in this road's direction).
            for (int side = -1; side <= 1; side += 2) {
                const int theirs = sameWay ? side : -side;
                const int k = side > 0 ? 1 : 0;
                const bool wall = s.profile.Wall(side) && s.profile.wallHeight >= 0.05f;
                const bool theirWall = o->profile.Wall(theirs) && o->profile.wallHeight >= 0.05f;
                if (wall && !theirWall) taper.wallClose[end][k] = taper.length;
            }
            // Both ends as they come out once each has closed what the other
            // lacks (across this road's way): the left kerbs meet by sliding,
            // what is left over at the right kerb and at each sidewalk's outer
            // edge is taken up by widening over the taper.
            const EndShape own = endShape(s, *o, sameWay);
            EndShape theirs = endShape(*o, s, sameWay);
            if (!sameWay)
                theirs = { -theirs.laneRight, -theirs.laneLeft, -theirs.outerRight, -theirs.outerLeft };
            // Both roads do this, so each goes half way: they meet between.
            // The lanes' left edges by sliding, their right edges by the outer
            // lane, and each side's outer edge by the shoulder or sidewalk.
            taper.slide[end] = (theirs.laneLeft - own.laneLeft) * 0.5f;
            if (std::fabs(taper.slide[end]) < 1e-4f) taper.slide[end] = 0.0f;
            const float lane = (own.laneRight + theirs.laneRight) * 0.5f - (own.laneRight + taper.slide[end]);
            if (std::fabs(lane) > 1e-3f) taper.laneStretch[end] = lane;
            const float ownOuter[2] = { own.laneLeft - own.outerLeft, own.outerRight - own.laneRight };
            const float theirOuter[2] = { theirs.laneLeft - theirs.outerLeft, theirs.outerRight - theirs.laneRight };
            for (int k = 0; k < 2; ++k) {
                const float more = (theirOuter[k] - ownOuter[k]) * 0.5f;
                if (std::fabs(more) > 1e-3f && ownOuter[k] > 0.01f) taper.outerStretch[end][k] = more;
            }
        }
        // Into a junction where another road has no shoulder: both close.
        for (int end = 0; end < 2; ++end)
            if (shoulderEndsAt(s, end ? s.b : s.a))
                taper.shoulderClose[end][0] = taper.shoulderClose[end][1] = taper.length;
        // Split off at the same node as another road (an exit and the lanes
        // going on): the shoulder on the side facing it closes towards the
        // node, so the two meet lane edge to lane edge and part in a gore.
        for (int end = 0; end < 2; ++end) {
            const Node* here = FindNode(end ? s.b : s.a);
            if (!here || !here->follow || !s.profile.AnyShoulder()) continue;
            const Station& st = end ? stations.back() : stations.front();
            for (int side = -1; side <= 1; side += 2) {
                if (s.profile.ShoulderAt(side) <= 0.01f) continue;
                const Vec3 laneEdge = st.At((s.profile.HalfRoad() - s.profile.ShoulderAt(side) + 0.05f) * side, 0.0f);
                bool facing = false;
                for (const Node& other : nodes_) {
                    if (other.follow != here->follow || other.id == here->id) continue;
                    for (u32 id : SegmentsAt(other.id)) {
                        const Segment* o = FindSegment(id);
                        if (!o) continue;
                        if (SideGap(*o, laneEdge, false) < 0.3f) facing = true;
                    }
                }
                if (facing) taper.shoulderClose[end][side > 0 ? 1 : 0] = taper.length;
            }
        }
        // Where a road splits in two (its end followed by the two roads'
        // ends, as Split Two Directions and exits leave it): each side's
        // shoulder against the road going on on that side, both ways.
        const auto rightAt = [&](const Segment& q, u32 nodeId) {
            const Curve c = CurveOf(q);
            return RightOf(HorizontalDirection(c.Tangent(q.a == nodeId ? 0.0f : 1.0f)));
        };
        for (int end = 0; end < 2; ++end) {
            const u32 nodeId = end ? s.b : s.a;
            const Node* here = FindNode(nodeId);
            if (!here) continue;
            const Vec3 ownRight = rightAt(s, nodeId);
            // This road is the one split: on each side, matched to the road
            // going on there -- what one has that the other lacks closes or
            // sinks, the lane edges slide and widen onto the branch's, and the
            // shoulder or sidewalk widens to the branch's outer edge.
            if (SegmentsAt(nodeId).size() == 1) {
                bool anyFollower = false;
                for (const Node& f : nodes_) anyFollower |= f.follow == nodeId;
                if (anyFollower) {
                    const RoadProfile& p = s.profile;
                    const float half = p.HalfRoad();
                    float laneTarget[2] = { 0.0f, 0.0f }, outerTarget[2] = { 0.0f, 0.0f };
                    bool found[2] = { false, false };
                    for (int side = -1; side <= 1; side += 2) {
                        const int k = side > 0 ? 1 : 0;
                        for (const Node& f : nodes_) {
                            if (f.follow != nodeId || found[k]) continue;
                            const float centre = Dot(Horizontal(f.position - here->position), ownRight);
                            if (centre * side <= 0.05f) continue;   // on the other side
                            for (u32 id : SegmentsAt(f.id)) {
                                const Segment* o = FindSegment(id);
                                if (!o || found[k]) continue;
                                const float same = Dot(rightAt(*o, f.id), ownRight) > 0.0f ? 1.0f : -1.0f;
                                const int outer = same * side > 0.0f ? 1 : -1;     // the branch's side facing out
                                const SideRule mine = sideRule(p, side, o->profile, outer);
                                if (mine.shoulderCloses) taper.shoulderClose[end][k] = taper.length;
                                if (mine.walkCloses) taper.walkClose[end][k] = taper.length;
                                if (mine.walkSinks) taper.walkSink[end][k] = taper.length;
                                const SideRule theirs = sideRule(o->profile, outer, p, side);
                                const RoadProfile& q = o->profile;
                                const float laneX = (q.HalfRoad() - q.ShoulderAt(outer)) * (float)outer;
                                laneTarget[k] = centre + same * (laneX + o->Offset());
                                float width = 0.0f;
                                if (q.ShoulderAt(outer) > 0.01f && !theirs.shoulderCloses) width += q.ShoulderAt(outer);
                                if (q.Walk(outer) && !theirs.walkCloses) width += q.sidewalkWidth;
                                outerTarget[k] = width;
                                found[k] = true;
                            }
                        }
                    }
                    const float ownLane[2] = { -half + p.ShoulderAt(-1) + s.Offset(), half - p.ShoulderAt(1) + s.Offset() };
                    if (found[0]) taper.slide[end] = laneTarget[0] - ownLane[0];
                    if (found[1]) {
                        const float more = laneTarget[1] - (ownLane[1] + taper.slide[end]);
                        if (std::fabs(more) > 1e-3f) taper.laneStretch[end] = more;
                    }
                    for (int k = 0; k < 2; ++k) {
                        if (!found[k]) continue;
                        const int side = k ? 1 : -1;
                        const SideRule rule{ taper.shoulderClose[end][k] > 0.0f, taper.walkClose[end][k] > 0.0f, false };
                        float own = 0.0f;
                        if (p.ShoulderAt(side) > 0.01f && !rule.shoulderCloses) own += p.ShoulderAt(side);
                        if (p.Walk(side) && !rule.walkCloses) own += p.sidewalkWidth;
                        if (own > 0.01f && std::fabs(outerTarget[k] - own) > 1e-3f) taper.outerStretch[end][k] = outerTarget[k] - own;
                    }
                }
            }
            // This road goes on from a split: its outer side against the road
            // that was split, on that side.
            if (here->follow) {
                const Node* trunkNode = FindNode(here->follow);
                if (!trunkNode) continue;
                const Vec3 d = Horizontal(here->position - trunkNode->position);
                if (Length(d) < 0.05f) continue;
                const int outer = Dot(d, ownRight) > 0.0f ? 1 : -1;
                for (u32 id : SegmentsAt(trunkNode->id)) {
                    const Segment* trunk = FindSegment(id);
                    if (!trunk) continue;
                    const int trunkSide = Dot(d, rightAt(*trunk, trunkNode->id)) > 0.0f ? 1 : -1;
                    const SideRule rule = sideRule(s.profile, outer, trunk->profile, trunkSide);
                    const int k = outer > 0 ? 1 : 0;
                    if (rule.shoulderCloses) taper.shoulderClose[end][k] = taper.length;
                    if (rule.walkCloses) taper.walkClose[end][k] = taper.length;
                    if (rule.walkSinks) taper.walkSink[end][k] = taper.length;
                }
            }
        }
        // Split off at the same node as another road (an exit and the lanes
        // going on): the walls between them start where they have parted.
        for (int end = 0; end < 2; ++end) {
            const Node* here = FindNode(end ? s.b : s.a);
            if (!here || !here->follow) continue;
            std::vector<const Segment*> siblings;
            for (const Node& other : nodes_)
                if (other.follow == here->follow && other.id != here->id)
                    for (u32 id : SegmentsAt(other.id))
                        if (const Segment* o = FindSegment(id)) siblings.push_back(o);
            if (siblings.empty()) continue;
            for (int side = -1; side <= 1; side += 2) {
                if (!s.profile.Wall(side)) continue;
                float skip = 0.0f;
                for (size_t k = 0; k < stations.size(); ++k) {
                    const Station& st = stations[end ? stations.size() - 1 - k : k];
                    const float from = end ? total - st.distance : st.distance;
                    if (from > 200.0f) break;
                    const Vec3 wallAt = st.At(s.profile.Edge(side) * side, 0.0f);
                    float gap = 1e30f;
                    for (const Segment* o : siblings) gap = std::min(gap, SideGap(*o, wallAt, true));
                    if (gap >= 0.5f) break;
                    skip = from + 0.01f;
                }
                if (skip > 0.0f) taper.wallSkip[end][side > 0 ? 1 : 0] = std::min(skip, total);
            }
        }
        // Moved by hand, per side.
        for (int end = 0; end < 2; ++end)
            for (int k = 0; k < 2; ++k) {
                const float extra = end ? (k ? s.profile.wallEndRight : s.profile.wallEndLeft)
                                        : (k ? s.profile.wallStartRight : s.profile.wallStartLeft);
                if (extra != 0.0f)
                    taper.wallSkip[end][k] = std::clamp(taper.wallSkip[end][k] + extra, 0.0f, total * 0.5f);
            }
        wallSkips[s.id] = { taper.wallSkip[0][0], taper.wallSkip[0][1], taper.wallSkip[1][0], taper.wallSkip[1][1] };
        const int count = std::max(1, (int)std::ceil(total / kPieceLength));
        size_t first = 0;
        for (int k = 0; k < count; ++k) {
            const float until = total * (float)(k + 1) / count;
            size_t last = first;
            while (last + 1 < stations.size() && (stations[last].distance < until - 1e-3f || last == first)) ++last;
            if (k == count - 1) last = stations.size() - 1;
            if (last <= first) break;
            MeshPiece piece;
            char name[64];
            std::snprintf(name, sizeof(name), "ROAD_S%u_%d", s.id, k);
            piece.name = name;
            piece.segment = s.id;
            BuildStretch(s.profile, stations, first, last, crossBefore, crossAfter, medianBefore, medianAfter, piece,
                         taper);
            pieces.push_back(std::move(piece));
            first = last;
        }
    }

    // ---- ground beside roads round junction corners ------------------------
    // Between each road and the next round the junction, the side of one
    // facing the other: the left of the first (looking out of the junction)
    // and the right of the second.
    if (!sideEnds.empty())
        for (const auto& [nodeId, ends] : junctions) {
            MeshPiece corner;
            char cornerName[64];
            std::snprintf(cornerName, sizeof(cornerName), "ROAD_GJ%u", nodeId);
            corner.name = cornerName;
            corner.node = nodeId;
            for (size_t i = 0; i < ends.size(); ++i) {
                const End& e0 = ends[i];
                const End& e1 = ends[(i + 1) % ends.size()];
                if (e0.segment == e1.segment) continue;
                // Left looking out along a road leaving from its node a is its
                // own left; leaving from node b, its right.
                const int s0 = e0.atStart ? 0 : 1, s1 = e1.atStart ? 1 : 0;
                const auto r0 = sideEnds.find({ e0.segment, (e0.atStart ? 0 : 2) + s0 });
                const auto r1 = sideEnds.find({ e1.segment, (e1.atStart ? 0 : 2) + s1 });
                if (r0 == sideEnds.end() || r1 == sideEnds.end()) continue;
                LoftCorner(r0->second, r1->second, groundTexture_, corner);
            }
            if (corner.VertexCount()) pieces.push_back(std::move(corner));
        }

    // ---- junctions made into the game's roads ---------------------------------
    for (const GameJunction& j : gameJunctions_) {
        float mouthHalf = 0.0f, branchWalk = 0.0f;
        MouthOf(j, mouthHalf, branchWalk);
        MeshPiece piece;
        char name[64];
        std::snprintf(name, sizeof(name), "ROAD_G%u", j.node);
        piece.name = name;
        piece.node = j.node;
        BuildGameJunction(j, mouthHalf, branchWalk, piece);
        pieces.push_back(std::move(piece));
    }

    // ---- junctions made of the game's faces ------------------------------------
    for (const FaceJunction& j : faceJunctions_) {
        MeshPiece piece;
        if (BuildFaceJunctionPiece(j, piece) && (piece.VertexCount() || !piece.walls.empty()))
            pieces.push_back(std::move(piece));
    }

    // ---- gores: between two roads split apart at a node ---------------------
    // From where they part to where the walls between them start, the gap
    // is paved with the game's hatched strip, a little under their edges.
    for (const Node& x : nodes_) {
        std::vector<const Node*> followers;
        for (const Node& f : nodes_)
            if (f.follow == x.id && SegmentsAt(f.id).size() == 1) followers.push_back(&f);
        if (followers.size() < 2) continue;
        std::sort(followers.begin(), followers.end(),
                  [](const Node* a, const Node* b) { return a->followOffset < b->followOffset; });
        for (size_t i = 0; i + 1 < followers.size(); ++i) {
            const Node* fa = followers[i];
            const Node* fb = followers[i + 1];
            const Segment* ra = FindSegment(SegmentsAt(fa->id).front());
            const Segment* rb = FindSegment(SegmentsAt(fb->id).front());
            if (!ra || !rb || ra->id == rb->id) continue;
            struct Side { const Segment* s; bool atStart; int side; float length; Curve curve; };
            const auto sideOf = [&](const Segment* q, const Node* f, const Node* other) {
                Side out;
                out.s = q;
                out.atStart = q->a == f->id;
                out.curve = CurveOf(*q);
                const Vec3 right = RightOf(HorizontalDirection(out.curve.Tangent(out.atStart ? 0.0f : 1.0f)));
                out.side = Dot(Horizontal(other->position - f->position), right) > 0.0f ? 1 : -1;
                float length = 0.0f;
                Vec3 previous = out.curve.At(0.0f);
                for (int k = 1; k <= 32; ++k) {
                    const Vec3 p = out.curve.At(k / 32.0f);
                    length += Length(p - previous);
                    previous = p;
                }
                out.length = std::max(1.0f, length);
                return out;
            };
            const Side a = sideOf(ra, fa, fb), b = sideOf(rb, fb, fa);
            const auto skipOf = [&](const Side& q) {
                const auto found = wallSkips.find(q.s->id);
                if (found == wallSkips.end()) return 0.0f;
                const int k = q.side > 0 ? 1 : 0;
                return found->second[(q.atStart ? 0 : 2) + k];
            };
            float reach = std::max(skipOf(a), skipOf(b));
            if (reach < 1.0f) continue;      // no walls between them: nothing to hide
            reach = std::min(reach + 3.0f, std::min(a.length, b.length) * 0.9f);   // on under the walls' ends
            // A point on a road's facing edge, `d` from the split, tucked
            // a little under its edge and surface.
            const auto edgeAt = [&](const Side& q, float d) {
                const float t = std::clamp(q.atStart ? d / q.length : 1.0f - d / q.length, 0.0f, 1.0f);
                const Vec3 right = RightOf(HorizontalDirection(q.curve.Tangent(t)));
                const RoadProfile& p = q.s->profile;
                const float len = std::min(std::max(5.0f, p.taperLength), q.length * 0.9f);
                const float tt = std::clamp(d / len, 0.0f, 1.0f);
                const float open = tt * tt * (3.0f - 2.0f * tt);
                const float edge = p.HalfRoad() - p.ShoulderAt(q.side) * (1.0f - open) - 0.15f;
                return q.curve.At(t) + right * (q.s->Offset() + edge * (float)q.side) + Vec3{ 0.0f, 0.0f, -0.02f };
            };
            MeshPiece piece;
            char name[64];
            std::snprintf(name, sizeof(name), "ROAD_W%u_%zu", x.id, i);
            piece.name = name;
            piece.node = x.id;
            MeshGroup& g = piece.Group("RDP_ROADSTRIP_MC");
            const Vec3 up{ 0.0f, 0.0f, 1.0f };
            const int steps = std::max(2, (int)std::ceil(reach / 2.0f));
            const float tile = std::max(0.5f, ra->profile.textureLength);
            Vec3 pa = edgeAt(a, 0.0f), pb = edgeAt(b, 0.0f);
            for (int k = 1; k <= steps; ++k) {
                const float d = reach * k / steps;
                const Vec3 qa = edgeAt(a, d), qb = edgeAt(b, d);
                const float v0 = reach * (k - 1) / steps / tile, v1 = d / tile;
                AddQuad(g, Vertex(pa, up, 0, v0, Grey(0.5f)), Vertex(pb, up, 1, v0, Grey(0.5f)),
                        Vertex(qb, up, 1, v1, Grey(0.5f)), Vertex(qa, up, 0, v1, Grey(0.5f)), up);
                CollisionArea area;
                area.outline = { pa, pb, qb, qa };
                area.surface = kSurfaceRoad;
                piece.areas.push_back(std::move(area));
                pa = qa;
                pb = qb;
            }
            if (piece.VertexCount()) pieces.push_back(std::move(piece));
        }
    }

    // ---- junction surfaces -------------------------------------------------
    for (const auto& [nodeId, ends] : junctions) {
        const Node& node = *FindNode(nodeId);
        if (std::any_of(ends.begin(), ends.end(), [](const End& e) { return !e.hasStation; })) continue;
        MeshPiece piece;
        char name[64];
        std::snprintf(name, sizeof(name), "ROAD_J%u", nodeId);
        piece.name = name;
        piece.node = nodeId;

        const Segment& firstSegment = *FindSegment(ends.front().segment);
        const u32 color = Grey(0.5f);   // lit afterwards (LightPieces)
        const Vec3 up{ 0.0f, 0.0f, 1.0f };

        // Each end's corners, left and right as seen leaving the junction.
        struct Corner { Vec3 left, right; float halfRoad; const RoadProfile* profile; bool wallLeft, wallRight; };
        std::vector<Corner> corners;
        for (const End& end : ends) {
            const Segment& s = *FindSegment(end.segment);
            // Kerb to kerb, at the lanes' edge on a side whose shoulder ends here.
            const bool ends = shoulderEndsAt(s, nodeId);
            const float half = s.profile.HalfRoad();
            const float halfLeft = half - (ends ? s.profile.ShoulderAt(-1) : 0.0f);
            const float halfRight = half - (ends ? s.profile.ShoulderAt(1) : 0.0f);
            // The station's own right runs along the segment; leaving the
            // junction at its far end turns it round.
            const Vec3 right = end.atStart ? end.station.right : end.station.right * -1.0f;
            Corner c;
            c.left = end.station.point - right * (end.atStart ? halfLeft : halfRight);
            c.right = end.station.point + right * (end.atStart ? halfRight : halfLeft);
            c.halfRoad = half;
            c.profile = &s.profile;
            c.wallLeft = end.atStart ? s.profile.wallLeft : s.profile.wallRight;
            c.wallRight = end.atStart ? s.profile.wallRight : s.profile.wallLeft;
            corners.push_back(c);
        }

        // The outline, counter-clockwise: across each end, then the fillet to
        // the next one.
        std::vector<Vec3> outline;
        for (size_t i = 0; i < ends.size(); ++i) {
            const size_t j = (i + 1) % ends.size();
            const Corner& c0 = corners[i];
            const Corner& c1 = corners[j];
            outline.push_back(c0.right);
            outline.push_back(c0.left);

            // The fillet from this end's left corner to the next one's right.
            float s = 0.0f, u = 0.0f;
            Vec3 control = (c0.left + c1.right) * 0.5f;
            const bool crossed = CrossLines(c0.left, ends[i].direction, c1.right, ends[j].direction, s, u);
            const bool rounded = crossed && s < 0.0f && u < 0.0f;
            if (rounded) control = c0.left + ends[i].direction * s;
            control.z = (c0.left.z + c1.right.z) * 0.5f;

            std::vector<Vec3> fillet;
            for (int k = 1; k < kFilletSteps; ++k)
                fillet.push_back(Quadratic(c0.left, control, c1.right, (float)k / kFilletSteps));
            outline.insert(outline.end(), fillet.begin(), fillet.end());

            // Sidewalk and wall round the corner.
            std::vector<Vec3> edge;
            edge.push_back(c0.left);
            edge.insert(edge.end(), fillet.begin(), fillet.end());
            edge.push_back(c1.right);
            const float walk = std::min(c0.profile->sidewalkWidth, c1.profile->sidewalkWidth);
            const float height = walk > 0.01f ? std::max(c0.profile->sidewalkHeight, c1.profile->sidewalkHeight) : 0.0f;
            std::vector<Vec3> outer = edge;
            std::vector<Vec3> outward(edge.size());
            for (size_t k = 0; k < edge.size(); ++k) {
                const Vec3 along = edge[std::min(k + 1, edge.size() - 1)] - edge[k > 0 ? k - 1 : 0];
                Vec3 n = RightOf(HorizontalDirection(along, ends[i].direction));
                if (Dot(n, Horizontal(edge[k] - node.position)) < 0.0f) n = n * -1.0f;
                outward[k] = n;
                outer[k] = edge[k] + n * walk;
            }
            float run = 0.0f;
            if (walk > 0.01f) {
                MeshGroup& g = piece.Group(c0.profile->sidewalkTexture);
                const float tile = c0.profile->textureLength;
                const float kerbU = std::clamp(height / walk, 0.0f, 1.0f);
                for (size_t k = 0; k + 1 < edge.size(); ++k) {
                    const float step = Length(edge[k + 1] - edge[k]);
                    const float v0 = run / tile, v1 = (run + step) / tile;
                    const Vec3 h{ 0.0f, 0.0f, height };
                    AddQuad(g, Vertex(edge[k] + h, up, 0, v0, color), Vertex(outer[k] + h, up, 1, v0, color),
                            Vertex(outer[k + 1] + h, up, 1, v1, color), Vertex(edge[k + 1] + h, up, 0, v1, color), up);
                    const Vec3 face = outward[k] * -1.0f;
                    AddQuad(g, Vertex(edge[k], face, 0, v0, color), Vertex(edge[k] + h, face, kerbU, v0, color),
                            Vertex(edge[k + 1] + h, face, kerbU, v1, color), Vertex(edge[k + 1], face, 0, v1, color), face);
                    run += step;
                }
            }
            if (walk > 0.01f) {
                CollisionStrip strip;
                for (size_t k = 0; k < edge.size(); k += (k + 2 < edge.size() ? 2 : 1)) {
                    strip.left.push_back(edge[k] + Vec3{ 0.0f, 0.0f, height });
                    strip.right.push_back(outer[k] + Vec3{ 0.0f, 0.0f, height });
                }
                strip.surface = kSurfaceSidewalk;
                piece.strips.push_back(strip);
                if (height > 0.02f) {
                    CollisionWall kerb;
                    kerb.points = edge;
                    kerb.barrier = false;
                    kerb.height = height;
                    kerb.surface = kSurfaceSidewalk;
                    piece.walls.push_back(kerb);
                }
            }
            if (c0.wallLeft && c1.wallRight) {
                CollisionWall wall;
                for (const Vec3& point : outer) wall.points.push_back(point + Vec3{ 0.0f, 0.0f, height });
                wall.barrier = true;
                piece.walls.push_back(wall);

                const RoadProfile& p = *c0.profile;
                MeshGroup& g = piece.Group(p.wallTexture);
                run = 0.0f;
                for (size_t k = 0; k + 1 < outer.size(); ++k) {
                    const float step = Length(outer[k + 1] - outer[k]);
                    const float v0 = run / p.wallTextureLength, v1 = (run + step) / p.wallTextureLength;
                    const Vec3 base{ 0.0f, 0.0f, height };
                    const Vec3 top{ 0.0f, 0.0f, height + p.wallHeight };
                    for (int face = -1; face <= 1; face += 2) {
                        const Vec3 n = outward[k] * (float)face;
                        AddQuad(g, Vertex(outer[k] + base, n, 0, v0, color), Vertex(outer[k] + top, n, 1, v0, color),
                                Vertex(outer[k + 1] + top, n, 1, v1, color), Vertex(outer[k + 1] + base, n, 0, v1, color), n);
                    }
                    run += step;
                }
            }
        }

        {
            CollisionArea area;
            area.outline = outline;
            area.surface = kSurfaceRoad;
            piece.areas.push_back(std::move(area));
        }

        // The surface: a fan from the node over the outline.
        MeshGroup& surface = piece.Group("RDP_INTERSECTMIDTILE");
        const u32 centre = (u32)surface.vertices.size();
        Vec3 middle = node.position;
        {
            float z = 0.0f;
            for (const Corner& c : corners) z += (c.left.z + c.right.z) * 0.5f;
            middle.z = z / (float)corners.size();
        }
        surface.vertices.push_back(Vertex(middle, up, middle.x / kJunctionTile, middle.y / kJunctionTile, color));
        for (const Vec3& p : outline)
            surface.vertices.push_back(Vertex(p, up, p.x / kJunctionTile, p.y / kJunctionTile, color));
        const u32 n = (u32)outline.size();
        for (u32 k = 0; k < n; ++k) {
            const u32 a = centre + 1 + k;
            const u32 b = centre + 1 + (k + 1) % n;
            // Counter-clockwise from above, whatever order the outline ran in.
            const Vec3 normal = Cross(surface.vertices[a].position - middle, surface.vertices[b].position - middle);
            surface.indices.push_back(centre);
            if (normal.z >= 0.0f) { surface.indices.push_back(a); surface.indices.push_back(b); }
            else { surface.indices.push_back(b); surface.indices.push_back(a); }
        }
        pieces.push_back(std::move(piece));
    }
    BuildFills(pieces);
    // The lights placed by hand, as street lights: they light what is built,
    // and their posts are placed with the rest.
    for (const Light& light : lights_) {
        Pole pole;
        pole.model = light.model;
        pole.position = light.position;
        pole.heading = light.heading;
        pole.lamp = light.Lamp();
        pole.color = light.color;
        pole.radius = light.radius;
        pole.strength = light.strength;
        poles.push_back(pole);
    }
    // Trees and props, placed with the street lights; a tree's trunk is solid.
    {
        MeshPiece trunks;
        trunks.name = "ROAD_TREES";
        for (const Prop& prop : props_) {
            Pole pole;
            pole.model = prop.model;
            pole.position = prop.position;
            pole.heading = prop.heading;
            pole.scale = prop.scale;
            pole.lamp = prop.position;
            poles.push_back(pole);
            if (!prop.tree) continue;
            const Vec3 c = prop.position;
            const float w = 0.3f * prop.scale;
            CollisionWall trunk;
            trunk.barrier = false;
            trunk.height = 4.0f * prop.scale;
            trunk.surface = kSurfaceWall;
            trunk.points = { { c.x - w, c.y - w, c.z }, { c.x + w, c.y - w, c.z }, { c.x + w, c.y + w, c.z },
                             { c.x - w, c.y + w, c.z }, { c.x - w, c.y - w, c.z } };
            trunks.walls.push_back(std::move(trunk));
        }
        if (!trunks.walls.empty()) pieces.push_back(std::move(trunks));
    }
    LightPieces(pieces, poles, [this](const MeshPiece& piece) -> const RoadProfile* {
        if (piece.segment) {
            const Segment* segment = FindSegment(piece.segment);
            return segment ? &segment->profile : nullptr;
        }
        for (const Segment& segment : segments_)
            if (segment.a == piece.node || segment.b == piece.node) return &segment.profile;
        return nullptr;
    }, light_.get());
    PlaceBuildings(poles, pieces);
    if (polesOut) *polesOut = std::move(poles);
    return pieces;
}

// ---------------------------------------------------------------------------

namespace {

std::string Token(const std::string& text) { return text.empty() ? "-" : text; }
std::string Untoken(const std::string& text) { return text == "-" ? std::string() : text; }

} // namespace

bool Network::Save(const std::wstring& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "FRSROADS 2\n";
    if (fillBetween_) out << "fill 1\n";
    out << "# node <id> <x> <y> <z> <hasHeading> <heading> <crosswalks>\n";
    out << "# segment <id> <a> <b> <lanesForward> <lanesBackward> <laneWidth> <textureLength> <laneCentre> "
           "<laneInner> <sidewalkWidth> <sidewalkHeight> <sidewalkTexture> <wallLeft> <wallRight> "
           "<wallHeight> <wallTexture> <wallTextureLength> <unused> <poleModel> <poleSpacing> <poleSides> "
           "<poleInset> <poleTurn> <ambient> <lampColor> <lampRadius> <lampStrength> <handleA> <handleB> <bridge> "
           "<deckThickness> <pillarSpacing> <pillarWidth> <deckTexture> <pillarTexture>\n";
    char line[1024];
    for (const Node& n : nodes_) {
        std::snprintf(line, sizeof(line), "node %u %.4f %.4f %.4f %d %.3f %d %u %.4f\n", n.id, n.position.x,
                      n.position.y, n.position.z, n.hasHeading ? 1 : 0, n.heading, n.crosswalks ? 1 : 0, n.follow,
                      n.followOffset);
        out << line;
    }
    for (const Segment& s : segments_) {
        const RoadProfile& p = s.profile;
        std::snprintf(line, sizeof(line),
                      "segment %u %u %u %d %d %.3f %.3f %s %s %.3f %.3f %s %d %d %.3f %s %.3f 0 %s %.3f %d %.3f %.3f "
                      "%06X %06X %.3f %.3f %.3f %.3f %d %.3f %.3f %.3f %s %s\n",
                      s.id, s.a,
                      s.b, p.lanesForward, p.lanesBackward, p.laneWidth, p.textureLength,
                      Token(p.laneCentre).c_str(), Token(p.laneInner).c_str(), p.sidewalkWidth, p.sidewalkHeight,
                      Token(p.sidewalkTexture).c_str(), p.wallLeft ? 1 : 0, p.wallRight ? 1 : 0, p.wallHeight,
                      Token(p.wallTexture).c_str(), p.wallTextureLength, Token(p.poleModel).c_str(), p.poleSpacing,
                      p.poleSides, p.poleInset, p.poleTurn, p.ambient & 0xFFFFFF, p.lampColor & 0xFFFFFF,
                      p.lampRadius, p.lampStrength, s.handleA, s.handleB, p.bridge ? 1 : 0, p.deckThickness,
                      p.pillarSpacing, p.pillarWidth, Token(p.deckTexture).c_str(), Token(p.pillarTexture).c_str());
        std::string text = line;
        if (!text.empty() && text.back() == '\n') text.pop_back();
        char banking[512];
        std::snprintf(banking, sizeof(banking), " %d %d %.3f %.3f %.3f %d %.3f %.3f %s %d %d %d %s %.3f %.3f %d %d %.3f",
                      p.keepGround ? 1 : 0, p.bankMode, p.bankMax, p.designSpeed, p.bankAngle, p.medianType,
                      p.medianWidth, p.medianHeight, Token(p.medianTexture).c_str(), p.sidewalkLeft ? 1 : 0,
                      p.sidewalkRight ? 1 : 0, p.wallStyle, Token(p.wallModel).c_str(), p.wallModelSpacing,
                      p.wallModelTurn, p.traffic ? 1 : 0, p.gps ? 1 : 0, p.speedLimit);
        out << text << banking << " " << p.pillarCount << " " << Token(p.pillarModel) << " " << p.pillarModelTurn
            << " " << (s.liftSetA ? 1 : 0) << " " << s.liftA << " " << (s.liftSetB ? 1 : 0) << " " << s.liftB << " "
            << p.taperLength << " " << s.shift << " " << p.medianTaper << " " << p.wallThickness << " " << p.shoulderType << " "
            << p.shoulderWidth << " " << Token(p.shoulderTexture) << " " << (p.shoulderLine ? 1 : 0) << " "
            << (p.groundAuto ? 2 : 0) << " " << p.shoulderSides << " " << p.wallStartLeft << " "
            << p.wallStartRight << " " << p.wallEndLeft << " " << p.wallEndRight << " " << p.zoneLeft << " "
            << p.zoneRight << " " << p.zoneDepth << " " << p.zoneSetback << " " << p.zoneGap << " " << p.zoneSeed
            << " " << p.zoneTurn << " " << s.shape << " " << s.dirA << " " << s.dirB;
        for (const RoadProfile::SideTerrain& t : p.terrain) {
            out << " " << (t.on ? 1 : 0) << " " << t.relief << " " << t.seed << " " << Token(t.texture) << " "
                << t.bands.size();
            for (const RoadProfile::TerrainBand& b : t.bands) out << " " << b.width << " " << b.rise;
        }
        for (const RoadProfile::SideTerrain& t : p.terrain)
            out << " " << (t.rock ? 1 : 0) << " " << t.rockAngle << " " << t.rockHeight << " " << (t.meetGround ? 1 : 0);
        out << "\n";
    }
    for (const Prop& p : props_)
        out << "prop " << p.id << " " << Token(p.model) << " " << p.position.x << " " << p.position.y << " "
            << p.position.z << " " << p.heading << " " << p.scale << " " << (p.tree ? 1 : 0) << "\n";
    for (const Light& l : lights_)
        out << "light " << l.id << " " << l.position.x << " " << l.position.y << " " << l.position.z << " "
            << l.height << " " << l.color << " " << l.radius << " " << l.strength << " " << Token(l.model) << " "
            << l.heading << "\n";
    for (const GameJunction& j : gameJunctions_) {
        std::snprintf(line, sizeof(line),
                      "gjunction %u %.4f %.4f %.4f %.5f %.5f %d %.4f %.4f %.4f %.4f %.4f %.4f %s %s %d",
                      j.node, j.centre.x, j.centre.y, j.centre.z, j.forward.x, j.forward.y, j.side, j.halfRoad,
                      j.walkNear, j.walkFar, j.walkHeight, j.zStart, j.zEnd, Token(j.walkTextureNear).c_str(),
                      Token(j.walkTextureFar).c_str(), j.crosswalks ? 1 : 0);
        out << line;
        for (const UvMap* map : { &j.uvNear, &j.uvFar }) {
            std::snprintf(line, sizeof(line), " %d %.4f %.4f %.6f %.6f %.6f %.6f %.6f %.6f", map->valid ? 1 : 0,
                          map->origin.x, map->origin.y, map->uv.x, map->uv.y, map->gu.x, map->gu.y, map->gv.x,
                          map->gv.y);
            out << line;
        }
        out << "\n";
    }
    for (const FaceJunction& j : faceJunctions_) {
        const UvMap& map = j.asphaltMap;
        std::snprintf(line, sizeof(line),
                      "fjunction %u %.3f %.5f %.5f %s %d %.4f %.4f %.6f %.6f %.6f %.6f %.6f %.6f %s %d\n", j.node,
                      j.fillet, j.gameForward.x, j.gameForward.y, Token(j.asphalt).c_str(), map.valid ? 1 : 0,
                      map.origin.x, map.origin.y, map.uv.x, map.uv.y, map.gu.x, map.gu.y, map.gv.x, map.gv.y,
                      Token(j.model).c_str(), j.crosswalks ? 1 : 0);
        out << line;
        for (const GameFace& f : j.faces) {
            std::snprintf(line, sizeof(line), "fface %u", j.node);
            out << line;
            for (int k = 0; k < 3; ++k) {
                std::snprintf(line, sizeof(line), " %.4f %.4f %.4f %.6f %.6f", f.p[k].x, f.p[k].y, f.p[k].z, f.uv[k].x,
                              f.uv[k].y);
                out << line;
            }
            out << " " << Token(f.texture) << " " << f.kind << "\n";
        }
        for (const RefinedTriangle& t : j.refined) {
            std::snprintf(line, sizeof(line), "frefine %u", j.node);
            out << line;
            for (int k = 0; k < 3; ++k) {
                std::snprintf(line, sizeof(line), " %.4f %.4f %.4f %.6f %.6f %.4f %.4f %.4f", t.p[k].x, t.p[k].y,
                              t.p[k].z, t.uv[k].x, t.uv[k].y, t.n[k].x, t.n[k].y, t.n[k].z);
                out << line;
            }
            out << " " << Token(t.texture) << "\n";
        }
    }
    for (const GameCut& c : cuts_) {
        std::snprintf(line, sizeof(line), "cut %u %.4f %.4f %.4f %.5f %.5f %.4f %.4f\n", c.node, c.centre.x, c.centre.y,
                      c.centre.z, c.forward.x, c.forward.y, c.halfLength, c.halfWidth);
        out << line;
    }
    return (bool)out;
}

bool Network::Load(const std::wstring& path) {
    Clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text;
    u32 highest = 0;
    while (std::getline(in, text)) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (text.empty() || text[0] == '#') continue;
        std::istringstream line(text);
        std::string kind;
        line >> kind;
        if (kind == "prop") {
            Prop p;
            std::string model;
            int tree = 0;
            if (line >> p.id >> model >> p.position.x >> p.position.y >> p.position.z >> p.heading >> p.scale >> tree) {
                p.model = Untoken(model);
                p.tree = tree != 0;
                if (!p.model.empty()) props_.push_back(p);
            }
        } else if (kind == "light") {
            Light l;
            std::string model;
            if (line >> l.id >> l.position.x >> l.position.y >> l.position.z >> l.height >> l.color >> l.radius >>
                l.strength >> model >> l.heading) {
                l.model = Untoken(model);
                lights_.push_back(l);
            }
        } else if (kind == "fill") {
            int on = 0;
            if (line >> on) fillBetween_ = on != 0;
        } else if (kind == "node") {
            Node n;
            int heading = 0;
            line >> n.id >> n.position.x >> n.position.y >> n.position.z >> heading >> n.heading;
            if (!line) continue;
            n.hasHeading = heading != 0;
            int crosswalks = 0;
            if (line >> crosswalks) n.crosswalks = crosswalks != 0;
            line >> n.follow >> n.followOffset;
            nodes_.push_back(n);
            highest = std::max(highest, n.id);
        } else if (kind == "gjunction") {
            GameJunction j;
            std::string nearTexture, farTexture;
            int crosswalks = 0;
            line >> j.node >> j.centre.x >> j.centre.y >> j.centre.z >> j.forward.x >> j.forward.y >> j.side >>
                j.halfRoad >> j.walkNear >> j.walkFar >> j.walkHeight >> j.zStart >> j.zEnd >> nearTexture >> farTexture >> crosswalks;
            for (UvMap* map : { &j.uvNear, &j.uvFar }) {
                int valid = 0;
                line >> valid >> map->origin.x >> map->origin.y >> map->uv.x >> map->uv.y >> map->gu.x >> map->gu.y >>
                    map->gv.x >> map->gv.y;
                map->valid = valid != 0;
            }
            if (!line) continue;
            j.walkTextureNear = Untoken(nearTexture);
            j.walkTextureFar = Untoken(farTexture);
            j.crosswalks = crosswalks != 0;
            gameJunctions_.push_back(j);
        } else if (kind == "fjunction") {
            FaceJunction j;
            std::string asphalt, model;
            int valid = 0;
            UvMap& map = j.asphaltMap;
            line >> j.node >> j.fillet >> j.gameForward.x >> j.gameForward.y >> asphalt >> valid >> map.origin.x >>
                map.origin.y >> map.uv.x >> map.uv.y >> map.gu.x >> map.gu.y >> map.gv.x >> map.gv.y >> model;
            if (!line) continue;
            map.valid = valid != 0;
            j.asphalt = Untoken(asphalt);
            j.model = Untoken(model);
            int crosswalks = 0;
            if (line >> crosswalks) j.crosswalks = crosswalks != 0;
            faceJunctions_.push_back(std::move(j));
        } else if (kind == "fface") {
            u32 node = 0;
            GameFace f;
            std::string texture;
            line >> node;
            for (int k = 0; k < 3; ++k) line >> f.p[k].x >> f.p[k].y >> f.p[k].z >> f.uv[k].x >> f.uv[k].y;
            line >> texture;
            if (!line) continue;
            f.texture = Untoken(texture);
            int kind = 0;
            if (line >> kind) f.kind = std::clamp(kind, 0, 2);
            for (auto it = faceJunctions_.rbegin(); it != faceJunctions_.rend(); ++it)
                if (it->node == node) { it->faces.push_back(std::move(f)); break; }
        } else if (kind == "frefine") {
            u32 node = 0;
            RefinedTriangle t;
            std::string texture;
            line >> node;
            for (int k = 0; k < 3; ++k)
                line >> t.p[k].x >> t.p[k].y >> t.p[k].z >> t.uv[k].x >> t.uv[k].y >> t.n[k].x >> t.n[k].y >> t.n[k].z;
            line >> texture;
            if (!line) continue;
            t.texture = Untoken(texture);
            for (auto it = faceJunctions_.rbegin(); it != faceJunctions_.rend(); ++it)
                if (it->node == node) { it->refined.push_back(std::move(t)); break; }
        } else if (kind == "cut") {
            GameCut c;
            line >> c.node >> c.centre.x >> c.centre.y >> c.centre.z >> c.forward.x >> c.forward.y >> c.halfLength >>
                c.halfWidth;
            if (line) cuts_.push_back(c);
        } else if (kind == "segment") {
            Segment s;
            RoadProfile& p = s.profile;
            std::string centre, inner, walk, wall;
            int left = 0, right = 0;
            float unused = 0.0f;
            line >> s.id >> s.a >> s.b >> p.lanesForward >> p.lanesBackward >> p.laneWidth >> p.textureLength >>
                centre >> inner >> p.sidewalkWidth >> p.sidewalkHeight >> walk >> left >> right >> p.wallHeight >>
                wall >> p.wallTextureLength >> unused;
            if (!line) continue;
            // Version 2: the street lights and the light they bake.
            std::string pole, ambient, lamp;
            RoadProfile defaults;
            if (line >> pole >> p.poleSpacing >> p.poleSides >> p.poleInset >> p.poleTurn >> ambient >> lamp >>
                p.lampRadius >> p.lampStrength) {
                p.poleModel = Untoken(pole);
                p.ambient = (u32)std::strtoul(ambient.c_str(), nullptr, 16) & 0xFFFFFF;
                p.lampColor = (u32)std::strtoul(lamp.c_str(), nullptr, 16) & 0xFFFFFF;
                float handleA = 0.0f, handleB = 0.0f;
                if (line >> handleA >> handleB) {
                    s.handleA = std::max(0.0f, handleA);
                    s.handleB = std::max(0.0f, handleB);
                }
                int bridge = 0;
                std::string deck, pillar;
                if (line >> bridge >> p.deckThickness >> p.pillarSpacing >> p.pillarWidth >> deck >> pillar) {
                    p.bridge = bridge != 0;
                    if (!Untoken(deck).empty()) p.deckTexture = Untoken(deck);
                    if (!Untoken(pillar).empty()) p.pillarTexture = Untoken(pillar);
                    int keep = 0;
                    if (line >> keep) p.keepGround = keep != 0;
                    int bankMode = 0;
                    if (line >> bankMode >> p.bankMax >> p.designSpeed >> p.bankAngle) p.bankMode = std::clamp(bankMode, 0, 2);
                    std::string medianTexture, wallModel;
                    int walkLeft = 1, walkRight = 1;
                    if (line >> p.medianType >> p.medianWidth >> p.medianHeight >> medianTexture >> walkLeft >>
                        walkRight >> p.wallStyle >> wallModel >> p.wallModelSpacing >> p.wallModelTurn) {
                        p.medianType = std::clamp(p.medianType, 0, 4);
                        p.medianTexture = Untoken(medianTexture);
                        p.sidewalkLeft = walkLeft != 0;
                        p.sidewalkRight = walkRight != 0;
                        p.wallStyle = std::clamp(p.wallStyle, 0, 2);
                        p.wallModel = Untoken(wallModel);
                        int traffic = 1, gps = 1;
                        if (line >> traffic >> gps >> p.speedLimit) {
                            p.traffic = traffic != 0;
                            p.gps = gps != 0;
                            int pillars = 0;
                            if (line >> pillars) p.pillarCount = std::clamp(pillars, 0, 16);
                            std::string pillarModel;
                            if (line >> pillarModel >> p.pillarModelTurn) p.pillarModel = Untoken(pillarModel);
                            int setA = 0, setB = 0;
                            if (line >> setA >> s.liftA >> setB >> s.liftB) {
                                s.liftSetA = setA != 0;
                                s.liftSetB = setB != 0;
                                float taperLength = 0.0f;
                                if (line >> taperLength) p.taperLength = std::clamp(taperLength, 5.0f, 500.0f);
                                float shift = 0.0f;
                                if (line >> shift) s.shift = std::clamp(shift, -50.0f, 50.0f);
                                float medianTaper = 0.0f;
                                if (line >> medianTaper) p.medianTaper = std::clamp(medianTaper, 0.0f, 100.0f);
                                float wallThickness = 0.0f;
                                if (line >> wallThickness) p.wallThickness = std::clamp(wallThickness, 0.0f, 5.0f);
                                std::string shoulderTexture;
                                if (line >> p.shoulderType >> p.shoulderWidth >> shoulderTexture) {
                                    p.shoulderType = std::clamp(p.shoulderType, 0, 2);
                                    p.shoulderWidth = std::clamp(p.shoulderWidth, 0.2f, 10.0f);
                                    p.shoulderTexture = Untoken(shoulderTexture);
                                    int shoulderLine = 1;
                                    if (line >> shoulderLine) p.shoulderLine = shoulderLine != 0;
                                    int groundAuto = 0;   // 2 is on: the 1 written while it was on by default reads as off
                                    if (line >> groundAuto) p.groundAuto = groundAuto == 2;
                                    int sides = 0;
                                    if (line >> sides) p.shoulderSides = std::clamp(sides, 0, 2);
                                    float startLeft = 0.0f, startRight = 0.0f;
                                    if (line >> startLeft >> startRight) {
                                        p.wallStartLeft = std::clamp(startLeft, -500.0f, 500.0f);
                                        p.wallStartRight = std::clamp(startRight, -500.0f, 500.0f);
                                        float endLeft = 0.0f, endRight = 0.0f;
                                        if (line >> endLeft >> endRight) {
                                            p.wallEndLeft = std::clamp(endLeft, -500.0f, 500.0f);
                                            p.wallEndRight = std::clamp(endRight, -500.0f, 500.0f);
                                            int zl = 0, zr = 0, seed = 0, turn = 0;
                                            float depth = 30.0f, setback = 1.5f, gap = 2.0f;
                                            if (line >> zl >> zr >> depth >> setback >> gap >> seed >> turn) {
                                                p.zoneLeft = std::clamp(zl, 0, kZoneKinds - 1);
                                                p.zoneRight = std::clamp(zr, 0, kZoneKinds - 1);
                                                p.zoneDepth = std::clamp(depth, 5.0f, 300.0f);
                                                p.zoneSetback = std::clamp(setback, -10.0f, 100.0f);
                                                p.zoneGap = std::clamp(gap, 0.0f, 100.0f);
                                                p.zoneSeed = seed;
                                                p.zoneTurn = ((turn % 360) + 360) % 360;
                                                int shape = 0;
                                                float dirA = 0.0f, dirB = 0.0f;
                                                if (line >> shape >> dirA >> dirB) {
                                                    s.shape = std::clamp(shape, 0, 2);
                                                    s.dirA = dirA;
                                                    s.dirB = dirB;
                                                    for (RoadProfile::SideTerrain& t : p.terrain) {
                                                        int on = 0, seed = 0;
                                                        float relief = 1.5f;
                                                        size_t count = 0;
                                                        std::string texture;
                                                        if (!(line >> on >> relief >> seed >> texture >> count)) break;
                                                        t.on = on != 0;
                                                        t.relief = std::clamp(relief, 0.0f, 50.0f);
                                                        t.seed = seed;
                                                        t.texture = Untoken(texture);
                                                        t.bands.clear();
                                                        for (size_t k = 0; k < count && k < 16; ++k) {
                                                            RoadProfile::TerrainBand b;
                                                            if (!(line >> b.width >> b.rise)) break;
                                                            b.width = std::clamp(b.width, 0.5f, 300.0f);
                                                            b.rise = std::clamp(b.rise, -200.0f, 200.0f);
                                                            t.bands.push_back(b);
                                                        }
                                                        if (t.bands.empty()) t.bands.push_back({});
                                                    }
                                                    for (RoadProfile::SideTerrain& t : p.terrain) {
                                                        int rock = 1;
                                                        float angle = 32.0f, rockHeight = 3.0f;
                                                        int meet = 1;
                                                        if (!(line >> rock >> angle >> rockHeight >> meet)) break;
                                                        t.meetGround = meet != 0;
                                                        t.rock = rock != 0;
                                                        t.rockAngle = std::clamp(angle, 1.0f, 89.0f);
                                                        t.rockHeight = std::clamp(rockHeight, 0.0f, 200.0f);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // Version 1: no street lights, the default light.
                p.poleModel.clear();
                p.poleSpacing = defaults.poleSpacing;
                p.poleSides = defaults.poleSides;
                p.poleInset = defaults.poleInset;
                p.poleTurn = defaults.poleTurn;
                p.lampRadius = defaults.lampRadius;
                p.lampStrength = defaults.lampStrength;
            }
            p.laneCentre = Untoken(centre);
            p.laneInner = Untoken(inner);
            p.sidewalkTexture = Untoken(walk);
            p.wallTexture = Untoken(wall);
            p.wallLeft = left != 0;
            p.wallRight = right != 0;
            segments_.push_back(s);
            highest = std::max(highest, s.id);
        }
    }
    // Drop segments whose nodes are gone.
    segments_.erase(std::remove_if(segments_.begin(), segments_.end(),
                                   [&](const Segment& s) { return !FindNode(s.a) || !FindNode(s.b); }),
                    segments_.end());
    for (const Light& l : lights_) highest = std::max(highest, l.id);
    for (const Prop& p : props_) highest = std::max(highest, p.id);
    nextId_ = highest + 1;
    dirty_ = true;
    LOG_INFO("roads: %zu nodes, %zu segments read from %S", nodes_.size(), segments_.size(), path.c_str());
    return true;
}

// ---------------------------------------------------------------------------

void ApplyLighting(std::vector<MeshPiece>& pieces, const std::vector<Pole>& poles, u32 ambient,
                   const Network::LightSources* sources) {
    RoadProfile profile;
    profile.ambient = ambient;
    LightPieces(pieces, poles, [&](const MeshPiece&) { return &profile; }, sources);
}

void RoadField::Build(const Network& network, const std::function<bool(const Segment&)>& include) {
    samples_.clear();
    cells_.clear();
    widest_ = 0.0f;
    Aabb box;
    for (const Segment& segment : network.Segments()) {
        if (include && !include(segment)) continue;
        const Curve curve = network.CurveOf(segment);
        const float span = Length(curve.p3 - curve.p0) + Length(curve.p1 - curve.p0) + Length(curve.p2 - curve.p3);
        const int steps = std::clamp((int)std::ceil(span / 1.5f), 2, 2000);
        Vec3 previous = curve.At(0.0f);
        for (int i = 1; i <= steps; ++i) {
            const Vec3 p = curve.At((float)i / steps);
            Sample sample{ previous, p, segment.profile.HalfWidth() };
            if (segment.profile.bankMode != 0) {
                const float tilt = std::sin(Radians(std::clamp(segment.profile.bankMode == 2
                                                                   ? std::fabs(segment.profile.bankAngle)
                                                                   : segment.profile.bankMax, 0.0f, 45.0f)));
                sample.a.z -= sample.half * tilt;
                sample.b.z -= sample.half * tilt;
            }
            samples_.push_back(sample);
            box.Add(previous);
            box.Add(p);
            previous = p;
        }
        widest_ = std::max(widest_, segment.profile.HalfWidth());
    }
    // Junctions: the node itself, as wide as its widest road.
    for (const Node& node : network.Nodes()) {
        const std::vector<u32> around = network.SegmentsAt(node.id);
        if (around.size() < 3) continue;
        float half = 0.0f;
        bool counted = false;
        for (u32 id : around) {
            const Segment& segment = *network.FindSegment(id);
            if (include && !include(segment)) continue;
            counted = true;
            half = std::max(half, segment.profile.HalfWidth());
        }
        if (!counted) continue;
        samples_.push_back({ node.position, node.position, half * 1.4f });
    }
    if (samples_.empty()) return;
    minX_ = box.min.x - 1.0f;
    minY_ = box.min.y - 1.0f;
    columns_ = std::max(1, (int)std::ceil((box.max.x - minX_ + 1.0f) / cellSize_));
    rows_ = std::max(1, (int)std::ceil((box.max.y - minY_ + 1.0f) / cellSize_));
    cells_.assign((size_t)columns_ * rows_, {});
    for (u32 i = 0; i < samples_.size(); ++i) {
        const Sample& s = samples_[i];
        const int x0 = std::clamp((int)((std::min(s.a.x, s.b.x) - minX_) / cellSize_), 0, columns_ - 1);
        const int x1 = std::clamp((int)((std::max(s.a.x, s.b.x) - minX_) / cellSize_), 0, columns_ - 1);
        const int y0 = std::clamp((int)((std::min(s.a.y, s.b.y) - minY_) / cellSize_), 0, rows_ - 1);
        const int y1 = std::clamp((int)((std::max(s.a.y, s.b.y) - minY_) / cellSize_), 0, rows_ - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells_[(size_t)y * columns_ + x].push_back(i);
    }
}

bool RoadField::Query(float x, float y, float reach, float& height, float& outside) const {
    if (samples_.empty()) return false;
    const float look = reach + widest_ * 1.4f;
    const int x0 = std::clamp((int)((x - look - minX_) / cellSize_), 0, columns_ - 1);
    const int x1 = std::clamp((int)((x + look - minX_) / cellSize_), 0, columns_ - 1);
    const int y0 = std::clamp((int)((y - look - minY_) / cellSize_), 0, rows_ - 1);
    const int y1 = std::clamp((int)((y + look - minY_) / cellSize_), 0, rows_ - 1);
    if (x + look < minX_ || y + look < minY_ || x - look > minX_ + columns_ * cellSize_ ||
        y - look > minY_ + rows_ * cellSize_)
        return false;
    bool found = false;
    float best = reach;
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
            for (u32 index : cells_[(size_t)cy * columns_ + cx]) {
                const Sample& s = samples_[index];
                const float dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
                const float length2 = dx * dx + dy * dy;
                float t = length2 > 1e-8f ? ((x - s.a.x) * dx + (y - s.a.y) * dy) / length2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float px = s.a.x + dx * t, py = s.a.y + dy * t;
                const float d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py)) - s.half;
                if (d < best) {
                    best = d;
                    height = s.a.z + (s.b.z - s.a.z) * t;
                    found = true;
                }
            }
    outside = best;
    return found;
}

} // namespace roads

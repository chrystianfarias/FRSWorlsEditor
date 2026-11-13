#include "roads/RoadNetwork.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
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
void BuildStretch(const RoadProfile& p, const std::vector<Station>& stations, size_t first, size_t last,
                  float crossBefore, float crossAfter, float medianBefore, float medianAfter, MeshPiece& piece) {
    // A bridge's deck: its underside and its two edges.
    if (p.bridge && p.deckThickness > 0.01f) {
        const u32 grey = Grey(0.5f);
        MeshGroup& g = piece.Group(p.deckTexture);
        for (size_t i = first; i < last; ++i) {
            const Station& s0 = stations[i];
            const Station& s1 = stations[i + 1];
            const auto at = [](const Station& s, float lateral, float height) { return s.At(lateral, height); };
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

    auto at = [&](const Station& s, float lateral, float height) { return s.At(lateral, height); };
    // Where a lane starts across the road: the backward lanes from the left
    // edge, then the median, then the forward lanes.
    const auto laneStart = [&](size_t k) {
        return -half + lane * (float)k + ((int)k >= p.lanesBackward ? median : 0.0f);
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
                // Raised: a top and a kerb or barrier face either side.
                const float h = p.medianType == 4 ? std::max(0.3f, p.medianHeight) : p.medianHeight;
                MeshGroup& g = piece.Group(p.MedianTexture());
                const float topW = p.medianType == 4 ? std::min(0.3f, median * 0.5f) : median;
                const float t0 = m0 + (median - topW) * 0.5f, t1 = t0 + topW;
                AddQuad(g, Vertex(at(s0, t0, h), up, 0, v0, color), Vertex(at(s0, t1, h), up, 1, v0, color),
                        Vertex(at(s1, t1, h), up, 1, v1, color), Vertex(at(s1, t0, h), up, 0, v1, color), up);
                for (int side = -1; side <= 1; side += 2) {
                    const float foot = side < 0 ? m0 : m1;
                    const float top = side < 0 ? t0 : t1;
                    const Vec3 n = s0.right * (float)side;
                    const float uTop = p.medianType == 4 ? 1.0f : std::clamp(h / std::max(median, 0.01f), 0.0f, 1.0f);
                    AddQuad(g, Vertex(at(s0, foot, 0), n, 0, v0, color), Vertex(at(s0, top, h), n, uTop, v0, color),
                            Vertex(at(s1, top, h), n, uTop, v1, color), Vertex(at(s1, foot, 0), n, 0, v1, color), n);
                }
            }
        }

        // Sidewalks: the top, and the kerb face towards the road.
        for (int side = -1; side <= 1; side += 2) {
            if (!p.Walk(side)) continue;
            MeshGroup& g = piece.Group(p.sidewalkTexture);
            const float h = p.sidewalkHeight;
            const float v0 = s0.distance / p.textureLength;
            const float v1 = s1.distance / p.textureLength;
            const float kerbU = std::clamp(h / std::max(p.sidewalkWidth, 0.01f), 0.0f, 1.0f);
            const float inner = half * side;
            const float outer = (half + p.sidewalkWidth) * side;
            const float uIn = side < 0 ? 1.0f : 0.0f;
            const float uOut = 1.0f - uIn;
            AddQuad(g, Vertex(at(s0, inner, h), up, uIn, v0, color), Vertex(at(s0, outer, h), up, uOut, v0, color),
                    Vertex(at(s1, outer, h), up, uOut, v1, color), Vertex(at(s1, inner, h), up, uIn, v1, color), up);
            const Vec3 face0 = s0.right * (float)-side;
            AddQuad(g, Vertex(at(s0, inner, 0), face0, 0, v0, color), Vertex(at(s0, inner, h), face0, kerbU, v0, color),
                    Vertex(at(s1, inner, h), face0, kerbU, v1, color), Vertex(at(s1, inner, 0), face0, 0, v1, color),
                    face0);
        }

        // Walls on the outer edges: flat (faced both ways), a guardrail, or
        // -- for a model -- nothing here (the model is stood along the edge).
        for (int side = -1; side <= 1; side += 2) {
            if (!p.Wall(side) || p.wallHeight < 0.05f || p.wallStyle == 2) continue;
            MeshGroup& g = piece.Group(p.wallTexture);
            const float x = p.Edge(side) * side;
            const float base = p.Walk(side) ? p.sidewalkHeight : 0.0f;
            const float v0 = s0.distance / p.wallTextureLength;
            const float v1 = s1.distance / p.wallTextureLength;
            if (p.wallStyle == 0) {
                const float top = base + p.wallHeight;
                for (int face = -1; face <= 1; face += 2) {
                    const Vec3 n = s0.right * (float)face;
                    AddQuad(g, Vertex(at(s0, x, base), n, 0, v0, color), Vertex(at(s0, x, top), n, 1, v0, color),
                            Vertex(at(s1, x, top), n, 1, v1, color), Vertex(at(s1, x, base), n, 0, v1, color), n);
                }
                continue;
            }
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
        CollisionStrip walk;
        walk.left = edge(side < 0 ? -p.Edge(-1) : half, p.sidewalkHeight);
        walk.right = edge(side < 0 ? -half : p.Edge(1), p.sidewalkHeight);
        walk.surface = kSurfaceSidewalk;
        piece.strips.push_back(walk);
        CollisionWall kerb;
        kerb.points = edge(half * side, 0.0f);
        kerb.barrier = false;
        kerb.height = p.sidewalkHeight;
        kerb.surface = kSurfaceSidewalk;
        if (p.sidewalkHeight > 0.02f) piece.walls.push_back(kerb);
    }
    for (int side = -1; side <= 1; side += 2) {
        if (!p.Wall(side) || p.wallHeight < 0.05f) continue;
        CollisionWall wall;
        wall.points = edge(p.Edge(side) * side, p.Walk(side) ? p.sidewalkHeight : 0.0f);
        wall.barrier = true;
        piece.walls.push_back(wall);
    }
    // A raised median: its top, and a kerb -- or for a barrier, a wall nothing
    // gets over -- along both sides, where it is not opened for a junction.
    if (median > 0.01f && p.medianType >= 2) {
        std::vector<std::vector<size_t>> runs(1);
        for (size_t i : picks) {
            const Station& s = stations[i];
            const bool open = s.distance < medianBefore - 0.01f || s.distance > medianAfter + 0.01f;
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
                  const Network::GroundQuery& ground, MeshPiece& piece) {
    const RoadProfile& p = segment.profile;
    if (!p.bridge || p.pillarSpacing < 2.0f || stations.size() < 2) return;
    const float total = stations.back().distance;
    const float w = std::max(0.3f, p.pillarWidth) * 0.5f;
    const u32 grey = Grey(0.5f);
    std::vector<float> laterals = { 0.0f };
    if (p.HalfWidth() > 7.0f) laterals = { -p.HalfWidth() * 0.55f, p.HalfWidth() * 0.55f };
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
            const float top = centre.z - p.deckThickness;
            float bottom = top - 30.0f;
            float found = 0.0f;
            if (ground && ground(centre.x, centre.y, top - 0.5f, found)) bottom = found - 1.0f;
            if (top - bottom < 0.5f) continue;
            const Vec3 c[4] = { centre - forward * w - right * w, centre + forward * w - right * w,
                                centre + forward * w + right * w, centre - forward * w + right * w };
            MeshGroup& g = piece.Group(p.pillarTexture);
            CollisionWall wall;
            wall.barrier = false;
            wall.height = top - bottom;
            wall.surface = kSurfaceWall;
            for (int k = 0; k < 4; ++k) {
                const Vec3 a = c[k], b = c[(k + 1) % 4];
                const Vec3 n = HorizontalDirection(Cross(b - a, Vec3{ 0, 0, 1 }), right);
                const float height = (top - bottom) / 4.0f;
                AddQuad(g, Vertex({ a.x, a.y, bottom }, n, 0, 0, grey), Vertex({ a.x, a.y, top }, n, 0, height, grey),
                        Vertex({ b.x, b.y, top }, n, 1, height, grey), Vertex({ b.x, b.y, bottom }, n, 1, 0, grey), n);
                wall.points.push_back({ a.x, a.y, bottom });
            }
            wall.points.push_back(wall.points.front());
            piece.walls.push_back(std::move(wall));
        }
    }
}

// Street lights along a stretch of road, every `poleSpacing` from half a
// spacing in, on the sides the profile asks for.
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

std::vector<GameCut> Network::Cuts() const {
    std::vector<GameCut> out = cuts_;
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
    AddSegment(copy.a, middle, copy.profile);
    AddSegment(middle, copy.b, copy.profile);
    SetDirty();
    return middle;
}

Curve Network::CurveOf(const Segment& segment) const {
    const Node* a = FindNode(segment.a);
    const Node* b = FindNode(segment.b);
    Curve curve;
    if (!a || !b) return curve;

    // The direction a segment leaves `node` in, towards `other`.
    auto leaving = [&](const Node& node, const Node& other) {
        const Vec3 chord = HorizontalDirection(other.position - node.position);
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
    curve.p1 = a->position + da * handleA + Vec3{ 0.0f, 0.0f, rise / 3.0f };
    curve.p2 = b->position + db * handleB - Vec3{ 0.0f, 0.0f, rise / 3.0f };
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

    // ---- segments ----------------------------------------------------------
    for (const Segment& s : segments_) {
        const Curve curve = CurveOf(s);
        const Trim trim = trims.count(s.id) ? trims[s.id] : Trim{};
        if (trim.endT - trim.startT < 0.01f) continue;
        std::vector<Station> stations = Stations(curve, trim.startT, trim.endT);
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
        if (s.profile.bridge) {
            MeshPiece pillars;
            char pillarName[64];
            std::snprintf(pillarName, sizeof(pillarName), "ROAD_P%u", s.id);
            pillars.name = pillarName;
            pillars.segment = s.id;
            PlacePillars(s, stations, ground_, pillars);
            if (pillars.VertexCount()) pieces.push_back(std::move(pillars));
        }
        // Crossings at the ends that meet a junction which asks for them.
        const auto crosses = [&](u32 nodeId) {
            const Node* n = FindNode(nodeId);
            return n && n->crosswalks && junctions.count(nodeId);
        };
        const float reach = std::min(kCrosswalkDepth, total * 0.4f);
        const float crossBefore = crosses(s.a) ? reach : -1.0f;
        const float crossAfter = crosses(s.b) ? total - reach : total + 1.0f;
        // The median opens out over the last stretch before a junction.
        const float open = std::min(kCrosswalkDepth + 6.0f, total * 0.4f);
        const float medianBefore = junctions.count(s.a) ? open : -1.0f;
        const float medianAfter = junctions.count(s.b) ? total - open : total + 1.0f;
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
            BuildStretch(s.profile, stations, first, last, crossBefore, crossAfter, medianBefore, medianAfter, piece);
            pieces.push_back(std::move(piece));
            first = last;
        }
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
            const float half = s.profile.HalfRoad();
            // The station's own right runs along the segment; leaving the
            // junction at its far end turns it round.
            const Vec3 right = end.atStart ? end.station.right : end.station.right * -1.0f;
            Corner c;
            c.left = end.station.point - right * half;
            c.right = end.station.point + right * half;
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
    LightPieces(pieces, poles, [this](const MeshPiece& piece) -> const RoadProfile* {
        if (piece.segment) {
            const Segment* segment = FindSegment(piece.segment);
            return segment ? &segment->profile : nullptr;
        }
        for (const Segment& segment : segments_)
            if (segment.a == piece.node || segment.b == piece.node) return &segment.profile;
        return nullptr;
    }, light_.get());
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
        out << text << banking << "\n";
    }
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
        if (kind == "node") {
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

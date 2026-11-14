#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// A stretch of one of the game's own roads that a procedural road replaces:
// a rectangle in plan, centred on the road and lined up with it. When the
// roads are baked, the game's ground meshes and collision inside it are taken
// out -- triangles across its edge are cut exactly on it, so the procedural
// road's straight ends meet what is left. Cutting again finds nothing more to
// take, so a cut can stay in the network after it was baked.
namespace roads {

struct GameCut {
    u32   node = 0;           // the junction node it was made for
    Vec3  centre;
    Vec3  forward{ 1, 0, 0 }; // along the road, horizontal, unit
    float halfLength = 0.0f;
    float halfWidth = 0.0f;
    // Or any convex polygon in plan, corners counter-clockwise, held to a band
    // of heights: one of the game's own triangles taken out, so a junction
    // made of the faces chosen cuts exactly on the game's own edges.
    std::vector<Vec2> corners;
    float zMin = -1e30f, zMax = 1e30f;

    int Sides() const { return corners.empty() ? 4 : (int)corners.size(); }
    bool InHeight(float z, float margin = 0.0f) const { return z >= zMin - margin && z <= zMax + margin; }

    Vec3 Right() const { return { forward.y, -forward.x, 0.0f }; }
    // Signed distances past each side; all <= 0 inside.
    float Past(int side, const Vec3& p) const {
        if (!corners.empty()) {
            const Vec2& a = corners[(size_t)side];
            const Vec2& b = corners[((size_t)side + 1) % corners.size()];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1e-6f) return -1e30f;
            return ((p.x - a.x) * dy - (p.y - a.y) * dx) / length;
        }
        const Vec3 d{ p.x - centre.x, p.y - centre.y, 0.0f };
        switch (side) {
            case 0:  return Dot(d, forward) - halfLength;
            case 1:  return -Dot(d, forward) - halfLength;
            case 2:  return Dot(d, Right()) - halfWidth;
            default: return -Dot(d, Right()) - halfWidth;
        }
    }
    bool Inside(const Vec3& p, float inset = 0.0f) const {
        if (!corners.empty() && !InHeight(p.z, std::max(0.05f, -inset))) return false;
        for (int side = 0; side < Sides(); ++side)
            if (Past(side, p) > -inset) return false;
        return true;
    }
    Aabb Bounds() const {
        Aabb box;
        if (!corners.empty()) {
            for (const Vec2& c : corners) {
                box.Add({ c.x, c.y, zMin });
                box.Add({ c.x, c.y, zMax });
            }
            return box;
        }
        const Vec3 r = Right();
        for (int a = -1; a <= 1; a += 2)
            for (int b = -1; b <= 1; b += 2)
                box.Add(centre + forward * (halfLength * a) + r * (halfWidth * b));
        return box;
    }
};

// A texture mapping read off one of the game's triangles: the coordinates at
// a point and how they change across the ground, so a new surface beside it
// carries the same texture on without a seam.
struct UvMap {
    bool valid = false;
    Vec3 origin;
    Vec2 uv;
    Vec2 gu, gv;    // d(u)/d(x, y), d(v)/d(x, y)
    Vec2 At(const Vec3& p) const {
        const float dx = p.x - origin.x, dy = p.y - origin.y;
        return { uv.x + gu.x * dx + gu.y * dy, uv.y + gv.x * dx + gv.y * dy };
    }
};

// One of the game's own triangles, as it was taken out.
struct GameFace {
    Vec3 p[3];
    Vec2 uv[3];
    std::string texture;
    int kind = 0;    // 0 told by its texture, 1 road, 2 sidewalk (marked by hand)
};

// A triangle of a junction refined by hand (in Blender): it is drawn as it is
// instead of the generated surface.
struct RefinedTriangle {
    Vec3 p[3];
    Vec2 uv[3];
    Vec3 n[3];
    std::string texture;
};

// A junction made of faces of the game's own ground, chosen by hand: a
// procedural road's end node, and the game's triangles in its way. The faces
// are taken out of the game; in their place the same faces come back cut
// along the new road's kerbs -- the parts in the new road's way become road
// surface at the road's height with kerbs where they drop, the rest stays
// exactly as the game had it -- so the edge to the game's ground is the
// game's own and nothing is left over or missing.
struct FaceJunction {
    u32   node = 0;                 // the procedural road's end
    std::string model;              // the game mesh the faces came from
    std::vector<GameFace> faces;
    Vec3  gameForward{ 1, 0, 0 };   // along the game's road, for the kerb corners
    float fillet = 3.0f;            // the corner kerbs' radius
    std::string asphalt;            // the surface laid where there was none
    UvMap asphaltMap;
    // Stripes across the game's road either side of the crossing, and across
    // the new road's mouth.
    bool  crosswalks = false;
    // Refined in Blender: the surface as it came back, in place of the one
    // generated (its collision from its own triangles). Empty: generated.
    std::vector<RefinedTriangle> refined;

    // The game's triangles it takes out, one cut each.
    std::vector<GameCut> Cuts() const;
};

// A junction made into one of the game's own roads. The road stays the
// game's: only a box across it is rebuilt -- the junction surface, the far
// sidewalk carried straight on, and on the side the new road leaves from,
// kerbs curving round into the new road's sidewalks. Sidewalks continue the
// game's own texture mapping, read where they were cut; heights come from the
// game's road at the box's ends.
struct GameJunction {
    u32   node = 0;           // the new road's first node, at the mouth
    Vec3  centre;             // on the road's middle line
    Vec3  forward{ 1, 0, 0 }; // along the game's road
    int   side = 1;           // +1: the new road leaves to the right of `forward`
    float halfRoad = 4.0f;    // the game's road, kerb to kerb, halved
    float walkNear = 0.0f, walkFar = 0.0f, walkHeight = 0.2f;
    float zStart = 0.0f, zEnd = 0.0f;   // road height at the box's two ends
    std::string walkTextureNear, walkTextureFar;
    UvMap uvNear, uvFar;
    bool  crosswalks = false;

    // Across the road, towards the new road.
    Vec3 Out() const { return Vec3{ forward.y, -forward.x, 0.0f } * (float)side; }
    // The box's half length along the road, for a new road `mouthHalf` wide
    // (kerb to kerb, halved) with `branchWalk` sidewalks.
    float HalfLength(float mouthHalf, float branchWalk) const {
        return mouthHalf + std::max(std::max(walkNear, branchWalk), 0.5f) + 1.5f;
    }
    // A point of the box: `along` the road, `across` towards the new road.
    Vec3 At(float along, float across, float halfLength, float lift = 0.0f) const {
        const float t = halfLength > 1e-3f ? (along + halfLength) / (2.0f * halfLength) : 0.5f;
        const Vec3 p = centre + forward * along + Out() * across;
        return { p.x, p.y, zStart + (zEnd - zStart) * std::clamp(t, 0.0f, 1.0f) + lift };
    }
    // The stretch of the game's road it replaces.
    GameCut Cut(float halfLength) const {
        GameCut c;
        c.node = node;
        c.forward = forward;
        c.halfLength = halfLength;
        c.halfWidth = halfRoad + (walkNear + walkFar) * 0.5f + 0.05f;
        c.centre = centre + Out() * ((walkNear - walkFar) * 0.5f);
        return c;
    }
};

// What is left of a convex polygon outside a cut, as convex pieces. `Lerp`
// mixes two corners, so a vertex type with texture coordinates and colours
// keeps them on the new corners. `position(v)` reads a corner's position.
template <typename V, typename Position, typename Lerp>
std::vector<std::vector<V>> ClipOutside(const std::vector<V>& polygon, const GameCut& cut, Position position,
                                        Lerp lerp, bool& touched) {
    constexpr float kEpsilon = 1e-3f;
    std::vector<std::vector<V>> out;
    touched = false;
    // Above or below a cut held to heights: kept as it is.
    if (!cut.corners.empty()) {
        bool above = true, below = true;
        for (const V& v : polygon) {
            above &= position(v).z > cut.zMax;
            below &= position(v).z < cut.zMin;
        }
        if (above || below) {
            out.push_back(polygon);
            return out;
        }
    }
    // Wholly outside one side: kept as it is.
    for (int side = 0; side < cut.Sides(); ++side) {
        bool beyond = true;
        for (const V& v : polygon) beyond &= cut.Past(side, position(v)) >= -kEpsilon;
        if (beyond) {
            out.push_back(polygon);
            return out;
        }
    }
    touched = true;
    std::vector<V> remaining = polygon;
    for (int side = 0; side < cut.Sides() && remaining.size() >= 3; ++side) {
        std::vector<V> outside, inside;
        for (size_t i = 0; i < remaining.size(); ++i) {
            const V& a = remaining[i];
            const V& b = remaining[(i + 1) % remaining.size()];
            float fa = cut.Past(side, position(a)), fb = cut.Past(side, position(b));
            if (std::fabs(fa) < kEpsilon) fa = 0.0f;
            if (std::fabs(fb) < kEpsilon) fb = 0.0f;
            if (fa >= 0.0f) outside.push_back(a);
            if (fa <= 0.0f) inside.push_back(a);
            if ((fa > 0.0f && fb < 0.0f) || (fa < 0.0f && fb > 0.0f)) {
                const V cross = lerp(a, b, fa / (fa - fb));
                outside.push_back(cross);
                inside.push_back(cross);
            }
        }
        if (outside.size() >= 3) out.push_back(std::move(outside));
        remaining = std::move(inside);
    }
    return out;   // whatever is still in `remaining` was inside: gone
}

inline std::vector<GameCut> FaceJunction::Cuts() const {
    std::vector<GameCut> out;
    for (const GameFace& f : faces) {
        GameCut c;
        c.node = node;
        c.zMin = std::min({ f.p[0].z, f.p[1].z, f.p[2].z });
        c.zMax = std::max({ f.p[0].z, f.p[1].z, f.p[2].z });
        c.centre = (f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f);
        const float area = (f.p[1].x - f.p[0].x) * (f.p[2].y - f.p[0].y) - (f.p[2].x - f.p[0].x) * (f.p[1].y - f.p[0].y);
        if (std::fabs(area) > 1e-3f) {
            c.zMin -= 0.4f;
            c.zMax += 0.4f;
            for (int k = 0; k < 3; ++k) c.corners.push_back({ f.p[k].x, f.p[k].y });
            if (area < 0.0f) std::swap(c.corners[1], c.corners[2]);
        } else {
            // Standing up (a kerb's face): a sliver around its foot.
            int a = 0, b = 1;
            float best = -1.0f;
            for (int i = 0; i < 3; ++i)
                for (int k = i + 1; k < 3; ++k) {
                    const float dx = f.p[k].x - f.p[i].x, dy = f.p[k].y - f.p[i].y;
                    if (dx * dx + dy * dy > best) { best = dx * dx + dy * dy; a = i; b = k; }
                }
            if (best < 1e-6f) continue;
            const float length = std::sqrt(best);
            const float dx = (f.p[b].x - f.p[a].x) / length, dy = (f.p[b].y - f.p[a].y) / length;
            const float nx = -dy * 0.003f, ny = dx * 0.003f;   // to the left: counter-clockwise below
            c.zMin -= 0.02f;
            c.zMax += 0.02f;
            c.corners = { { f.p[a].x - nx, f.p[a].y - ny }, { f.p[b].x - nx, f.p[b].y - ny },
                          { f.p[b].x + nx, f.p[b].y + ny }, { f.p[a].x + nx, f.p[a].y + ny } };
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace roads

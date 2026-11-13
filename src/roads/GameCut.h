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

    Vec3 Right() const { return { forward.y, -forward.x, 0.0f }; }
    // Signed distances past the four sides; all <= 0 inside.
    float Past(int side, const Vec3& p) const {
        const Vec3 d{ p.x - centre.x, p.y - centre.y, 0.0f };
        switch (side) {
            case 0:  return Dot(d, forward) - halfLength;
            case 1:  return -Dot(d, forward) - halfLength;
            case 2:  return Dot(d, Right()) - halfWidth;
            default: return -Dot(d, Right()) - halfWidth;
        }
    }
    bool Inside(const Vec3& p, float inset = 0.0f) const {
        for (int side = 0; side < 4; ++side)
            if (Past(side, p) > -inset) return false;
        return true;
    }
    Aabb Bounds() const {
        Aabb box;
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
    // Wholly outside one side: kept as it is.
    for (int side = 0; side < 4; ++side) {
        bool beyond = true;
        for (const V& v : polygon) beyond &= cut.Past(side, position(v)) >= -kEpsilon;
        if (beyond) {
            out.push_back(polygon);
            return out;
        }
    }
    touched = true;
    std::vector<V> remaining = polygon;
    for (int side = 0; side < 4 && remaining.size() >= 3; ++side) {
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

} // namespace roads

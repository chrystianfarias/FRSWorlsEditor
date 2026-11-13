#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <cmath>
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

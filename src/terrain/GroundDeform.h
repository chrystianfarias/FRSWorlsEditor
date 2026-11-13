#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "terrain/Terrain.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Sculpting the game's own ground.
//
// The region's terrain meshes are not regular grids, so they are not edited
// vertex by vertex. A brush paints a height offset into a sparse grid laid
// over the world instead, and every terrain vertex and collision corner under
// it moves by the offset found where it stands -- whatever the mesh's own
// density. Baking writes the moved vertices and polygons into the region and
// empties the grid: from then on the moved ground is simply the ground, and
// the next strokes build on it. Each bake is an ordinary mesh and collision
// edit, so File > Region Edits can take it back.
namespace terrain {

class GroundDeform {
public:
    static constexpr float kCell = 2.0f;     // world units between grid corners
    static constexpr int   kTile = 32;       // corners a side in one stored tile

    bool Empty() const { return tiles_.empty(); }
    void Clear() { tiles_.clear(); ++revision_; }

    // The offset at a world point, bilinear between corners.
    float At(float x, float y) const;

    // One step of a brush. `base(x, y)` is the ground's height before any
    // offset, which Flatten and Smooth need; Raise and Lower do not ask.
    // Returns the box of world space that changed (invalid when nothing did).
    Aabb Apply(Brush brush, const Vec3& centre, float radius, float amount, float level,
               const std::function<float(float, float)>& base);

    // The area holding offsets, for finding the meshes they reach.
    Aabb Bounds() const;

    bool Save(const std::wstring& path) const;
    bool Load(const std::wstring& path);

    u64 Revision() const { return revision_; }

private:
    struct Tile {
        std::vector<float> offset;   // kTile * kTile
        std::vector<float> base;     // sampled ground, NaN until asked for
    };
    static long long Key(int tx, int ty) { return ((long long)tx << 32) ^ (long long)(u32)ty; }
    float* Corner(int ix, int iy, bool create, float** base = nullptr);
    float  Offset(int ix, int iy) const;

    std::unordered_map<long long, Tile> tiles_;
    u64 revision_ = 1;
};

} // namespace terrain

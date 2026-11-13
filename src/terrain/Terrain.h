#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "roads/RoadNetwork.h"

#include <functional>
#include <string>
#include <vector>

// Terrain the editor makes: patches of ground on a regular height grid,
// sculpted with brushes and painted with the game's textures.
//
// Like the roads, a patch is kept as its description -- heights and the
// texture each cell is painted with -- and the mesh is generated from it.
// The roads are taken into account as it is generated, never written into
// the heights: under a road the ground sits just below the road surface, and
// from the road's edge it rises or falls to the sculpted height over a
// shoulder, so a road can be moved and the ground follows. Baking hands the
// pieces to the same path as the roads (roads/RoadBake.h).
namespace terrain {

// A texture a patch can be painted with, and what driving on it feels like.
struct Layer {
    std::string texture = "RDP_GRAVEL_A_CK";
    float tile = 8.0f;               // world units one repeat covers
    u8    surface = 0x0F;            // collision surface: 0x0F grass, 0x01 road, 0x10 sidewalk
};

struct Patch {
    u32  id = 0;
    Vec2 origin;                     // the corner of cell (0, 0)
    float cell = 4.0f;               // cell size in world units
    int  columns = 32, rows = 32;    // cells
    std::vector<float> heights;      // (columns + 1) * (rows + 1), absolute z
    std::vector<u8> paint;           // columns * rows, a layer index each, or kEmpty
    std::vector<Layer> layers;       // at least one; up to kMaxLayers

    bool  conform = true;            // fit around the roads
    float shoulder = 12.0f;          // from a road's edge to the sculpted height
    float sink = 0.15f;              // how far under the road surface the ground sits
    u32   ambient = 0x383C42;        // RRGGBB, the night light, as on the roads

    float& H(int i, int j) { return heights[(size_t)j * (columns + 1) + i]; }
    float  H(int i, int j) const { return heights[(size_t)j * (columns + 1) + i]; }
    Vec3   Corner(int i, int j) const { return { origin.x + i * cell, origin.y + j * cell, H(i, j) }; }
    Aabb   Bounds() const;
};

constexpr int kMaxLayers = 6;
constexpr u8  kEmpty = 0xFF;          // a cell with no ground: the patch's outline goes round it

// The ground's height where nothing has been sculpted yet: what new cells
// start at, so a patch begins where the world already is.
using HeightFn = std::function<float(float x, float y)>;

enum class Brush { None, Raise, Lower, Smooth, Flatten, Paint, AddArea, EraseArea };
constexpr int kBrushCount = 8;
const wchar_t* BrushName(Brush brush);

class Terrain {
public:
    void Clear() { patches_.clear(); nextId_ = 1; ++revision_; }

    const std::vector<Patch>& Patches() const { return patches_; }
    Patch* Find(u32 id);
    const Patch* Find(u32 id) const;

    // A flat patch `size` across centred on `centre`, at its height.
    u32 Add(const Vec3& centre, float size, float cell);
    // A patch over the box (min, max) holding the cells whose middle
    // `inside` accepts, its corners at `height`. 0 when no cell is inside.
    u32 AddWhere(const Vec2& min, const Vec2& max, float cell, const std::function<bool(float, float)>& inside,
                 const HeightFn& height);
    // Makes the grid big enough for `area`; the new cells are empty and the
    // new corners take `height`.
    bool Grow(Patch& patch, const Aabb& area, const HeightFn& height);
    // Moves a whole patch.
    void Move(Patch& patch, const Vec3& by);
    void Remove(u32 id);

    // One step of a brush at `centre`. `amount` is the strength times the
    // time it ran for; `level` is the height Flatten works towards, `layer`
    // the one Paint lays. True when something changed.
    bool Apply(Patch& patch, Brush brush, const Vec3& centre, float radius, float amount, float level, u8 layer,
               const HeightFn& height = nullptr);

    // The generated ground, in pieces of 16 x 16 cells (ROAD_T<patch>_<n>,
    // so they bake, hide and are removed with the roads), lit by `poles`.
    std::vector<roads::MeshPiece> BuildMesh(const Patch& patch, const roads::RoadField* roads,
                                            const std::vector<roads::Pole>& poles) const;

    bool Save(const std::wstring& path) const;
    bool Load(const std::wstring& path);

    u64  Revision() const { return revision_; }
    void Touch() { ++revision_; }

private:
    std::vector<Patch> patches_;
    u32 nextId_ = 1;
    u64 revision_ = 1;
};

} // namespace terrain

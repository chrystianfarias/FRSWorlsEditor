#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <vector>

namespace nfs {

// Canonical vertex the editor works with.
//
// The files store two layouts, 24 and 36 bytes, that differ only by the presence
// of a normal. Both are widened to this single struct at load time so the
// renderer needs one vertex declaration and the editing tools one code path.
#pragma pack(push, 1)
struct SolidVertex {
    Vec3 position;
    Vec3 normal;    // (0,0,1) is substituted when the source layout has none
    u32  color;     // D3DCOLOR, ARGB
    Vec2 uv;
};
#pragma pack(pop)

static_assert(sizeof(SolidVertex) == 36, "SolidVertex must stay tightly packed");

// What role an object plays in the scene.
//
// A region ships its painted backdrop as ordinary solid objects: a 19 000 unit
// sky dome plus the PAN_ panorama pieces that fake the distant skyline. They
// enclose the whole city, so an editor that draws them unconditionally shows
// nothing but the inside of a dome. Classifying them lets the viewport hide the
// layer and lets the camera frame the drivable world instead.
enum class ObjectKind : u8 {
    Terrain,    // TRN_: the roads and ground of a section, already in world space
    Prop,       // buildings, signs, street furniture, placed by their own matrix
    Backdrop,   // SKYDOME and the PAN_ panorama pieces
};

// Decides the kind from the object name, which is how the artists tagged them.
ObjectKind ClassifyObject(const std::string& name);

// A contiguous run of the index buffer drawn with one texture.
struct SolidMaterial {
    u32  firstIndex   = 0;
    u32  indexCount   = 0;
    u32  textureIndex = 0;   // index into SolidObject::textureHashes
    u32  flags        = 0;   // the 0xFF field; meaning not yet established
    // The strip flags at +0x38. Their OR over a mesh picks the game's effect
    // (sub_5C58A0): 0x01 WORLDBONE, 0x40 WORLDREFLECT, 0x80 CAR, else WORLD.
    u32  stripFlags   = 0;
    Aabb bounds;             // per-material bounds as stored in the file
};

// One drawable mesh.
struct SolidObject {
    std::string name;
    u32         hash    = 0;
    u32         version = 0;
    ObjectKind  kind    = ObjectKind::Prop;

    // True when another object already places this same mesh at this same spot.
    // A region ships 1794 such pairs -- section X50 is a verbatim copy of part
    // of X0 -- and drawing both makes every shared surface z-fight, which reads
    // as the city breaking up into flickering fragments.
    bool        duplicatePlacement = false;

    Aabb bounds;       // bounds as stored in the header

    // False for the 24-byte vertex layout, which carries no normal at all.
    // Terrain uses it, so its lighting has to be rebuilt from the triangles by
    // anything that needs shading -- the substitute normal the parser puts in
    // is a placeholder, not data.
    bool hasNormals = false;

    // Where this mesh's vertices sit in the stream file, and how far apart.
    // Editing a shape means putting new positions back exactly here: the first
    // twelve bytes of each vertex, with nothing around them touched, so no
    // chunk changes size and the section table stays true.
    u64 vertexFileOffset = 0;
    u32 vertexStride     = 0;

    Mat4 transform;    // places the mesh into world space

    std::vector<u32>           textureHashes;
    std::vector<SolidMaterial> materials;
    std::vector<SolidVertex>   vertices;
    std::vector<u16>           indices;

    u32 TriangleCount() const { return (u32)indices.size() / 3; }

    bool IsBackdrop() const { return kind == ObjectKind::Backdrop; }
    bool IsTerrain()  const { return kind == ObjectKind::Terrain; }

    // World-space bounds, derived from the header bounds and the transform.
    //
    // Objects the game never needs bounds for store the empty-box sentinel
    // (min = +FLT_MAX, max = -FLT_MAX); Aabb treats that as invalid, so those
    // objects simply do not contribute to any box they are added to.
    Aabb WorldBounds() const { return Transform(bounds, transform); }
};

// A pack of solid objects, the unit in which geometry is stored inside a
// streaming section. One section usually holds several packs.
struct SolidList {
    std::string              path;     // e.g. "Location4\\RegionA\\Pipeline\\PC\\..."
    std::vector<SolidObject> objects;
};

// Parses one 0x80134000 pack. Returns false when the pack holds no geometry.
bool ParseSolidList(const Chunk& pack, SolidList& out);

// Parses one 0x80134010 object.
bool ParseSolidObject(const Chunk& objectChunk, SolidObject& out);

// Walks `buffer` and parses every solid pack it contains, appending to `out`.
// `fileOffset` is where the buffer starts inside the stream file, which the
// chunk reader needs to resolve payload alignment.
void ParseAllSolidLists(ByteSpan buffer, u64 fileOffset, std::vector<SolidList>& out);

} // namespace nfs

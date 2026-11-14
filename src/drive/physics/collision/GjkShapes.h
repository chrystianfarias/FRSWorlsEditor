#pragma once

// Port of NFSU2Unity Core/Physics/Collision/GjkShapes.cs: the GJK collision volumes (polytope hulls, the box of
// a car without a hull, the polygon of a track polygon body).

#include "drive/numerics/X87.h"
#include "drive/physics/collision/WorldCollision.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bb {

/// GJKVertex (0x30-byte vertex record / scratch vertex of the GJK code): +0x08 neighbour count, +0x0A first
/// neighbour index, +0x0C vertex id, +0x0E visit mark, +0x10 position (x, y, z, w), +0x20 cached support dot.
struct GjkVertex {
    int16_t NeighbourCount = 0, NeighbourFirst = 0, Id = 0;
    uint16_t Mark = 0;
    float X = 0, Y = 0, Z = 0, W = 0;
    float Dot = 0;
};

/// GJK collision volume (the object a CollisionBody's +0x0C points to). +0x10 type: 1 polytope (chunk 0x39200),
/// 2 polygon (track polygons), 3 box (GJKBoxPoly), 4 point, 5 rounded shape; +0x14 vertex count.
/// Types 4 and 5 belong to world objects (smokeables) and are not ported.
class GjkShape {
public:
    virtual ~GjkShape() = default;
    virtual int Type() const = 0;
    /// +0x14.
    int Count = 0;

    /// The GetPoint of the type (sub_5AAD20 dispatch): vertex id in shape space.
    virtual void GetPoint(int id, GjkVertex& v) = 0;

    /// The support function of the type: the vertex furthest along dir (world space), returned in world space
    /// through m (rotation rows + position row) in v. mt is the transposed rotation (world direction to shape space).
    virtual void Support(const float* dir, GjkVertex& v, const float* m, const float* mt) = 0;
};

/// GJKPolytope (type 1): a convex hull loaded from chunk 0x80034020 (0x39200 header, 0x39201 vertices, 0x39202
/// neighbour lists) by LoaderCollisionVolumes 0x5A1750; looked up by FindGJKPolytope 0x5A18C0. Car hulls are
/// "<CAR>_CV" in CARS\<CAR>\GEOMETRY.BIN; L4RA.BUN holds the world objects' ones.
/// Header +0x18 name hash, +0x1C name, +0x3C neighbour index count, +0x3E COG set, +0x40 COG, +0x50 neighbours,
/// +0x54 vertices.
class GjkPolytope : public GjkShape {
public:
    int Type() const override { return 1; }
    uint32_t NameHash = 0;
    std::string Name;
    std::vector<GjkVertex> Vertices;
    std::vector<int16_t> Neighbours;
    /// +0x3E / +0x40: centre of gravity subtracted by GetPoint (set per test from the body's COG).
    bool CogSet = false;
    float Cog[3] = {};

    /// GJKPolytope::GetPoint 0x5A1970: the vertex record, position minus the COG when set.
    void GetPoint(int id, GjkVertex& v) override;

    /// sub_5AFBA0 (support of a polytope): hill climb from the start vertex (v.Id) over the neighbour lists when
    /// the hull has more than 16 vertices (only for the start vertex: dword_86B2D8 is 0), otherwise / then a scan
    /// of every vertex. The game tags visited vertices with a 16-bit stamp (bRandom(0xFFFF) + bGetTicker); every
    /// call here starts with all vertices unvisited (the game only differs if a stale tag happened to equal the
    /// new stamp). The result position is M * (raw vertex): the COG-relative copy the original writes first is
    /// overwritten (kept literally).
    void Support(const float* dir, GjkVertex& v, const float* m, const float* mt) override;

    /// GJKPolytope::GetBoundingBox 0x5A19F0: box of the GetPoint positions (bInitializeBoundingBox /
    /// bExpandBoundingBox).
    void GetBoundingBox(float* min, float* max);

    /// Reads the collision volumes of a chunk container (0x80034020 of a car GEOMETRY.BIN or of L4Rx.BUN) as
    /// LoaderCollisionVolumes 0x5A1750 does: each 0x39200 header (16-aligned data), then its vertex chunk
    /// (count = size / 48 after the 16-byte alignment) and neighbour chunk (count = size / 2).
    static std::vector<std::unique_ptr<GjkPolytope>> ReadAll(const std::vector<uint8_t>& file);

private:
    F24 DotOf(int i, const float* ld) const;
    static void Expand(float* min, float* max, int k, float x);
    static void ScanContainers(const uint8_t* b, int start, int end, std::vector<std::unique_ptr<GjkPolytope>>& list);
    static void ReadPack(const uint8_t* b, int p, int end, std::vector<std::unique_ptr<GjkPolytope>>& list);
};

/// GJKBoxPoly (type 3, ctor 0x598760): +0x20 centre, +0x30 half extents, 8 corners whose id bits 1/2/4 select
/// the negative x/y/z half. The default car volume when the car has no hull: (-2.217, -0.971, -0.2) ..
/// (2.165, 0.971, 0.624) (CarCollisionBody ctor 0x5AEC20).
class GjkBox : public GjkShape {
public:
    int Type() const override { return 3; }
    float Center[3] = {}, Half[3] = {};

    GjkBox(float minX, float minY, float minZ, float maxX, float maxY, float maxZ);

    static GjkBox DefaultCar();

    /// sub_5987D0: corner id.
    void GetPoint(int id, GjkVertex& v) override;

    /// sub_5A2280: the corner on the side of the direction (shape space), to world through m.
    void Support(const float* dir, GjkVertex& v, const float* m, const float* mt) override;
};

/// The GJK polygon (type 2) of a track polygon collision body (body+0x70), built by sub_5A1A90 from the
/// expanded polygon: the 3 or 4 corners (ids 0..n-1) and the same corners pushed 1.0 against the normal
/// (ids n..2n-1); +0x1D0 "use the extruded half" flag, +0x1D4 thickness, +0x1E0 centroid of the face,
/// +0x1F0 centroid of all vertices.
class GjkPolygon : public GjkShape {
public:
    int Type() const override { return 2; }
    GjkVertex Vertices[9] = {};
    uint8_t UseExtruded = 0;            // +0x1D0
    float Thickness = 0;                // +0x1D4
    float FaceCentroid[3] = {}, Centroid[3] = {};   // +0x1E0, +0x1F0

    /// sub_5A1A90(polygon, offset, thickness) with offset (0, 0, 0) and thickness dword_7A1878 (1.0).
    void Build(const TrackPolygonShape& tp, const float* offset, float thickness);

    /// loc_5A2010: vertex id (already in world space).
    void GetPoint(int id, GjkVertex& v) override;

    /// sub_5A2060: the vertex with the largest dot product with the (world) direction; only the face vertices
    /// unless the extruded half is enabled or the thickness is not positive. The sums are associated as the
    /// original's unrolled loop does (first of each group of 4 and the tail: (x*dx + z*dz) + y*dy; the other
    /// three: (y*dy + z*dz) + x*dx); ties keep the first.
    void Support(const float* dir, GjkVertex& v, const float* m, const float* mt) override;
};

} // namespace bb

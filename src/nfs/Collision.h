#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <vector>

namespace nfs {

// World collision: what the car actually drives on and bounces off.
//
// It is not the render mesh. Each streaming section carries its own
// 0x80034130 pack of simplified ground polygons and barrier lines, stored in
// 16-bit fixed point and bucketed on a coarse grid so the game can find the
// few polygons under a wheel without searching the section. See
// docs/NFSU2-FORMATS.md for the byte layout.

// One collision primitive.
struct CollisionPolygon {
    // Corners in world space. A triangle repeats its third corner in the
    // fourth slot, so `corners[3]` is always safe to read.
    Vec3 corners[4];
    bool quad    = false;
    // A vertical wall along corners[0] -> corners[2], unbounded in height.
    // Its z values are left at zero; a viewer has to pick its own height.
    bool barrier = false;
    u8   surface = 0;   // byte 1: groups polygons by material, 121 values in L4RA
    u8   flags   = 0;   // byte 3: 0x10 quad, 0x16 barrier, others not yet known
    // Bytes 0 to 3 of the record as stored. Only two of them are understood,
    // so an export carries all four and an import puts them back unchanged.
    u8   attributes[4] = {};
};

// The collision of one streaming section.
struct CollisionPack {
    u32  sectionNumber = 0;
    u16  gridWidth  = 0;    // cells along x
    u16  gridHeight = 0;    // cells along y
    Vec3 gridOrigin;        // corner of cell (0, 0); cells are 32 units square
    Aabb bounds;            // barriers stretch it to z = +/-127
    std::vector<CollisionPolygon> polygons;

    // The order polygons are listed in within a grid cell, as a rank per
    // polygon (lower first). It is not the file order: the game's tool lists
    // them grouped -- ground, then vertical faces, then barriers, roughly --
    // by some order of its own sources that the file does not record. Every
    // shipped pack is consistent with one global order, which the parser
    // recovers from the cell lists so an encode can reproduce them. Empty
    // means file order.
    std::vector<u32> gridRank;

    size_t BarrierCount() const;

    // Height of the ground corner nearest to `point` in plan, or `fallback`
    // when the pack has no ground.
    float GroundHeightNear(const Vec3& point, float fallback) const;

    // A barrier drawn as a wall standing on the ground at both ends, from
    // `below` under it to `above` over it. The file gives it no height, so
    // this is a presentation choice shared by the viewport and the exporter.
    // Corners come out as bottom-start, top-start, top-end, bottom-end.
    void BarrierQuad(const CollisionPolygon& barrier, Vec3 out[4]) const;
    static constexpr float kBarrierBelow = 0.5f;
    static constexpr float kBarrierAbove = 3.0f;
};

// A fingerprint of the polygons, so an import can tell which state of a pack
// an export was taken from.
u32 CollisionFingerprint(const CollisionPack& pack);

// Parses one 0x80034130 chunk.
bool ParseCollision(const Chunk& collisionChunk, CollisionPack& out);

// Writes a pack back out as a complete 0x80034130 chunk, header included.
//
// Only the section number and the polygons are taken from `pack`. The bounds,
// the grid and its cell lists are rebuilt from the polygons with the rules the
// game's own tool follows, which reproduce every shipped pack of all eight
// regions byte for byte -- so re-encoding an untouched pack changes nothing,
// and an edited one is indexed the way the game expects.
//
// Ground corners are quantised to 1/8 in plan and 1/256 in height; a triangle
// must repeat its third corner in the fourth slot. Fails when a coordinate or a
// count no longer fits the file's 16-bit fields.
bool EncodeCollision(const CollisionPack& pack, std::vector<u8>& chunk, std::wstring& error);

} // namespace nfs

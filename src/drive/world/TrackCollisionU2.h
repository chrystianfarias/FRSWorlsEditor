#pragma once

// Port of NFSU2Unity Core/World/TrackCollisionU2.cs: the collision pack of one streaming section (0x80034130).

#include "drive/io/Vec3.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bb {

/// World chunk ids (the subset of Core/World/TrackSectionTableU2.cs WorldChunks the collision needs).
namespace WorldChunks {
constexpr uint32_t VisibleSections = 0x80034150;
constexpr uint32_t Collision = 0x80034130;
constexpr uint32_t CollisionHeader = 0x00034131;
constexpr uint32_t CollisionPolygons = 0x00034134;
constexpr uint32_t CollisionCellRefs = 0x00034136;
constexpr uint32_t CollisionCells = 0x00034137;
} // namespace WorldChunks

/// One collision polygon (32 bytes, chunk 0x00034134), decoded from 16-bit fixed point:
/// x = X/8, y = Y/8, z = base + Z/256. A triangle repeats its third corner in slot 3.
struct CollisionPolygonU2 {
    static constexpr int Size = 32;
    static constexpr int16_t BarrierHeight = 32512;

    /// Bytes 0..3 as stored: [0] unknown, [1] extreme-corner code, [2] unknown (surface?), [3] flags.
    uint8_t Attr0 = 0, CornerCode = 0, Attr2 = 0, Flags = 0;
    /// +0x04: whole-unit height base.
    int16_t HeightBase = 0;
    Vec3 C0, C1, C2, C3;

    static constexpr uint8_t FlagVertical = 0x02;
    static constexpr uint8_t FlagQuad = 0x10;

    bool IsQuad() const { return (Flags & FlagQuad) != 0; }
    /// Infinitely tall wall from corner 0 to corner 2 (z = -32512, +32512, +32512, -32512, zero base). Its z is stored as 0.
    bool IsBarrier = false;
    /// Flags 0x12: a wall with a real height (zero area in plan).
    bool IsVertical() const { return !IsBarrier && (Flags & FlagVertical) != 0; }
    bool IsGround() const { return !IsBarrier && !IsVertical(); }

    const Vec3& Corner(int i) const { return i == 0 ? C0 : i == 1 ? C1 : i == 2 ? C2 : C3; }
};

/// The collision pack of one streaming section (0x80034130): ground polygons, vertical faces and
/// barrier lines bucketed on a 32-unit grid (each reference carries a 4x4 mask of 8-unit sub-cells).
class TrackCollisionU2 {
public:
    static constexpr float CellSize = 32.0f;

    int SectionNumber = 0;
    int GridWidth = 0, GridHeight = 0;
    float OriginX = 0, OriginY = 0;
    Vec3 BoundsMin, BoundsMax;
    std::vector<CollisionPolygonU2> Polygons;
    /// Per cell (row-major, cy * width + cx): first reference and count.
    std::vector<uint16_t> CellFirst, CellCount;
    /// References: polygon index and sub-cell mask (bit sy * 4 + sx).
    std::vector<uint16_t> RefPolygon, RefMask;
    /// The polygon records exactly as the game addresses them (chunk 0x00034134 from its data offset,
    /// 32 bytes each): the fixed-point corners the topology queries (TrackWorld) work on.
    std::vector<uint8_t> RawPolygons;

    int BarrierCount() const;

    /// Reads a complete 0x80034130 chunk (8-byte header included), e.g. the output of nfs::EncodeCollision.
    /// The children are read from their data offset: no shipped (or encoded) pack has 0x11 alignment filler.
    /// Throws std::runtime_error when the chunk is not a collision pack or has no header.
    static TrackCollisionU2 Read(const uint8_t* chunk, size_t size);

    /// Viewer helper (not the game's GetElevation): height of the ground polygon under (x, y) found through
    /// the grid, preferring the highest surface not above maxZ.
    bool TryGetGroundHeight(float x, float y, float maxZ, float& z) const;

    /// Centre of the ground polygon closest (in plan) to (x, y); false when the pack has no ground.
    bool TryNearestGround(float x, float y, Vec3& point) const;

private:
    static bool HeightInTriangle(const Vec3& a, const Vec3& b, const Vec3& c, float x, float y, float& z);
};

} // namespace bb

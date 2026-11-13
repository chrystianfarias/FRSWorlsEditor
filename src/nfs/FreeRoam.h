#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs {

class World;

// The zone records of a region's free-roam road graph
// (TRACKS\ROUTES<region>\PathsFreeRoam.bin, chunk 0x3414A): shops, race
// triggers and the city's named neighbourhoods, each a centre, a facing, a
// radius and a footprint polygon, all in plan. See docs/NFSU2-FORMATS.md.
struct FreeRoamZone {
    u32  offset = 0;         // of the record, in PathsFreeRoam.bin
    u32  type = 0;           // +0x00: 0x0D race trigger, 0x0E shop, 0x11 city zone, ...
    Vec2 centre;             // +0x04
    Vec2 facing;             // +0x0C: unit vector in plan
    float radius = 0.0f;     // +0x14
    u32  flag = 0;           // +0x18: 1 on every shop, 0 on race triggers
    Vec2 boundsMin, boundsMax;   // +0x20: plan box of the polygon
    u32  hash = 0;           // +0x30: what the career data names it by
    i32  cellX = 0, cellY = 0;   // +0x34: whole units; the centre on race triggers
    u32  extra = 0;          // +0x3C
    std::vector<Vec2> polygon;   // +0x44: count at +0x40, record size at +0x42
};

// Reads every zone record of the region. False, and nothing read, for a
// region without a free-roam graph (every closed track).
bool ReadFreeRoamZones(const std::wstring& gameFolder, const std::wstring& region,
                       std::vector<FreeRoamZone>& out);

// Height of the collision ground at a point in plan: the lowest surface
// there, the street under any bridge.
bool GroundHeight(const World& world, float x, float y, float& height);

// The highest collision surface at a point in plan that is not above
// `ceiling`: what something dropped from there lands on.
bool GroundBelow(const World& world, float x, float y, float ceiling, float& height);

} // namespace nfs

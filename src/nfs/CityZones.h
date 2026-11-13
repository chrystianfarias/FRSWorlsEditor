#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs {

struct FreeRoamZone;

// The city's named neighbourhoods -- Pigeon Park, Coal Harbour East, Jackson
// Heights -- as the game draws them.
//
// They are zone records of type 0x11 in PathsFreeRoam.bin (0x3414A), in the
// same run as the shop and race-trigger zones: a plan box, a footprint
// polygon, and at +0x30 the bin hash of the name. L4RA has thirteen. The
// names themselves are nowhere in the file; the hashes match strings in the
// game's own files (JACKSON_HEIGHTS, BEACON_HILL_WEST, ...), which is where
// the list below comes from. One zone carries hash 0 and so has no name.
//
// Grouping them into the five districts the game advertises -- City Core,
// Beacon Hill, Jackson Heights, Coal Harbour, the Airport -- is the editor's,
// from the names; the file holds no such grouping.
struct CityZone {
    u32               hash = 0;
    std::wstring      name;        // "Pigeon Park"
    std::wstring      district;    // "City Core"
    std::vector<Vec2> polygon;
    Vec2              boundsMin, boundsMax;

    Vec2 Centre() const;
    bool Contains(float x, float y) const;
    float Area() const { return (boundsMax.x - boundsMin.x) * (boundsMax.y - boundsMin.y); }
};

constexpr u32 kCityZoneType = 0x11;

std::vector<CityZone> ReadCityZones(const std::vector<FreeRoamZone>& zones);

// The districts in the order the editor lists them.
const std::vector<std::wstring>& DistrictOrder();

// The smallest zone holding the point, or -1: zones overlap at their edges,
// and the smaller one is the more particular answer.
int ZoneAt(const std::vector<CityZone>& zones, float x, float y);

} // namespace nfs

#pragma once

// Port of NFSU2Unity Core/World/TrackPositionMarkersU2.cs: the start grids, shop and crib markers of
// TRACKS\ROUTES<region>\TrackPosMarkers*.bin.

#include "drive/io/BinHash.h"
#include "drive/io/Vec3.h"

#include <cstdint>
#include <vector>

namespace bb {

/// One track position marker (48 bytes, chunk 0x00034146 of TRACKS\ROUTES<region>\TrackPosMarkers*.bin),
/// as read by GetTrackPositionMarker (0x5D85D0): +0x08 name hash (bStringHash), +0x0C grid slot,
/// +0x10 position (x, y, z), +0x20 heading (low u16 only, 65536 = 360 degrees, counter-clockwise from +X),
/// +0x24 track id (4000 = free roam, 4001+ = races). +0x00/+0x04 are list links in memory.
struct TrackPositionMarkerU2 {
    static constexpr int Size = 48;
    uint32_t NameHash = 0;
    int Slot = 0;
    Vec3 Position;
    uint16_t Heading = 0;
    int TrackId = 0;

    /// Heading in radians (counter-clockwise from game +X; forward = (cos, sin)).
    float HeadingRadians() const { return Heading * static_cast<float>(2 * 3.14159265358979323846 / 65536.0); }
};

/// Track position markers. The free-roam start of a fresh career is the "ForwardStartingPosition" marker of
/// track 4000 (free roam), slot 0: RaceStarter::StartCareerFreeRoam (0x5404A0) -> RaceCoordinator::InitCars
/// (0x606656) -> sub_5EB5D0, which takes "BackwardStartingPosition" instead when dword_89E7A4 is set. A saved
/// career spot (ThePlayerCareer+0x8744.., written when leaving free roam) overrides it; a fresh port has none.
namespace TrackPositionMarkersU2 {
constexpr uint32_t ChunkId = 0x00034146;
constexpr int FreeRoamTrackId = 4000;
inline const uint32_t ForwardStartingPosition = BinHash::Hash("ForwardStartingPosition");   // 0x8E55C895
inline const uint32_t BackwardStartingPosition = BinHash::Hash("BackwardStartingPosition");

/// Reads every marker record of every 0x34146 chunk in a file.
std::vector<TrackPositionMarkerU2> Read(const std::vector<uint8_t>& file);

/// The marker with a name hash, track id and slot (GetTrackPositionMarker's filter); false if absent.
bool TryFind(const std::vector<TrackPositionMarkerU2>& markers, uint32_t nameHash, int trackId, int slot,
             TrackPositionMarkerU2& marker);
} // namespace TrackPositionMarkersU2

} // namespace bb

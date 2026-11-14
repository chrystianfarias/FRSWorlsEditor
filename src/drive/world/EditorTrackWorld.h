#pragma once

// The bridge between the editor's region (nfs::World) and the ported track topology (bb::TrackWorld), plus
// the spawn / respawn helpers of NFSU2Unity Runtime/CityDrive.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/world/TrackPositionMarkersU2.h"
#include "drive/world/TrackWorld.h"

#include <memory>
#include <string>
#include <vector>

namespace nfs { class World; }

namespace bb {

/// Builds the track world of the open region from the editor's data, as edited right now: the layout file
/// with the editor's layout spans applied, and for every positioned section that has collision (placeholders,
/// library sections and empty entries skipped, like TrackWorld.cs) its last collision pack, encoded with
/// nfs::EncodeCollision. Parses every section that is not parsed yet (World::LoadSection).
/// gameFolder: for the cars' CARS\<CAR>\GEOMETRY.BIN hulls; when empty, the parent of the world's overlay
/// folder is used if SPEED2.EXE is there. Returns null with a message in `error` on failure.
std::unique_ptr<TrackWorld> BuildTrackWorld(nfs::World& world, const Speed2Image& exe, std::wstring& error,
                                            const std::wstring& gameFolder = std::wstring());

/// The inputs BuildTrackWorld hands to TrackWorld (exposed for tools and tests).
bool GatherTrackWorldInputs(nfs::World& world, std::vector<uint8_t>& layout,
                            std::vector<SectionCollisionInput>& sections, std::wstring& error);

/// CityDrive.ResolveSpawn: the highest ground under (x, y) not above zHint (the game's elevation query, see
/// TrackWorld::TryFindGround), else the highest ground there at all (zHint 10000). Pass the point's z + 5 to
/// snap like the city drive does with the free-roam marker. False when there is no ground under the point.
bool SpawnGroundHeight(TrackWorld& track, float x, float y, float zHint, float& ground);

/// CityDrive.RespawnOnRoad (F): the centre of the collision ground polygon nearest (in plan) to (x, y), its
/// height refined by the game's query at the centre's z + 1. The caller keeps the car's heading
/// (atan2(R[1], R[0]) of its rotation rows) and adds the spawn height (0.8). False when there is no ground.
bool NearestGround(TrackWorld& track, float x, float y, float& outX, float& outY, float& outZ);

/// The free-roam start of a fresh career (CityDrive.ResolveSpawnHint): marker ForwardStartingPosition of
/// track 4000, slot 0, in <gameFolder>\TRACKS\ROUTES<region>\TrackPosMarkersFreeRoam.bin. Its z is not the
/// ground: snap it with SpawnGroundHeight(x, y, z + 5).
bool FindFreeRoamStart(const std::wstring& gameFolder, const std::wstring& region, TrackPositionMarkerU2& marker);

} // namespace bb

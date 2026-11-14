#pragma once

#include "nfs/Markers.h"

#include <string>
#include <vector>

namespace io {

// Hands the markers placed in the editor to the game.
//
// A shop's beacon is records in several files, tied together by hashes:
//
//   GLOBAL\GLOBALB           the career's shop list (0x34A12): name, zone hash,
//                            front-end scene, type, career stage; the race
//                            list (0x34A11) names each race's start trigger
//   ROUTES<r>\PathsFreeRoam.bin        the trigger zone (0x3414A), by zone hash
//   ROUTES<r>\TrackPosMarkersFreeRoam.bin  where the beacon stands in 3D
//                                      (0x34146), by the bin hash of the name
//   TRACKS\<r>.BUN           the garage door (0x37090), by zone hash
//
// The game walks the shop list and looks the rest up, so every placed shop
// becomes a shop record copied from a shipped shop of its kind, plus its zone,
// its position and its door; a placed race start becomes a trigger zone, and
// the races chosen for it are moved to it. The world map and the minimap get
// an indicator more for each placed shop (UI_InGame_WorldMap.fng in GLOBALB,
// HUD_SingleRace.fng in GLOBAL\InGameFreeRoam.bun). The rebuilt files go to
// <game>\models\files, which the loader opens in place of the shipped ones;
// nothing in the game's own folder changes. With nothing placed the copies are
// deleted.
bool WriteMarkersToGame(const std::wstring& gameFolder, const std::wstring& region,
                        const nfs::MarkerSet& markers, std::wstring& message);

// The files that writes, in models\files.
std::vector<std::wstring> MarkerFiles(const std::wstring& gameFolder, const std::wstring& region);

} // namespace io

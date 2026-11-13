#pragma once

#include "roads/RoadNetwork.h"

#include <string>
#include <vector>

// Baking: the procedural roads written out as the game's own models.
//
// The generated pieces go into an OBJ with no manifest, each one a shape of
// its own, so the editor's import (io/SelectionImport.h) makes every piece a
// new model -- FRS_<piece name>, built in the section it stands over from one
// of that section's own objects -- and places it where it stands. Materials
// name the game's textures, which the import finds in the region and copies
// into sections that cannot see them.
//
// A bake replaces the last one: the models it made are removed first (they
// are recognised by their prefix), so the road network on disk is the only
// source of what the game is handed.
namespace roads {

// The prefix of every model a bake makes: FRS_ + ROAD_...
constexpr const char* kBakedModelPrefix = "FRS_ROAD_";

bool IsBakedRoadModel(const std::string& name);

// Writes `pieces` to `objPath` around `origin` (world space, which the OBJ's
// zero stands for). Fills `origin` with the middle of the pieces.
bool WriteRoadObj(const std::vector<MeshPiece>& pieces, const std::wstring& objPath, Vec3& origin,
                  std::wstring& error);

} // namespace roads

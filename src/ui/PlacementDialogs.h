#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include "ui/ModelCatalog.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nfs { class World; struct SolidObject; }


// Picks the model to place from a catalogue: shelves by kind on the left,
// pictures on the right, drawn by `thumbnail` as they come into view. `targetSection` is the loaded section the
// placement will go into: a model stored in the region's library can go
// anywhere, one stored in a positioned section only into that section, and
// the list says which is which. Returns false when cancelled.
//
// A model stored in another positioned section can still be chosen: the
// answer then asks for it to be copied into the target section, mesh and any
// textures the target cannot see, when the placements are written.
bool ChooseModelToPlace(HINSTANCE instance, HWND parent, const nfs::World& world,
                        size_t targetSection, u32& modelHash, bool& copyIntoSection,
                        bool onlyMine = false, ModelThumbnailFn thumbnail = {});

// The objects made in this editor, with where they stand, to tick for
// deleting. Fills `hashes` with the ticked ones; false when cancelled.
bool ChooseMyObjectsToDelete(HINSTANCE instance, HWND parent, const nfs::World& world,
                             std::vector<u32>& hashes);

// Numeric editing of one placement. `apply` is called with every change so the
// viewport follows; cancelling calls it once more with the original.
// `snap` drops a transform onto the ground under it and returns false when
// there is none.
struct PlacementEdit {
    std::wstring description;                       // model and section, for the header
    Mat4 transform;
    std::function<void(const Mat4&)> apply;
    std::function<bool(Mat4&)>       snap;
};
bool EditPlacement(HINSTANCE instance, HWND parent, PlacementEdit& edit);

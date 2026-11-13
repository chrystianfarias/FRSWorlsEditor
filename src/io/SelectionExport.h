#pragma once

#include "render/WorldRenderer.h"

#include <string>
#include <vector>

namespace nfs { class World; }

// Writes a selection out for Blender, in a form that can be brought back.
//
// The format is Wavefront OBJ with its MTL companion, plus a JSON manifest and
// the textures beside them. OBJ was chosen over FBX for one reason: it is text.
// Blender reads and writes it without an addon, a person can open it and see
// what is wrong, and the importer that reads it back is a parser anyone can
// check. FBX would mean a binary writer many times the size for no gain here.
//
// What OBJ cannot carry -- which mesh a shape came from, where the game had it,
// how it was placed -- goes in the manifest next to it, keyed by the same
// object names the OBJ uses. Editing the geometry in Blender and exporting over
// the same file leaves those names intact, so the two still line up on the way
// back in.
//
// Vertices are written in world space. The alternative, local space plus a
// transform, would make every piece land on top of the others in Blender and
// force the editor to be right about the transform twice.
struct ExportSummary {
    size_t objects  = 0;
    // Placements left out because their mesh was already written: the same
    // building picked at both ends of a street is one shape to edit.
    size_t collapsed = 0;
    size_t triangles = 0;
    size_t textures = 0;
    std::wstring objPath;
    std::wstring manifestPath;
};

// `objPath` names the .obj; the .mtl, the .json and a textures folder are
// written beside it. Returns false and logs when nothing could be written.
bool ExportSelection(const nfs::World& world,
                     const std::vector<PickResult>& selection,
                     const std::wstring& objPath,
                     const std::wstring& region,
                     ExportSummary& summary);

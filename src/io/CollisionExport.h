#pragma once

#include "render/WorldRenderer.h"

#include <string>
#include <vector>

namespace nfs { class World; }

// Writes selected collision polygons out for Blender, in a form that can be
// brought back.
//
// Same conventions as the mesh export (SelectionExport.h): Wavefront OBJ in
// world space, moved by a shared origin and turned Y-up, with a JSON manifest
// beside it for what OBJ cannot hold.
//
// One OBJ object per section, named `collision_<section>`. Each polygon is one
// face, a triangle or a quad as the game stores it, and its material name
// carries the four attribute bytes of its record -- `ground_00_34_01_00`,
// `barrier_00_5F_14_16` -- so a face keeps its surface and flags through
// Blender without the editor having to know what they mean. Barriers have no
// height in the file, so they are written as the same wall quads the viewport
// draws; on the way back only their bottom edge counts.
//
// The manifest lists, per object, which polygons of the pack were exported,
// in face order. That is what an import replaces.
struct CollisionExportSummary {
    size_t sections  = 0;
    size_t polygons  = 0;
    size_t barriers  = 0;
    std::wstring objPath;
    std::wstring manifestPath;
};

bool ExportCollision(const nfs::World& world,
                     const std::vector<CollisionPick>& selection,
                     const std::wstring& objPath,
                     const std::wstring& region,
                     CollisionExportSummary& summary);

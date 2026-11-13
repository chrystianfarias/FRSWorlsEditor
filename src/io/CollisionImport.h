#pragma once

#include "core/Types.h"
#include "io/SectionSplice.h"
#include "nfs/Collision.h"

#include <map>
#include <string>
#include <vector>

namespace nfs { class World; }

// Reads edited collision back from Blender into overlay files.
//
// The way back from CollisionExport: each OBJ object names a section's pack
// through the manifest, and its faces replace the polygons the manifest says
// were exported -- in their slots, so the rest of the pack keeps its order.
// Faces become polygons as the game stores them: a material name carries the
// attribute bytes back, a barrier face turns into its bottom edge, a quad that
// is not convex in plan and anything with more than four corners is split into
// triangles. The pack is then re-encoded, grid and all (EncodeCollision), and
// the section is rebuilt around it.
//
// The section is rebuilt from the bytes the editor is showing, not the shipped
// ones, so a mesh imported into the same section earlier stays in. The span it
// came from is replaced, not stacked on.
//
// Like the mesh import, nothing in the game folder's .BUN files is touched: the
// result is span overlays under <game>\models that the editor and the ASI
// both apply.
struct CollisionImportPlan {
    std::vector<SectionSpan>  spans;
    std::vector<std::wstring> superseded;   // span files the new ones replace

    size_t objectsRead = 0;
    std::wstring report;                    // one paragraph per object, for a person

    bool Empty() const { return spans.empty(); }
};

// True when `objPath` holds collision exported by this editor: its objects are
// named collision_<section> and a collision manifest sits beside it.
bool IsCollisionObj(const std::wstring& objPath);

// Works out what would be written. Touches no files.
// `bases`, when given, are the packs as they were when the OBJ was exported,
// by the OBJ object name: each save of a Blender session is laid on that same
// state, so saving again replaces the last save instead of being refused.
bool PlanCollisionImport(const nfs::World& world, const std::wstring& objPath,
                         const std::wstring& modelsFolder, CollisionImportPlan& plan,
                         const std::map<std::string, nfs::CollisionPack>* bases = nullptr);

// Writes the spans and removes the ones they supersede. Returns how many span
// files were written.
size_t ApplyCollisionImport(const CollisionImportPlan& plan, const std::wstring& modelsFolder,
                            const std::wstring& region);

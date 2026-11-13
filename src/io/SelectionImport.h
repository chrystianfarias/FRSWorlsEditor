#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "io/SectionEdits.h"
#include "io/SectionSplice.h"

#include <string>
#include <vector>

namespace nfs { class World; }

// Reads an edited OBJ back into the game's own files.
//
// The export writes world-space, Y-up geometry plus a manifest that says which
// mesh each shape came from. Coming back the other way is that undone: Blender's
// axes turned back into the game's, the shared origin added, and the placement
// matrix inverted so the vertices land in the space the mesh is stored in.
//
// Only vertex positions are written, and only into the exact bytes they already
// occupy. That keeps every chunk the size it was, which means the section table
// in L4Rx.BUN stays true and the game reads the file it expects. A mesh whose
// vertex count changed in Blender cannot be put back this way and is reported
// rather than guessed at -- adding or removing geometry needs the stream and its
// table rebuilt, which is a different job from moving a kerb.
//
// Nothing is written into the game's files. Each edited mesh becomes an overlay
// under <game>\models, which both the editor and an ASI apply as the geometry
// is read. Deleting the file undoes the edit, and the shipped .BUN is never
// touched -- which also sidesteps the editor holding the stream in a read-only
// mapping that no writable handle can sit beside.
struct ImportPlan {
    struct Mesh {
        std::string       name;         // the object name in the OBJ
        std::string       model;        // the mesh's own name in the game
        u32               hash       = 0;
        u64               fileOffset = 0;
        u32               stride     = 0;
        std::vector<Vec3> positions;    // in the space the mesh is stored in
    };

    std::vector<Mesh> meshes;

    // A span of the game's files that has to read differently.
    //
    // A mesh that changed vertex count cannot be put back a vertex at a time:
    // its chunks change size, so the whole section is rebuilt and lands here
    // as one span. When the rebuilt section no longer fits where it was, it is
    // given a home past the end of the stream and a second span corrects the
    // offset and size in the region's table -- the loader serves bytes the file
    // does not contain, so there is no ceiling on how much a mesh can grow.
    struct Span {
        std::string     name;
        u64             fileOffset = 0;
        bool            layoutFile = false;
        std::vector<u8> bytes;
        std::string     section;   // the table name of the section it rebuilds
    };
    std::vector<Span> spans;

    // The rebuilt sections as they are written: every edit already in force
    // in each section, with the new mesh in place of the old one -- so an
    // import never undoes the collision, the placements or another mesh.
    std::vector<SectionSpan>  sectionSpans;
    std::vector<std::wstring> superseded;

    // Shapes the manifest did not know, made into models of their own and
    // placed where they stand. Recorded in the manifest once written, so the
    // next import of the same OBJ edits them instead of making them again.
    struct Created {
        std::string objectName;   // in the OBJ
        std::string model;        // in the game: FRS_<name>
        u32         hash = 0;
        size_t      vertices = 0;
        size_t      triangles = 0;
        Mat4        transform;    // the placement, and the space the mesh is in
        std::string section;
    };
    std::vector<Created> created;
    // Edits built whole, by section: added models, their textures, placements.
    std::vector<std::pair<std::string, std::vector<SectionEdit>>> addedEdits;

    std::wstring objPath;
    std::wstring manifestPath;    // the one read, or the one to start
    Vec3         origin{};

    size_t objectsRead   = 0;
    size_t rebuilt       = 0;   // vertex count changed, so the section was rebuilt
    size_t countMismatch = 0;   // could not be put back at all
    size_t unknownName   = 0;   // no manifest entry, so there is nowhere to put it
    std::wstring report;        // one line per thing left alone
    std::wstring rebuiltReport; // one line per mesh whose section was rebuilt

    bool Empty() const { return meshes.empty() && spans.empty() && sectionSpans.empty(); }
};

struct ImportSummary {
    size_t objectsWritten  = 0;
    size_t verticesWritten = 0;
    size_t objectsCreated  = 0;
    std::wstring modelsFolder;
};

// Works out what would be written. Needs the region open, and touches no files.
// `objPath` is the edited .obj; the manifest is looked for beside it, first by
// the same name and then among the manifests in that folder.
//
// A shape the manifest does not name becomes a new object. With no manifest at
// all, `newOrigin` says where the OBJ's origin goes and every shape in it is
// new; without one either, the import is refused.
bool PlanImport(const nfs::World& world, const std::wstring& objPath, ImportPlan& plan,
                const Vec3* newOrigin = nullptr);

// Writes the plan as overlay files under `gameFolder`\models.
bool ApplyImport(const ImportPlan& plan, const std::wstring& gameFolder,
                 const std::wstring& region, ImportSummary& summary);

#pragma once

#include "core/FileMap.h"
#include "core/Math.h"
#include "io/MeshOverlay.h"
#include "nfs/WorldAnimation.h"
#include "nfs/Collision.h"
#include "nfs/Preculler.h"
#include "nfs/SectionTable.h"
#include "nfs/Scenery.h"
#include "nfs/SceneryEdit.h"
#include "nfs/SolidList.h"
#include "nfs/TexturePack.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace nfs {

// Fingerprint of "this mesh, drawn here". Used to make sure a model that is
// stored twice, or placed both by its own header and by a scenery instance,
// still ends up on screen exactly once.
u64 PlacementKey(u32 modelHash, const Mat4& transform);

// The parsed contents of one streaming section.
struct LoadedSection {
    const Section*          info = nullptr;
    std::vector<SolidList>  solidLists;
    std::vector<std::unique_ptr<TexturePack>> texturePacks;
    Scenery                 scenery;
    std::vector<CollisionPack> collision;

    // Scenery the editor placed, kept apart from the shipped instances so
    // those stay exactly as they were and a save knows what to append. A
    // section read back from an earlier save has its extra records moved here.
    struct UserPlacement {
        u32            sceneryNumber = 0;   // which of the section's scenery chunks
        PlacedInstance instance;
    };
    std::vector<UserPlacement> placements;
    std::vector<u32> sceneryNumbers;        // each scenery chunk's number, in file order
    bool placementsChanged = false;         // differs from what is on disk

    // Shipped scenery records marked for removal and not written yet:
    // (scenery number, record index). Written ones come back as records with
    // their matrix zeroed.
    std::vector<std::pair<u32, u32>> removedNatives;

    // Collision edited in the editor and not written yet: the packs (indices
    // into `collision`) whose polygons differ from what is on disk.
    bool collisionChanged = false;
    std::vector<u32> changedCollisionPacks;
    void MarkCollisionChanged(u32 pack) {
        collisionChanged = true;
        for (u32 p : changedCollisionPacks) if (p == pack) return;
        changedCollisionPacks.push_back(pack);
    }

    // Models from other positioned sections to be copied into this one when
    // the placements are written, so they can be placed here. Hashes.
    std::vector<u32> importedModels;

    bool HoldsModel(u32 hash) const;
    Aabb                    bounds;        // everything, backdrop included
    Aabb                    trackBounds;   // drivable world only
    // Bounds of the terrain alone. Props are frequently shared between
    // neighbouring sections and can sit well outside the footprint of the
    // section that stores them, so the terrain is what marks where it lies.
    Aabb                    terrainBounds;

    // The box the camera should frame when this section is selected.
    const Aabb& FocusBounds() const;

    size_t ObjectCount() const;
    size_t TriangleCount() const;
};

// A texture that cycles through frames: water, flashing signs, blinking
// lights. Materials name the first frame's hash; the game swaps in the frame
// of the moment (chunk 0xB0300100, beside a section's texture pack).
struct TextureAnimation {
    std::string      name;
    std::vector<u32> frames;     // texture hashes, the first is the one materials use
    float            fps = 0.0f;
};

// One track region: the L4Rx.BUN layout file plus its STREAML4Rx.BUN payload.
//
// Sections are parsed on demand. Because the stream stays memory-mapped, a
// section that is never visited costs nothing beyond its table entry.
class World {
public:
    // Region identifiers as they appear in the file names: "L4RA", "L4RB", ...
    static std::vector<std::wstring> AvailableRegions(const std::wstring& tracksDirectory);

    // Where the edited meshes live, usually <game>\models. Set it before
    // opening: overlays are applied as each section is parsed, so the editor
    // shows the same geometry the game will be handed.
    void SetOverlayFolder(const std::wstring& folder) { overlayFolder_ = folder; }
    const std::wstring& OverlayFolder() const { return overlayFolder_; }

    size_t OverlayCount() const { return overlays_.size(); }

    // Opens <tracksDirectory>\<region>.BUN and STREAM<region>.BUN.
    bool Open(const std::wstring& tracksDirectory, const std::wstring& region);
    void Close();

    bool IsOpen() const { return stream_.IsOpen(); }

    const std::wstring& Region() const { return region_; }
    const SectionTable& Table()  const { return table_; }

    // The mapped stream, for anything that has to work in file bytes rather
    // than in parsed geometry -- rebuilding a section, for one.
    ByteSpan StreamBytes() const { return stream_.Span(); }

    // A section's bytes as the editor reads them: the rebuilt span when an
    // earlier import replaced the section, the stream file otherwise. Building
    // a new edit on these keeps the edits already in force.
    ByteSpan SectionBytes(const Section& section) const;
    const Preculler&    Visibility() const { return preculler_; }
    // The zones as the game shipped them, before any edited list: a section
    // a zone loads only because its list was edited is drawn there, but its
    // collision is not taken (seen in game: the car loses the ground crossing
    // into such a zone), so collision goes into sections the shipped lists name.
    const Preculler&    ShippedVisibility() const { return shippedVisibility_; }
    // The layout file (L4Rx.BUN) as shipped, and the spans the models folder
    // lays over it and the stream.
    ByteSpan LayoutBytes() const { return layout_.Span(); }
    const std::vector<SpanOverlay>& Spans() const { return spans_; }

    // Parses a section if it has not been parsed yet. Returns nullptr when the
    // section holds no geometry.
    const LoadedSection* LoadSection(const Section& section);

    // Parses every section. Reports progress through the optional callback,
    // which receives (sectionsDone, sectionsTotal).
    void LoadAllSections(void (*progress)(size_t, size_t, void*) = nullptr,
                         void* userData = nullptr);

    const std::vector<std::unique_ptr<LoadedSection>>& Loaded() const { return loaded_; }

    // Bounds of everything parsed so far, including the distant backdrop.
    Aabb Bounds() const { return bounds_; }

    // Bounds of the drivable world only.
    //
    // The first sections of a region hold shared assets and the painted
    // backdrop, which stretch tens of thousands of units in every direction and
    // would swamp any camera framed on them. Those sections carry no world
    // position in the table, so positioned sections alone give the real extent.
    Aabb TrackBounds() const;

    // The box the camera should frame for one section.
    Aabb SectionBounds(const LoadedSection& section) const;

    // Looks a texture up across every pack loaded so far.
    const Texture* FindTexture(u32 hash) const;

    // The same, falling back to the packs the game keeps loaded for every
    // track (GLOBAL\InGameCommon.bun, GLOBAL\GLOBALB.BUN). The race barriers,
    // their tape and neon, and a few effect sprites draw from those. Only for
    // looking at: a texture found there is not the region's to copy or export.
    const Texture* FindDisplayTexture(u32 hash) const;

    // Every distinct texture in the region, deduplicated by hash. A region
    // repeats textures across its packs, so the raw packs are not a useful list.
    std::vector<const Texture*> AllTextures() const;

    // How many material slots draw with this texture.
    u32 TextureUsage(u32 hash) const;

    // Looks a model up by hash across every section loaded so far. Scenery
    // instances reference models that live in a different section, usually the
    // shared library the region loads first.
    const SolidObject* FindModel(u32 hash) const;

    size_t InstanceCount() const { return instanceCount_; }

    // Every texture animation of the sections loaded, by the hash materials use.
    const std::unordered_map<u32, TextureAnimation>& TextureAnimations() const { return textureAnimations_; }

    // The moving scenery of the layout file: doors, planes, cranes, trams.
    const WorldAnimations& ObjectAnimations() const { return objectAnimations_; }

    // The race barrier groups the layout file names (chunk 0x3412F): byte 0
    // of a collision polygon indexes this list, entry 0 being "NONE". A group
    // exists for the game only during the races that name it -- in free roam
    // its polygons are not there at all.
    const std::vector<std::string>& RaceBarrierGroups() const { return raceGroups_; }
    std::string RaceBarrierGroupName(u8 group) const;

    // --- placing scenery -----------------------------------------------------

    LoadedSection* MutableLoaded(size_t index) {
        return index < loaded_.size() ? loaded_[index].get() : nullptr;
    }

    // The loaded section a model is stored in, or SIZE_MAX.
    size_t ModelHome(u32 hash) const;

    // Sections without a world position hold the region's shared library,
    // which the game keeps loaded everywhere. Only a model stored there can be
    // placed anywhere: no shipped instance refers to a model that lives in a
    // different positioned section, which would only exist while that section
    // happened to be streamed in.
    bool IsLibrary(const LoadedSection& section) const;

    // A section of the second set (Section::IsAlternate) that no visibility
    // list of the game loads: a true alternative, hidden unless asked for.
    // Without visibility lists, the second set as a whole.
    bool IsUnusedAlternate(const Section& section) const {
        return section.IsAlternate() && (preculler_.IsEmpty() || !preculler_.Listed(section.number));
    }

    // Loaded sections (indices into Loaded()) that every point's visibility
    // zone lists: what the game has loaded wherever along these points the
    // car is. A model or collision placed in one of them never vanishes on
    // the way. Empty when the points cross zones that share no section, or
    // when the region has no visibility lists.
    std::vector<size_t> SectionsLoadedThroughout(const std::vector<Vec3>& points) const;

    // The positioned section a placement at `point` belongs in: the one whose
    // ground covers it, else the nearest. SIZE_MAX when none can take scenery.
    size_t SectionForPlacement(const Vec3& point) const;

    // The section for a model or placement spanning `points`: one that can
    // take scenery and that every point's visibility zone loads, so it never
    // vanishes along them -- SectionForPlacement's choice for their middle
    // when it is one of those, else the nearest of them. Without visibility
    // lists, SectionForPlacement. SIZE_MAX when no section qualifies.
    size_t SectionForPlacementAlong(const std::vector<Vec3>& points) const;

    // Every model in the region, one per hash.
    std::vector<const SolidObject*> AllModels() const;

private:
    std::wstring region_;
    FileMap      layout_;   // L4Rx.BUN
    FileMap      stream_;   // STREAML4Rx.BUN
    std::vector<std::unique_ptr<FileMap>>     globalFiles_;
    std::vector<std::unique_ptr<TexturePack>> globalPacks_;
    void OpenGlobalPacks(const std::wstring& tracksDirectory);
    SectionTable table_;
    std::vector<std::string> raceGroups_;
    std::unordered_map<u32, TextureAnimation> textureAnimations_;
    WorldAnimations objectAnimations_;
    void ReadTextureAnimations(const Chunk& chunk);
    void ReadRaceBarrierGroups();

    std::wstring overlayFolder_;
    std::unordered_map<u32, std::vector<Vec3>> overlays_;   // mesh hash -> positions

    // Rebuilt sections and the table corrections that go with them. A mesh that
    // gained or lost vertices cannot be patched a vertex at a time, so its whole
    // section arrives as one span -- and the editor has to read it the same way
    // the game will, or the viewport shows the shipped geometry while the game
    // shows the edited one.
    std::vector<SpanOverlay> spans_;
    Preculler    preculler_;
    Preculler    shippedVisibility_;

    std::vector<std::unique_ptr<LoadedSection>> loaded_;
    std::unordered_map<u32, const SolidObject*>  modelsByHash_;
    std::unordered_map<u32, size_t>              modelHome_;
    std::unordered_set<u32>                      instancedHashes_;
    std::unordered_set<u64>                      placements_;
    size_t                                       instanceCount_ = 0;
    std::unordered_map<u32, u32>                 textureUsage_;
    std::vector<int>                            sectionToLoaded_;  // -1 when not parsed
    Aabb                                        bounds_;

    // Half the typical distance between neighbouring section centres.
    float SectionPadding() const;
};

} // namespace nfs

#pragma once

#include "render/Camera.h"
#include "render/CollisionMesh.h"
#include "render/D3D9Context.h"
#include "render/Gizmo.h"
#include "render/GpuResources.h"
// MINIMAP PREVIEW (temporary)
#include "minimap/MinimapPreview.h"

class RenderExtension;

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

// How the world is shaded in the viewport.
enum class RenderMode {
    // Texture modulated by the baked vertex colour at 2x. The game bakes its
    // night lighting into the vertex colours and combines them doubled, which is
    // what the original renderer shows; plain modulation leaves the city black.
    Textured,
    // Texture at full brightness, lighting ignored. A night-time city is close
    // to unreadable while editing, so this is the working view.
    Unlit,
    // Baked lighting only, no texture.
    VertexColor,
    Wireframe,
};

// What one frame cost, for the status bar.
struct RenderStats {
    u32 sectionsVisible  = 0;
    u32 sectionsTotal    = 0;
    u32 objectsVisible   = 0;
    u32 objectsTotal     = 0;
    u32 instancesVisible = 0;
    u32 instancesTotal   = 0;
    u32 drawCalls        = 0;
    u32 triangles        = 0;
    float frameMilliseconds = 0.0f;
};

// What the cursor is over.
//
// Picking is how a person reports a problem without having to describe it: they
// click the thing that looks wrong and the editor names it, says where it came
// from and how it was placed.
struct PickResult {
    const nfs::SolidObject* object = nullptr;
    Mat4        transform;
    Aabb        worldBounds;
    float       distance = 0.0f;
    bool        fromInstance = false;   // placed by scenery rather than its own matrix

    // Where the mesh lives on the GPU, so the selection can be painted rather
    // than merely boxed. Indices rather than pointers: a region reload rebuilds
    // the sections and any pointer kept here would dangle.
    u32         gpuSection = 0;
    u32         gpuObject  = 0;
    std::string sectionName;            // the section that placed it

    // Set when the hit is a placement the editor made: which loaded section
    // holds it and its index in that section's list. -1 for anything shipped.
    u32         loadedSection = 0;
    int         placement = -1;
    bool IsPlacement() const { return placement >= 0; }

    // Set when the hit is a shipped scenery instance: its index in the
    // section's scenery list. -1 otherwise.
    int         native = -1;
    bool IsNative() const { return native >= 0; }

    bool Hit() const { return object != nullptr; }
};

// One picked collision polygon.
//
// Indices rather than pointers, for the same reason as PickResult: a region
// reload rebuilds the sections underneath.
struct CollisionPick {
    u32   section = 0;    // index into World::Loaded()
    u32   pack    = 0;    // index into that section's collision packs
    u32   polygon = 0;
    float distance = 0.0f;

    bool SamePolygon(const CollisionPick& other) const {
        return section == other.section && pack == other.pack && polygon == other.polygon;
    }
};

// One placement of a shared model.
//
// The model lives in whichever section's buffers hold it -- usually the region's
// library section -- so an instance has to name the section as well as the
// object inside it.
struct GpuInstance {
    u32  section = 0;
    u32  object  = 0;
    Mat4 transform;
    Aabb worldBounds;

    // True when the placement mirrors the mesh rather than just turning it.
    // A mirrored transform reverses the winding of every triangle, so the
    // faces that used to point outwards now point in and the usual cull mode
    // removes exactly the ones that should be visible: the object reads as
    // inside out, and a mirrored piece of ground reads as a hole.
    bool mirrored = false;

    int  placement = -1;     // index into the section's editor placements, or -1
    int  native    = -1;     // index into the section's shipped scenery, or -1
};

// Draws a parsed world with the Direct3D 9 fixed-function pipeline.
//
// The game's track geometry carries baked lighting in the vertex colours and a
// single diffuse texture per material, which the fixed-function combiner
// reproduces exactly -- no shader is needed to match the original look.
class WorldRenderer {
public:
    void SetContext(D3D9Context* context) { context_ = context; }

    // Uploads every loaded section of `world` to the GPU and resolves its
    // scenery instances. Safe to call again once more sections have been parsed.
    void SyncWithWorld(const nfs::World& world);

    void Clear();

    // Rebuilds the instance lists from the world, after placements were added
    // or removed. Cheap: nothing is uploaded.
    void RebuildInstances(const nfs::World& world);

    // Moves one placement on screen without rebuilding anything, for dragging.
    void UpdatePlacement(u32 loadedSection, int placement, const Mat4& transform);

    // Makes one placement the whole selection. False when it is not drawn.
    bool SelectPlacement(u32 loadedSection, int placement);
    // Makes these placements the selection.
    void SelectPlacements(const std::vector<std::pair<u32, int>>& placements);

    // Leaves one placement out of picking, so snapping it to the ground does
    // not land it on itself.
    void SetPickExclusion(u32 loadedSection, int placement) {
        excludeSection_ = loadedSection;
        excludePlacement_ = placement;
    }

    void Render(const Camera& camera);

    RenderMode Mode() const { return mode_; }
    void SetMode(RenderMode mode) { mode_ = mode; }

    bool CullingEnabled() const { return culling_; }
    void SetCullingEnabled(bool enabled) { culling_ = enabled; }

    bool BackfaceCullingEnabled() const { return backfaceCulling_; }
    void SetBackfaceCullingEnabled(bool enabled) { backfaceCulling_ = enabled; }

    // The painted backdrop encloses the city, so it is hidden by default.
    bool BackdropVisible() const { return showBackdrop_; }
    void SetBackdropVisible(bool visible) { showBackdrop_ = visible; }

    // Scenery instances are the world. Hiding them leaves an empty region.
    bool SceneryVisible() const { return showScenery_; }
    void SetSceneryVisible(bool visible) { showScenery_ = visible; }

    // The second set of sections. Off by default: where it overlaps the first
    // set it is an alternative version of the same streets, not more of them.
    bool AlternateSectionsVisible() const { return showAlternate_; }
    void SetAlternateSectionsVisible(bool visible) { showAlternate_ = visible; }

    // The model library: every solid object drawn at its own header matrix.
    //
    // Those matrices are storage slots, not placements. A region parks its
    // meshes on a grid east of the city -- variant pairs side by side, five
    // freeway posts in a row -- and builds the city from scenery instances that
    // reference them. Drawing the library puts a depot of unrelated objects in
    // the middle of the map, so it is off unless someone wants to inspect it.
    bool LibraryVisible() const { return showLibrary_; }
    void SetLibraryVisible(bool visible) { showLibrary_ = visible; }

    // Drawing every section at once is not what the game does. It streams a
    // handful around the car -- around eleven of a region's several hundred --
    // and a region's districts overlap enough that showing them all at once
    // stacks props from streets that are never on screen together. Keeping only
    // the sections near the camera stands in for that until the preculler's own
    // visibility lists are read.
    bool StreamingEnabled() const { return streaming_; }
    void SetStreamingEnabled(bool enabled) { streaming_ = enabled; }

    // The collision the game drives on, drawn over the world. It follows the
    // same streaming as the geometry, so what shows is the collision of the
    // sections that are loaded around the camera.
    bool CollisionVisible() const { return showCollision_; }
    void SetCollisionVisible(bool visible) { showCollision_ = visible; }

    // Race barrier groups (polygon byte 0 != 0) exist only during the races
    // that use them. Hidden, they are neither drawn nor picked.
    bool RaceGroupsVisible() const { return showRaceGroups_; }
    // The race-only objects -- track barriers, start and finish lines -- which
    // free roam never shows. Hidden by default, like the race collision.
    // The game's animated textures -- water, signs, blinking lights -- played
    // as it plays them. On by default.
    bool AnimationsEnabled() const { return animate_; }
    // The game's moving scenery (doors, planes, cranes, trams), drawn where
    // the layout file places it; still at its first frame when animations
    // are off.
    void DrawObjectAnimations(IDirect3DDevice9* device, bool usingVisibility, const Vec3& eye, RenderStats& stats);
    void SetAnimationsEnabled(bool on) { animate_ = on; }

    // The sky dome drawn round the camera, always behind everything, as the
    // game draws it. On by default.
    bool SkyVisible() const { return showSky_; }
    void SetSkyVisible(bool on) { showSky_ = on; }

    bool RacePropsVisible() const { return showRaceProps_; }
    void SetRacePropsVisible(bool visible) { showRaceProps_ = visible; }
    void SetRaceGroupsVisible(bool visible);

    // Rebuilds the drawn collision of one loaded section after it was edited.
    void RefreshCollision(size_t loadedSection);

    // Lines and triangles an editing tool draws over the world -- a wall
    // being drawn, the point under the cursor. In world space, with their
    // own colours; empty draws nothing.
    // Each tool keeps its own layer, named by `owner`.
    struct OverlayVertex { float x, y, z; D3DCOLOR colour; };
    void SetToolOverlay(std::vector<OverlayVertex> triangles, std::vector<OverlayVertex> lines,
                        const void* owner = nullptr) {
        ToolLayer& layer = toolLayers_[owner];
        layer.triangles = std::move(triangles);
        layer.lines = std::move(lines);
    }

    // New positions for every drawn copy of a model, in the space its mesh
    // is stored in, one per vertex -- written straight into the GPU buffers,
    // for an edit seen as it is made. The caller keeps the parsed model in step.
    void UpdateModelPositions(u32 modelHash, const std::vector<Vec3>& positions);
    // The same for vertex colours (D3DCOLOR), for baked lighting seen as it is made.
    void UpdateModelColours(u32 modelHash, const std::vector<u32>& colours);

    float StreamRadius() const { return streamRadius_; }
    void  SetStreamRadius(float radius) { streamRadius_ = radius < 50.0f ? 50.0f : radius; }

    // Finds the nearest triangle the ray passes through. Respects the current
    // visibility toggles, so what can be picked is what can be seen.
    bool Pick(const Ray& ray, PickResult& out) const;

    // Everything currently selected, in the order it was picked.
    //
    // A selection is a list because the point of selecting is to act on a
    // group: exporting one lamp post at a time to edit a street would be
    // busywork. The single-pick case is just a list of one.
    const std::vector<PickResult>& Selection() const { return selection_; }

    // Replaces the selection with this one hit.
    void SetSelection(const PickResult& selection) {
        selection_ = { selection };
        collisionSelection_.clear();
        selectedOwner_ = nullptr; selectedExtra_ = -1;
    }

    // Adds the hit, or removes it when the same placement is already in.
    // Returns true when it was added.
    bool ToggleSelection(const PickResult& selection);

    void ClearSelection() {
        selection_.clear(); collisionSelection_.clear();
        selectedOwner_ = nullptr; selectedExtra_ = -1;
    }

    // Collision polygons are selected apart from meshes: they are a different
    // thing to export and put back, so a selection is one kind or the other and
    // picking one kind drops the other.
    bool PickCollision(const Ray& ray, CollisionPick& out) const;

    const std::vector<CollisionPick>& CollisionSelection() const { return collisionSelection_; }
    void SetCollisionSelection(const CollisionPick& pick);
    // Returns true when the polygon was added, false when it was removed.
    bool ToggleCollisionSelection(const CollisionPick& pick);
    // Adds every polygon of the pack `pick` belongs to. Returns how many were new.
    size_t SelectCollisionPack(const CollisionPick& pick, bool add);
    // Replaces the collision selection with these polygons.
    void SetCollisionSelectionList(std::vector<CollisionPick> picks);

    // Things drawn over the world that belong to an editor extension. The
    // extension outlives the renderer's use of it.
    void AddExtension(RenderExtension* extension) { extensions_.push_back(extension); }
    const std::vector<RenderExtension*>& Extensions() const { return extensions_; }

    // The extension whose pick the ray meets first, and its index.
    bool PickExtension(const Ray& ray, const RenderExtension*& owner, int& index, float& distance) const;

    // An extension's thing is a selection of its own, like a collision
    // polygon: selecting one drops everything else, and selecting anything
    // else drops it. -1 when `owner` has nothing selected.
    int  ExtensionSelection(const RenderExtension* owner) const {
        return owner && owner == selectedOwner_ ? selectedExtra_ : -1;
    }
    bool HasExtensionSelection() const { return selectedOwner_ != nullptr; }
    void SelectExtension(const RenderExtension* owner, int index) {
        selection_.clear();
        collisionSelection_.clear();
        selectedOwner_ = index >= 0 ? owner : nullptr;
        selectedExtra_ = index >= 0 ? index : -1;
    }

    // For extensions drawing models of their own the way the world's are
    // drawn: bind the buffers, then draw each object at the world transform
    // set on the device, a solid pass and then a translucent one.
    void BeginModels(IDirect3DDevice9* device, const GpuSection& gpu);
    void DrawModel(IDirect3DDevice9* device, const GpuObject& object, bool translucent);
    void BeginTranslucentModels(IDirect3DDevice9* device);
    void EndTranslucentModels(IDirect3DDevice9* device);
    // Untextured, vertex-coloured drawing at the identity transform with the
    // CollisionMesh vertex format, and back to the world's states after.
    void BeginOverlay(IDirect3DDevice9* device);
    void EndOverlay(IDirect3DDevice9* device);
    D3D9Context* Context() const { return context_; }
    // Geometry an extension generates itself, in world space, in the world's
    // vertex layout and lit the way the world is: BeginUserMesh once, then
    // BindUserTexture before each draw with a region texture's hash.
    // A region model's GPU copy, by hash, to draw with BeginModels/DrawModel.
    bool FindGpuModel(u32 hash, const GpuSection*& gpu, const GpuObject*& object);
    void BeginUserMesh(IDirect3DDevice9* device);
    void BindUserTexture(IDirect3DDevice9* device, u32 textureHash);

    // Models the editor shows some other way -- the baked roads, drawn from
    // their procedural description instead -- left out of drawing and picking.
    void SetHiddenModels(std::function<bool(const nfs::SolidObject&)> hidden) { hiddenModels_ = std::move(hidden); }

    // Placed objects hidden in the editor only (ui/HiddenItems.h): neither
    // drawn nor picked. A key names one placement -- the section that places
    // it and its index there -- and is empty for what is not a placement.
    void SetHiddenInstances(std::set<std::string> keys) { hiddenKeys_ = std::move(keys); ++hiddenVersion_; }
    std::string HiddenKey(const PickResult& picked) const;
    // Textures the region does not hold, for an extension's models.
    void SetExtraTextureLookup(TextureCache::ExtraLookup lookup) { textures_.SetExtraLookup(std::move(lookup)); }

    // Writes every placement the renderer would draw: the model, where it
    // came from and where it ends up. This is the editor's answer in numbers
    // rather than pixels, so it can be checked against the running game
    // instead of against anyone's impression of a screenshot.
    bool DumpPlacements(const std::wstring& path) const;

    // Verifies that every instance points at the model it claims, by
    // rebuilding its bounding box from the model and comparing with the one
    // the file stores for that instance.
    bool CheckReferences(const std::wstring& path) const;

    // Draws one model on its own into `size` x `size` BGRA pixels, top row
    // first: framed on its bounds, from above and to one side, lit by its
    // textures alone. False when the model has nothing on the GPU or the
    // device cannot do it right now. The targets it makes live in the default
    // pool, so ReleaseThumbnailTargets must run before the device is reset.
    bool RenderThumbnail(u32 modelHash, u32 size, std::vector<u32>& bgra);
    void ReleaseThumbnailTargets();

    // The handles over the selected object, drawn over everything else.
    void SetGizmo(bool visible, GizmoMode mode, const Vec3& origin, int hot) {
        gizmoVisible_ = visible; gizmoMode_ = mode; gizmoOrigin_ = origin; gizmoHot_ = hot;
    }

    // --- MINIMAP PREVIEW (temporary) --------------------------------------
    // The buildings the world draws, each where it stands, for the minimap
    // generator; and the generated result drawn in their place.
    std::vector<minimap::PlacedModel> PlacedBuildings() const;
    bool SetMinimapBuildings(const std::vector<minimap::Building>& buildings);
    bool HasMinimapBuildings() const { return !minimapPreview_.Empty(); }
    bool MinimapPreviewVisible() const { return showMinimap_; }
    void SetMinimapPreviewVisible(bool visible) { showMinimap_ = visible; }
    bool OriginalBuildingsVisible() const { return showOriginalBuildings_; }
    void SetOriginalBuildingsVisible(bool visible) { showOriginalBuildings_ = visible; }
    // --- end MINIMAP PREVIEW ----------------------------------------------

    const RenderStats& Stats() const { return stats_; }

    Aabb Bounds() const { return bounds_; }

    size_t ResidentTextures() const { return textures_.ResidentCount(); }
    size_t UnresolvedInstances() const { return unresolvedInstances_; }
    size_t DuplicateInstances() const { return duplicateInstances_; }

private:
    // Everything the renderer needs for one streaming section: its own geometry,
    // and the instances it places. Sections with scenery but no geometry of their
    // own are common, so the GPU buffers may be absent.
    struct SectionRender {
        std::unique_ptr<GpuSection> gpu;
        std::unique_ptr<CollisionMesh> collision;
        std::vector<GpuInstance>    instances;
        Aabb                        instanceBounds;   // what the section places
        Aabb                        libraryBounds;    // where its meshes are stored
        std::string                 name;             // the table's four-letter tag
        u32                         number = 0;       // the table's section number
        Vec3                        tablePosition;    // where the table says it lies
        bool                        positioned = false;
        bool                        alternate = false;
        mutable std::vector<char>   hidden;           // per instance, from hiddenKeys_
        mutable u64                 hiddenVersion = ~0ull;

        Aabb CullBounds(bool withLibrary) const {
            if (!withLibrary) return instanceBounds;
            Aabb both = instanceBounds;
            both.Add(libraryBounds);
            return both;
        }
    };

    std::set<std::string> hiddenKeys_;
    u64 hiddenVersion_ = 0;
    static std::string InstanceKey(const std::string& section, int native, int placement);
    const std::vector<char>& HiddenFlags(size_t section) const;

    void ApplyRenderStates();
    void BuildInstances(const nfs::World& world);

    // Points texture stage 0 at `texture`, or at the flat factor colour when it
    // is null, and picks the colour and alpha sources the current mode wants.
    void BindTextureStage(IDirect3DDevice9* device, const CachedTexture& texture);

    // Opaque and cut-out materials write depth and go first; blended and
    // additive ones go after them, back to front, over the finished picture.
    enum class DrawPass { Solid, Translucent };

    // Issues the draw calls of one object that belong to `pass`, assuming its
    // buffers are bound and the world transform is set. True when the object
    // also has draws of the other pass.
    bool DrawObject(IDirect3DDevice9* device, const GpuObject& object, RenderStats& stats,
                    DrawPass pass = DrawPass::Solid);

    // Blended surfaces met during the solid pass, drawn once it is over.
    struct TranslucentObject {
        const GpuObject* object = nullptr;
        size_t section  = 0;
        Mat4   transform;
        bool   mirrored = false;
        float  distance = 0.0f;
    };
    void QueueTranslucent(const GpuObject& object, size_t section, const Mat4& transform,
                          bool mirrored, const Aabb& bounds, const Vec3& eye);
    void DrawTranslucent(IDirect3DDevice9* device, RenderStats& stats);
    void BeginTranslucentStates(IDirect3DDevice9* device);
    void EndTranslucentStates(IDirect3DDevice9* device);

    // Outlines the selected object so it can be told apart from its neighbours.
    void DrawSelection(IDirect3DDevice9* device);

    // Draws the collision of the sections that passed this frame's culling.
    void DrawCollision(IDirect3DDevice9* device, const std::vector<size_t>& visible);
    void DrawCollisionSelection(IDirect3DDevice9* device);

    void DrawGizmo(IDirect3DDevice9* device, const Camera& camera);

    // The polygon's corners as drawn: barriers become their wall quad.
    bool CollisionCorners(const CollisionPick& pick, Vec3 out[4], bool& quad) const;

    // Tests one placement and keeps it when it is the closest hit so far.
    void PickOne(const Ray& ray, const GpuObject& object, const Mat4& transform,
                 const Aabb& worldBounds, bool fromInstance, size_t sectionIndex,
                 u32 ownerSection, u32 ownerObject, PickResult& best,
                 int placement = -1, int native = -1) const;

    D3D9Context*      context_ = nullptr;
    const nfs::World* world_   = nullptr;

    // Thumbnails: where each model's GPU copy is, and the off-screen targets.
    std::unordered_map<u32, std::pair<size_t, size_t>> objectByHash_;
    bool      gizmoVisible_ = false;
    GizmoMode gizmoMode_ = GizmoMode::Move;
    Vec3      gizmoOrigin_;
    int       gizmoHot_ = kGizmoNone;
    IDirect3DTexture9* thumbTarget_   = nullptr;
    IDirect3DSurface9* thumbDepth_    = nullptr;
    IDirect3DSurface9* thumbReadback_ = nullptr;
    u32                thumbSize_     = 0;

    std::vector<SectionRender> sections_;
    size_t       uploadedSections_ = 0;
    TextureCache textures_;
    std::vector<TranslucentObject> translucent_;

    RenderMode mode_ = RenderMode::Textured;
    bool       culling_ = true;
    bool       backfaceCulling_ = true;
    bool       showBackdrop_ = false;
    bool       showScenery_ = true;
    bool       showLibrary_ = false;
    bool       showAlternate_ = false;
    bool       showCollision_ = false;
    bool       showRaceGroups_ = false;
    bool       showRaceProps_ = false;
    bool       animate_ = true;
    bool       scrolling_ = false;   // texture stage 0 has a scroll transform on
    void ScrollTextureStage(IDirect3DDevice9* device, const CachedTexture& texture);
    bool       showSky_ = true;
    double     animationClock_ = 0.0;
    // The texture to draw for a material's: the frame of the moment when it
    // is animated.
    u32 AnimatedTexture(u32 hash) const;
    void DrawSky(IDirect3DDevice9* device, const Camera& camera, RenderStats& stats);
    struct ToolLayer { std::vector<OverlayVertex> triangles, lines; };
    std::unordered_map<const void*, ToolLayer> toolLayers_;
    void DrawToolOverlay(IDirect3DDevice9* device);
    // Off by default: the whole region drawn at once is what editing wants;
    // streaming shows what the game would have loaded, a question asked less.
    bool       streaming_ = false;
    // The visible set for the camera's current zone, recomputed only when the
    // camera leaves the zone it was in: the lists are stable over a whole city
    // block and testing every polygon each frame would be wasted work.
    mutable std::vector<u32> visibleSections_;
    mutable Vec3             visibleFrom_;
    mutable bool             haveVisible_ = false;
    float      streamRadius_ = 400.0f;
    DWORD      cullMode_ = D3DCULL_CW;

    size_t      unresolvedInstances_ = 0;
    size_t      duplicateInstances_  = 0;
    Aabb        bounds_;
    RenderStats stats_;
    std::vector<PickResult> selection_;
    std::vector<CollisionPick> collisionSelection_;
    std::vector<RenderExtension*> extensions_;
    const RenderExtension* selectedOwner_ = nullptr;
    int  selectedExtra_ = -1;
    u32 excludeSection_   = 0;
    int excludePlacement_ = -1;
    // MINIMAP PREVIEW (temporary)
    minimap::Preview minimapPreview_;
    bool showMinimap_ = false;
    bool showOriginalBuildings_ = true;
    bool HiddenBuilding(const GpuObject& object) const {
        return !showOriginalBuildings_ && object.source && minimap::IsMinimapBuilding(*object.source);
    }

    std::function<bool(const nfs::SolidObject&)> hiddenModels_;
    bool HiddenModel(const GpuObject& object) const {
        return (object.raceProp && !showRaceProps_) ||
               (hiddenModels_ && object.source && hiddenModels_(*object.source));
    }

    // The sections whose collision was drawn last frame, so picking only finds
    // what is on screen.
    std::vector<size_t> collisionDrawn_;
};

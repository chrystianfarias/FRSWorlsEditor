#pragma once

#include "io/CollisionEdit.h"
#include "render/WorldRenderer.h"
#include "ui/EditorExtension.h"
#include "ui/UndoRegistry.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <array>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nfs { class World; }

struct MeshToolsHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    HACCEL*   accelerators = nullptr;
    Viewport* viewport = nullptr;
    std::function<nfs::World*()> world;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
    // Reads an exported OBJ back the way the Blender round trip does.
    std::function<void(const std::wstring& objPath)> importQuietly;
    int undoCommand = 0;                  // the editor's Ctrl+Z command
    // The collision tools: their editor, undo history and redraw.
    std::function<class CollisionTools*()> collision;
};

// A small mesh editor for the object selected in the viewport: vertices,
// edges or faces picked and moved, turned or scaled with the gizmo -- the
// shape changes on screen as it is dragged.
//
// Tab starts it on the one selected object and Tab again finishes: the model
// is then written the way an edit in Blender comes back (exported to an OBJ
// beside its manifest and imported quietly), so every rule of that round trip
// -- the section rebuilt around it, the edit listed and revertible -- holds
// here too. Vertex count and triangles never change; only positions do.
//
// A model is shared by every placement of it, and all of them change.
// 1 / 2 / 3 pick vertices, edges or faces while editing (the toolbar keeps
// the gizmo's move / rotate / scale), Ctrl+click adds, Ctrl+A takes all,
// Ctrl+Z steps back, Esc drops the selection, Mesh > Discard throws the edit away.
//
// With collision polygons selected instead (the collision layer shown), Tab
// edits the collision the same way: the selected polygons and the ones that
// share a corner with them, each copy in a neighbouring section moving with
// it. Ground and walls are faces, barriers are edges (only where they stand
// matters, not their height). Each drag is one step of the collision undo;
// the next save writes it. E extrudes the picked edges into new faces.
class MeshTools : public EditorExtension, public ViewportTool, public UndoParticipant {
public:
    explicit MeshTools(MeshToolsHost host) : host_(std::move(host)) {}

    bool Active() const { return active_; }
    // 0 vertices, 1 edges, 2 faces.
    int PickIndex() const { return (int)pick_; }

    // The Mesh menu's commands, for a control elsewhere that drives it (the
    // mode switch over the view).
    static constexpr int kEditCommand = 44501, kVerticesCommand = 44502, kEdgesCommand = 44503,
                         kFacesCommand = 44504, kFinishCommand = 44506,
                         kBakeCommand = 44508;   // handled by the window: its dialog

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override { Stop(false); }
    bool StatusText(std::wstring& text) const override;
    bool FillProperties(PropertiesPanel::View& view) override;
    bool DeleteSelection() override;
    bool MoveSelectionToView() override;
    void Tick(float deltaSeconds) override;   // diagnostics only (FRS_ANIM_REPORT)

    // ---- ViewportTool
    bool MouseDown(int x, int y, WPARAM keys) override;
    bool KeyDown(WPARAM key, bool repeat) override;
    bool GizmoPivot(Vec3& pivot) override;
    void GizmoChanged(const Mat4& transform, bool done, bool keep) override;

    // ---- UndoParticipant: the steps of the mesh being edited
    u64 LatestUndoStamp() const override { return active_ && !undoStamps_.empty() ? undoStamps_.back() : 0; }
    bool UndoLatest() override;

private:
    enum class Pick { Vertex, Edge, Face };

    nfs::World* World() const { return host_.world ? host_.world() : nullptr; }
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    bool Begin();
    bool BeginCollision();
    // The edit session over these polygons (copies added): points, welds,
    // edges and faces. A polygon already in the session keeps its starting
    // shape, for Discard.
    bool BuildCollision(std::vector<collision::PolygonRef> refs);
    // E: each picked edge grows a new face, its far edge 2 m outward (up, for
    // the top of a wall) and picked, ready for the gizmo.
    void ExtrudeCollision();
    void Finish();                 // writes the edit
    void Stop(bool restore);       // leaves edit mode; `restore` puts the shape back
    void SetPickMode(Pick mode);
    void SelectAll();
    void Undo();

    // The edited positions into the parsed model (every stored copy) and the
    // GPU, so the view, picking and a later export all see them.
    void Push();
    std::vector<u32> SelectedWelds() const;
    Vec3 WeldPosition(u32 weld) const { return world_[weldVertices_[weld].front()]; }
    void UpdateOverlay();

    MeshToolsHost host_;
    bool active_ = false;
    Pick pick_ = Pick::Vertex;

    u32  modelHash_ = 0;
    std::string modelName_;
    Mat4 transform_, inverse_;
    size_t placements_ = 0;        // how many places draw this model

    std::vector<Vec3> world_;      // per model vertex, world space, as edited
    std::vector<Vec3> original_;   // as it was when editing began
    std::vector<Vec3> originalLocal_;   // the same, exactly as the model stores it
    std::vector<u32>  weldOf_;     // model vertex -> weld (vertices at one spot)
    std::vector<std::vector<u32>> weldVertices_;
    std::vector<std::pair<u32, u32>> edges_;     // welds
    std::vector<std::array<u32, 3>> faces_;      // welds

    std::set<u32> selected_;       // welds, edges or faces, by the pick mode
    std::vector<std::vector<Vec3>> undo_;
    std::vector<u64> undoStamps_;      // one per undo_ step, from undo::NextStamp()

    bool dragging_ = false;
    std::vector<Vec3> dragStart_;
    Vec3 pivotStart_{};
    HMENU popup_ = nullptr;

    // --- editing collision instead of a model ----------------------------------
    // Points: one per distinct corner of each polygon (a barrier has two, a
    // triangle three); world_ holds their positions and the welds join the
    // ones at one spot, so a shared corner and the edge copies move as one.
    struct CollisionPoint {
        u32 polygon = 0;   // index into collisionRefs_
        u8  slots = 0;     // corners it sets, bit per slot
        bool barrier = false;
    };
    class CollisionTools* Collision() const { return host_.collision ? host_.collision() : nullptr; }
    void ReadCollision();          // positions from the packs (after an undo)
    void PushCollision();          // positions into the packs, while dragging
    void StopCollision(bool restore);
    bool collision_ = false;
    std::vector<collision::PolygonRef> collisionRefs_;
    std::vector<collision::PolygonRef> collisionSelected_;   // the selection editing began from
    std::vector<nfs::CollisionPolygon> collisionStart_;      // as editing began
    std::vector<CollisionPoint> collisionPoints_;
    std::vector<u32> facePolygon_;   // per face, its polygon: a quad's two halves pick together
    u64 collisionUndone_ = 0;        // the editor's counter, to notice an undo
    bool collisionSteps_ = false;    // any drag kept
};

#pragma once

#include "io/CollisionEdit.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"
#include "ui/EditorExtension.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nfs { class World; }

// What the collision tools need from the editor window.
struct CollisionToolsHost {
    HINSTANCE instance = nullptr;
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    HACCEL*   accelerators = nullptr;     // extended with the tools' keys
    Viewport* viewport = nullptr;
    PropertiesPanel* properties = nullptr;
    std::function<nfs::World*()> world;
    std::function<void(const std::wstring&)> status;   // the status bar's message part
    std::function<void()> menuChecks;                   // the window's UpdateMenuChecks
};

// Collision editing inside the editor: the Collision menu, the drawing tools
// and the selection's properties. Built into every build, as an editor
// extension of its own (ui/EditorExtension.h) plus a viewport tool.
//
// Tools (Collision menu, or the keys):
//   Ctrl+Shift+W  barrier: a wall of unbounded height, point by point
//   Ctrl+Shift+H  wall with a real height (+/- change it)
//   Ctrl+Shift+G  ground: an outline, filled
// Click to add a point (snapping to collision corners; Alt to place freely),
// Backspace takes the last back, Enter or a double click finishes, Esc
// cancels. With collision polygons selected, G moves them (copies in
// neighbouring sections included), Del deletes them, and the properties
// panel sets their surface. Selected objects get collision made from their
// mesh (the properties panel's Make collision, Ctrl+Shift+K).
//
// Ctrl+Z is the editor's undo for both kinds of edit it keeps until a save:
// collision, and objects placed, moved, turned or deleted -- the latter
// noticed by watching the placements settle after each change.
// Nothing reaches the game until the next save (Ctrl+Shift+S).
class CollisionTools : public EditorExtension, public ViewportTool {
public:
    explicit CollisionTools(CollisionToolsHost host) : host_(std::move(host)) {}

    // The command Ctrl+Z sends (Collision > Undo), for a tool that takes it first.
    static constexpr int kUndoCommand = 44004;

    // The edits themselves, for anything else that makes collision -- a road
    // tool, a generator. Call RefreshAfterEdit() after changing through it.
    collision::Editor& Editor() { return editor_; }
    void RefreshAfterEdit();

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override;
    bool StatusText(std::wstring& text) const override;
    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;
    bool DeleteSelection() override;
    void Tick(float deltaSeconds) override;

    // ---- ViewportTool
    bool MouseDown(int x, int y, WPARAM keys) override;
    bool MouseDoubleClick(int x, int y, WPARAM keys) override;
    void MouseMove(int x, int y) override;
    bool MouseWheel(int notches, WPARAM keys) override;
    bool KeyDown(WPARAM key, bool repeat) override;

private:
    enum class Mode { None, Barrier, Wall, Ground };

    nfs::World* World() const { return host_.world ? host_.world() : nullptr; }
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    void BuildMenu();
    void ExtendAccelerators();

    // Collision for the selected objects, from their meshes.
    void GenerateForSelection();

    // --- the undo history ----------------------------------------------------
    // What each step is, newest last: a collision edit (taken back by the
    // collision editor) or a state of the placements to go back to.
    struct PlacementState {
        std::vector<std::vector<nfs::LoadedSection::UserPlacement>> placements;
        std::vector<std::vector<std::pair<u32, u32>>> removed;
        std::vector<std::vector<u32>> imported;
        std::vector<char> changed;
    };
    PlacementState CapturePlacements() const;
    void RestorePlacements(const PlacementState& state);
    size_t PlacementSignature() const;
    void ResetHistory();
    void WatchHistory();
    enum class StepKind : char { Collision, Placements };
    std::vector<StepKind> history_;
    std::vector<PlacementState> placementUndo_;
    PlacementState settled_;          // the placements as they last came to rest
    size_t settledSignature_ = 0;
    u64 seenRecorded_ = 0, seenUndone_ = 0;

    void StartTool(Mode mode);
    void StopTool();
    void Finish();
    void UpdateOverlay();
    // The point a click at (x, y) would place, snapped. False over nothing.
    bool PointUnder(int x, int y, Vec3& point) const;

    std::vector<collision::PolygonRef> SelectionRefs() const;
    void SelectRefs(const std::vector<collision::PolygonRef>& refs);
    void SetSelectionSurface(u8 surface);
    void ClearSelectionRaceGroup();
    void SelectCopies();
    void Undo();

    bool BeginMove();
    void UpdateMove(int x, int y);
    void EndMove(bool keep);

    CollisionToolsHost host_;
    collision::Editor editor_;

    Mode mode_ = Mode::None;
    std::vector<Vec3> points_;
    Vec3 cursor_{};
    bool haveCursor_ = false;
    bool cursorSnapped_ = false;

    float wallHeight_ = 3.0f;
    u8 groundSurface_ = nfs::kSurfaceRoad;
    u8 wallSurface_ = nfs::kSurfaceSolidWall;

    bool  moving_ = false;
    float movePlane_ = 0.0f;       // height of the plane the drag is measured on
    Vec3  moveAnchor_{};
    float moveRise_ = 0.0f;
    size_t moveCount_ = 0;

    HMENU popup_ = nullptr;
};

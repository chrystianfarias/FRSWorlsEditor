#pragma once

#include "nfs/World.h"
#include "render/Camera.h"
#include "render/D3D9Context.h"
#include "render/WorldRenderer.h"
#include "ui/ImGuiLayer.h"

#include <windows.h>

#include <functional>
#include <vector>

// Something that takes the viewport's mouse and keys before the viewport's
// own handling -- a drawing tool. Each call returns true when it used the
// event; the default uses nothing.
class ViewportTool {
public:
    virtual ~ViewportTool() = default;
    virtual bool MouseDown(int x, int y, WPARAM keys) { (void)x; (void)y; (void)keys; return false; }
    // The second click of a double click; unused, it counts as a plain click.
    virtual bool MouseDoubleClick(int x, int y, WPARAM keys) { (void)x; (void)y; (void)keys; return false; }
    // Every move while the view is not being turned. Not consumable.
    virtual void MouseMove(int x, int y) { (void)x; (void)y; }
    virtual bool MouseWheel(int notches, WPARAM keys) { (void)notches; (void)keys; return false; }
    // `repeat` is the keyboard's auto-repeat.
    virtual bool KeyDown(WPARAM key, bool repeat) { (void)key; (void)repeat; return false; }
    // Something of the tool's own under the gizmo -- vertices being edited:
    // where its handles stand. False when the tool has nothing for it.
    virtual bool GizmoPivot(Vec3& pivot) { (void)pivot; return false; }
    // The gizmo moved it. `transform` takes the pivot's frame as the drag
    // began (no turn, unit scale, at the pivot) to where it is now; `done`
    // once let go, `keep` false when the drag was called off.
    virtual void GizmoChanged(const Mat4& transform, bool done, bool keep) {
        (void)transform; (void)done; (void)keep;
    }
};

// The 3D child window.
//
// It owns the Direct3D device, the camera and the renderer. The parent frame
// only tells it which world to show and asks it to draw a frame; everything
// about navigation stays here.
//
// Navigation follows the convention of the level editors this tool is modelled
// on: hold the right mouse button to look around, WASD to move on the view
// plane, E and Q to rise and fall, Shift to sprint, Ctrl to crawl, and the wheel
// to change the base speed.
class Viewport {
public:
    bool Create(HWND parent, int controlId);
    void Destroy();

    HWND Handle() const { return window_; }

    // Advances the camera by `deltaSeconds` and renders one frame.
    void Tick(float deltaSeconds);

    void SetWorld(nfs::World* world);
    void RefreshWorld();          // re-upload after more sections were parsed

    void FrameBounds(const Aabb& bounds);
    void FrameWorld();

    // Flies the camera to a viewpoint over `seconds` rather than cutting to
    // it, so the jump across the city reads as one. Moving or looking takes
    // the camera back at once.
    void FlyTo(const Vec3& position, float yaw, float pitch, float seconds = 0.9f);
    bool Flying() const { return flying_; }


    Camera&        GetCamera()   { return camera_; }
    WorldRenderer& GetRenderer() { return renderer_; }

    bool HasDevice() const { return context_.IsValid(); }

    // Selects whatever is under the given client position.
    // `add` is the Ctrl-click case: the hit joins the selection, or leaves it
    // when it was already in, instead of replacing everything.
    // Ctrl adds to the selection; Shift takes a collision polygon's whole pack.
    void PickAt(int x, int y, bool add, bool wholePack);
    void PickCollisionAt(const Ray& ray, bool add, bool wholePack);

    // --- moving placed objects ------------------------------------------------

    // The one editor placement selected, if exactly one is.
    bool SelectedPlacement(u32& loadedSection, int& placement) const;
    // Every editor placement in the selection.
    std::vector<std::pair<u32, int>> SelectedPlacements() const;

    // Starts dragging the selected placement across its own height (G) or
    // turning it about the vertical (R). The mouse drives it; a left click
    // keeps the result, Esc or the right button puts it back.
    enum class TransformMode { None, Move, Rotate };
    bool BeginTransform(TransformMode mode);
    bool Transforming() const { return transform_ != TransformMode::None; }

    // Something that is one point with a heading rather than a placement --
    // an extension's own (ui/EditorExtension.h) -- moved and turned by the same G and R. `get`
    // says whether one is selected and where it stands; `set` puts it
    // somewhere, with `commit` true once the drag is kept.
    using PointGet = std::function<bool(Vec3& position, float& heading)>;
    using PointSet = std::function<void(const Vec3& position, float heading, bool commit)>;
    void SetPointEditor(PointGet get, PointSet set) { pointGet_ = std::move(get); pointSet_ = std::move(set); }

    // The map's own objects in the selection become editor placements, so
    // they can be moved: each shipped record is marked removed (as Delete
    // does) and a placement of the same model, with the same flags and
    // scenery entry, takes its place in the same scenery chunk. The game
    // then reads them like any placed object -- which has been tried in it,
    // where moving the shipped record in place would leave the scenery tree
    // still bounding it where it was. Keeps them selected; returns how many.
    // `remember` records them for CancelDetach.
    size_t DetachNatives(bool remember = false);
    // Takes back what the last remembered DetachNatives did.
    void CancelDetach();

    // Moves a placement, on screen and in the world, and marks it unsaved.
    void SetPlacementTransform(u32 loadedSection, int placement, const Mat4& transform);

    // Moves placements into the sections whose ground they now stand on, when
    // that is another one. The game draws a section's scenery only while that
    // section is loaded, so an object dragged across a boundary has to change
    // hands; a model that is not in the region's library is copied along.
    // Keeps them selected.
    void RehomePlacements(std::vector<std::pair<u32, int>> placements);
    void RehomePlacement(u32 loadedSection, int placement) { RehomePlacements({ { loadedSection, placement } }); }

    // The gizmo on the selected placement: arrows, rings or boxes to drag.
    void SetGizmoMode(GizmoMode mode) { gizmoMode_ = mode; }
    GizmoMode GetGizmoMode() const { return gizmoMode_; }
    bool GizmoDragging() const { return gizmoActive_ != kGizmoNone; }

    // Height of whatever is under `point`, ignoring the placement itself.
    bool GroundBelow(const Vec3& point, u32 loadedSection, int placement, float& height);

    // The ray through a client-area pixel, and the one through the centre.
    Ray RayAt(int x, int y) const;

    // The panels drawn over the view (ui/ImGuiLayer.h): `draw` lays them out
    // once a frame, between the scene and the present.
    void SetUiCallback(std::function<void()> draw) { uiDraw_ = std::move(draw); }
    // What takes the edges (menu, toolbar, status bar), before the panels.
    void SetUiBars(std::function<void()> bars) { uiBars_ = std::move(bars); }
    // A panel's text field has the keyboard: shortcuts leave it alone.
    bool UiWantsText() const { return ui_.WantsText(); }

    // A tool that sees the mouse and keys first. Not owned.
    void AddTool(ViewportTool* tool) { tools_.push_back(tool); }
    // The view the gizmo is measured in, for a tool projecting points to pixels.
    GizmoView ToolView() const { return CurrentGizmoView(); }
    bool Looking() const { return looking_; }
    bool KeyHeld(int key) const { return key >= 0 && key < 256 && keys_[key]; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void UpdateCamera(float deltaSeconds);
    void UpdateTransform(int x, int y);
    void EndTransform(bool keep);
    void BeginLook();
    void EndLook();

    HWND         window_ = nullptr;
    D3D9Context  context_;
    Camera       camera_;

    void UpdateFlight(float deltaSeconds);

    // The gizmo: which object it is on, the view it is measured in, and a drag.
    // `placement` is kGizmoPoint when the gizmo is on an extension's point
    // (SetPointEditor) rather than on a placement.
    static constexpr int kGizmoPoint = -2;
    // ... and kGizmoTool when it is on a tool's own selection (ViewportTool::GizmoPivot);
    // `section` is then the tool's index.
    static constexpr int kGizmoTool = -3;
    // ... and kGizmoNative on one of the map's own objects, which becomes an
    // editor placement the moment a drag begins (DetachNatives).
    static constexpr int kGizmoNative = -4;
    bool GizmoTarget(u32& section, int& placement, Mat4& transform) const;
    static float PointHeading(const Mat4& transform);
    Mat4  gizmoNow_;                    // a point's transform as the drag left it
    GizmoView CurrentGizmoView() const;
    bool BeginGizmoDrag(int x, int y);
    void UpdateGizmoDrag(int x, int y);
    void EndGizmoDrag(bool keep);
    GizmoMode gizmoMode_ = GizmoMode::Move;
    int   gizmoHot_ = kGizmoNone;
    int   gizmoActive_ = kGizmoNone;
    u32   gizmoSection_ = 0;
    int   gizmoPlacement_ = -1;
    Mat4  gizmoStart_;
    Vec2  gizmoMouseStart_;
    float gizmoAxisStart_ = 0.0f;       // where along the axis the grab began
    Vec3  gizmoPlaneStart_;             // where on the ground plane it began
    bool  flying_ = false;
    float flightTime_ = 0.0f, flightLength_ = 1.0f;
    Vec3  flightFrom_, flightTo_;
    float yawFrom_ = 0.0f, yawTo_ = 0.0f, pitchFrom_ = 0.0f, pitchTo_ = 0.0f;
    WorldRenderer renderer_;
    nfs::World*  world_ = nullptr;

    // Key state tracked from the window messages rather than polled from the
    // system. GetAsyncKeyState reports the physical keyboard, so a key left
    // down by anything else -- another application, synthetic input -- drives
    // the camera forever with no way to stop it.
    bool  keys_[256] = {};

    // The drag in progress.
    // Every selected placement moves together, about the group's pivot: the
    // centre of their positions.
    TransformMode transform_ = TransformMode::None;
    struct Moving { u32 section; int placement; Mat4 start; };
    std::vector<Moving> moving_;
    Vec3  transformPivot_;
    Vec3  transformAnchor_;      // where the drag began, on the pivot's plane
    float transformRise_ = 0.0f; // height added with the wheel
    int   transformMouseX_ = 0;

    // The point being dragged, when it is one and not placements.
    PointGet pointGet_;
    PointSet pointSet_;
    bool  movingPoint_ = false;
    Vec3  pointStart_, pointNow_;
    float pointHeadingStart_ = 0.0f, pointHeadingNow_ = 0.0f;

    std::vector<ViewportTool*> tools_;
    ImGuiLayer ui_;
    std::function<void()> uiDraw_, uiBars_;
    bool inFrame_ = false;
    // FRS_CAPTURE=<file.bmp>: the frame, panels included, saved once the
    // region has been up a few seconds -- for checking the look without a
    // screen capture.
    float captureClock_ = 0.0f;
    bool  captured_ = false;

    // What the last remembered DetachNatives changed, to cancel it.
    struct Detached { u32 section; std::pair<u32, u32> record; size_t placement; bool wasChanged; };
    std::vector<Detached> detached_;

    bool  looking_   = false;
    POINT lookAnchor_{};   // screen position the cursor is pinned to while looking
};

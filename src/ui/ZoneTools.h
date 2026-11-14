#pragma once

#include "ui/EditorExtension.h"
#include "ui/PropertiesPanel.h"
#include "ui/UndoRegistry.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nfs { class World; }

struct ZoneToolsHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    Viewport* viewport = nullptr;
    PropertiesPanel* properties = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> modelsFolder;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
    std::function<void()> reopenRegion;   // reads the edited lists back in
    std::function<bool()> busy;           // another edit mode (an object's mesh) has the clicks
};

// The game's visibility zones (L4Rx.BUN chunk 0x80034150), seen and edited.
//
// Standing in a zone, the game loads only the sections its list names:
// anything placed in a section the list leaves out -- a new road crossing
// into the zone -- is gone while the car is there. View > Visibility Zones
// draws every zone's outline on the ground; a click then picks the zone under
// it (the smallest, as the game picks), its outline lit and its sections
// listed in the properties, where the list can be edited: section names,
// separated by spaces. Lists only grow into the room the file already leaves
// after each one (4 to 63 more), so nothing in the file moves; the edit is a
// span of the layout file in <game>\models, listed and revertible in the
// Edits tab, and part of the world.
class ZoneTools : public EditorExtension, public ViewportTool, public UndoParticipant {
public:
    explicit ZoneTools(ZoneToolsHost host) : host_(std::move(host)) {}
    ~ZoneTools() override { undo::Unregister(this); }

    // ---- UndoParticipant: Ctrl+Z puts a zone's span file back as it was
    // before the edit (or removes it, when there was none) and reopens.
    u64 LatestUndoStamp() const override { return undo_.empty() ? 0 : undo_.back().stamp; }
    bool UndoLatest() override;

    static constexpr int kShowCommand = 44801;
    static constexpr int kFitCommand = 44802;

    // Every new piece (FRS_ models) made visible from the zones around it:
    // its section added to the list of each zone within kFitReach of it that
    // lacks it -- otherwise a road vanishes once the car crosses into a zone
    // that does not load the section it was placed in. One Ctrl+Z.
    void FitListsToPieces();
    static constexpr float kFitReach = 40.0f;
    static constexpr float kFitHoleReach = 150.0f;   // for a piece standing where no zone is

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override { selected_ = -1; vertex_ = -1; dragging_ = false; }
    size_t SceneSignature() const override { return shown_ ? 7 : 0; }
    void PopulateTree(SceneTreeBuilder& tree) override;
    void OnTreeItem(intptr_t code) override;
    bool StatusText(std::wstring& text) const override;
    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;
    void EditRows(std::vector<EditsPanel::ExtraRow>& rows) override;
    void RevertRows(const std::vector<size_t>& indices) override;
    void Tick(float deltaSeconds) override;   // FRS_ZONE_SELFTEST only

    // The edit mode (Tab, the Edit Mode button) on the picked zone: its
    // corners shown, a click picks one, the gizmo moves it.
    bool BeginEdit();
    bool Editing() const { return editing_; }
    bool HasPick() const { return shown_ && selected_ >= 0; }
    void EndEdit();

    // ---- ViewportTool: a click picks a zone while they are shown
    bool MouseDown(int x, int y, WPARAM keys) override;
    // A vertex of the picked zone's outline, moved with the gizmo.
    bool GizmoPivot(Vec3& pivot) override;
    void GizmoChanged(const Mat4& transform, bool done, bool keep) override;

private:
    void UpdateOverlay();
    void Select(int zone);
    bool WriteList(u32 zoneId, const std::vector<u32>& sections, std::wstring& error);
    bool WriteOutline(size_t zone, const std::vector<Vec3>& ring, std::wstring& error);
    std::vector<u8> CurrentBytes(u64 offset, u32 length) const;   // the layout as the game sees it
    // The height each corner of a ring is drawn and picked at: the ground
    // under it (or near it, a corner inside a building has none), else the
    // mean of the corners that found ground -- never tied to the camera, so
    // the outline does not climb as the view moves. Found heights are kept.
    std::vector<float> RingHeights(const std::vector<Vec3>& ring) const;
    bool CornerGround(float x, float y, float& z) const;
    mutable std::unordered_map<u64, float> heights_;
    int vertex_ = -1;                 // of the picked zone's ring
    bool editing_ = false;
    bool dragging_ = false;
    std::vector<Vec3> dragRing_;      // the ring as it is being dragged
    std::wstring SpanKey(u32 zoneId) const;
    nfs::World* World() const { return host_.world ? host_.world() : nullptr; }
    WorldRenderer& Renderer() const { return host_.viewport->GetRenderer(); }
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }

    ZoneToolsHost host_;
    bool shown_ = false;
    int  selected_ = -1;              // index into Preculler::Zones()
    std::vector<std::wstring> spans_; // edited lists on disk, for the Edits tab
    struct UndoStep {
        u64 stamp = 0;
        std::wstring path;          // the span file the edit wrote
        bool existed = false;       // and what it held before
        std::vector<u8> bytes;
        int zone = -1, vertex = -1; // picked again after the undo
    };
    std::vector<UndoStep> undo_;
    void RememberSpan(const std::wstring& key);   // before writing <key>.frssection
    u64 groupStamp_ = 0;   // several writes taken back by one Ctrl+Z
};

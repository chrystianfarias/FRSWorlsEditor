#pragma once

#include "core/Math.h"
#include "ui/EditsPanel.h"
#include "ui/ModelCatalog.h"
#include "ui/PropertiesPanel.h"

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class MainWindow;

// The scene tree while it is rebuilt, as an extension adds to it.
struct SceneTreeBuilder {
    // Adds a row under `parent` (TVI_ROOT for a branch of its own). `code` is
    // the extension's own, handed back to OnTreeItem when the row is clicked;
    // -1 makes a row that does nothing, as a branch's own row does.
    std::function<HTREEITEM(HTREEITEM parent, const std::wstring& text, intptr_t code)> insert;
    // Whether the sidebar's search keeps a row with this text.
    std::function<bool(const std::wstring& text)> matches;
    // Drops a branch the search left empty, and opens one it filled.
    std::function<void(HTREEITEM branch, size_t shown, bool open)> finish;
};

// Something the editor can do that is not part of this repository.
//
// The editor is built with whatever extensions CreateEditorExtensions returns:
// none in this repository (ui/NoExtensions.cpp), more in builds that link
// their own in. An extension adds menus and toolbar buttons of its own, draws
// and picks through its RenderExtension (render/RenderExtension.h), and gets
// the calls below at the points where the editor works with the selection,
// the scene tree, the Library tab and the Edits tab. Every hook has a default
// that does nothing, so an extension overrides only what it uses.
class EditorExtension {
public:
    virtual ~EditorExtension() = default;

    // ---- the window

    // Once the window and its controls exist: menus, toolbar buttons and
    // accelerators of its own.
    virtual void OnCreate() {}
    // A menu, toolbar or accelerator command. True when it was the extension's.
    virtual bool OnCommand(int id) { (void)id; return false; }
    virtual void UpdateMenuChecks(HMENU menu) { (void)menu; }
    virtual void Tick(float deltaSeconds) { (void)deltaSeconds; }

    // ---- the region

    virtual void OnRegionLoaded() {}
    virtual void OnRegionClosed() {}
    // File > Save World: whatever it has not written yet.
    virtual void Save() {}
    // The edits were just written to the game (Ctrl+Shift+S or Save World),
    // before the region reopens: a record kept beside them is written now.
    virtual void OnSaved() {}
    // Just before the editor starts the game: the moment to put into the
    // game's files what is still only in the editor.
    virtual void OnBeforeLaunch() {}
    // After OnBeforeLaunch: true when the extension set this launch up to go
    // somewhere of its own (a race), so the car is not put at the camera.
    virtual bool LaunchesElsewhere() const { return false; }
    // Inside the ImGui frame: something of the extension's own over the
    // view (the 3D area's rectangle, in screen pixels).
    virtual void DrawHud(float x, float y, float width, float height) {
        (void)x; (void)y; (void)width; (void)height;
    }

    // ---- the scene tree

    // Changes when what it lists changes, so the tree is rebuilt.
    virtual size_t SceneSignature() const { return 0; }
    virtual void PopulateTree(SceneTreeBuilder& tree) { (void)tree; }
    virtual void OnTreeItem(intptr_t code) { (void)code; }

    // ---- its own selection (WorldRenderer::SelectExtension)
    //
    // Each returns false when the extension has nothing selected, so the
    // editor goes on to its own selection.

    virtual bool StatusText(std::wstring& text) const { (void)text; return false; }
    virtual bool FillProperties(PropertiesPanel::View& view) { (void)view; return false; }
    // A value committed in the properties grid.
    virtual bool ApplyProperties() { return false; }
    virtual bool DeleteSelection() { return false; }
    virtual bool MoveSelectionToView() { return false; }
    // G and R on a point with a heading (Viewport::SetPointEditor).
    virtual bool GetPoint(Vec3& position, float& heading) { (void)position; (void)heading; return false; }
    virtual bool SetPoint(const Vec3& position, float heading, bool commit) {
        (void)position; (void)heading; (void)commit;
        return false;
    }
    // Ctrl+C on its selection; ClearClipboard when something else is copied.
    virtual bool Copy() { return false; }
    virtual void ClearClipboard() {}
    virtual bool Paste() { return false; }

    // ---- the Library tab

    // Rows of its own (Row::owner set to this).
    virtual void CatalogRows(std::vector<ModelCatalog::Row>& rows) { (void)rows; }
    // Double click on one of its rows, and the status line while it is selected.
    virtual void ActivateRow(const ModelCatalog::Row& row) { (void)row; }
    virtual bool DescribeRow(const ModelCatalog::Row& row, std::wstring& text) {
        (void)row; (void)text;
        return false;
    }
    // Dragging one of its rows onto the map: started, the ground under the
    // pointer as it moves, and let go -- `ground` null when it was let go
    // outside the view or cancelled.
    virtual bool BeginDrag(const ModelCatalog::Row& row) { (void)row; return false; }
    virtual void UpdateDrag(const Vec3& ground) { (void)ground; }
    virtual void EndDrag(const Vec3* ground) { (void)ground; }

    // ---- the Edits tab

    // Rows of its own (ExtraRow::owner set to this), and the ticked ones of
    // them to take back.
    virtual void EditRows(std::vector<EditsPanel::ExtraRow>& rows) { (void)rows; }
    virtual void RevertRows(const std::vector<size_t>& indices) { (void)indices; }
};

// The extensions this build of the editor has, in the order they are asked.
std::vector<std::unique_ptr<EditorExtension>> CreateEditorExtensions(MainWindow& window);

#pragma once

#include "core/Math.h"

#include <windows.h>

#include <string>
#include <vector>

// The selected object's properties, docked on the right of the viewport.
//
// A property grid (third_party/propertygrid) in the manner of the one Visual
// Studio uses: named rows in collapsible groups, shown by group or
// alphabetically, with a description of the row under the cursor. Placed
// objects can be edited here -- position, heading, pitch, roll, scale -- and
// the view follows each committed value. The map's own objects show the same
// facts read only, with the one action they allow: removal from the world.
class PropertiesPanel {
public:
    // Extension: something an editor extension shows (ui/EditorExtension.h),
    // with the two buttons labelled by the view.
    enum class Mode { Nothing, Placement, Native, Library, Several, Extension };

    // One row: its group, name, value, what it means, and whether it can be
    // edited. `field` names the transform rows (1..7); 0 for the rest.
    struct Row {
        std::wstring group, name, value, description;
        bool editable = false;
        int  field = 0;
        // An editable row with choices is a drop-down list of them.
        std::vector<std::wstring> choices;
    };

    struct View {
        Mode mode = Mode::Nothing;
        std::wstring title;
        std::vector<Row> rows;
        Mat4 transform;
        bool removed = false;
        // Mode::Extension: what the two buttons say, empty to disable one.
        std::wstring moveLabel, removeLabel;
    };

    void Create(HWND parent, HINSTANCE instance, HFONT font);
    void Layout(int x, int y, int width, int height);

    // Shows `view`. The grid is rebuilt only when the object or its rows
    // change; a transform that moved -- under G or R -- updates the values in
    // place, leaving alone whatever is being typed.
    void Show(const View& view);

    // The transform in the rows, when they all hold numbers.
    bool Read(Mat4& transform) const;

    // The text in the row named by `field`, and the same read as a number.
    bool ReadText(int field, std::wstring& text) const;
    bool ReadNumber(int field, float& value) const;

    // Routed from the parent: the grid's change notification, and the two
    // view buttons. True when the notification was the grid's.
    bool OnNotify(const NMHDR* header);
    bool OnCommand(int id);
    bool TakeChange() { const bool changed = changed_; changed_ = false; return changed; }

    // Rows for a transform, editable or not.
    static void AddTransformRows(View& view, bool editable);

private:
    void Rebuild();
    int  FindField(int field) const;

    HWND title_ = nullptr, grid_ = nullptr;
    HWND byGroup_ = nullptr, alphabetical_ = nullptr;
    HWND snap_ = nullptr, remove_ = nullptr, blender_ = nullptr;
    View shown_;
    bool haveShown_ = false;
    bool sorted_ = false;
    bool changed_ = false;
};

#pragma once

#include "core/Math.h"

#include <windows.h>
#include <d3d9.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The selected object's properties: a dockable ImGui panel ("Properties")
// over the viewport (ui/ImGuiLayer.h).
//
// Named rows in collapsible groups, or one list in name order, with the
// description of the row under the pointer at the foot. Placed objects can
// be edited here -- position, heading, pitch, roll, scale -- and the view
// follows each committed value; an editor extension shows rows of its own,
// editable ones included, and names the two action buttons.
//
// The interface is the one the native grid had, so the editor and its
// extensions fill it the same way: Show a View, read committed values back
// with Read / ReadText / ReadNumber. A commit and the buttons reach the
// parent window as WM_COMMAND, posted -- never sent from inside a frame.
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
        // The value names a picture the image provider knows (a texture, a
        // model): it is shown under the rows, and the choices with theirs.
        bool preview = false;
    };

    struct View {
        Mode mode = Mode::Nothing;
        std::wstring title;
        std::vector<Row> rows;
        Mat4 transform;
        bool removed = false;
        // Mode::Extension: what the two buttons say, empty to disable one.
        std::wstring moveLabel, removeLabel;
        // Mode::Extension: more buttons under those, each posting its command
        // to the parent window as a menu item would.
        struct Action {
            std::wstring label;
            int command = 0;
            std::wstring group;     // buttons of a group sit together, two to a row
            std::wstring help;      // shown at the foot while the pointer is on it
        };
        std::vector<Action> actions;
    };

    // `parent` receives the commands. The font is unused: ImGui has its own.
    void Create(HWND parent, HINSTANCE instance, HFONT font);
    // Nothing to lay out: the panel docks inside the viewport.
    void Layout(int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }

    // Shows `view`. Rows are rebuilt only when the object or its rows change;
    // a transform that moved -- under G or R -- updates the values in place,
    // leaving alone whatever is being typed.
    void Show(const View& view);

    // Draws the panel; from the viewport's UI pass.
    void Draw(IDirect3DDevice9* device);

    // The transform in the rows, when they all hold numbers.
    bool Read(Mat4& transform) const;

    // The text in the row named by `field`, and the same read as a number.
    bool ReadText(int field, std::wstring& text) const;
    bool ReadNumber(int field, float& value) const;

    // A committed value arrives as the command IDC_PROP_GRID; OnCommand takes
    // it, and then the change handler runs (the editor's ApplyProperties).
    void SetChangeHandler(std::function<void()> handler) { onChange_ = std::move(handler); }
    bool OnNotify(const NMHDR* header) { (void)header; return false; }
    bool OnCommand(int id);
    bool TakeChange() { const bool changed = changed_; changed_ = false; return changed; }
    // The field of the row committed last (0 for none): with several objects
    // shown at once, only that value goes to all of them.
    int LastField() const { return lastField_; }

    // Pictures for names -- textures, models -- fitted into `size` pixels
    // square. The provider keeps what it returns; null when it has none.
    using ImageProvider = std::function<HBITMAP(const std::wstring& name, int size)>;
    static void SetImageProvider(ImageProvider provider);
    // Drops the textures made from the provider's bitmaps. The provider frees
    // its bitmaps with the region, and Windows may hand a freed handle out
    // again for another picture -- so the cache goes when the region closes,
    // and whenever a provider is set.
    void ForgetPictures();

    // Rows for a transform, editable or not.
    static void AddTransformRows(View& view, bool editable);

private:
    void Post(int id) const;
    void Commit(size_t row);
    void DrawRow(size_t index, IDirect3DDevice9* device);
    // Position / rotation: three transform rows on one line, each with a
    // coloured axis tag that changes the value when dragged.
    void DrawVector(const char* label, const size_t rows[3], const char* const axes[3],
                    const unsigned colours[3], float speed);
    // A label that changes a number when dragged sideways (Shift faster,
    // Ctrl finer). `badge` 0 draws it as plain text. `live` commits while
    // dragging, not only on release.
    void DragLabel(const char* id, const char* text, unsigned badge, float width, size_t row,
                   float speed, bool live);
    // The text input of an editable row, committed on Enter or leaving it.
    void ValueInput(size_t row, bool numeric);
    void* Picture(IDirect3DDevice9* device, const std::wstring& name, int size);

    HWND parent_ = nullptr;
    View shown_;
    std::vector<std::wstring> values_;   // per row, as edited
    std::vector<std::string> buffers_;   // per row, the UTF-8 text an input edits
    bool haveShown_ = false;
    bool sorted_ = false;
    bool changed_ = false;
    int  lastField_ = 0;
    int  activeRow_ = -1;                // the row being typed into, or dragged
    bool dragging_ = false;              // a label drag is changing activeRow_
    double lastLiveCommit_ = 0.0;
    std::wstring hoverHelp_;
    std::function<void()> onChange_;
    std::unordered_map<HBITMAP, IDirect3DTexture9*> textures_;
    unsigned picturesGeneration_ = 0;    // the provider generation they were made in
};

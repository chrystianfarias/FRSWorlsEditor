#include "ui/PropertiesPanel.h"

#include "nfs/SceneryEdit.h"
#include "ui/Resource.h"

#include "../../third_party/propertygrid/propertyGrid.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace {

const wchar_t* kFieldNames[8] = { L"", L"X", L"Y", L"Z (height)", L"Heading", L"Pitch", L"Roll", L"Scale" };
const wchar_t* kFieldHelp[8] = {
    L"",
    L"World position along X, in game units.",
    L"World position along Y, in game units.",
    L"Height of the object's origin. Ctrl+G or Snap to ground drops it onto what is below.",
    L"Turn about the vertical, in degrees. R in the view turns it with the mouse.",
    L"Tilt about Y, in degrees.",
    L"Tilt about X, in degrees.",
    L"Uniform scale, 0.01 to 3.9: the file keeps the matrix in fixed point.",
};

bool SameRows(const std::vector<PropertiesPanel::Row>& a, const std::vector<PropertiesPanel::Row>& b,
              bool ignoreFields) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].group != b[i].group || a[i].name != b[i].name || a[i].editable != b[i].editable ||
            a[i].field != b[i].field || a[i].choices != b[i].choices)
            return false;
        if ((!ignoreFields || a[i].field == 0) && a[i].value != b[i].value) return false;
    }
    return true;
}

// The grid's form of a list: each string followed by its null, and one more.
std::wstring ChoiceList(const std::vector<std::wstring>& choices) {
    std::wstring list;
    for (const std::wstring& choice : choices) {
        list += choice;
        list += wchar_t(0);
    }
    list += wchar_t(0);
    return list;
}

int ItemType(const PropertiesPanel::Row& row) {
    if (!row.editable) return PIT_STATIC;
    return row.choices.empty() ? PIT_EDIT : PIT_COMBO;
}

} // namespace

void PropertiesPanel::AddTransformRows(View& view, bool editable) {
    Vec3 position;
    float heading, pitch, roll, scale;
    nfs::DecomposePlacement(view.transform, position, heading, pitch, roll, scale);
    const float values[8] = { 0, position.x, position.y, position.z, heading, pitch, roll, scale };
    for (int field = 1; field <= 7; ++field) {
        wchar_t text[32];
        ::swprintf_s(text, L"%.3f", values[field]);
        Row row;
        row.group = L"Transform";
        row.name = kFieldNames[field];
        row.value = text;
        row.description = kFieldHelp[field];
        row.editable = editable;
        row.field = field;
        view.rows.push_back(std::move(row));
    }
}

void PropertiesPanel::Create(HWND parent, HINSTANCE instance, HFONT font) {
    title_ = ::CreateWindowExW(0, L"STATIC", L"Nothing selected", WS_CHILD | WS_VISIBLE | SS_LEFT |
                               SS_ENDELLIPSIS, 0, 0, 10, 10, parent, (HMENU)IDC_PROP_TITLE, instance, nullptr);
    byGroup_ = ::CreateWindowExW(0, L"BUTTON", L"Categorized", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                 0, 0, 10, 10, parent, (HMENU)IDC_PROP_CATEGORIZED, instance, nullptr);
    alphabetical_ = ::CreateWindowExW(0, L"BUTTON", L"Alphabetical", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                      0, 0, 10, 10, parent, (HMENU)IDC_PROP_ALPHABETICAL, instance, nullptr);
    grid_ = New_PropertyGrid(parent, IDC_PROP_GRID);
    snap_ = ::CreateWindowExW(0, L"BUTTON", L"Snap to ground", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, 10, 10, parent, (HMENU)IDC_PROP_SNAP, instance, nullptr);
    remove_ = ::CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, 10, 10, parent, (HMENU)IDC_PROP_REMOVE, instance, nullptr);
    blender_ = ::CreateWindowExW(0, L"BUTTON", L"Edit in Blender", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 10, 10, parent, (HMENU)IDC_PROP_BLENDER, instance, nullptr);

    for (HWND control : { title_, byGroup_, alphabetical_, grid_, snap_, remove_, blender_ })
        if (control) ::SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
    ::SendMessageW(byGroup_, BM_SETCHECK, BST_CHECKED, 0);
    if (grid_) {
        PropGrid_ShowToolTips(grid_, TRUE);
        PropGrid_ShowPropertyDescriptions(grid_, TRUE);
        PropGrid_SetFlatStyleChecks(grid_, TRUE);
    }
    Show(View{});
}

void PropertiesPanel::Layout(int x, int y, int width, int height) {
    const int pad = 6, gap = 4;
    const int inner = width - 2 * pad;
    int at = y + pad;
    ::MoveWindow(title_, x + pad, at, inner, 18, TRUE);
    at += 22;
    const int half = (inner - gap) / 2;
    ::MoveWindow(byGroup_, x + pad, at, half, 24, TRUE);
    ::MoveWindow(alphabetical_, x + pad + half + gap, at, inner - half - gap, 24, TRUE);
    at += 28;
    const int buttons = 3 * 26 + 3 * gap + pad;
    const int gridHeight = std::max(80, height - (at - y) - buttons);
    if (grid_) ::MoveWindow(grid_, x + pad, at, inner, gridHeight, TRUE);
    at += gridHeight + gap;
    ::MoveWindow(snap_, x + pad, at, inner, 26, TRUE);
    at += 26 + gap;
    ::MoveWindow(remove_, x + pad, at, inner, 26, TRUE);
    at += 26 + gap;
    ::MoveWindow(blender_, x + pad, at, inner, 26, TRUE);
}

void PropertiesPanel::Rebuild() {
    if (!grid_) return;
    ::SendMessageW(grid_, WM_SETREDRAW, FALSE, 0);
    PropGrid_ResetContent(grid_);

    std::vector<Row> rows = shown_.rows;
    if (sorted_) {
        // One group, names in order, as Visual Studio's alphabetical view.
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return ::_wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        for (Row& row : rows) row.group = L"Properties";
    }

    for (Row& row : rows) {
        PROPGRIDITEM item;
        PropGrid_ItemInit(item);
        item.lpszCatalog = const_cast<LPTSTR>(row.group.c_str());
        item.lpszPropName = const_cast<LPTSTR>(row.name.c_str());
        item.lpszPropDesc = const_cast<LPTSTR>(row.description.c_str());
        item.lpCurValue = (LPARAM)row.value.c_str();
        item.lpUserData = (LPVOID)(INT_PTR)row.field;
        item.iItemType = ItemType(row);
        const std::wstring choices = ChoiceList(row.choices);
        if (item.iItemType == PIT_COMBO) item.lpszzCmbItems = const_cast<LPTSTR>(choices.c_str());
        PropGrid_AddItem(grid_, &item);
    }
    PropGrid_ExpandAllCatalogs(grid_);
    ::SendMessageW(grid_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(grid_, nullptr, TRUE);
}

int PropertiesPanel::FindField(int field) const {
    if (!grid_) return -1;
    const int count = PropGrid_GetCount(grid_);
    for (int i = 0; i < count; ++i) {
        const LPPROPGRIDITEM item = PropGrid_GetItemData(grid_, i);
        if (item && item != (LPPROPGRIDITEM)LB_ERR && (int)(INT_PTR)item->lpUserData == field) return i;
    }
    return -1;
}

void PropertiesPanel::Show(const View& view) {
    const bool sameObject = haveShown_ && view.mode == shown_.mode && view.title == shown_.title &&
                            view.removed == shown_.removed;

    if (sameObject && SameRows(view.rows, shown_.rows, false)) return;

    if (sameObject && SameRows(view.rows, shown_.rows, true)) {
        // Only the transform moved: the values change in place, except in a
        // row someone is typing into.
        const HWND focus = ::GetFocus();
        const bool typing = grid_ && focus && ::IsChild(grid_, focus);
        if (!typing) {
            for (size_t i = 0; i < view.rows.size(); ++i) {
                const Row& row = view.rows[i];
                if (row.field == 0 || row.value == shown_.rows[i].value) continue;
                const int index = FindField(row.field);
                if (index < 0) continue;
                PROPGRIDITEM item;
                PropGrid_ItemInit(item);
                item.lpszCatalog = const_cast<LPTSTR>(row.group.c_str());
                item.lpszPropName = const_cast<LPTSTR>(row.name.c_str());
                item.lpszPropDesc = const_cast<LPTSTR>(row.description.c_str());
                item.lpCurValue = (LPARAM)row.value.c_str();
                item.lpUserData = (LPVOID)(INT_PTR)row.field;
                item.iItemType = ItemType(row);
                const std::wstring choices = ChoiceList(row.choices);
                if (item.iItemType == PIT_COMBO) item.lpszzCmbItems = const_cast<LPTSTR>(choices.c_str());
                PropGrid_SetItemData(grid_, index, &item);
            }
            shown_ = view;
        }
        return;
    }

    shown_ = view;
    haveShown_ = true;
    ::SetWindowTextW(title_, view.title.empty() ? L"Nothing selected" : view.title.c_str());
    Rebuild();

    // An extension's object names its own two actions; an empty label is no action.
    const bool extension = view.mode == Mode::Extension;
    ::EnableWindow(snap_, view.mode == Mode::Placement || (extension && !view.moveLabel.empty()));
    ::SetWindowTextW(snap_, extension && !view.moveLabel.empty() ? view.moveLabel.c_str() : L"Snap to ground");
    const bool canRemove = view.mode == Mode::Placement || view.mode == Mode::Native ||
                           view.mode == Mode::Several || (extension && !view.removeLabel.empty());
    ::EnableWindow(remove_, canRemove && !view.removed);
    // Any mesh can go to Blender: a placed one, one of the map's own, several.
    ::EnableWindow(blender_, view.mode == Mode::Placement || view.mode == Mode::Native ||
                             view.mode == Mode::Library || view.mode == Mode::Several);
    ::SetWindowTextW(remove_, view.mode == Mode::Native ? L"Remove from the world"
                              : extension && !view.removeLabel.empty() ? view.removeLabel.c_str()
                                                                        : L"Delete");
}

bool PropertiesPanel::Read(Mat4& transform) const {
    float values[8] = {};
    for (int field = 1; field <= 7; ++field) {
        const int index = FindField(field);
        if (index < 0) return false;
        const LPPROPGRIDITEM item = PropGrid_GetItemData(grid_, index);
        if (!item || item == (LPPROPGRIDITEM)LB_ERR || !item->lpCurValue) return false;
        wchar_t text[64] = {};
        ::wcsncpy_s(text, (const wchar_t*)item->lpCurValue, _TRUNCATE);
        for (wchar_t& c : text) if (c == L',') c = L'.';
        wchar_t* end = nullptr;
        values[field] = std::wcstof(text, &end);
        if (end == text) return false;
    }
    if (values[7] <= 0.01f || values[7] > 3.9f) return false;
    transform = nfs::ComposePlacement({ values[1], values[2], values[3] }, values[4], values[5],
                                      values[6], values[7]);
    return true;
}

bool PropertiesPanel::ReadText(int field, std::wstring& text) const {
    const int index = FindField(field);
    if (index < 0) return false;
    const LPPROPGRIDITEM item = PropGrid_GetItemData(grid_, index);
    if (!item || item == (LPPROPGRIDITEM)LB_ERR || !item->lpCurValue) return false;
    text = (const wchar_t*)item->lpCurValue;
    return true;
}

bool PropertiesPanel::ReadNumber(int field, float& value) const {
    std::wstring text;
    if (!ReadText(field, text)) return false;
    for (wchar_t& c : text) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    value = std::wcstof(text.c_str(), &end);
    return end != text.c_str();
}

bool PropertiesPanel::OnNotify(const NMHDR* header) {
    if (!grid_ || header->hwndFrom != grid_ || header->code != PGN_PROPERTYCHANGE) return false;
    changed_ = true;
    return true;
}

bool PropertiesPanel::OnCommand(int id) {
    if (id != IDC_PROP_CATEGORIZED && id != IDC_PROP_ALPHABETICAL) return false;
    const bool sorted = id == IDC_PROP_ALPHABETICAL;
    if (sorted != sorted_) {
        sorted_ = sorted;
        Rebuild();
    }
    return true;
}

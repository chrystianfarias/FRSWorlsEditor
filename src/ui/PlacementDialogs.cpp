#include "ui/PlacementDialogs.h"

#include "nfs/ModelCategory.h"
#include "nfs/SceneryEdit.h"
#include "nfs/World.h"
#include "render/ModelThumbnails.h"
#include "ui/ModelCatalog.h"
#include "ui/Resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>
#include <vector>

namespace {

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)std::towlower(c);
    return text;
}

// --- choosing a model --------------------------------------------------------
//
// The catalogue itself lives in ModelCatalog, shared with the sidebar's
// Library tab; this is the dialog around it.

constexpr UINT_PTR kPictureTimer = 1;

struct ChooseState {
    const nfs::World* world = nullptr;
    size_t target = SIZE_MAX;
    ModelCatalog catalog;
    u32 chosen = 0;
    bool copy = false;
    bool onlyMine = false;
    ModelThumbnailFn thumbnail;
};

bool PlaceableHere(const ChooseState& state, const ModelCatalog::Row& row) {
    return row.library || (state.target < state.world->Loaded().size() &&
                           state.world->Loaded()[state.target]->HoldsModel(row.model->hash));
}

void ShowDetails(HWND dialog, const ChooseState& state) {
    std::wstring text;
    if (const ModelCatalog::Row* row = state.catalog.Selected()) {
        const bool here = PlaceableHere(state, *row);
        text = row->name + L"  --  " + std::to_wstring(row->model->TriangleCount()) +
               L" triangles, stored in " + row->home + L", " +
               (row->library ? L"can go anywhere (library)"
                             : here ? L"can go here (in this section)" : L"copied in from " + row->home);
    } else {
        text = std::to_wstring(state.catalog.ShownCount()) + L" models";
    }
    ::SetDlgItemTextW(dialog, IDC_PLACE_INFO, text.c_str());
}

INT_PTR CALLBACK ChooseProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = (ChooseState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
        case WM_INITDIALOG: {
            state = (ChooseState*)lParam;
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)state);

            state->catalog.Attach(::GetDlgItem(dialog, IDC_PLACE_LIST),
                                  ::GetDlgItem(dialog, IDC_PLACE_CATEGORY),
                                  ::GetDlgItem(dialog, IDC_PLACE_FILTER),
                                  ::GetDlgItem(dialog, IDC_PLACE_USED));
            state->catalog.SetThumbnails(state->thumbnail);
            state->catalog.onSelect = [dialog, state]() { ShowDetails(dialog, *state); };
            state->catalog.onActivate = [dialog]() { ::PostMessageW(dialog, WM_COMMAND, IDOK, 0); };
            state->catalog.SetWorld(state->world);
            if (state->onlyMine) state->catalog.ShowShelf(L"My objects");

            const nfs::LoadedSection* target =
                state->target < state->world->Loaded().size() ? state->world->Loaded()[state->target].get()
                                                              : nullptr;
            const std::wstring title = L"Place an object in section " +
                                       (target && target->info ? Widen(target->info->name) : L"?");
            ::SetWindowTextW(dialog, title.c_str());
            ::SetTimer(dialog, kPictureTimer, 15, nullptr);
            ::SetFocus(::GetDlgItem(dialog, IDC_PLACE_FILTER));
            return FALSE;
        }

        case WM_TIMER:
            if (wParam == kPictureTimer) state->catalog.DrawPending(30);
            return TRUE;

        case WM_DESTROY:
            ::KillTimer(dialog, kPictureTimer);
            break;

        case WM_COMMAND:
            if (state->catalog.OnCommand((HWND)lParam, HIWORD(wParam))) return TRUE;
            if (LOWORD(wParam) == IDOK) {
                const ModelCatalog::Row* row = state->catalog.Selected();
                if (!row && state->catalog.ShownCount() == 1) {
                    ListView_SetItemState(::GetDlgItem(dialog, IDC_PLACE_LIST), 0, LVIS_SELECTED, LVIS_SELECTED);
                    row = state->catalog.Selected();
                }
                if (!row) {
                    ::MessageBoxW(dialog, L"Pick a model in the list first.", L"Place object",
                                  MB_ICONINFORMATION | MB_OK);
                    return TRUE;
                }
                // One stored in another positioned section is copied into
                // this one when the placements are written.
                state->copy = !PlaceableHere(*state, *row);
                state->chosen = row->model->hash;
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;

        case WM_NOTIFY:
            if (state && state->catalog.OnNotify((const NMHDR*)lParam)) {
                ::SetWindowLongPtrW(dialog, DWLP_MSGRESULT, 0);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// --- editing a placement -----------------------------------------------------

struct EditState {
    PlacementEdit* edit = nullptr;
    Mat4 original;
    bool filling = false;
};

void Show(HWND dialog, const Mat4& transform) {
    auto* state = (EditState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);
    Vec3 position;
    float heading, pitch, roll, scale;
    nfs::DecomposePlacement(transform, position, heading, pitch, roll, scale);

    state->filling = true;
    const struct { int id; float value; } fields[] = {
        { IDC_PL_X, position.x }, { IDC_PL_Y, position.y }, { IDC_PL_Z, position.z },
        { IDC_PL_HEADING, heading }, { IDC_PL_PITCH, pitch }, { IDC_PL_ROLL, roll },
        { IDC_PL_SCALE, scale } };
    for (const auto& field : fields) {
        wchar_t text[32];
        ::swprintf_s(text, L"%.3f", field.value);
        ::SetDlgItemTextW(dialog, field.id, text);
    }
    state->filling = false;
}

bool Read(HWND dialog, Mat4& out) {
    float values[7];
    const int ids[7] = { IDC_PL_X, IDC_PL_Y, IDC_PL_Z, IDC_PL_HEADING, IDC_PL_PITCH,
                         IDC_PL_ROLL, IDC_PL_SCALE };
    for (int i = 0; i < 7; ++i) {
        wchar_t text[64] = {};
        ::GetDlgItemTextW(dialog, ids[i], text, 64);
        for (wchar_t& c : text) if (c == L',') c = L'.';
        wchar_t* end = nullptr;
        values[i] = std::wcstof(text, &end);
        if (end == text) return false;
    }
    if (values[6] <= 0.01f || values[6] > 3.9f) return false;
    out = nfs::ComposePlacement({ values[0], values[1], values[2] }, values[3], values[4],
                                values[5], values[6]);
    return true;
}

INT_PTR CALLBACK EditProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = (EditState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
        case WM_INITDIALOG:
            state = (EditState*)lParam;
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)state);
            ::SetDlgItemTextW(dialog, IDC_PL_INFO, state->edit->description.c_str());
            Show(dialog, state->edit->transform);
            return TRUE;

        case WM_COMMAND: {
            if (!state) break;
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            // Every keystroke that leaves a readable transform is shown at once.
            if (code == EN_CHANGE && !state->filling && id >= IDC_PL_X && id <= IDC_PL_SCALE) {
                Mat4 transform;
                if (Read(dialog, transform)) {
                    state->edit->transform = transform;
                    state->edit->apply(transform);
                }
                return TRUE;
            }
            if (id == IDC_PL_SNAP) {
                Mat4 transform;
                if (!Read(dialog, transform)) transform = state->edit->transform;
                if (state->edit->snap && state->edit->snap(transform)) {
                    state->edit->transform = transform;
                    state->edit->apply(transform);
                    Show(dialog, transform);
                } else {
                    ::MessageBoxW(dialog, L"There is no ground under it.", L"Placement",
                                  MB_ICONINFORMATION | MB_OK);
                }
                return TRUE;
            }
            if (id == IDOK) {
                Mat4 transform;
                if (!Read(dialog, transform)) {
                    ::MessageBoxW(dialog, L"Every field needs a number, and the scale must be "
                                          L"between 0.01 and 3.9.", L"Placement",
                                  MB_ICONINFORMATION | MB_OK);
                    return TRUE;
                }
                state->edit->transform = transform;
                state->edit->apply(transform);
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                state->edit->transform = state->original;
                state->edit->apply(state->original);
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

} // namespace

bool ChooseModelToPlace(HINSTANCE instance, HWND parent, const nfs::World& world,
                        size_t targetSection, u32& modelHash, bool& copyIntoSection,
                        bool onlyMine, ModelThumbnailFn thumbnail) {
    ChooseState state;
    state.world = &world;
    state.target = targetSection;
    state.onlyMine = onlyMine;
    state.thumbnail = std::move(thumbnail);
    if (::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_PLACE_MODEL), parent, &ChooseProc,
                          (LPARAM)&state) != IDOK)
        return false;
    modelHash = state.chosen;
    copyIntoSection = state.copy;
    return modelHash != 0;
}

// --- deleting my objects -----------------------------------------------------

struct MineState {
    struct Row { u32 hash = 0; std::wstring name, sections; size_t placed = 0; u32 triangles = 0; };
    std::vector<Row> rows;
    std::vector<u32> ticked;
};

INT_PTR CALLBACK MineProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = (MineState*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);
    switch (message) {
        case WM_INITDIALOG: {
            state = (MineState*)lParam;
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)state);
            HWND list = ::GetDlgItem(dialog, IDC_MINE_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                        LVS_EX_DOUBLEBUFFER);
            const struct { const wchar_t* title; int width; } columns[] = {
                { L"Object", 200 }, { L"Placed", 60 }, { L"Stored in", 150 }, { L"Triangles", 70 } };
            for (int c = 0; c < 4; ++c) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = (LPWSTR)columns[c].title;
                column.cx = columns[c].width;
                ListView_InsertColumn(list, c, &column);
            }
            for (size_t i = 0; i < state->rows.size(); ++i) {
                const MineState::Row& row = state->rows[i];
                LVITEMW item{};
                item.mask = LVIF_TEXT | LVIF_PARAM;
                item.iItem = (int)i;
                item.pszText = (LPWSTR)row.name.c_str();
                item.lParam = (LPARAM)i;
                const int at = ListView_InsertItem(list, &item);
                const std::wstring placed = std::to_wstring(row.placed);
                const std::wstring triangles = std::to_wstring(row.triangles);
                ListView_SetItemText(list, at, 1, (LPWSTR)placed.c_str());
                ListView_SetItemText(list, at, 2, (LPWSTR)row.sections.c_str());
                ListView_SetItemText(list, at, 3, (LPWSTR)triangles.c_str());
            }
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                HWND list = ::GetDlgItem(dialog, IDC_MINE_LIST);
                state->ticked.clear();
                for (int i = 0; i < ListView_GetItemCount(list); ++i)
                    if (ListView_GetCheckState(list, i)) state->ticked.push_back(state->rows[(size_t)i].hash);
                if (state->ticked.empty()) {
                    ::MessageBoxW(dialog, L"Tick the objects to delete first.", L"Delete my objects",
                                  MB_ICONINFORMATION | MB_OK);
                    return TRUE;
                }
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

bool ChooseMyObjectsToDelete(HINSTANCE instance, HWND parent, const nfs::World& world,
                             std::vector<u32>& hashes) {
    MineState state;
    for (const nfs::SolidObject* model : world.AllModels()) {
        if (!model || model->name.rfind("FRS_", 0) != 0) continue;
        MineState::Row row;
        row.hash = model->hash;
        row.name = Widen(model->name);
        row.triangles = model->TriangleCount();
        for (const auto& section : world.Loaded()) {
            if (!section->info || !section->HoldsModel(model->hash)) continue;
            if (!row.sections.empty()) row.sections += L", ";
            row.sections += Widen(section->info->name);
            for (const auto& placement : section->placements)
                if (placement.instance.modelHash == model->hash) ++row.placed;
        }
        state.rows.push_back(std::move(row));
    }
    if (state.rows.empty()) {
        ::MessageBoxW(parent, L"There are no objects of your own in this region yet.\n\n"
                              L"Make one with Objects > My Objects > New Object.",
                      L"Delete my objects", MB_ICONINFORMATION | MB_OK);
        return false;
    }
    std::sort(state.rows.begin(), state.rows.end(),
              [](const MineState::Row& a, const MineState::Row& b) { return a.name < b.name; });
    if (::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MY_OBJECTS), parent, &MineProc,
                          (LPARAM)&state) != IDOK)
        return false;
    hashes = state.ticked;
    return !hashes.empty();
}

bool EditPlacement(HINSTANCE instance, HWND parent, PlacementEdit& edit) {
    EditState state;
    state.edit = &edit;
    state.original = edit.transform;
    return ::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_PLACEMENT), parent, &EditProc,
                             (LPARAM)&state) == IDOK;
}

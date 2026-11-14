#include "ui/EditsPanel.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "io/SectionEdits.h"
#include "nfs/ChunkReader.h"
#include "nfs/World.h"
#include "ui/Resource.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// One line of the list.
struct Row {
    const nfs::Section* section = nullptr;   // null for a vertex overlay
    size_t edit = 0;                         // index into that section's edits
    std::wstring overlayPath;                // a .frsmesh file
    const void* extraOwner = nullptr;        // an extension's edit: its owner and index
    size_t extra = 0;
    std::wstring sectionName, kind, what, detail;
};

struct State {
    const nfs::World* world = nullptr;
    std::function<bool(const std::string&)> collisionLogged;
    std::wstring modelsFolder;
    std::vector<const nfs::Section*> sections;
    std::vector<std::vector<SectionEdit>> edits;     // parallel to `sections`
    std::vector<Row> rows;
    std::wstring summary;
    bool reverted = false;
};

// The textures an object chunk references.
std::vector<u32> ObjectTextures(const std::vector<u8>& chunk) {
    std::vector<u32> out;
    if (chunk.size() < 8) return out;
    nfs::Chunk object;
    object.id = nfs::kChunkSolidObject;
    object.raw = ByteSpan{ chunk.data() + 8, chunk.size() - 8 };
    object.data = object.raw;
    const nfs::Chunk refs = nfs::FindChunk(object, nfs::kChunkSolidObjectTexRefs);
    for (size_t at = 0; at + 8 <= refs.data.size; at += 8) out.push_back(refs.data.read<u32>(at));
    return out;
}

void Collect(State& state) {
    const nfs::World& world = *state.world;
    for (const auto& loaded : world.Loaded()) {
        if (!loaded->info) continue;
        std::vector<SectionEdit> edits;
        std::wstring error;
        if (!DiffSection(world, *loaded->info, edits, error)) {
            LOG_WARN("edits of %s could not be listed: %S", loaded->info->name.c_str(), error.c_str());
            continue;
        }
        if (edits.empty()) continue;
        state.sections.push_back(loaded->info);
        state.edits.push_back(std::move(edits));
    }
    for (size_t s = 0; s < state.sections.size(); ++s) {
        for (size_t e = 0; e < state.edits[s].size(); ++e) {
            const SectionEdit& edit = state.edits[s][e];
            if (edit.kind == SectionEdit::Kind::Collision && state.collisionLogged &&
                state.collisionLogged(state.sections[s]->name))
                continue;
            Row row;
            row.section = state.sections[s];
            row.edit = e;
            row.sectionName = Widen(row.section->name);
            row.kind = EditKindName(edit.kind);
            row.what = Widen(edit.label);
            row.detail = edit.detail;
            state.rows.push_back(std::move(row));
        }
    }

    std::vector<MeshOverlayInfo> overlays;
    ListMeshOverlays(state.modelsFolder, overlays);
    for (const MeshOverlayInfo& overlay : overlays) {
        if (Widen(overlay.region) != world.Region()) continue;
        Row row;
        row.overlayPath = overlay.path;
        row.sectionName = L"-";
        row.kind = L"Vertices moved";
        row.what = Widen(overlay.model);
        row.detail = std::to_wstring(overlay.vertexCount) + L" vertices";
        state.rows.push_back(std::move(row));
    }
}

// Reverts the ticked rows. Per section: what stays is every edit not ticked,
// less the ones that cannot outlive what was ticked. (Extension rows are
// taken out by EditsPanel::RevertTicked before this.)
bool RevertRows(HWND dialog, const std::set<size_t>& ticked, State& state) {
    if (ticked.empty()) {
        ::MessageBoxW(dialog, L"Tick the edits to revert first.", L"Region edits",
                      MB_ICONINFORMATION | MB_OK);
        return false;
    }

    std::wostringstream notes;
    std::vector<SectionSpan> spans;
    std::vector<std::wstring> superseded;
    std::vector<SpanOverlayInfo> existing;
    ListSpanOverlays(state.modelsFolder, state.world->Region(), existing);
    size_t reverted = 0;

    for (size_t s = 0; s < state.sections.size(); ++s) {
        const nfs::Section& section = *state.sections[s];
        const std::vector<SectionEdit>& edits = state.edits[s];
        std::vector<bool> drop(edits.size(), false);
        for (const Row& row : state.rows)
            if (row.section == &section)
                drop[row.edit] = ticked.count((size_t)(&row - state.rows.data())) != 0;
        if (std::none_of(drop.begin(), drop.end(), [](bool d) { return d; })) continue;

        // A model copied in takes the placements that use it along.
        for (size_t e = 0; e < edits.size(); ++e) {
            if (!drop[e] || edits[e].kind != SectionEdit::Kind::ModelAdded) continue;
            for (size_t p = 0; p < edits.size(); ++p)
                if (!drop[p] && edits[p].kind == SectionEdit::Kind::Placement &&
                    edits[p].placement.modelHash == edits[e].hash) {
                    drop[p] = true;
                    notes << Widen(section.name) << L": the placed " << Widen(edits[p].label)
                          << L" goes with its model.\n";
                }
        }

        // Textures follow the objects that use them: one nothing kept refers
        // to goes, one something kept still needs stays.
        std::set<u32> needed;
        const nfs::LoadedSection* loaded = nullptr;
        for (const auto& candidate : state.world->Loaded())
            if (candidate->info == &section) loaded = candidate.get();
        std::set<u32> droppedObjects;
        for (size_t e = 0; e < edits.size(); ++e)
            if (drop[e] && edits[e].kind == SectionEdit::Kind::ModelAdded) droppedObjects.insert(edits[e].hash);
        if (loaded)
            for (const nfs::SolidList& solids : loaded->solidLists)
                for (const nfs::SolidObject& object : solids.objects)
                    if (!droppedObjects.count(object.hash))
                        needed.insert(object.textureHashes.begin(), object.textureHashes.end());
        for (size_t e = 0; e < edits.size(); ++e) {
            const bool texture = edits[e].kind == SectionEdit::Kind::TextureAdded;
            if (!texture) continue;
            if (drop[e] && needed.count(edits[e].hash)) {
                drop[e] = false;
                notes << Widen(section.name) << L": the texture " << Widen(edits[e].label)
                      << L" stays, an object kept in the section uses it.\n";
            } else if (!drop[e] && !needed.count(edits[e].hash) && !droppedObjects.empty()) {
                bool usedByDropped = false;
                for (size_t m = 0; m < edits.size(); ++m)
                    if (drop[m] && edits[m].kind == SectionEdit::Kind::ModelAdded)
                        for (const u32 t : ObjectTextures(edits[m].chunk))
                            if (t == edits[e].hash) usedByDropped = true;
                if (usedByDropped) {
                    drop[e] = true;
                    notes << Widen(section.name) << L": the texture " << Widen(edits[e].label)
                          << L" goes with the model that brought it.\n";
                }
            }
        }

        std::vector<SectionEdit> keep;
        for (size_t e = 0; e < edits.size(); ++e) {
            if (drop[e]) { ++reverted; continue; }
            keep.push_back(edits[e]);
        }

        SectionSpanPlan plan;
        std::wstring error;
        if (!PlanSectionFromEdits(*state.world, section, keep, existing, plan, error)) {
            ::MessageBoxW(dialog, (Widen(section.name) + L" could not be rebuilt: " + error +
                                   L"\n\nNothing was changed.").c_str(),
                          L"Region edits", MB_ICONWARNING | MB_OK);
            return false;
        }
        for (SectionSpan& span : plan.spans) spans.push_back(std::move(span));
        for (std::wstring& path : plan.superseded) superseded.push_back(std::move(path));
        if (keep.empty()) notes << Widen(section.name) << L": back to what the game shipped.\n";
    }

    const size_t written = WriteSectionSpans(spans, superseded, state.modelsFolder, state.world->Region());
    if (written != spans.size()) {
        ::MessageBoxW(dialog, L"Some overlays could not be written; the old ones were kept. "
                              L"The log has why.", L"Region edits", MB_ICONWARNING | MB_OK);
        return false;
    }

    for (const size_t index : ticked) {
        const Row& row = state.rows[index];
        if (row.overlayPath.empty()) continue;
        if (::DeleteFileW(row.overlayPath.c_str())) ++reverted;
        else notes << L"Could not remove " << row.overlayPath << L".\n";
    }

    state.summary = std::to_wstring(reverted) + L" edits reverted.\n\n" + notes.str();
    state.reverted = true;
    LOG_INFO("region edits: %zu reverted", reverted);
    return true;
}

} // namespace

struct EditsPanel::Impl {
    State state;
};

EditsPanel::EditsPanel() : impl_(std::make_unique<Impl>()) {}
EditsPanel::~EditsPanel() = default;

void EditsPanel::Create(HWND parent, HINSTANCE instance, HFONT font) {
    parent_ = parent;
    title_ = ::CreateWindowExW(0, L"STATIC", L"Edits in force", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               0, 0, 10, 10, parent, (HMENU)IDC_EDITS_INFO, instance, nullptr);
    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, parent, (HMENU)IDC_EDITS_LIST, instance, nullptr);
    revert_ = ::CreateWindowExW(0, L"BUTTON", L"Revert ticked", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, 10, 10, parent, (HMENU)IDC_EDITS_REVERT, instance, nullptr);
    refresh_ = ::CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 10, 10, parent, (HMENU)IDC_EDITS_REFRESH, instance, nullptr);
    for (HWND control : { title_, list_, revert_, refresh_ })
        ::SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                                 LVS_EX_DOUBLEBUFFER);
    // What first: in a sidebar column it is the part that must show.
    const struct { const wchar_t* title; int width; } columns[] = {
        { L"Section", 50 }, { L"Change", 100 }, { L"What", 170 }, { L"Detail", 200 } };
    for (int c = 0; c < 4; ++c) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = (LPWSTR)columns[c].title;
        column.cx = columns[c].width;
        ListView_InsertColumn(list_, c, &column);
    }
}

void EditsPanel::Layout(int x, int y, int width, int height) {
    // A sidebar column: the count over two lines, the buttons side by side
    // under it, and the list filling the rest.
    const int row = 24, gap = 4, titleHeight = 32;
    const int half = (width - gap) / 2;
    ::MoveWindow(title_, x, y, width, titleHeight, TRUE);
    ::MoveWindow(refresh_, x, y + titleHeight + gap, half, row, TRUE);
    ::MoveWindow(revert_, x + half + gap, y + titleHeight + gap, width - half - gap, row, TRUE);
    const int listTop = y + titleHeight + gap + row + gap;
    ::MoveWindow(list_, x, listTop, width, std::max(0, y + height - listTop), TRUE);
}

void EditsPanel::Show(bool visible) {
    for (HWND control : { title_, list_, revert_, refresh_ })
        ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

size_t EditsPanel::Count() const {
    return impl_->state.rows.size();
}

void EditsPanel::Refresh(const nfs::World* world, const std::wstring& modelsFolder) {
    impl_->state = State{};
    ListView_DeleteAllItems(list_);
    if (!world) {
        ::SetWindowTextW(title_, L"Edits in force: no region open");
        return;
    }
    State& state = impl_->state;
    state.world = world;
    state.modelsFolder = modelsFolder;
    state.collisionLogged = collisionLogged;
    Collect(state);
    RefreshExtra();
}

void EditsPanel::RefreshExtra() {
    State& state = impl_->state;
    if (!state.world) return;
    state.rows.erase(std::remove_if(state.rows.begin(), state.rows.end(),
                                    [](const Row& row) { return row.extraOwner != nullptr; }),
                     state.rows.end());
    if (extraRows_) {
        std::vector<ExtraRow> extra;
        extraRows_(extra);
        for (ExtraRow& one : extra) {
            Row row;
            row.extraOwner = one.owner;
            row.extra = one.index;
            row.sectionName = L"-";
            row.kind = std::move(one.kind);
            row.what = std::move(one.what);
            row.detail = std::move(one.detail);
            state.rows.push_back(std::move(row));
        }
    }
    FillList();
}

void EditsPanel::FillList() {
    State& state = impl_->state;
    ListView_DeleteAllItems(list_);
    for (size_t i = 0; i < state.rows.size(); ++i) {
        const Row& row = state.rows[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.pszText = (LPWSTR)row.sectionName.c_str();
        item.lParam = (LPARAM)i;
        const int at = ListView_InsertItem(list_, &item);
        ListView_SetItemText(list_, at, 1, (LPWSTR)row.kind.c_str());
        ListView_SetItemText(list_, at, 2, (LPWSTR)row.what.c_str());
        ListView_SetItemText(list_, at, 3, (LPWSTR)row.detail.c_str());
    }
    const std::wstring title = L"Edits in force in " + state.world->Region() + L": " +
                               std::to_wstring(state.rows.size()) +
                               L". Tick and revert; unsaved changes show once written.";
    ::SetWindowTextW(title_, title.c_str());
}

bool EditsPanel::RevertModels(HWND owner, const std::vector<u32>& hashes, std::wstring& summary) {
    State& state = impl_->state;
    if (!state.world) return false;
    const std::set<u32> going(hashes.begin(), hashes.end());

    // The textures every model staying in the region draws with.
    std::set<u32> needed;
    for (const nfs::SolidObject* model : state.world->AllModels())
        if (model && !going.count(model->hash))
            needed.insert(model->textureHashes.begin(), model->textureHashes.end());

    std::set<u32> brought;
    std::set<size_t> ticked;
    for (size_t r = 0; r < state.rows.size(); ++r) {
        const Row& row = state.rows[r];
        if (!row.section) continue;
        const size_t s = (size_t)(std::find(state.sections.begin(), state.sections.end(), row.section) -
                                  state.sections.begin());
        if (s >= state.edits.size()) continue;
        const SectionEdit& edit = state.edits[s][row.edit];
        if (edit.kind == SectionEdit::Kind::ModelAdded && going.count(edit.hash)) {
            ticked.insert(r);
            for (const u32 t : ObjectTextures(edit.chunk)) brought.insert(t);
        }
    }
    // A texture a new object brought may sit in the library rather than in
    // its section -- the section had no pack of its own -- where the rule
    // that follows objects within one section never sees it.
    for (size_t r = 0; r < state.rows.size(); ++r) {
        const Row& row = state.rows[r];
        if (!row.section) continue;
        const size_t s = (size_t)(std::find(state.sections.begin(), state.sections.end(), row.section) -
                                  state.sections.begin());
        if (s >= state.edits.size()) continue;
        const SectionEdit& edit = state.edits[s][row.edit];
        if (edit.kind == SectionEdit::Kind::TextureAdded && brought.count(edit.hash) &&
            !needed.count(edit.hash))
            ticked.insert(r);
    }
    if (ticked.empty()) {
        ::MessageBoxW(owner, L"Those objects are not written into the region yet, so there is "
                             L"nothing to delete on disk.", L"Delete my objects",
                      MB_ICONINFORMATION | MB_OK);
        return false;
    }
    if (!RevertRows(owner, ticked, state)) return false;
    summary = state.summary;
    return true;
}

std::vector<std::wstring> EditsPanel::TickedKeys() const {
    std::vector<std::wstring> keys;
    const State& state = impl_->state;
    for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
        if (!ListView_GetCheckState(list_, i)) continue;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(list_, &item);
        const size_t index = (size_t)item.lParam;
        if (index >= state.rows.size()) continue;
        const Row& row = state.rows[index];
        keys.push_back(row.sectionName + L"|" + row.kind + L"|" + row.what + L"|" + row.detail);
    }
    return keys;
}

void EditsPanel::TickKeys(const std::vector<std::wstring>& keys) {
    const State& state = impl_->state;
    for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(list_, &item);
        const size_t index = (size_t)item.lParam;
        if (index >= state.rows.size()) continue;
        const Row& row = state.rows[index];
        const std::wstring key = row.sectionName + L"|" + row.kind + L"|" + row.what + L"|" + row.detail;
        ListView_SetCheckState(list_, i, std::find(keys.begin(), keys.end(), key) != keys.end());
    }
}

bool EditsPanel::RevertTicked(HWND owner, std::wstring& summary) {
    if (!impl_->state.world) return false;
    const int answer = ::MessageBoxW(owner,
        L"Revert the ticked edits? The sections they are in are rebuilt from the "
        L"game's own files with every other edit kept.",
        L"Region edits", MB_ICONQUESTION | MB_OKCANCEL);
    if (answer != IDOK) return false;

    // Extension rows are not in the sections: they go back to their owner,
    // which takes them out itself. Only the rest rebuilds sections.
    State& state = impl_->state;
    std::set<size_t> ticked;
    std::map<const void*, std::vector<size_t>> extra;
    for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
        if (!ListView_GetCheckState(list_, i)) continue;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(list_, &item);
        const size_t index = (size_t)item.lParam;
        if (index >= state.rows.size()) continue;
        if (state.rows[index].extraOwner) extra[state.rows[index].extraOwner].push_back(state.rows[index].extra);
        else ticked.insert(index);
    }
    if (ticked.empty() && extra.empty()) {
        ::MessageBoxW(owner, L"Tick the edits to revert first.", L"Region edits", MB_ICONINFORMATION | MB_OK);
        return false;
    }
    bool rebuilt = false;
    if (!ticked.empty()) {
        if (!RevertRows(owner, ticked, state)) return false;
        summary = state.summary;
        rebuilt = true;
    }
    // Last: the callback refreshes this list, which drops `state`.
    if (onRevertExtra)
        for (auto& [extraOwner, indices] : extra) {
            std::sort(indices.begin(), indices.end());
            onRevertExtra(extraOwner, indices);
        }
    return rebuilt;
}

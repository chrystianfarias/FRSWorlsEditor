#include "ui/ModelCatalog.h"

#include "nfs/ModelCategory.h"
#include "nfs/World.h"
#include "render/ModelThumbnails.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>

namespace {

constexpr int kPlaceholderImage = 0;
constexpr int kNoPictureImage = 1;

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// What a row's picture is cached by: its model's hash, or for an extension's
// row with a picture of its own, a hash of its owner and key (FNV-1a).
u32 PictureKey(const ModelCatalog::Row& row) {
    if (row.model) return row.model->hash;
    u32 hash = 2166136261u;
    const auto mix = [&hash](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= ((const u8*)data)[i];
            hash *= 16777619u;
        }
    };
    mix(&row.owner, sizeof(row.owner));
    mix(row.key.data(), row.key.size());
    return hash;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)std::towlower(c);
    return text;
}

HBITMAP Bitmap(const std::vector<u32>& bgra, u32 size) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = (LONG)size;
    info.bmiHeader.biHeight = -(LONG)size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) std::memcpy(bits, bgra.data(), (size_t)size * size * 4);
    return bitmap;
}

int AddPicture(HIMAGELIST images, const std::vector<u32>& bgra) {
    HBITMAP bitmap = Bitmap(bgra, ModelThumbnails::kSize);
    if (!bitmap) return kNoPictureImage;
    const int index = ImageList_Add(images, bitmap, nullptr);
    ::DeleteObject(bitmap);
    return index < 0 ? kNoPictureImage : index;
}

// A square of flat colour with a darker edge, for "coming" and "none".
std::vector<u32> Swatch(u32 fill, u32 edge) {
    const u32 size = ModelThumbnails::kSize;
    std::vector<u32> pixels((size_t)size * size, fill);
    for (u32 i = 0; i < size; ++i) {
        pixels[i] = pixels[(size_t)(size - 1) * size + i] = edge;
        pixels[(size_t)i * size] = pixels[(size_t)i * size + size - 1] = edge;
    }
    return pixels;
}

} // namespace

ModelCatalog::~ModelCatalog() {
    if (images_) ImageList_Destroy(images_);
}

void ModelCatalog::Attach(HWND list, HWND shelves, HWND search, HWND usedOnly) {
    list_ = list;
    shelves_ = shelves;
    search_ = search;
    usedOnly_ = usedOnly;

    wchar_t kind[32] = {};
    ::GetClassNameW(shelves_, kind, 32);
    shelvesAreCombo_ = ::_wcsicmp(kind, WC_COMBOBOXW) == 0;

    if (!images_) {
        const int size = (int)ModelThumbnails::kSize;
        images_ = ImageList_Create(size, size, ILC_COLOR32, 64, 64);
        for (const std::vector<u32>& swatch : { Swatch(0xFF3C4148, 0xFF30343A),
                                                Swatch(0xFF26282C, 0xFF30343A) }) {
            HBITMAP bitmap = Bitmap(swatch, ModelThumbnails::kSize);
            ImageList_Add(images_, bitmap, nullptr);
            ::DeleteObject(bitmap);
        }
    }
    ListView_SetExtendedListViewStyle(list_, LVS_EX_DOUBLEBUFFER);
    ListView_SetImageList(list_, images_, LVSIL_NORMAL);
    ListView_SetIconSpacing(list_, (int)ModelThumbnails::kSize + 28, (int)ModelThumbnails::kSize + 36);
    if (usedOnly_) ::SendMessageW(usedOnly_, BM_SETCHECK, BST_CHECKED, 0);
}

void ModelCatalog::SetWorld(const nfs::World* world) {
    world_ = world;
    rows_.clear();
    rowOf_.clear();
    // The pictures belong to meshes that may have changed with the region.
    if (images_) ImageList_SetImageCount(images_, 2);
    imageOf_.clear();
    pending_.clear();
    queued_.clear();

    if (world) {
        std::unordered_set<u32> used;
        for (const auto& section : world->Loaded())
            for (const nfs::SceneryInstance& instance : section->scenery.instances)
                used.insert(section->scenery.ModelHash(instance));

        for (const nfs::SolidObject* model : world->AllModels()) {
            if (!model || model->IsBackdrop()) continue;
            Row row;
            row.model = model;
            row.name = Widen(model->name);
            const size_t home = world->ModelHome(model->hash);
            const nfs::LoadedSection* section =
                home < world->Loaded().size() ? world->Loaded()[home].get() : nullptr;
            row.home = section && section->info ? Widen(section->info->name) : L"?";
            row.library = section && world->IsLibrary(*section);
            row.mine = model->name.rfind("FRS_", 0) == 0;
            row.used = used.count(model->hash) != 0;
            const nfs::ModelCategory category = nfs::CategorizeModel(model->name);
            row.group = category.group;
            row.sub = category.sub;
            rows_.push_back(std::move(row));
        }
        // Shelf by shelf, in the catalogue's order, so "All models" opens on
        // your own and then the buildings rather than on whatever sorts first.
        const auto& order = nfs::ModelCategoryOrder();
        const auto rank = [&](const std::wstring& group) {
            return (size_t)(std::find(order.begin(), order.end(), group) - order.begin());
        };
        std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
            const size_t ra = rank(a.group), rb = rank(b.group);
            if (ra != rb) return ra < rb;
            if (a.sub != b.sub) return a.sub < b.sub;
            return a.name < b.name;
        });
        // The extensions' rows last; their models are not the region's.
        if (extraRows_) {
            std::vector<Row> extra;
            extraRows_(extra);
            for (Row& row : extra)
                if (row.model || row.picture) rows_.push_back(std::move(row));
        }
        for (size_t i = 0; i < rows_.size(); ++i) rowOf_[PictureKey(rows_[i])] = i;
    }
    FillShelves();
    Fill();
}

void ModelCatalog::ShowShelf(const std::wstring& group) {
    wantedShelf_ = group;
    FillShelves();
    Fill();
}

bool ModelCatalog::Passes(const Row& row) const {
    const bool usedOnly = !usedOnly_ || ::SendMessageW(usedOnly_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!usedOnly || row.mine || row.IsExtra()) return true;
    // The ground is placed by scenery records like everything else -- all
    // 4,372 pieces of it in L4RA -- so being used does not keep it out. It is
    // the city's floor, not something to put down on it.
    return row.used && row.group != L"Terrain and roads";
}

int ModelCatalog::ShelfSelection() const {
    return (int)::SendMessageW(shelves_, shelvesAreCombo_ ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
}

// The shelf list, with how many models each shelf holds under the filter.
void ModelCatalog::FillShelves() {
    const int was = ShelfSelection();
    Shelf keep = was >= 0 && (size_t)was < shelfList_.size() ? shelfList_[(size_t)was] : Shelf{};
    if (!wantedShelf_.empty()) keep = { wantedShelf_, L"" };
    wantedShelf_.clear();

    std::map<std::pair<std::wstring, std::wstring>, size_t> counts;
    size_t total = 0;
    for (const Row& row : rows_) {
        if (!Passes(row)) continue;
        ++total;
        ++counts[{ row.group, L"" }];
        if (!row.sub.empty()) ++counts[{ row.group, row.sub }];
    }

    const UINT reset = shelvesAreCombo_ ? CB_RESETCONTENT : LB_RESETCONTENT;
    const UINT addString = shelvesAreCombo_ ? CB_ADDSTRING : LB_ADDSTRING;
    ::SendMessageW(shelves_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(shelves_, reset, 0, 0);
    shelfList_.clear();
    const auto add = [&](const Shelf& shelf, const std::wstring& text) {
        ::SendMessageW(shelves_, addString, 0, (LPARAM)text.c_str());
        shelfList_.push_back(shelf);
    };
    add({}, L"All models (" + std::to_wstring(total) + L")");
    // The extensions' shelves first: they are not among the region's
    // categories, and easy to miss at the bottom of the list.
    {
        const auto& order = nfs::ModelCategoryOrder();
        for (const auto& [key, count] : counts)
            if (key.second.empty() && count &&
                std::find(order.begin(), order.end(), key.first) == order.end())
                add({ key.first, L"" }, key.first + L" (" + std::to_wstring(count) + L")");
    }
    for (const std::wstring& group : nfs::ModelCategoryOrder()) {
        const auto found = counts.find({ group, L"" });
        // Your own shelf stays even when it is empty: it says where they go.
        if ((found == counts.end() || found->second == 0) && group != L"My objects") continue;
        add({ group, L"" }, group + L" (" + std::to_wstring(found == counts.end() ? 0 : found->second) + L")");
        for (const auto& [key, count] : counts)
            if (key.first == group && !key.second.empty())
                add({ group, key.second }, L"      " + key.second + L" (" + std::to_wstring(count) + L")");
    }
    int select = 0;
    for (size_t i = 0; i < shelfList_.size(); ++i)
        if (shelfList_[i].group == keep.group && shelfList_[i].sub == keep.sub) select = (int)i;
    ::SendMessageW(shelves_, shelvesAreCombo_ ? CB_SETCURSEL : LB_SETCURSEL, (WPARAM)select, 0);
    ::SendMessageW(shelves_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(shelves_, nullptr, TRUE);
}

void ModelCatalog::Fill() {
    wchar_t filter[128] = {};
    if (search_) ::GetWindowTextW(search_, filter, 128);
    const std::wstring needle = Lower(filter);

    const int shelfIndex = ShelfSelection();
    const Shelf shelf = shelfIndex >= 0 && (size_t)shelfIndex < shelfList_.size()
                            ? shelfList_[(size_t)shelfIndex] : Shelf{};

    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    shown_.clear();
    // What was waiting belongs to the page that is no longer there.
    pending_.clear();
    queued_.clear();

    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (!Passes(row)) continue;
        if (!shelf.group.empty() &&
            (row.group != shelf.group || (!shelf.sub.empty() && row.sub != shelf.sub)))
            continue;
        if (!needle.empty() && Lower(row.name).find(needle) == std::wstring::npos) continue;
        shown_.push_back(i);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = (int)shown_.size() - 1;
        item.pszText = (LPWSTR)row.name.c_str();
        item.iImage = I_IMAGECALLBACK;
        item.lParam = (LPARAM)i;
        ListView_InsertItem(list_, &item);
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    if (onSelect) onSelect();
}

bool ModelCatalog::OnCommand(HWND control, int notification) {
    if (control && control == search_ && notification == EN_CHANGE) {
        Fill();
        return true;
    }
    if (control && control == shelves_ &&
        notification == (shelvesAreCombo_ ? CBN_SELCHANGE : LBN_SELCHANGE)) {
        Fill();
        return true;
    }
    if (control && control == usedOnly_ && notification == BN_CLICKED) {
        FillShelves();
        Fill();
        return true;
    }
    return false;
}

bool ModelCatalog::OnNotify(const NMHDR* header) {
    if (!header || header->hwndFrom != list_) return false;
    switch (header->code) {
        case NM_DBLCLK:
        case NM_RETURN:
            if (onActivate && Selected()) onActivate();
            return true;
        case LVN_ITEMCHANGED:
            if (onSelect) onSelect();
            return true;
        case LVN_GETDISPINFOW: {
            auto* info = (NMLVDISPINFOW*)header;
            if (!(info->item.mask & LVIF_IMAGE)) return true;
            const size_t index = (size_t)info->item.lParam;
            if (index >= rows_.size()) return true;
            const u32 hash = PictureKey(rows_[index]);
            const auto found = imageOf_.find(hash);
            if (found != imageOf_.end()) {
                info->item.iImage = found->second;
            } else {
                info->item.iImage = kPlaceholderImage;
                if (queued_.insert(hash).second) pending_.push_back(hash);
            }
            return true;
        }
    }
    return false;
}

bool ModelCatalog::DrawPending(DWORD budgetMs) {
    if (pending_.empty() || !thumbnail_) return false;
    const DWORD start = ::GetTickCount();
    bool added = false;
    std::vector<u32> pixels;
    while (!pending_.empty() && ::GetTickCount() - start < budgetMs) {
        const u32 hash = pending_.back();
        pending_.pop_back();
        queued_.erase(hash);
        if (imageOf_.count(hash)) continue;
        const auto row = rowOf_.find(hash);
        if (row == rowOf_.end()) continue;
        const Row& source = rows_[row->second];
        pixels.clear();
        const bool drawn = source.model ? thumbnail_(*source.model, pixels)
                                        : source.picture && source.picture(pixels);
        imageOf_[hash] = drawn && pixels.size() == (size_t)ModelThumbnails::kSize * ModelThumbnails::kSize
                             ? AddPicture(images_, pixels) : kNoPictureImage;
        added = true;
    }
    if (added) ::InvalidateRect(list_, nullptr, FALSE);
    return added;
}

const ModelCatalog::Row* ModelCatalog::Selected() const {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (selected < 0 || (size_t)selected >= shown_.size()) return nullptr;
    return &rows_[shown_[(size_t)selected]];
}

const ModelCatalog::Row* ModelCatalog::RowAtItem(int item) const {
    if (item < 0 || (size_t)item >= shown_.size()) return nullptr;
    return &rows_[shown_[(size_t)item]];
}

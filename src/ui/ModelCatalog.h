#pragma once

#include "core/Types.h"

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfs { class World; struct SolidObject; }

// Draws, or finds, the picture of a model for the catalogue.
using ModelThumbnailFn = std::function<bool(const nfs::SolidObject&, std::vector<u32>&)>;

// The region's models as a catalogue: shelves by kind, pictures of what is on
// the chosen one, a search box and a "used by the map" filter.
//
// It drives controls someone else owns -- the Place Object dialog's and the
// sidebar's Library tab -- so the two cannot drift apart. A picture is drawn
// the first time the list view asks for it, which it does only for the items
// about to be shown, so a region of thousands of models opens at once.
class ModelCatalog {
public:
    struct Row {
        const nfs::SolidObject* model = nullptr;
        std::wstring name;
        std::wstring home;          // the section the model is stored in
        std::wstring group, sub;    // its shelf
        bool mine = false;          // made in this editor (FRS_)
        bool used = false;          // placed somewhere by the map
        bool library = false;       // stored in the library, so placeable anywhere
        // A row an extension added (ui/EditorExtension.h): not a region
        // model but something of the extension's own, named by `key`. Its
        // shelf, when not one of the region's, is listed first.
        const void* owner = nullptr;
        std::string key;
        bool IsExtra() const { return owner != nullptr; }
    };

    ~ModelCatalog();

    // `shelves` is a list box or a drop-down combo box; `usedOnly` a check
    // box, or null for none. The list must be an icon view.
    void Attach(HWND list, HWND shelves, HWND search, HWND usedOnly);
    void SetThumbnails(ModelThumbnailFn thumbnail) { thumbnail_ = std::move(thumbnail); }

    // Reads the region's models afresh. Null empties the catalogue.
    void SetWorld(const nfs::World* world);
    // Rows of the extensions' own, asked for each time the region's are read.
    // Refresh asks again, after what they list has changed.
    void SetExtraRows(std::function<void(std::vector<Row>&)> rows) { extraRows_ = std::move(rows); }
    void Refresh() { SetWorld(world_); }
    void ShowShelf(const std::wstring& group);

    // Messages from the attached controls. True when the catalogue used it.
    bool OnCommand(HWND control, int notification);
    bool OnNotify(const NMHDR* header);

    // Draws what the list asked for, newest first, for up to `budgetMs`.
    // True when it drew anything.
    bool DrawPending(DWORD budgetMs);

    const Row* Selected() const;
    const Row* RowAtItem(int item) const;
    size_t ShownCount() const { return shown_.size(); }

    std::function<void()> onActivate;   // double click or Enter on a picture
    std::function<void()> onSelect;     // the selection changed

private:
    struct Shelf { std::wstring group, sub; };

    void FillShelves();
    void Fill();
    bool Passes(const Row& row) const;
    int  ShelfSelection() const;

    HWND list_ = nullptr, shelves_ = nullptr, search_ = nullptr, usedOnly_ = nullptr;
    const nfs::World* world_ = nullptr;
    std::function<void(std::vector<Row>&)> extraRows_;
    bool shelvesAreCombo_ = false;

    std::vector<Row>    rows_;
    std::vector<size_t> shown_;
    std::vector<Shelf>  shelfList_;
    std::wstring        wantedShelf_;

    ModelThumbnailFn thumbnail_;
    HIMAGELIST images_ = nullptr;
    std::unordered_map<u32, int>    imageOf_;
    std::unordered_map<u32, size_t> rowOf_;
    std::vector<u32>                pending_;
    std::unordered_set<u32>         queued_;
};

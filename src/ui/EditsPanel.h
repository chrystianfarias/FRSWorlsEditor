#pragma once

#include "core/Types.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nfs { class World; }

// The edits in force in the open region, on the sidebar's Edits tab.
//
// One row per edit, per section, as the comparison with the shipped files
// finds them, plus the vertex-only mesh overlays. Ticked rows are reverted by
// rebuilding their sections from the game's own files with everything else
// kept. The list is read from what is on disk, so it is refreshed after every
// load, save and revert.
class EditsPanel {
public:
    EditsPanel();
    ~EditsPanel();

    void Create(HWND parent, HINSTANCE instance, HFONT font);
    void Layout(int x, int y, int width, int height);
    void Show(bool visible);
    size_t Count() const;
    void Refresh(const nfs::World* world, const std::wstring& modelsFolder);

    // Edits an extension keeps outside the sections (ui/EditorExtension.h)
    // are listed too, one row each. Reverting one hands it back to
    // `onRevertExtra`, with its owner and the index the owner gave it.
    // RefreshExtra redoes only their rows, cheaply, after a change.
    struct ExtraRow {
        const void* owner = nullptr;
        size_t index = 0;
        std::wstring kind, what, detail;
    };
    void SetExtraRows(std::function<void(std::vector<ExtraRow>&)> rows) { extraRows_ = std::move(rows); }
    void RefreshExtra();
    // The list itself, for a frame that draws it (ui/MainWindowShell.cpp).
    HWND ListHandle() const { return list_; }
    std::function<void(const void* owner, const std::vector<size_t>& indices)> onRevertExtra;

    // Asks, then reverts the ticked rows. True when something was reverted,
    // so the caller reloads the region; `summary` says what happened.
    bool RevertTicked(HWND owner, std::wstring& summary);
    // Reverts, without the list, every model added under these hashes in
    // every section that holds one -- with the placements that go along --
    // and the textures they brought that nothing else in the region uses,
    // the library's included. How objects made in the editor are deleted.
    bool RevertModels(HWND owner, const std::vector<u32>& hashes, std::wstring& summary);

private:
    void FillList();

    struct Impl;
    std::function<void(std::vector<ExtraRow>&)> extraRows_;
    std::unique_ptr<Impl> impl_;
    HWND parent_  = nullptr;
    HWND title_   = nullptr;
    HWND list_    = nullptr;
    HWND revert_  = nullptr;
    HWND refresh_ = nullptr;
};

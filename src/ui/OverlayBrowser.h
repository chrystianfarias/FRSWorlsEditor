#pragma once

#include "io/MeshOverlay.h"

#include <windows.h>

#include <functional>
#include <vector>

// Lists the edited meshes and lets them be taken back out.
//
// An edit here is a file, not a change to the game, so undoing one is deleting
// it. That is worth showing plainly: without a list, the only record of what has
// been changed is the folder itself, and a mesh edited by mistake would keep
// being applied every time a region loads with no sign of where it came from.
class OverlayBrowser {
public:
    // `onChanged` is called after anything is removed, so the caller can reload
    // the region and show the world without it.
    void Show(HINSTANCE instance, HWND parent, const std::wstring& modelsFolder,
              std::function<void()> onChanged);

    void Close();
    HWND Handle() const { return dialog_; }

private:
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message,
                                       WPARAM wParam, LPARAM lParam);

    void OnInit(HWND dialog);
    void Populate();
    void RemoveSelected();
    void Layout();

    HWND dialog_ = nullptr;
    HWND list_   = nullptr;

    std::wstring modelsFolder_;
    std::function<void()> onChanged_;
    std::vector<MeshOverlayInfo> overlays_;
};

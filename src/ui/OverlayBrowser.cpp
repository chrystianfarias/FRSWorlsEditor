#include "ui/OverlayBrowser.h"

#include "core/Log.h"
#include "ui/Resource.h"

#include <commctrl.h>

#include <cstdio>

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(),
                                            nullptr, 0);
    std::wstring out((size_t)needed, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), needed);
    return out;
}

} // namespace

void OverlayBrowser::Show(HINSTANCE instance, HWND parent,
                          const std::wstring& modelsFolder,
                          std::function<void()> onChanged) {
    modelsFolder_ = modelsFolder;
    onChanged_    = std::move(onChanged);

    if (dialog_) {
        Populate();
        ::ShowWindow(dialog_, SW_SHOW);
        ::SetForegroundWindow(dialog_);
        return;
    }

    dialog_ = ::CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_OVERLAYS), parent,
                                   &OverlayBrowser::DialogProc, (LPARAM)this);
    if (dialog_) ::ShowWindow(dialog_, SW_SHOW);
}

void OverlayBrowser::Close() {
    if (dialog_) { ::DestroyWindow(dialog_); dialog_ = nullptr; }
}

INT_PTR CALLBACK OverlayBrowser::DialogProc(HWND dialog, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
    auto* self = (OverlayBrowser*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
        case WM_INITDIALOG:
            self = (OverlayBrowser*)lParam;
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)self);
            self->OnInit(dialog);
            return TRUE;

        case WM_SIZE:
            if (self) self->Layout();
            return TRUE;

        case WM_COMMAND:
            if (!self) return FALSE;
            switch (LOWORD(wParam)) {
                case IDC_OVERLAY_REMOVE: self->RemoveSelected(); return TRUE;
                case IDCANCEL:
                case IDC_OVERLAY_CLOSE:  self->Close();          return TRUE;
                default: break;
            }
            return FALSE;

        case WM_CLOSE:
            if (self) self->Close();
            return TRUE;

        default:
            return FALSE;
    }
}

void OverlayBrowser::OnInit(HWND dialog) {
    dialog_ = dialog;
    list_   = ::GetDlgItem(dialog, IDC_OVERLAY_LIST);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    const struct { const wchar_t* title; int width; } columns[] = {
        { L"Model",    240 },
        { L"Hash",      80 },
        { L"Vertices",  70 },
        { L"Region",    60 },
    };

    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column{};
        column.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText  = (LPWSTR)columns[i].title;
        column.cx       = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }

    Populate();
    Layout();
}

void OverlayBrowser::Populate() {
    if (!list_) return;

    ListView_DeleteAllItems(list_);
    overlays_.clear();
    ListMeshOverlays(modelsFolder_, overlays_);
    LOG_INFO("edited meshes: %zu found in %S", overlays_.size(), modelsFolder_.c_str());

    for (size_t i = 0; i < overlays_.size(); ++i) {
        const MeshOverlayInfo& overlay = overlays_[i];

        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_PARAM;
        item.iItem    = (int)i;
        item.lParam   = (LPARAM)i;
        const std::wstring model = Widen(overlay.model);
        item.pszText  = (LPWSTR)model.c_str();
        ListView_InsertItem(list_, &item);

        wchar_t text[64];
        ::_snwprintf_s(text, _TRUNCATE, L"%08X", overlay.hash);
        ListView_SetItemText(list_, (int)i, 1, text);

        ::_snwprintf_s(text, _TRUNCATE, L"%u", overlay.vertexCount);
        ListView_SetItemText(list_, (int)i, 2, text);

        const std::wstring region = Widen(overlay.region);
        ListView_SetItemText(list_, (int)i, 3, (LPWSTR)region.c_str());
    }

    wchar_t caption[128];
    ::_snwprintf_s(caption, _TRUNCATE, L"Edited meshes - %zu in the models folder",
                   overlays_.size());
    ::SetWindowTextW(dialog_, caption);
}

void OverlayBrowser::RemoveSelected() {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (selected < 0 || (size_t)selected >= overlays_.size()) return;

    const MeshOverlayInfo& overlay = overlays_[(size_t)selected];

    wchar_t question[512];
    ::_snwprintf_s(question, _TRUNCATE,
                   L"Remove the edit to %S?\n\n"
                   L"The mesh goes back to the shape the game shipped. Nothing else "
                   L"is touched -- the game's own files never held the edit.",
                   overlay.model.c_str());

    if (::MessageBoxW(dialog_, question, L"Edited meshes",
                      MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    if (!::DeleteFileW(overlay.path.c_str())) {
        ::MessageBoxW(dialog_, L"That file could not be deleted.", L"Edited meshes",
                      MB_ICONWARNING | MB_OK);
        return;
    }

    LOG_INFO("edit removed: %S", overlay.path.c_str());

    Populate();
    if (onChanged_) onChanged_();
}

void OverlayBrowser::Layout() {
    if (!dialog_ || !list_) return;

    RECT client{};
    ::GetClientRect(dialog_, &client);

    constexpr int pad = 8;
    constexpr int footer = 30;

    ::MoveWindow(list_, pad, pad, client.right - 2 * pad,
                 client.bottom - footer - 2 * pad, TRUE);

    HWND remove = ::GetDlgItem(dialog_, IDC_OVERLAY_REMOVE);
    HWND close  = ::GetDlgItem(dialog_, IDC_OVERLAY_CLOSE);
    const int top = client.bottom - footer + 2;

    ::MoveWindow(remove, pad, top, 110, 22, TRUE);
    ::MoveWindow(close, client.right - pad - 80, top, 80, 22, TRUE);
}

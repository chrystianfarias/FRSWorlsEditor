#include "ui/TextureBrowser.h"

#include "core/Log.h"
#include "nfs/TextureDecode.h"
#include "ui/Resource.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
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

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

// Writes a 32-bit bottom-up BMP. Uncompressed, so no encoder is needed and the
// file opens in anything.
bool WriteBmp(const std::wstring& path, const std::vector<u32>& pixels,
              u32 width, u32 height) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;

    const u32 imageBytes = width * height * 4;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType    = 0x4D42;   // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + imageBytes;

    BITMAPINFOHEADER info{};
    info.biSize        = sizeof(info);
    info.biWidth       = (LONG)width;
    info.biHeight      = (LONG)height;   // positive: rows run bottom-up
    info.biPlanes      = 1;
    info.biBitCount    = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage   = imageBytes;

    std::fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    std::fwrite(&info, sizeof(info), 1, file);
    for (u32 y = 0; y < height; ++y)
        std::fwrite(&pixels[(size_t)(height - 1 - y) * width], 4, width, file);

    std::fclose(file);
    return true;
}

} // namespace

void TextureBrowser::Show(HINSTANCE instance, HWND parent, const nfs::World* world) {
    world_ = world;

    if (!dialog_) {
        dialog_ = ::CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_TEXTURES),
                                       parent, &TextureBrowser::DialogProc,
                                       (LPARAM)this);
        if (!dialog_) {
            LOG_ERROR("could not create the texture browser");
            return;
        }
    }

    Populate();
    ::ShowWindow(dialog_, SW_SHOW);
    ::SetForegroundWindow(dialog_);
}

void TextureBrowser::SetWorld(const nfs::World* world) {
    world_ = world;
    if (dialog_) Populate();
}

void TextureBrowser::Close() {
    if (dialog_) {
        ::DestroyWindow(dialog_);
        dialog_ = nullptr;
        list_   = nullptr;
    }
}

INT_PTR CALLBACK TextureBrowser::DialogProc(HWND dialog, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
    auto* self = (TextureBrowser*)::GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
        case WM_INITDIALOG:
            ::SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)lParam);
            ((TextureBrowser*)lParam)->OnInit(dialog);
            return TRUE;

        case WM_NOTIFY: {
            if (!self) break;
            auto* header = (NMHDR*)lParam;
            if (header->idFrom == IDC_TEX_LIST && header->code == LVN_ITEMCHANGED) {
                auto* change = (NMLISTVIEW*)lParam;
                if (change->uNewState & LVIS_SELECTED) self->OnSelectionChanged();
            }
            return TRUE;
        }

        case WM_DRAWITEM:
            if (self && wParam == IDC_TEX_PREVIEW) {
                self->DrawPreview(*(DRAWITEMSTRUCT*)lParam);
                return TRUE;
            }
            break;

        case WM_COMMAND:
            if (!self) break;
            switch (LOWORD(wParam)) {
                case IDC_TEX_SAVE: self->SaveSelected(); return TRUE;
                case IDC_TEX_ZOOM:
                    ::InvalidateRect(::GetDlgItem(dialog, IDC_TEX_PREVIEW), nullptr, TRUE);
                    return TRUE;
                case IDCANCEL:
                    ::ShowWindow(dialog, SW_HIDE);
                    return TRUE;
                default: break;
            }
            break;

        case WM_SIZE:
            if (self) self->Layout();
            return TRUE;

        case WM_GETMINMAXINFO: {
            auto* info = (MINMAXINFO*)lParam;
            info->ptMinTrackSize.x = 660;
            info->ptMinTrackSize.y = 480;
            return TRUE;
        }

        case WM_CLOSE:
            ::ShowWindow(dialog, SW_HIDE);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

void TextureBrowser::OnInit(HWND dialog) {
    dialog_ = dialog;
    list_   = ::GetDlgItem(dialog, IDC_TEX_LIST);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    struct { const wchar_t* title; int width; } columns[] = {
        { L"Name",   170 },
        { L"Size",    70 },
        { L"Format",  50 },
        { L"Used",    50 },
    };

    for (int i = 0; i < (int)std::size(columns); ++i) {
        LVCOLUMNW column{};
        column.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText  = (LPWSTR)columns[i].title;
        column.cx       = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
}

void TextureBrowser::Populate() {
    if (!list_) return;

    ListView_DeleteAllItems(list_);
    textures_.clear();
    pixels_.clear();
    pixelWidth_ = pixelHeight_ = 0;

    if (!world_) return;

    textures_ = world_->AllTextures();

    // Most used first: those are the ones that decide how the world looks.
    std::sort(textures_.begin(), textures_.end(),
              [this](const nfs::Texture* a, const nfs::Texture* b) {
                  const u32 ua = world_->TextureUsage(a->hash);
                  const u32 ub = world_->TextureUsage(b->hash);
                  if (ua != ub) return ua > ub;
                  return a->name < b->name;
              });

    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < textures_.size(); ++i) {
        const nfs::Texture& texture = *textures_[i];

        std::wstring name = Widen(texture.name);
        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_PARAM;
        item.iItem    = (int)i;
        item.pszText  = name.data();
        item.lParam   = (LPARAM)i;
        const int row = ListView_InsertItem(list_, &item);

        std::wstring size = Format(L"%ux%u", texture.width, texture.height);
        ListView_SetItemText(list_, row, 1, size.data());

        std::wstring format = Widen(nfs::ToString(texture.format));
        ListView_SetItemText(list_, row, 2, format.data());

        std::wstring used = Format(L"%u", world_->TextureUsage(texture.hash));
        ListView_SetItemText(list_, row, 3, used.data());
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);

    ::SetWindowTextW(dialog_, Format(L"Texture browser - %zu textures",
                                     textures_.size()).c_str());

    if (!textures_.empty()) {
        ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
}

const nfs::Texture* TextureBrowser::Selected() const {
    if (!list_) return nullptr;
    const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (row < 0 || (size_t)row >= textures_.size()) return nullptr;
    return textures_[(size_t)row];
}

void TextureBrowser::OnSelectionChanged() {
    const nfs::Texture* texture = Selected();
    pixels_.clear();
    pixelWidth_ = pixelHeight_ = 0;

    if (texture && !nfs::DecodeTexture(*texture, 0, pixels_, pixelWidth_, pixelHeight_)) {
        pixels_.clear();
        pixelWidth_ = pixelHeight_ = 0;
    }

    if (texture) {
        ::SetDlgItemTextW(dialog_, IDC_TEX_INFO,
                          Format(L"%s\n%u x %u   %s   %u mip levels\nhash %08X   "
                                 L"used by %u materials",
                                 Widen(texture->name).c_str(),
                                 texture->width, texture->height,
                                 Widen(nfs::ToString(texture->format)).c_str(),
                                 texture->mipCount, texture->hash,
                                 world_ ? world_->TextureUsage(texture->hash) : 0).c_str());
    } else {
        ::SetDlgItemTextW(dialog_, IDC_TEX_INFO, L"");
    }

    ::InvalidateRect(::GetDlgItem(dialog_, IDC_TEX_PREVIEW), nullptr, TRUE);
}

void TextureBrowser::DrawPreview(const DRAWITEMSTRUCT& item) {
    const RECT& box = item.rcItem;
    ::FillRect(item.hDC, &box, (HBRUSH)::GetStockObject(DKGRAY_BRUSH));

    if (pixels_.empty()) {
        ::SetBkMode(item.hDC, TRANSPARENT);
        ::SetTextColor(item.hDC, RGB(220, 220, 220));
        RECT text = box;
        ::DrawTextW(item.hDC, L"no preview", -1, &text,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const int boxWidth  = box.right - box.left;
    const int boxHeight = box.bottom - box.top;

    // Integer zoom keeps texels honest: a blurred preview would hide exactly the
    // kind of detail this window exists to judge.
    int drawWidth  = (int)pixelWidth_;
    int drawHeight = (int)pixelHeight_;
    const bool actualSize = ::IsDlgButtonChecked(dialog_, IDC_TEX_ZOOM) == BST_CHECKED;

    if (!actualSize) {
        if (drawWidth > boxWidth || drawHeight > boxHeight) {
            const int divisor = std::max(1, std::max((drawWidth + boxWidth - 1) / boxWidth,
                                                     (drawHeight + boxHeight - 1) / boxHeight));
            drawWidth  /= divisor;
            drawHeight /= divisor;
        } else {
            const int factor = std::max(1, std::min(boxWidth / drawWidth,
                                                    boxHeight / drawHeight));
            drawWidth  *= factor;
            drawHeight *= factor;
        }
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = (LONG)pixelWidth_;
    info.bmiHeader.biHeight      = -(LONG)pixelHeight_;   // negative: rows run top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    ::SetStretchBltMode(item.hDC, COLORONCOLOR);
    ::StretchDIBits(item.hDC,
                    box.left + (boxWidth - drawWidth) / 2,
                    box.top + (boxHeight - drawHeight) / 2,
                    drawWidth, drawHeight,
                    0, 0, (int)pixelWidth_, (int)pixelHeight_,
                    pixels_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

void TextureBrowser::SaveSelected() {
    const nfs::Texture* texture = Selected();
    if (!texture || pixels_.empty()) return;

    wchar_t path[MAX_PATH] = {};
    const std::wstring name = Widen(texture->name);
    ::wcsncpy_s(path, name.c_str(), _TRUNCATE);
    ::wcsncat_s(path, L".bmp", _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = dialog_;
    dialog.lpstrFilter = L"Bitmap\0*.bmp\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrDefExt = L"bmp";
    dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!::GetSaveFileNameW(&dialog)) return;

    if (!WriteBmp(path, pixels_, pixelWidth_, pixelHeight_))
        ::MessageBoxW(dialog_, L"Could not write the file.", L"Texture browser",
                      MB_ICONERROR | MB_OK);
}

void TextureBrowser::Layout() {
    if (!dialog_ || !list_) return;

    RECT client{};
    ::GetClientRect(dialog_, &client);

    const int pad      = 8;
    const int listWidth = 300;
    const int footer   = 56;

    ::MoveWindow(list_, pad, pad, listWidth, client.bottom - 2 * pad, TRUE);

    const int previewX = pad * 2 + listWidth;
    const int previewW = client.right - previewX - pad;
    const int previewH = client.bottom - footer - 2 * pad;
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_PREVIEW), previewX, pad,
                 previewW, previewH, TRUE);

    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_INFO), previewX, pad + previewH + 4,
                 previewW, 36, TRUE);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_ZOOM), previewX,
                 client.bottom - pad - 18, 90, 18, TRUE);
    ::MoveWindow(::GetDlgItem(dialog_, IDC_TEX_SAVE), client.right - pad - 110,
                 client.bottom - pad - 20, 110, 20, TRUE);

    ::InvalidateRect(dialog_, nullptr, TRUE);
}

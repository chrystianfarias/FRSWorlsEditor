// The editor's frame drawn with ImGui: menu bar, toolbar, status bar and the
// Explorer panel (Scene / Library / Edits).
//
// The native controls are still made -- hidden -- and remain what everything
// writes to: the menu and toolbar the editor and its extensions add items to,
// the status bar's parts, the scene tree, the catalogue's list, the edits
// list. This file only draws them and passes clicks back the way the native
// control would have (WM_COMMAND, a selection the control notifies about),
// so no part of the editor, and no extension, had to change for it. Anything
// that may open a dialog is posted, never run inside the frame.

#include "ui/MainWindow.h"

#include "core/Log.h"
#include "ui/ImGuiLayer.h"
#include "io/LightBake.h"
#include "io/SelectionExport.h"
#include "ui/MeshTools.h"
#include "ui/OnlineLibrary.h"
#include "ui/Resource.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <commctrl.h>

#include <algorithm>
#include <set>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(const char* text) {
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out((size_t)size - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}

// "&Open World\tCtrl+O" -> ("Open World", "Ctrl+O").
void SplitMenuText(const std::wstring& raw, std::string& label, std::string& shortcut) {
    std::wstring text = raw, keys;
    const size_t tab = text.find(L'\t');
    if (tab != std::wstring::npos) {
        keys = text.substr(tab + 1);
        text = text.substr(0, tab);
    }
    std::wstring clean;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&') {
            if (i + 1 < text.size() && text[i + 1] == L'&') { clean += L'&'; ++i; }
            continue;
        }
        clean += text[i];
    }
    label = Utf8(clean);
    shortcut = Utf8(keys);
}

const char* ToolbarIcon(const std::wstring& text) {
    struct Pair { const wchar_t* text; const char* icon; };
    static const Pair kIcons[] = {
        { L"New world", ICON_PH_FILE_PLUS },       { L"Open world", ICON_PH_FOLDER_OPEN },
        { L"Save", ICON_PH_FLOPPY_DISK },           { L"Marker", ICON_PH_MAP_PIN },
        { L"Car", ICON_PH_CAR },                    { L"Launch game", ICON_PH_PLAY },
        { L"Show map", ICON_PH_MAP_TRIFOLD },       { L"Show game", ICON_PH_GAME_CONTROLLER },
        { L"Move", ICON_PH_ARROWS_OUT_CARDINAL },   { L"Rotate", ICON_PH_ARROW_CLOCKWISE },
        { L"Scale", ICON_PH_ARROWS_OUT },           { L"Road", ICON_PH_ROAD_HORIZON },
        { L"Breaker", ICON_PH_STACK },
    };
    for (const Pair& pair : kIcons)
        if (text.rfind(pair.text, 0) == 0) return pair.icon;
    return nullptr;
}

std::wstring WindowText(HWND window) {
    const int length = ::GetWindowTextLengthW(window);
    std::wstring text((size_t)std::max(0, length), L'\0');
    if (length > 0) ::GetWindowTextW(window, text.data(), length + 1);
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// Bars: menu, toolbar, status
// ---------------------------------------------------------------------------

void MainWindow::DrawShellMenu(HMENU menu, bool top) {
    const int count = ::GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[256] = {};
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = buffer;
        info.cch = 255;
        if (!::GetMenuItemInfoW(menu, (UINT)i, TRUE, &info)) continue;
        if (info.fType & MFT_SEPARATOR) {
            if (!top) ImGui::Separator();
            continue;
        }
        std::string label, shortcut;
        SplitMenuText(buffer, label, shortcut);
        const bool enabled = !(info.fState & (MFS_DISABLED | MFS_GRAYED));
        ImGui::PushID(i);
        if (info.hSubMenu) {
            if (ImGui::BeginMenu(label.c_str(), enabled)) {
                DrawShellMenu(info.hSubMenu, false);
                ImGui::EndMenu();
            }
        } else if (ImGui::MenuItem(label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(),
                                   (info.fState & MFS_CHECKED) != 0, enabled)) {
            ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(info.wID, 0), 0);
        }
        ImGui::PopID();
    }
}

void MainWindow::DrawShellBars() {
    // --- menu ---------------------------------------------------------------
    if (ImGui::BeginMainMenuBar()) {
        if (menu_) DrawShellMenu(menu_, true);
        // The world being edited, at the right of the bar.
        const std::string world = "World: " + Utf8(worldName_.empty() ? std::wstring(L"-") : worldName_);
        const float width = ImGui::CalcTextSize(world.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x * 2);
        ImGui::TextDisabled("%s", world.c_str());
        ImGui::EndMainMenuBar();
    }

    const float bar = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 1.2f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;

    // --- toolbar: the native toolbar's buttons ---------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    if (ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, bar, flags)) {
        const int buttons = toolbar_ ? (int)::SendMessageW(toolbar_, TB_BUTTONCOUNT, 0, 0) : 0;
        bool first = true;
        for (int i = 0; i < buttons; ++i) {
            TBBUTTON button{};
            if (!::SendMessageW(toolbar_, TB_GETBUTTON, (WPARAM)i, (LPARAM)&button)) continue;
            if (button.fsState & TBSTATE_HIDDEN) continue;
            if (!first) ImGui::SameLine();
            first = false;
            if (button.fsStyle & BTNS_SEP) {
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, 1.0f);
                continue;
            }
            wchar_t text[128] = {};
            ::SendMessageW(toolbar_, TB_GETBUTTONTEXTW, (WPARAM)button.idCommand, (LPARAM)text);
            const std::wstring wide = text;
            const char* icon = ToolbarIcon(wide);
            const std::string label = (icon ? std::string(icon) + "  " : std::string()) + Utf8(wide) +
                                      "##tb" + std::to_string(button.idCommand);
            const bool checked = (button.fsState & TBSTATE_CHECKED) != 0;
            const bool enabled = (button.fsState & TBSTATE_ENABLED) != 0;
            if (checked) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Button(label.c_str()))
                ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(button.idCommand, BN_CLICKED), (LPARAM)toolbar_);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    // --- status bar: the native status bar's parts --------------------------------
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                    ImGui::GetTextLineHeight() + 8.0f, flags)) {
        const int parts = statusBar_ ? (int)::SendMessageW(statusBar_, SB_GETPARTS, 0, 0) : 0;
        for (int i = 0; i < parts; ++i) {
            const LRESULT length = ::SendMessageW(statusBar_, SB_GETTEXTLENGTHW, (WPARAM)i, 0);
            std::wstring text((size_t)LOWORD(length) + 1, L'\0');
            ::SendMessageW(statusBar_, SB_GETTEXTW, (WPARAM)i, (LPARAM)text.data());
            text.resize(wcslen(text.c_str()));
            if (text.empty()) continue;
            if (i) {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
            }
            if (i == 2) ImGui::TextUnformatted(Utf8(text).c_str());
            else ImGui::TextDisabled("%s", Utf8(text).c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Explorer: region, Scene / Library / Edits
// ---------------------------------------------------------------------------

void MainWindow::DrawShellTree(HTREEITEM item, int depth) {
    for (; item; item = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)item)) {
        wchar_t text[260] = {};
        TVITEMW info{};
        info.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_STATE;
        info.hItem = item;
        info.pszText = text;
        info.cchTextMax = 259;
        info.stateMask = TVIS_EXPANDED;
        ::SendMessageW(sectionTree_, TVM_GETITEMW, 0, (LPARAM)&info);
        const HTREEITEM child = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)item);
        const HTREEITEM selected = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_CARET, 0);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!child) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (item == selected) flags |= ImGuiTreeNodeFlags_Selected;
        const char* icon = depth == 0 ? ICON_PH_FOLDER_SIMPLE : child ? ICON_PH_FOLDER : ICON_PH_CUBE;
        const std::string label = std::string(icon) + "  " + Utf8(text);
        const bool open = ImGui::TreeNodeEx((void*)item, flags, "%s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && item != selected)
            ::SendMessageW(sectionTree_, TVM_SELECTITEM, TVGN_CARET, (LPARAM)item);   // notifies, as a click would
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !child)
            ::PostMessageW(window_, kShellTreeActivate, 0, 0);
        if (child && open) {
            DrawShellTree(child, depth + 1);
            ImGui::TreePop();
        }
    }
}

void* MainWindow::ShellThumbnail(int image) {
    IDirect3DDevice9* device = viewport_.GetRenderer().Context() ? viewport_.GetRenderer().Context()->Device() : nullptr;
    HIMAGELIST list = (HIMAGELIST)::SendMessageW(libList_, LVM_GETIMAGELIST, LVSIL_NORMAL, 0);
    if (!device || !list || image < 0) return nullptr;
    if (list != thumbList_) {
        for (auto& [index, texture] : thumbTextures_)
            if (texture) ((IDirect3DTexture9*)texture)->Release();
        thumbTextures_.clear();
        thumbList_ = list;
    }
    auto found = thumbTextures_.find(image);
    if (found != thumbTextures_.end()) return found->second;

    int width = 0, height = 0;
    ImageList_GetIconSize(list, &width, &height);
    if (width <= 0 || height <= 0) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    IDirect3DTexture9* texture = nullptr;
    if (dib && bits) {
        HGDIOBJ old = ::SelectObject(dc, dib);
        RECT all{ 0, 0, width, height };
        HBRUSH back = ::CreateSolidBrush(RGB(0x23, 0x24, 0x28));
        ::FillRect(dc, &all, back);
        ::DeleteObject(back);
        ImageList_Draw(list, image, dc, 0, 0, ILD_NORMAL);
        ::GdiFlush();
        if (SUCCEEDED(device->CreateTexture((UINT)width, (UINT)height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                            &texture, nullptr))) {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
                for (int y = 0; y < height; ++y) {
                    u32* row = (u32*)((u8*)locked.pBits + (size_t)y * locked.Pitch);
                    const u32* source = (const u32*)bits + (size_t)y * width;
                    for (int x = 0; x < width; ++x) row[x] = source[x] | 0xFF000000u;
                }
                texture->UnlockRect(0);
            }
        }
        ::SelectObject(dc, old);
    }
    if (dib) ::DeleteObject(dib);
    ::DeleteDC(dc);
    thumbTextures_[image] = texture;
    return texture;
}

void MainWindow::ForgetShellThumbnails() {
    for (auto& [index, texture] : thumbTextures_)
        if (texture) ((IDirect3DTexture9*)texture)->Release();
    thumbTextures_.clear();
    thumbList_ = nullptr;
}

void MainWindow::DrawShellLibrary() {
    // Shelf and the "used by the map" filter: the native ones, driven.
    {
        const int count = (int)::SendMessageW(libShelf_, CB_GETCOUNT, 0, 0);
        const int current = (int)::SendMessageW(libShelf_, CB_GETCURSEL, 0, 0);
        wchar_t text[200] = {};
        if (current >= 0) ::SendMessageW(libShelf_, CB_GETLBTEXT, (WPARAM)current, (LPARAM)text);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##shelf", (std::string(ICON_PH_BOOKS) + "  " + Utf8(text)).c_str(),
                              ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < count; ++i) {
                wchar_t item[200] = {};
                ::SendMessageW(libShelf_, CB_GETLBTEXT, (WPARAM)i, (LPARAM)item);
                if (ImGui::Selectable((Utf8(item) + "##" + std::to_string(i)).c_str(), i == current) && i != current) {
                    ::SendMessageW(libShelf_, CB_SETCURSEL, (WPARAM)i, 0);
                    ::SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IDC_LIB_SHELF, CBN_SELCHANGE), (LPARAM)libShelf_);
                }
            }
            ImGui::EndCombo();
        }
        bool used = ::SendMessageW(libUsed_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (ImGui::Checkbox("Only models the map places", &used)) {
            ::SendMessageW(libUsed_, BM_SETCHECK, used ? BST_CHECKED : BST_UNCHECKED, 0);
            ::SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IDC_LIB_USED, BN_CLICKED), (LPARAM)libUsed_);
        }
    }

    // The pictures, as a grid, only the visible rows asked for.
    const int items = (int)::SendMessageW(libList_, LVM_GETITEMCOUNT, 0, 0);
    const int selected = (int)::SendMessageW(libList_, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    const float cell = 96.0f, picture = 64.0f;
    if (ImGui::BeginChild("##grid")) {
        const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
        const int rows = (items + columns - 1) / columns;
        ImGuiListClipper clipper;
        clipper.Begin(rows, cell + ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const int index = row * columns + column;
                    if (index >= items) break;
                    if (column) ImGui::SameLine();
                    wchar_t text[200] = {};
                    LVITEMW item{};
                    item.mask = LVIF_TEXT | LVIF_IMAGE;   // asks the catalogue for its picture
                    item.iItem = index;
                    item.pszText = text;
                    item.cchTextMax = 199;
                    ::SendMessageW(libList_, LVM_GETITEMW, 0, (LPARAM)&item);

                    ImGui::PushID(index);
                    ImGui::BeginGroup();
                    const ImVec2 at = ImGui::GetCursorPos();
                    const bool clicked = ImGui::Selectable("##cell", index == selected, ImGuiSelectableFlags_AllowDoubleClick,
                                                           ImVec2(cell - 8.0f, cell + ImGui::GetTextLineHeight() - 6.0f));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f);
                    ImGui::SetCursorPos(ImVec2(at.x + (cell - 8.0f - picture) * 0.5f, at.y + 4.0f));
                    if (void* texture = ShellThumbnail(item.iImage))
                        ImGui::Image((ImTextureID)texture, ImVec2(picture, picture));
                    else
                        ImGui::Dummy(ImVec2(picture, picture));
                    const std::string name = Utf8(text);
                    ImGui::SetCursorPos(ImVec2(at.x + 2.0f, at.y + picture + 8.0f));
                    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                        ImVec2(ImGui::GetCursorScreenPos().x + cell - 12.0f,
                                               ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight()), true);
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopClipRect();
                    ImGui::EndGroup();
                    if (hovered) ImGui::SetTooltip("%s", name.c_str());
                    ImGui::PopID();

                    if (clicked && index != selected) {
                        LVITEMW state{};
                        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        state.state = LVIS_SELECTED | LVIS_FOCUSED;
                        ::SendMessageW(libList_, LVM_SETITEMSTATE, (WPARAM)index, (LPARAM)&state);
                    }
                    if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        ::PostMessageW(window_, kShellLibraryActivate, 0, 0);
                    if (dragging && !shellDragging_) {
                        LVITEMW state{};
                        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        state.state = LVIS_SELECTED | LVIS_FOCUSED;
                        ::SendMessageW(libList_, LVM_SETITEMSTATE, (WPARAM)index, (LPARAM)&state);
                        shellDragging_ = true;
                        shellDragItem_ = index;
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    if (shellDragging_ && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(ICON_PH_CUBE "  Let go on the map to place it");
        ImGui::EndTooltip();
    }
}

void MainWindow::UpdateShellDrag() {
    // A picture dragged out of the Library: once the pointer is over the map
    // the placement is made and follows the ground, as the native drag did.
    if (!shellDragging_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGuiDockNode* centre = ImGui::DockBuilderGetCentralNode(ImGui::GetID("EditorDockSpace"));
    const bool overMap = centre && centre->Rect().Contains(io.MousePos);
    if (io.MouseDown[ImGuiMouseButton_Left]) {
        if (overMap && !libDragging_) {
            if (const ModelCatalog::Row* row = catalog_.RowAtItem(shellDragItem_)) {
                if (EditorExtension* owner = ExtensionOf(row->owner)) {
                    if (world_ && owner->BeginDrag(*row)) {
                        libDragging_ = true;
                        dragExtension_ = owner;
                        dragPlaced_ = false;
                    }
                } else if (row->model) {
                    BeginLibraryDrag(row->model->hash);
                    if (::GetCapture() == window_) ::ReleaseCapture();   // the panels keep the mouse
                    libDragging_ = true;
                }
            }
        }
        if (libDragging_) UpdateLibraryDrag();
        return;
    }
    // Let go: on the map it stays, anywhere else it is taken back.
    shellDragging_ = false;
    if (libDragging_) EndLibraryDrag(overMap);
}

void MainWindow::DrawShellEdits() {
    HWND list = edits_.ListHandle();
    if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE "  Revert ticked"))
        ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(IDC_EDITS_REVERT, BN_CLICKED), 0);
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_ARROWS_CLOCKWISE "  Refresh"))
        ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(IDC_EDITS_REFRESH, BN_CLICKED), 0);
    if (!list) return;

    HWND header = (HWND)::SendMessageW(list, LVM_GETHEADER, 0, 0);
    const int columns = std::clamp(header ? (int)::SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) : 1, 1, 8);
    const int items = (int)::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    if (items == 0) {
        ImGui::TextDisabled("No edits in this region yet.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##edits", columns + 1, flags)) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    for (int c = 0; c < columns; ++c) {
        wchar_t text[100] = {};
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT;
        column.pszText = text;
        column.cchTextMax = 99;
        ::SendMessageW(list, LVM_GETCOLUMNW, (WPARAM)c, (LPARAM)&column);
        ImGui::TableSetupColumn(Utf8(text).c_str());
    }
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(items);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            const UINT state = (UINT)::SendMessageW(list, LVM_GETITEMSTATE, (WPARAM)i, LVIS_STATEIMAGEMASK);
            bool ticked = ((state & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (ImGui::Checkbox("##tick", &ticked)) {
                LVITEMW item{};
                item.stateMask = LVIS_STATEIMAGEMASK;
                item.state = INDEXTOSTATEIMAGEMASK(ticked ? 2 : 1);
                ::SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&item);
            }
            for (int c = 0; c < columns; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                wchar_t text[300] = {};
                LVITEMW item{};
                item.iSubItem = c;
                item.pszText = text;
                item.cchTextMax = 299;
                ::SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)&item);
                ImGui::TextUnformatted(Utf8(text).c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void MainWindow::DrawViewModes() {
    // Top left of the view -- the dock space's middle -- clear of the panels.
    ImGuiDockNode* centre = ImGui::DockBuilderGetCentralNode(ImGui::GetID("EditorDockSpace"));
    if (!centre) return;
    const ImVec2 at(centre->Pos.x + 10.0f, centre->Pos.y + 10.0f);
    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 3.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##viewmodes", nullptr, flags)) {
        const bool editing = meshTools_ && meshTools_->Active();
        const auto& selection = viewport_.GetRenderer().Selection();
        const bool oneObject = selection.size() == 1 && selection.front().object;

        // One button per state, the current one lit, as a segmented switch.
        const auto segment = [&](const char* label, bool on, bool enabled, const char* tip) {
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)
                                                      : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive)
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Button(label);
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        if (segment(ICON_PH_CUBE "  Object Mode", !editing, true,
                    editing ? "Back to objects: the mesh edit is written (Tab)" : "Select, move, turn and place objects"))
            if (editing) ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(MeshTools::kFinishCommand, 0), 0);
        ImGui::SameLine();
        if (segment(ICON_PH_PENCIL_SIMPLE "  Edit Mode", editing, editing || oneObject,
                    oneObject || editing ? "Edit the selected object's vertices, edges and faces (Tab)"
                                         : "Select one object first"))
            if (!editing) ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(MeshTools::kEditCommand, 0), 0);

        if (editing) {
            // What a click picks, as the 1 / 2 / 3 keys choose.
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, 1.0f);
            ImGui::SameLine();
            const int pick = meshTools_->PickIndex();
            if (segment(ICON_PH_DOT_OUTLINE "##v", pick == 0, true, "Vertices (1)"))
                ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(MeshTools::kVerticesCommand, 0), 0);
            ImGui::SameLine();
            if (segment(ICON_PH_LINE_SEGMENT "##e", pick == 1, true, "Edges (2)"))
                ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(MeshTools::kEdgesCommand, 0), 0);
            ImGui::SameLine();
            if (segment(ICON_PH_POLYGON "##f", pick == 2, true, "Faces (3)"))
                ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(MeshTools::kFacesCommand, 0), 0);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// Night lighting
// ---------------------------------------------------------------------------

namespace {
lightbake::Environment g_bakeEnvironment;   // measured when the dialog opens
}

void MainWindow::ApplyBake(bool preview) {
    (void)preview;
    if (!world_) return;
    lightbake::Settings settings;
    settings.ambientScale = bakeAmbient_;
    settings.lampScale = bakeLamps_;
    settings.lampRadius = bakeRadius_;
    std::set<u32> done;
    for (const PickResult& picked : bakeTargets_) {
        if (!picked.object || !done.insert(picked.object->hash).second) continue;
        const u32 hash = picked.object->hash;
        if (!bakeOriginal_.count(hash)) {
            std::vector<u32> original;
            for (const auto& v : picked.object->vertices) original.push_back(v.color);
            bakeOriginal_[hash] = std::move(original);
        }
        const std::vector<u32> colours = lightbake::Bake(*picked.object, picked.transform, g_bakeEnvironment, settings);
        // Every stored copy of the model, and what the GPU draws.
        for (size_t s = 0; s < world_->Loaded().size(); ++s)
            for (nfs::SolidList& list : world_->MutableLoaded(s)->solidLists)
                for (nfs::SolidObject& object : list.objects)
                    if (object.hash == hash && object.vertices.size() == colours.size())
                        for (size_t i = 0; i < colours.size(); ++i) object.vertices[i].color = colours[i];
        viewport_.GetRenderer().UpdateModelColours(hash, colours);
    }
}

void MainWindow::RestoreBake() {
    if (!world_) { bakeOriginal_.clear(); return; }
    for (const auto& [hash, colours] : bakeOriginal_) {
        for (size_t s = 0; s < world_->Loaded().size(); ++s)
            for (nfs::SolidList& list : world_->MutableLoaded(s)->solidLists)
                for (nfs::SolidObject& object : list.objects)
                    if (object.hash == hash && object.vertices.size() == colours.size())
                        for (size_t i = 0; i < colours.size(); ++i) object.vertices[i].color = colours[i];
        viewport_.GetRenderer().UpdateModelColours(hash, colours);
    }
    bakeOriginal_.clear();
}

void MainWindow::WriteBake() {
    if (!world_ || bakeTargets_.empty()) return;
    ApplyBake(false);
    // Written as an edit from Blender comes back: exported with the new
    // colours, read back quietly.
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\bake";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t name[64];
    ::_snwprintf_s(name, _TRUNCATE, L"\\bake-%04u%02u%02u-%02u%02u%02u.obj", now.wYear, now.wMonth, now.wDay,
                   now.wHour, now.wMinute, now.wSecond);
    const std::wstring objPath = folder + name;
    ExportSummary summary;
    if (!ExportSelection(*world_, bakeTargets_, objPath, world_->Region(), summary)) {
        SetStatusText(2, L"The lighting could not be written: the export failed (see the log)");
        return;
    }
    bakeOriginal_.clear();
    bakeTargets_.clear();
    LOG_INFO("light bake: writing %zu models through %S", summary.objects, objPath.c_str());
    ImportObj(objPath, true);
    SetStatusText(2, L"Night lighting baked into " + std::to_wstring(summary.objects) + L" models and written");
}

void MainWindow::DrawBakeDialog() {
    if (bakeOpen_) {
        bakeOpen_ = false;
        bakeMeasured_ = false;
        bakeOriginal_.clear();
        ImGui::OpenPopup("Bake night lighting");
    }
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Bake night lighting", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    // What it applies to: the selection -- by default the objects made in
    // this editor, whose models nothing else shares.
    std::vector<PickResult> targets;
    size_t shared = 0;
    for (const PickResult& picked : viewport_.GetRenderer().Selection()) {
        if (!picked.object) continue;
        const bool mine = picked.object->name.rfind("FRS_", 0) == 0;
        if (!mine && !bakeNatives_) { ++shared; continue; }
        targets.push_back(picked);
    }
    if (!bakeMeasured_ && world_ && !targets.empty()) {
        Aabb area;
        for (const PickResult& picked : targets)
            if (picked.object->bounds.Valid()) area.Add(Transform(picked.object->bounds, picked.transform));
        lightbake::Settings settings;
        settings.lampRadius = bakeRadius_;
        g_bakeEnvironment = lightbake::Measure(*world_, area, settings);
        bakeMeasured_ = true;
    }
    bakeTargets_ = targets;

    ImGui::TextWrapped("The game lights its world at night through vertex colours. This gives the selected "
                       "objects the light of where they stand: the street's own darkness, and the glow of the "
                       "street lights around them.");
    ImGui::Spacing();
    if (targets.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Select objects made in the editor (FRS_...) first.");
    } else {
        ImGui::Text(ICON_PH_CUBE "  %zu objects", targets.size());
        const lightbake::Environment& e = g_bakeEnvironment;
        ImGui::SameLine();
        ImGui::TextDisabled("|  %zu street lights in reach", e.lamps.size());
        const ImVec4 ambient(std::min(1.0f, e.ambient.x * 0.5f), std::min(1.0f, e.ambient.y * 0.5f),
                             std::min(1.0f, e.ambient.z * 0.5f), 1.0f);
        const ImVec4 lamp(std::min(1.0f, e.lampColour.x * 0.5f), std::min(1.0f, e.lampColour.y * 0.5f),
                          std::min(1.0f, e.lampColour.z * 0.5f), 1.0f);
        ImGui::ColorButton("##ambient", ambient, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        ImGui::SameLine();
        ImGui::Text("Ambient, from %zu vertices of the map around", e.ambientSamples);
        ImGui::ColorButton("##lamp", lamp, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        ImGui::SameLine();
        ImGui::Text("Street light, from %zu vertices of the pools under them", e.poolSamples);
    }
    if (shared)
        ImGui::TextDisabled("%zu of the map's own objects in the selection are left out", shared);
    if (ImGui::Checkbox("Also the map's own models (they are shared: every placement changes)", &bakeNatives_))
        bakeMeasured_ = false;

    ImGui::SeparatorText("Strength");
    bool changed = false;
    changed |= ImGui::SliderFloat("Ambient", &bakeAmbient_, 0.0f, 2.5f, "x %.2f");
    changed |= ImGui::SliderFloat("Street lights", &bakeLamps_, 0.0f, 3.0f, "x %.2f");
    if (ImGui::SliderFloat("Reach", &bakeRadius_, 6.0f, 60.0f, "%.0f m")) changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) bakeMeasured_ = false;   // a new reach finds other lamps
    if (changed && !bakeOriginal_.empty()) ApplyBake(true);   // a preview follows the sliders

    ImGui::Spacing();
    ImGui::BeginDisabled(targets.empty());
    if (ImGui::Button(ICON_PH_EYE "  Preview", ImVec2(140, 0))) ApplyBake(true);
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_FLOPPY_DISK "  Bake and write", ImVec2(160, 0))) {
        ::PostMessageW(window_, kBakeWriteMessage, 0, 0);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) {
        RestoreBake();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::DrawShellPanels() {
    UpdateShellDrag();
    DrawBakeDialog();
    // FRS_UI_DEMO=online: the Online tab, searching, for a look at it.
    static bool demoOnline = [] {
        char value[16] = {};
        return ::GetEnvironmentVariableA("FRS_UI_DEMO", value, sizeof(value)) && std::string(value) == "online";
    }();
    if (demoOnline && online_) {
        demoOnline = false;
        sideTab_ = 3;
        online_->Search("street lamp");
    }
    DrawViewModes();

    const std::string title = std::string(ICON_PH_TREE_STRUCTURE) + "  Explorer###Explorer";
    if (ImGui::Begin(title.c_str())) {
        // Region
        {
            const int count = (int)::SendMessageW(regionCombo_, CB_GETCOUNT, 0, 0);
            const int current = (int)::SendMessageW(regionCombo_, CB_GETCURSEL, 0, 0);
            wchar_t text[64] = {};
            if (current >= 0) ::SendMessageW(regionCombo_, CB_GETLBTEXT, (WPARAM)current, (LPARAM)text);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##region", (std::string(ICON_PH_GLOBE_HEMISPHERE_WEST) + "  " + Utf8(text)).c_str())) {
                for (int i = 0; i < count; ++i) {
                    wchar_t item[64] = {};
                    ::SendMessageW(regionCombo_, CB_GETLBTEXT, (WPARAM)i, (LPARAM)item);
                    if (ImGui::Selectable(Utf8(item).c_str(), i == current) && i != current) {
                        ::SendMessageW(regionCombo_, CB_SETCURSEL, (WPARAM)i, 0);
                        // Opening a region asks questions and reloads: after the frame.
                        ::PostMessageW(window_, WM_COMMAND, MAKEWPARAM(IDC_REGION_COMBO, CBN_SELCHANGE),
                                       (LPARAM)regionCombo_);
                    }
                }
                ImGui::EndCombo();
            }
        }

        // Scene / Library / Edits
        int wanted = sideTab_;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, ImGui::GetStyle().FramePadding.y));
        const bool tabs = ImGui::BeginTabBar("##sidetabs", ImGuiTabBarFlags_FittingPolicyResizeDown);
        ImGui::PopStyleVar();
        if (tabs) {
            // A narrow panel shows the icons alone; the name is in the tooltip.
            const bool narrow = ImGui::GetContentRegionAvail().x < 360.0f;
            struct Tab { int index; const char* icon; std::string name; const char* id; };
            const Tab tabsList[] = {
                { 0, ICON_PH_TREE_STRUCTURE, "Scene", "###scene" },
                { 1, ICON_PH_CUBE, "Library", "###library" },
                { 3, ICON_PH_GLOBE, "Online", "###online" },
                { 2, ICON_PH_LIST_CHECKS, "Edits " + std::to_string(edits_.Count()), "###edits" },
            };
            for (const Tab& tab : tabsList) {
                const std::string label = std::string(tab.icon) +
                                          (narrow ? (tab.index == 2 ? " " + std::to_string(edits_.Count()) : std::string())
                                                  : "  " + tab.name) + tab.id;
                // Follow a tab chosen elsewhere (a menu, the code) as well as a click.
                const ImGuiTabItemFlags flags = tab.index == sideTab_ && shownTab_ != sideTab_
                                                    ? ImGuiTabItemFlags_SetSelected : 0;
                const bool open = ImGui::BeginTabItem(label.c_str(), nullptr, flags);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tab.name.c_str());
                if (open) { wanted = tab.index; ImGui::EndTabItem(); }
            }
            ImGui::EndTabBar();
        }
        if (shownTab_ != sideTab_) wanted = sideTab_;   // this frame selected it
        shownTab_ = sideTab_;
        if (wanted != sideTab_) { ShowSideTab(wanted); shownTab_ = sideTab_; }

        // The shared search box drives the native one, which filters.
        if (sideTab_ != 2 && sideTab_ != 3) {
            char search[256] = {};
            const std::string current = Utf8(WindowText(sideSearch_));
            std::memcpy(search, current.c_str(), std::min(current.size(), sizeof(search) - 1));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##search", ICON_PH_MAGNIFYING_GLASS "  Search", search, sizeof(search)))
                ::SetWindowTextW(sideSearch_, Wide(search).c_str());
        }

        if (sideTab_ == 0) {
            if (ImGui::BeginChild("##tree")) {
                const HTREEITEM root = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_ROOT, 0);
                DrawShellTree(root, 0);
            }
            ImGui::EndChild();
        } else if (sideTab_ == 1) {
            DrawShellLibrary();
        } else if (sideTab_ == 3) {
            if (online_)
                online_->Draw(viewport_.GetRenderer().Context() ? viewport_.GetRenderer().Context()->Device() : nullptr);
        } else {
            DrawShellEdits();
        }
    }
    ImGui::End();
}

void MainWindow::PlaceOnlineModel() {
    std::wstring obj;
    std::string credit;
    if (!online_ || !online_->TakeReady(obj, credit) || !world_) return;
    Vec3 point;
    if (!ViewCentrePoint(point)) {
        online_->SetStatus(L"Point the middle of the view at the ground where it should stand, and place it again.");
        return;
    }
    // The author is credited beside the edits, as the licence asks.
    const std::wstring credits = modelsFolder_ + L"\\CREDITS.txt";
    if (FILE* file = _wfopen(credits.c_str(), L"ab")) {
        const std::string line = "Sketchfab: " + credit + "\r\n";
        std::fwrite(line.data(), 1, line.size(), file);
        std::fclose(file);
    }
    LOG_INFO("placing a Sketchfab model: %s", credit.c_str());
    ImportObj(obj, false, &point);
}

void MainWindow::HideNativeShell() {
    for (HWND control : { regionCombo_, sideTabs_, sideSearch_, sectionTree_, libShelf_, libUsed_, libList_, toolbar_,
                          statusBar_ })
        if (control) ::ShowWindow(control, SW_HIDE);
    edits_.Show(false);
}

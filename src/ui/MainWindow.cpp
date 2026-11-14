#include "ui/MainWindow.h"

#include "core/Log.h"
#include "io/CollisionExport.h"
#include "io/CollisionImport.h"
#include "ui/CollisionTools.h"
#include "ui/CrashReport.h"
#include "ui/MeshTools.h"
#include "drive/DriveMode.h"
#include "ui/HiddenItems.h"
#include "ui/ZoneTools.h"
#include "ui/OnlineLibrary.h"
#include "io/PlacementWrite.h"
#include "io/SectionEdits.h"
#include "ui/PlacementDialogs.h"
#include "nfs/FreeRoam.h"
#include "nfs/ModelCategory.h"
#include "nfs/SceneryEdit.h"
#include "io/SelectionExport.h"
#include "io/SelectionImport.h"
#include "io/WorldProfiles.h"
#include "io/WorldFile.h"
#include "io/WorldPublish.h"
#include "minimap/MinimapBuildings.h"   // MINIMAP PREVIEW (temporary)
#include "render/RenderExtension.h"
#include "ui/Resource.h"
#include "ui/MarkersExtension.h"
#include "ui/RoadsExtension.h"
#include "ui/SetupWizard.h"
#include "ui/WorldDialogs.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <set>
#include <stdexcept>

namespace {

const wchar_t* kWindowClass = L"FRSWorldEditor.MainWindow";
const wchar_t* kWindowTitle = L"FRS World Editor - Need for Speed Underground 2";
const wchar_t* kSettingsKey = L"Software\\FRSWorldEditor";
const wchar_t* kSupportUrl = L"https://buymeacoffee.com/chrystianfarias";
// Bumped when the setup has something new to install; an older value runs it again.
constexpr DWORD kSetupVersion = 2;   // 2: loader applies edits only to their own region
// Asks for the setup once the window is up, on the first run.
constexpr UINT kRunSetupMessage = WM_APP + 1;
// FRSFastTest reads its switch when the game starts; this long after the
// launch it certainly has, and the switch goes back off.
constexpr DWORD kFastBootRestoreMs = 15000;

constexpr int kComboHeight  = 24;
// Also the width of the gaps that resize the panels.
constexpr int kPanelPadding = 6;
constexpr int kMinSideWidth  = 170;
constexpr int kMinEditsHeight = 70;
constexpr int kMinViewport   = 200;

// Tree item parameters: sections are their index, the region root is -1.
// Keeps the embedded game over the viewport and notices when it quits.
constexpr UINT_PTR kGameViewTimer = 1;
// The scene tree's own branches. A placement is -(kPlacedItemBase + section
// * 65536 + index) and an object of yours -(kMineItemBase + i).
constexpr intptr_t kPlacedBranchItem = -4;
constexpr intptr_t kMineBranchItem   = -5;
constexpr intptr_t kPlacedItemBase   = 10000000;
constexpr intptr_t kMineItemBase     = 2000000000;
// Zones and districts, below everything else: -(kZoneItemBase + i) and
// -(kDistrictItemBase + i).
constexpr intptr_t kZoneBranchItem    = -6;
constexpr intptr_t kZoneItemBase      = 3000000000;
constexpr intptr_t kDistrictItemBase  = 4000000000;
// An extension's rows: extension e's code c is -(kExtensionItemBase + e *
// kExtensionItemSpan + c), below the districts; rows that do nothing are -8.
constexpr intptr_t kExtensionNoItem    = -8;
constexpr intptr_t kExtensionItemBase  = 6000000000;
constexpr intptr_t kExtensionItemSpan  = 1000000000;

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)needed, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), needed);
    return out;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The toolbar's pictures, drawn rather than shipped so they follow the DPI:
// new world, open world, save, launch game, then the gizmo's move, rotate and
// scale. Coordinates are on a 16-unit grid.
HIMAGELIST BuildToolbarIcons(int size) {
    constexpr int kCount = 7;
    const COLORREF key = RGB(255, 0, 255);
    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, size * kCount, size);
    ::ReleaseDC(nullptr, screen);
    HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);

    RECT all{ 0, 0, size * kCount, size };
    HBRUSH keyBrush = ::CreateSolidBrush(key);
    ::FillRect(dc, &all, keyBrush);
    ::DeleteObject(keyBrush);

    const float unit = size / 16.0f;
    int origin = 0;
    auto at = [&](float x, float y) {
        return POINT{ origin + (int)std::lround(x * unit), (int)std::lround(y * unit) };
    };
    auto style = [&](COLORREF fill, COLORREF line, auto draw) {
        HBRUSH brush = ::CreateSolidBrush(fill);
        HPEN pen = ::CreatePen(PS_SOLID, std::max(1, (int)unit), line);
        HGDIOBJ oldBrush = ::SelectObject(dc, brush);
        HGDIOBJ oldPen = ::SelectObject(dc, pen);
        draw();
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(brush);
        ::DeleteObject(pen);
    };
    auto ellipse = [&](float l, float t, float r, float b) {
        const POINT a = at(l, t), c = at(r, b);
        ::Ellipse(dc, a.x, a.y, c.x, c.y);
    };
    auto rect = [&](float l, float t, float r, float b) {
        const POINT a = at(l, t), c = at(r, b);
        ::Rectangle(dc, a.x, a.y, c.x, c.y);
    };
    struct Corner { float x, y; };
    auto polygon = [&](std::initializer_list<Corner> points) {
        std::vector<POINT> corners;
        for (const Corner& corner : points) corners.push_back(at(corner.x, corner.y));
        ::Polygon(dc, corners.data(), (int)corners.size());
    };

    // New world: a globe with a plus.
    style(RGB(66, 133, 214), RGB(30, 70, 140), [&] { ellipse(0.5f, 0.5f, 14.5f, 14.5f); });
    style(RGB(96, 176, 84), RGB(96, 176, 84), [&] {
        polygon({ { 3, 4 }, { 7, 2.5f }, { 8.5f, 5.5f }, { 6, 8 }, { 3.5f, 7 } });
        polygon({ { 7.5f, 9 }, { 11.5f, 7.5f }, { 11, 11.5f }, { 8, 12 } });
    });
    style(RGB(255, 255, 255), RGB(40, 130, 40), [&] { ellipse(8.5f, 8.5f, 16, 16); });
    style(RGB(40, 150, 40), RGB(40, 150, 40), [&] {
        rect(10.5f, 11.5f, 14, 13);
        rect(11.5f, 10.5f, 13, 14);
    });

    // Open world: a folder.
    origin += size;
    style(RGB(226, 176, 60), RGB(150, 104, 20), [&] {
        polygon({ { 0.5f, 2.5f }, { 5.5f, 2.5f }, { 7, 4.5f }, { 14, 4.5f }, { 14, 13.5f }, { 0.5f, 13.5f } });
    });
    style(RGB(250, 212, 96), RGB(150, 104, 20), [&] {
        polygon({ { 0.5f, 13.5f }, { 3, 7 }, { 15.5f, 7 }, { 14, 13.5f } });
    });

    // Save: a diskette.
    origin += size;
    style(RGB(60, 110, 190), RGB(30, 60, 120), [&] {
        polygon({ { 1, 1 }, { 13, 1 }, { 15, 3 }, { 15, 15 }, { 1, 15 } });
    });
    style(RGB(255, 255, 255), RGB(200, 205, 215), [&] { rect(3.5f, 1.5f, 12.5f, 7); });
    style(RGB(45, 45, 55), RGB(45, 45, 55), [&] { rect(4, 10, 12, 15); });
    style(RGB(200, 200, 210), RGB(200, 200, 210), [&] { rect(9, 11, 10.5f, 14); });

    // Launch game: a play button.
    origin += size;
    style(RGB(46, 160, 67), RGB(24, 100, 40), [&] { ellipse(0.5f, 0.5f, 15.5f, 15.5f); });
    style(RGB(255, 255, 255), RGB(255, 255, 255), [&] { polygon({ { 6, 4.5f }, { 11.5f, 8 }, { 6, 11.5f } }); });

    // Gizmo, move: arrows out four ways, in the gizmo's own axis colours.
    origin += size;
    style(RGB(226, 72, 72), RGB(226, 72, 72), [&] {
        rect(3, 7.2f, 12.5f, 8.8f);
        polygon({ { 1, 8 }, { 4, 5 }, { 4, 11 } });
        polygon({ { 14.5f, 8 }, { 11.5f, 5 }, { 11.5f, 11 } });
    });
    style(RGB(76, 203, 76), RGB(76, 203, 76), [&] {
        rect(7.2f, 3, 8.8f, 12.5f);
        polygon({ { 8, 1 }, { 5, 4 }, { 11, 4 } });
        polygon({ { 8, 14.5f }, { 5, 11.5f }, { 11, 11.5f } });
    });

    // Gizmo, rotate: a thick ring most of the way round, and an arrow head
    // carrying on along it where it stops -- the usual "turn" sign.
    origin += size;
    {
        const float cx = 8.0f, cy = 8.5f, radius = 5.2f;
        const float start = 1.05f, end = 2.0f * kPi - 0.35f;     // radians, anticlockwise on screen
        const auto onRing = [&](float angle, float r) {
            return Corner{ cx + std::cos(angle) * r, cy - std::sin(angle) * r };
        };
        std::vector<POINT> arc;
        for (int k = 0; k <= 24; ++k) {
            const Corner c = onRing(start + (end - start) * k / 24.0f, radius);
            arc.push_back(at(c.x, c.y));
        }
        HPEN thick = ::CreatePen(PS_SOLID, std::max(2, (int)std::lround(unit * 2.2f)), RGB(74, 134, 255));
        HGDIOBJ previous = ::SelectObject(dc, thick);
        ::Polyline(dc, arc.data(), (int)arc.size());
        ::SelectObject(dc, previous);
        ::DeleteObject(thick);

        // The head: its base across the ring at the end, its point further
        // on along the way the ring turns.
        const Corner outer = onRing(end, radius + 2.8f);
        const Corner inner = onRing(end, radius - 2.8f);
        const Corner tip = onRing(end + 0.95f, radius);
        style(RGB(74, 134, 255), RGB(74, 134, 255), [&] { polygon({ outer, tip, inner }); });
    }

    // Gizmo, scale: a box with an arrow pulling a corner out.
    origin += size;
    style(RGB(232, 232, 232), RGB(90, 90, 100), [&] { rect(1, 7, 9, 15); });
    style(RGB(255, 224, 69), RGB(200, 160, 20), [&] {
        polygon({ { 6, 9 }, { 12, 3 }, { 10, 1.5f }, { 15, 1 }, { 14.5f, 6 }, { 13, 4 }, { 7, 10 } });
    });

    ::SelectObject(dc, oldBitmap);
    ::DeleteDC(dc);
    HIMAGELIST images = ::ImageList_Create(size, size, ILC_COLOR24 | ILC_MASK, kCount, 0);
    ::ImageList_AddMasked(images, bitmap, key);
    ::DeleteObject(bitmap);
    return images;
}

} // namespace

void MainWindow::QueueThumbnailWarmUp() {
    thumbsToWarm_.clear();
    thumbsWarmed_ = 0;
    if (!world_) return;
    // What the catalogue can show: no backdrop, and no ground, which it keeps
    // off its shelves -- L4RA has 4,372 pieces of it.
    for (const nfs::SolidObject* model : world_->AllModels()) {
        if (!model || model->IsBackdrop()) continue;
        if (nfs::CategorizeModel(model->name).group == L"Terrain and roads") continue;
        if (!thumbnails_.Cached(*model)) thumbsToWarm_.push_back(model);
    }
    // Drawn from the back of the list, so reverse it to go in catalogue order.
    std::reverse(thumbsToWarm_.begin(), thumbsToWarm_.end());
    if (!thumbsToWarm_.empty())
        LOG_INFO("thumbnails: %zu to draw in the background", thumbsToWarm_.size());
}

void MainWindow::WarmThumbnails(DWORD budgetMs) {
    const DWORD start = ::GetTickCount();
    std::vector<u32> pixels;
    while (!thumbsToWarm_.empty() && ::GetTickCount() - start < budgetMs) {
        const nfs::SolidObject* model = thumbsToWarm_.back();
        thumbsToWarm_.pop_back();
        // The Library tab may have drawn it meanwhile.
        if (!thumbnails_.Cached(*model)) thumbnails_.Get(viewport_.GetRenderer(), *model, pixels);
        ++thumbsWarmed_;
    }
    // Default-pool targets would stop the device being reset on a resize.
    viewport_.GetRenderer().ReleaseThumbnailTargets();
    if (thumbsToWarm_.empty()) {
        LOG_INFO("thumbnails: %zu drawn", thumbsWarmed_);
        SetStatusText(2, Format(L"Catalogue pictures ready (%zu drawn)", thumbsWarmed_));
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

namespace {

// DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 20H1 on, 19 before it.
void DarkTitleBar(HWND window) {
    if (!window || !(::GetWindowLongW(window, GWL_STYLE) & WS_CAPTION)) return;
    const BOOL dark = TRUE;
    if (FAILED(::DwmSetWindowAttribute(window, 20, &dark, sizeof(dark))))
        ::DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
}

} // namespace

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC  = ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &MainWindow::WndProc;
    wc.hInstance     = instance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.lpszMenuName  = MAKEINTRESOURCEW(IDR_MAIN_MENU);
    wc.hIcon         = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm       = (HICON)::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                           ::GetSystemMetrics(SM_CXSMICON),
                                           ::GetSystemMetrics(SM_CYSMICON), 0);

    if (!::RegisterClassExW(&wc)) {
        LOG_ERROR("failed to register the main window class");
        return false;
    }

    // Dark title bars, for the main window and every dialog and message box the
    // editor opens: a thread hook darkens each captioned window as it activates.
    static HHOOK darkHook = ::SetWindowsHookExW(WH_CBT, [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (code == HCBT_ACTIVATE) DarkTitleBar((HWND)wParam);
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }, nullptr, ::GetCurrentThreadId());
    (void)darkHook;

    window_ = ::CreateWindowExW(0, kWindowClass, kWindowTitle,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1400, 860,
                                nullptr, nullptr, instance, this);
    if (!window_) {
        LOG_ERROR("failed to create the main window");
        return false;
    }
    DarkTitleBar(window_);

    accelerators_ = ::LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    ::ShowWindow(window_, showCommand);
    ::UpdateWindow(window_);
    return true;
}

void MainWindow::Destroy() {
    textures_.Close();
    if (toolbarImages_) { ::ImageList_Destroy(toolbarImages_); toolbarImages_ = nullptr; }
    viewport_.Destroy();
    world_.reset();

    if (uiFont_) { ::DeleteObject(uiFont_); uiFont_ = nullptr; }
    if (window_) { ::DestroyWindow(window_); window_ = nullptr; }
}

LRESULT CALLBACK MainWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = (CREATESTRUCTW*)lParam;
        self = (MainWindow*)create->lpCreateParams;
        self->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (MainWindow*)::GetWindowLongPtrW(window, GWLP_USERDATA);
    }

    if (self) return self->Handle(message, wParam, lParam);
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MainWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;

        case WM_SIZE:
            OnSize();
            UpdateGameView();
            return 0;

        case WM_SETCURSOR:
            if ((HWND)wParam == window_ && LOWORD(lParam) == HTCLIENT) {
                POINT cursor{};
                ::GetCursorPos(&cursor);
                ::ScreenToClient(window_, &cursor);
                const Splitter hit = HitSplitter(cursor);
                if (hit != Splitter::None) {
                    ::SetCursor(::LoadCursorW(nullptr, hit == Splitter::Bottom ? IDC_SIZENS : IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;

        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (libDragging_) {
                if (message == WM_MOUSEMOVE) UpdateLibraryDrag();
                else if (message == WM_LBUTTONUP) EndLibraryDrag(true);
                else if (message == WM_CAPTURECHANGED) EndLibraryDrag(false);
                return 0;
            }
            if (OnSplitterMouse(message, lParam)) return 0;
            break;

        case WM_KEYDOWN:
            if (libDragging_ && wParam == VK_ESCAPE) {
                EndLibraryDrag(false);
                return 0;
            }
            break;

        case WM_MOVE:
            UpdateGameView();
            return 0;

        case WM_TIMER:
            if (wParam == kGameViewTimer) UpdateGameView();
            return 0;

        case kRunSetupMessage:
            RunSetup();
            return 0;

        case kStopGameMessage:
            StopGame();
            return 0;

        case kBakeWriteMessage:
            WriteBake();
            return 0;

        case OnlineLibrary::kPlaceMessage:
            PlaceOnlineModel();
            return 0;

        case kShellLibraryActivate:
            if (catalog_.onActivate) catalog_.onActivate();
            return 0;

        case kShellTreeActivate: {
            // A double click in the drawn scene tree, as on the native one.
            HTREEITEM selected = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_CARET, 0);
            TVITEMW item{};
            item.mask = TVIF_PARAM;
            item.hItem = selected;
            if (world_ && selected && ::SendMessageW(sectionTree_, TVM_GETITEMW, 0, (LPARAM)&item)) {
                const intptr_t index = (intptr_t)item.lParam;
                if (index <= -kMineItemBase && (size_t)(-index - kMineItemBase) < mineHashes_.size())
                    PlaceModelAtView(mineHashes_[(size_t)(-index - kMineItemBase)]);
            }
            return 0;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam), (HWND)lParam);
            return 0;

        case WM_NOTIFY: {
            auto* header = (NMHDR*)lParam;
            if (header->idFrom == IDC_SECTION_TREE && header->code == TVN_SELCHANGEDW)
                OnSectionSelected();
            // Double-clicking one of your objects in the scene places another.
            if (header->idFrom == IDC_SECTION_TREE && header->code == NM_DBLCLK && world_) {
                HTREEITEM selected = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM, TVGN_CARET, 0);
                TVITEMW item{};
                item.mask = TVIF_PARAM;
                item.hItem = selected;
                if (selected && ::SendMessageW(sectionTree_, TVM_GETITEMW, 0, (LPARAM)&item)) {
                    const intptr_t index = (intptr_t)item.lParam;
                    if (index <= -kMineItemBase && (size_t)(-index - kMineItemBase) < mineHashes_.size())
                        PlaceModelAtView(mineHashes_[(size_t)(-index - kMineItemBase)]);
                }
            }
            if (header->idFrom == IDC_SIDE_TABS && header->code == TCN_SELCHANGE)
                ShowSideTab(TabCtrl_GetCurSel(sideTabs_));
            if (header->hwndFrom == libList_ && header->code == LVN_BEGINDRAG) {
                if (const ModelCatalog::Row* row = catalog_.RowAtItem(((const NMLISTVIEW*)lParam)->iItem)) {
                    EditorExtension* owner = ExtensionOf(row->owner);
                    if (owner) {
                        if (world_ && owner->BeginDrag(*row)) {
                            libDragging_ = true;
                            dragExtension_ = owner;
                            dragPlaced_ = false;
                            ::SetCapture(window_);
                            ::SetFocus(window_);      // for Escape
                            ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
                            SetStatusText(2, L"Drag onto the map and let go where it should stand -- Escape cancels");
                        }
                    } else {
                        BeginLibraryDrag(row->model->hash);
                    }
                }
                return 0;
            }
            if (catalog_.OnNotify(header)) return 0;
            // A value committed in the properties grid moves the object.
            if (properties_.OnNotify(header) && properties_.TakeChange()) {
                bool taken = false;
                for (auto& extension : extensions_)
                    if (!taken) taken = extension->ApplyProperties();
                if (!taken) ApplyPropertyFields(true);
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = (MINMAXINFO*)lParam;
            info->ptMinTrackSize.x = 800;
            info->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_SETFOCUS:
            if (viewport_.Handle()) ::SetFocus(viewport_.Handle());
            return 0;

        case WM_CLOSE:
            if (!ConfirmUnsavedPlacements() || !ConfirmWorldFileSaved()) return 0;
            RestoreFastBoot();
            game_.Release();   // an owned window would be destroyed with the editor
            ::DestroyWindow(window_);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(window_, message, wParam, lParam);
}

void MainWindow::OnCreate() {
    menu_ = ::GetMenu(window_);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    uiFont_ = ::CreateFontIndirectW(&metrics.lfMessageFont);

    LoadPanelSizes();
    CreateToolbar();

    regionCombo_ = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                     0, 0, 10, 200, window_,
                                     (HMENU)IDC_REGION_COMBO, instance_, nullptr);

    sectionTree_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                     TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                                     0, 0, 10, 10, window_,
                                     (HMENU)IDC_SECTION_TREE, instance_, nullptr);

    // The sidebar's two tabs and the search box they share. The Library tab's
    // controls start hidden; ShowSideTab swaps them with the scene tree.
    sideTabs_ = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                  0, 0, 10, 10, window_, (HMENU)IDC_SIDE_TABS, instance_, nullptr);
    for (const wchar_t* name : { L"Scene", L"Library", L"Edits" }) {
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = (LPWSTR)name;
        TabCtrl_InsertItem(sideTabs_, TabCtrl_GetItemCount(sideTabs_), &tab);
    }
    sideSearch_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, window_, (HMENU)IDC_SIDE_SEARCH, instance_, nullptr);
    ::SendMessageW(sideSearch_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    libShelf_ = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr,
                                  WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                  0, 0, 10, 400, window_, (HMENU)IDC_LIB_SHELF, instance_, nullptr);
    libUsed_ = ::CreateWindowExW(0, L"BUTTON", L"Only models the map places",
                                 WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, window_, (HMENU)IDC_LIB_USED, instance_, nullptr);
    libList_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                 WS_CHILD | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                     LVS_AUTOARRANGE | LVS_SHAREIMAGELISTS,
                                 0, 0, 10, 10, window_, (HMENU)IDC_LIB_LIST, instance_, nullptr);
    catalog_.Attach(libList_, libShelf_, sideSearch_, libUsed_);
    catalog_.SetThumbnails([this](const nfs::SolidObject& model, std::vector<u32>& pixels) {
        return thumbnails_.Get(viewport_.GetRenderer(), model, pixels);
    });
    catalog_.onActivate = [this]() {
        const ModelCatalog::Row* row = catalog_.Selected();
        if (EditorExtension* owner = row ? ExtensionOf(row->owner) : nullptr) owner->ActivateRow(*row);
        else if (row) PlaceModelAtView(row->model->hash);
    };
    catalog_.onSelect = [this]() {
        const ModelCatalog::Row* row = catalog_.Selected();
        std::wstring text;
        EditorExtension* owner = row ? ExtensionOf(row->owner) : nullptr;
        if (owner && owner->DescribeRow(*row, text))
            SetStatusText(2, text);
        else if (row)
            SetStatusText(2, Format(L"%s: %u triangles, stored in %s -- double-click to place it at "
                                    L"the view centre", row->name.c_str(),
                                    row->model->TriangleCount(), row->home.c_str()));
    };

    statusBar_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, window_,
                                   (HMENU)IDC_STATUS_BAR, instance_, nullptr);

    // Region summary, camera, per-frame statistics, then the navigation hint.
    const int parts[] = { 300, 640, 1180, -1 };
    ::SendMessageW(statusBar_, SB_SETPARTS, 4, (LPARAM)parts);

    for (HWND control : { regionCombo_, sectionTree_, statusBar_, sideTabs_, sideSearch_, libShelf_,
                          libUsed_ })
        ::SendMessageW(control, WM_SETFONT, (WPARAM)uiFont_, TRUE);

    edits_.Create(window_, instance_, uiFont_);
    edits_.Show(false);         // it lives on the Edits tab, which starts hidden
    edits_.SetExtraRows([this](std::vector<EditsPanel::ExtraRow>& rows) {
        for (auto& extension : extensions_) extension->EditRows(rows);
    });
    // Collision changes are listed one by one from the collision log, not
    // as whole packs.
    edits_.collisionLogged = [this](const std::string& section) {
        return collisionTools_ && collisionTools_->LogCovers(section);
    };
    edits_.onRevertExtra = [this](const void* owner, const std::vector<size_t>& indices) {
        if (EditorExtension* extension = ExtensionOf(owner)) extension->RevertRows(indices);
    };
    catalog_.SetExtraRows([this](std::vector<ModelCatalog::Row>& rows) {
        for (auto& extension : extensions_) extension->CatalogRows(rows);
    });
    properties_.Create(window_, instance_, uiFont_);
    // A committed value: the extension whose rows they are takes it, else
    // it is the selected placement's transform.
    properties_.SetChangeHandler([this] {
        bool taken = false;
        for (auto& extension : extensions_)
            if (!taken) taken = extension->ApplyProperties();
        if (!taken) ApplyPropertyFields(true);
    });
    viewport_.SetUiBars([this] { DrawShellBars(); });
    viewport_.SetUiCallback([this] {
        DrawShellPanels();
        if (viewport_.GetRenderer().Context()) properties_.Draw(viewport_.GetRenderer().Context()->Device());
    });

    if (!viewport_.Create(window_, IDC_VIEWPORT)) {
        ::MessageBoxW(window_,
                      L"Could not create the Direct3D 9 device.\n"
                      L"Make sure the DirectX 9 runtime is installed.",
                      kWindowTitle, MB_ICONERROR | MB_OK);
    }

    // G and R on an extension's point.
    viewport_.SetPointEditor(
        [this](Vec3& position, float& heading) {
            for (auto& extension : extensions_)
                if (extension->GetPoint(position, heading)) return true;
            return false;
        },
        [this](const Vec3& position, float heading, bool commit) {
            for (auto& extension : extensions_)
                if (extension->SetPoint(position, heading, commit)) return;
        });

    // Whatever this build adds, once everything it may build on is up.
    extensions_ = CreateEditorExtensions(*this);
    // After the private ones: a click tries what stands in front of a beacon first.
    extensions_.push_back(CreateMarkersExtension(*this));
    extensions_.push_back(CreateRoadsExtension(*this));
    // The mesh editor (ui/MeshTools.h), asked before the collision tools so
    // that while it edits, Ctrl+Z steps its own edit back.
    {
        MeshToolsHost host;
        host.window = window_;
        host.menu = menu_;
        host.accelerators = &accelerators_;
        host.viewport = &viewport_;
        host.world = [this] { return world_.get(); };
        host.status = [this](const std::wstring& text) { SetStatusText(2, text); };
        host.menuChecks = [this] { UpdateMenuChecks(); };
        host.importQuietly = [this](const std::wstring& path) { ImportObj(path, true); };
        host.undoCommand = CollisionTools::kUndoCommand;
        host.collision = [this] { return collisionTools_; };
        host.otherBegin = [this] { return zoneTools_ && zoneTools_->BeginEdit(); };
        host.otherActive = [this] { return zoneTools_ && zoneTools_->Editing(); };
        host.otherEnd = [this] { if (zoneTools_) zoneTools_->EndEdit(); };
        auto mesh = std::make_unique<MeshTools>(std::move(host));
        meshTools_ = mesh.get();
        extensions_.push_back(std::move(mesh));
    }
    // Objects hidden in the editor only (ui/HiddenItems.h): H, Alt+H.
    {
        HiddenItemsHost host;
        host.window = window_;
        host.menu = menu_;
        host.viewport = &viewport_;
        host.world = [this] { return world_.get(); };
        host.modelsFolder = [this] { return modelsFolder_; };
        host.status = [this](const std::wstring& text) { SetStatusText(2, text); };
        host.menuChecks = [this] { UpdateMenuChecks(); };
        extensions_.push_back(std::make_unique<HiddenItems>(std::move(host)));
    }
    // The game's visibility zones, seen and their lists edited (ui/ZoneTools.h).
    {
        ZoneToolsHost host;
        host.window = window_;
        host.menu = menu_;
        host.viewport = &viewport_;
        host.properties = &properties_;
        host.world = [this] { return world_.get(); };
        host.modelsFolder = [this] { return modelsFolder_; };
        host.status = [this](const std::wstring& text) { SetStatusText(2, text); };
        host.menuChecks = [this] { UpdateMenuChecks(); };
        host.reopenRegion = [this] {
            if (!world_) return;
            Camera& camera = viewport_.GetCamera();
            const Vec3 position = camera.Position();
            const float yaw = camera.Yaw(), pitch = camera.Pitch();
            OpenRegion(world_->Region());
            camera.SetPosition(position);
            camera.SetYawPitch(yaw, pitch);
        };
        host.busy = [this] { return meshTools_ && meshTools_->Active(); };
        auto zones = std::make_unique<ZoneTools>(std::move(host));
        zoneTools_ = zones.get();
        extensions_.push_back(std::move(zones));
    }
    // Collision editing (ui/CollisionTools.h) is part of every build, asked
    // after the build's own extensions.
    {
        CollisionToolsHost host;
        host.instance = instance_;
        host.window = window_;
        host.menu = menu_;
        host.accelerators = &accelerators_;
        host.viewport = &viewport_;
        host.properties = &properties_;
        host.world = [this] { return world_.get(); };
        host.status = [this](const std::wstring& text) { SetStatusText(2, text); };
        host.menuChecks = [this] { UpdateMenuChecks(); };
        auto tools = std::make_unique<CollisionTools>(std::move(host));
        collisionTools_ = tools.get();
        extensions_.push_back(std::move(tools));
    }
    // Test drive with the game's own physics (drive/DriveMode.h).
    {
        DriveModeHost host;
        host.window = window_;
        host.menu = menu_;
        host.toolbar = toolbar_;
        host.viewport = &viewport_;
        host.world = [this] { return world_.get(); };
        host.gameFolder = [this] { return gameFolder_; };
        host.viewCentre = [this](Vec3& point) { return world_ && ViewCentrePoint(point); };
        host.status = [this](const std::wstring& text) { SetStatusText(2, text); };
        host.menuChecks = [this] { UpdateMenuChecks(); };
        extensions_.push_back(std::make_unique<DriveMode>(std::move(host)));
    }
    for (auto& extension : extensions_) extension->OnCreate();

    // From here the frame is drawn with ImGui (ui/MainWindowShell.cpp): the
    // native menu, toolbar, status bar and side panel stay as hidden sources.
    // View > Sky and Animations, after what the menu already holds.
    for (int i = 0; menu_ && i < ::GetMenuItemCount(menu_); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(menu_, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"View") == std::wstring::npos) continue;
        if (HMENU view = ::GetSubMenu(menu_, i)) {
            ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(view, MF_STRING, kViewSkyCommand, L"S&ky (follows the camera)");
            ::AppendMenuW(view, MF_STRING, kViewAnimationsCommand, L"&Animations");
            ::AppendMenuW(view, MF_STRING, kViewGameLookCommand, L"&Game look (the game's shaders, fog and reflections)");
            ::AppendMenuW(view, MF_STRING, kViewHideBackgroundCommand, L"Hide &background scenery (PAN_ panoramas)");
            ::AppendMenuW(view, MF_STRING, kViewHideDistantLodCommand, L"Hide &distant LOD models (PAN_*_Z, LOD_, _1Z_)");
            // Remembered between sessions.
            for (auto [name, set] : { std::pair<const wchar_t*, void (WorldRenderer::*)(bool)>{
                                          L"HideBackgroundScenery", &WorldRenderer::SetBackgroundSceneryHidden },
                                      { L"HideDistantLod", &WorldRenderer::SetDistantLodHidden } }) {
                DWORD stored = 0, size = sizeof(stored);
                if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &stored, &size) ==
                    ERROR_SUCCESS)
                    (viewport_.GetRenderer().*set)(stored != 0);
            }
            // FRS_GAMELOOK=1: start in the game look (for captures).
            if (const char* look = std::getenv("FRS_GAMELOOK"); look && *look == '1')
                viewport_.GetRenderer().SetGameLook(true);
        }
        break;
    }
    shell_ = true;
    online_ = std::make_unique<OnlineLibrary>();
    online_->SetWindow(window_);
    ::SetMenu(window_, nullptr);
    HideNativeShell();
    OnSize();

    UpdateMenuChecks();
    SetStatusText(0, L"No region loaded");
    SetStatusText(3, L"Left click: select   Right mouse: look   WASD: move   Q/E: down/up   Wheel: speed");

    // Pick the folder back up from the last session. The first run -- or one
    // after an update with something new to install -- opens the setup once
    // the window is on screen.
    // FRS_GAME_FOLDER opens another game folder for this session only, without
    // replacing the stored one (testing Underground 1 beside Underground 2).
    const wchar_t* overrideFolder = _wgetenv(L"FRS_GAME_FOLDER");
    const std::wstring stored = overrideFolder && *overrideFolder ? std::wstring(overrideFolder) : LoadStoredGameFolder();
    if (!stored.empty() && DirectoryExists(stored + L"\\TRACKS"))
        SetGameFolder(stored);
    if (StoredSetupVersion() < kSetupVersion) ::PostMessageW(window_, kRunSetupMessage, 0, 0);
}

void MainWindow::OnSize() {
    if (!statusBar_) return;
    if (shell_) {
        // Everything is drawn in the viewport now: it takes the whole window.
        HideNativeShell();
        RECT client{};
        ::GetClientRect(window_, &client);
        viewRect_ = client;
        contentBottom_ = client.bottom;
        ::MoveWindow(viewport_.Handle(), 0, 0, client.right, client.bottom, TRUE);
        return;
    }

    ::SendMessageW(statusBar_, WM_SIZE, 0, 0);
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);

    RECT client{};
    ::GetClientRect(window_, &client);

    RECT status{}, tools{};
    ::GetWindowRect(statusBar_, &status);
    ::GetWindowRect(toolbar_, &tools);
    const int top = tools.bottom - tools.top;
    const int bottom = client.bottom - (status.bottom - status.top);
    const int contentHeight = std::max(0, bottom - top);
    const int width = client.right;

    // The stored sizes as asked for; a window too small for them squeezes
    // the panels without forgetting what they were.
    const int left = std::clamp(leftWidth_, kMinSideWidth,
                                std::max(kMinSideWidth, width - kMinSideWidth - kMinViewport));
    // The properties are a panel docked inside the view now (ImGui).
    const int right = 0;
    // The edits live on the sidebar's Edits tab: nothing under the view.
    const int edits = 0;

    ::MoveWindow(regionCombo_, kPanelPadding, top + kPanelPadding,
                 left - 2 * kPanelPadding, 200, TRUE);

    // Region, the two tabs, the search box, then whichever tab is showing.
    const int sideWidth = left - 2 * kPanelPadding;
    int at = top + kPanelPadding + kComboHeight + kPanelPadding;
    ::MoveWindow(sideTabs_, kPanelPadding, at, sideWidth, 24, TRUE);
    at += 24 + 2;
    const int contentBottomSide = bottom - kPanelPadding;
    // The Edits tab has nothing to search, so its list starts where the box is.
    edits_.Layout(kPanelPadding, at, sideWidth, std::max(0, contentBottomSide - at));
    ::MoveWindow(sideSearch_, kPanelPadding, at, sideWidth, 22, TRUE);
    at += 22 + kPanelPadding;
    ::MoveWindow(sectionTree_, kPanelPadding, at, sideWidth, std::max(0, contentBottomSide - at), TRUE);
    ::MoveWindow(libShelf_, kPanelPadding, at, sideWidth, 400, TRUE);
    ::MoveWindow(libUsed_, kPanelPadding, at + 26, sideWidth, 18, TRUE);
    const int listTop = at + 26 + 18 + 4;
    ::MoveWindow(libList_, kPanelPadding, listTop, sideWidth, std::max(0, contentBottomSide - listTop), TRUE);

    // The viewport in the middle, the selection's properties on its right and
    // the region's edits under it. The padding gaps around the viewport are
    // what the pointer drags to resize them.
    const int middle = std::max(100, width - left - right);
    const int viewHeight = std::max(100, contentHeight - edits);
    viewRect_ = RECT{ left, top, left + middle, top + viewHeight };
    contentBottom_ = bottom;
    ::MoveWindow(viewport_.Handle(), left, top, middle, viewHeight, TRUE);
    (void)edits;
    properties_.Layout(left + middle, top, right, contentHeight);
}

void MainWindow::CreateToolbar() {
    toolbar_ = ::CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                 WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_TOP,
                                 0, 0, 0, 0, window_, (HMENU)IDC_TOOLBAR, instance_, nullptr);
    ::SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    ::SendMessageW(toolbar_, WM_SETFONT, (WPARAM)uiFont_, FALSE);
    ::SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);

    const int iconSize = ::MulDiv(16, (int)::GetDpiForWindow(window_), 96);
    toolbarImages_ = BuildToolbarIcons(iconSize);
    ::SendMessageW(toolbar_, TB_SETIMAGELIST, 0, (LPARAM)toolbarImages_);

    // What a session keeps reaching for: the game, then the tools. The world's
    // own commands (new, open, save) are in the File menu and on Ctrl+S.
    const struct { int image; int id; const wchar_t* text; } items[] = {
        // Becomes Show map / Show game while the game runs (SetLaunchLabel).
        { 3, IDC_LAUNCH_GAME, L"Launch game" },
        { -1, 0, nullptr },
        // Which handles the selected object shows; one of the three is
        // always the one in use (UpdateMenuChecks keeps it pressed).
        { 4, ID_GIZMO_MOVE,   L"Move" },
        { 5, ID_GIZMO_ROTATE, L"Rotate" },
        { 6, ID_GIZMO_SCALE,  L"Scale" },
    };
    std::vector<TBBUTTON> buttons;
    for (const auto& item : items) {
        TBBUTTON button{};
        if (item.id == 0) {
            button.fsStyle = BTNS_SEP;
        } else {
            button.iBitmap = item.image;
            button.idCommand = item.id;
            button.fsState = TBSTATE_ENABLED;
            const bool gizmo = item.id == ID_GIZMO_MOVE || item.id == ID_GIZMO_ROTATE ||
                               item.id == ID_GIZMO_SCALE;
            button.fsStyle = (BYTE)((gizmo ? BTNS_CHECKGROUP : BTNS_BUTTON) | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
            if (item.id == ID_GIZMO_MOVE) button.fsState |= TBSTATE_CHECKED;
            button.iString = (INT_PTR)item.text;
        }
        buttons.push_back(button);
    }
    ::SendMessageW(toolbar_, TB_ADDBUTTONSW, buttons.size(), (LPARAM)buttons.data());
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

void MainWindow::SetLaunchLabel(const wchar_t* text) {
    TBBUTTONINFOW info{};
    info.cbSize = sizeof(info);
    info.dwMask = TBIF_TEXT;
    info.pszText = (LPWSTR)text;
    ::SendMessageW(toolbar_, TB_SETBUTTONINFOW, IDC_LAUNCH_GAME, (LPARAM)&info);
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

MainWindow::Splitter MainWindow::HitSplitter(POINT point) const {
    if (point.y < viewRect_.top || point.y >= contentBottom_) return Splitter::None;
    if (point.x >= viewRect_.left - kPanelPadding && point.x < viewRect_.left) return Splitter::Left;
    if (point.x >= viewRect_.left && point.x < viewRect_.right &&
        point.y >= viewRect_.bottom && point.y < viewRect_.bottom + kPanelPadding)
        return Splitter::Bottom;
    return Splitter::None;
}

bool MainWindow::OnSplitterMouse(UINT message, LPARAM lParam) {
    const POINT point{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
    RECT client{};
    ::GetClientRect(window_, &client);

    // Dragging starts from the sizes on screen, so a panel squeezed by a small
    // window does not jump back to its stored size under the pointer.
    auto settle = [&] {
        leftWidth_    = viewRect_.left;
        rightWidth_   = client.right - viewRect_.right;
        bottomHeight_ = contentBottom_ - viewRect_.bottom;
    };

    switch (message) {
        case WM_LBUTTONDOWN: {
            const Splitter hit = HitSplitter(point);
            if (hit == Splitter::None) return false;
            settle();
            dragging_ = hit;
            dragOffset_ = hit == Splitter::Left  ? viewRect_.left - point.x
                        : hit == Splitter::Right ? viewRect_.right - point.x
                                                 : viewRect_.bottom - point.y;
            ::SetCapture(window_);
            return true;
        }
        case WM_MOUSEMOVE:
            if (dragging_ == Splitter::None) return false;
            ::SetCursor(::LoadCursorW(nullptr, dragging_ == Splitter::Bottom ? IDC_SIZENS : IDC_SIZEWE));
            if (dragging_ == Splitter::Left)       leftWidth_ = point.x + dragOffset_;
            else if (dragging_ == Splitter::Right) rightWidth_ = client.right - (point.x + dragOffset_);
            else                                   bottomHeight_ = contentBottom_ - (point.y + dragOffset_);
            OnSize();
            UpdateGameView();
            return true;
        case WM_LBUTTONUP:
            if (dragging_ == Splitter::None) return false;
            ::ReleaseCapture();         // WM_CAPTURECHANGED finishes the drag
            return true;
        case WM_CAPTURECHANGED:
            if (dragging_ == Splitter::None) return false;
            dragging_ = Splitter::None;
            settle();
            StorePanelSizes();
            return true;
        default:
            return false;
    }
}

void MainWindow::LoadPanelSizes() {
    for (auto [name, value] : { std::pair<const wchar_t*, int*>{ L"LeftPanelWidth", &leftWidth_ },
                                { L"RightPanelWidth", &rightWidth_ },
                                { L"EditsPanelHeight", &bottomHeight_ } }) {
        DWORD stored = 0, size = sizeof(stored);
        if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &stored, &size) ==
                ERROR_SUCCESS &&
            stored > 0 && stored < 10000)
            *value = (int)stored;
    }
}

void MainWindow::StorePanelSizes() const {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    for (auto [name, value] : { std::pair<const wchar_t*, int>{ L"LeftPanelWidth", leftWidth_ },
                                { L"RightPanelWidth", rightWidth_ },
                                { L"EditsPanelHeight", bottomHeight_ } }) {
        const DWORD stored = (DWORD)value;
        ::RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&stored, sizeof(stored));
    }
    ::RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void MainWindow::OnCommand(int id, int notification, HWND control) {
    if (properties_.OnCommand(id)) return;
    if (control && control == sideSearch_ && notification == EN_CHANGE && sideTab_ == 0 && !shell_) {
        PopulateSectionTree();
        return;
    }
    if (control && catalog_.OnCommand(control, notification)) return;
    if (control == regionCombo_ && notification == CBN_SELCHANGE) {
        const int index = (int)::SendMessageW(regionCombo_, CB_GETCURSEL, 0, 0);
        if (index >= 0) {
            wchar_t region[32] = {};
            ::SendMessageW(regionCombo_, CB_GETLBTEXT, (WPARAM)index, (LPARAM)region);
            OpenRegion(region);
        }
        return;
    }

    // Move, rotate and scale are tools like the extensions' own: choosing one
    // turns those off. Their keys (1, 2, 3) go to a tool that is on first.
    if (id == ID_GIZMO_MOVE || id == ID_GIZMO_ROTATE || id == ID_GIZMO_SCALE) {
        const int digit = id == ID_GIZMO_MOVE ? 1 : id == ID_GIZMO_ROTATE ? 2 : 3;
        if (notification == 1)
            for (auto& extension : extensions_)
                if (extension->ToolActive() && extension->ToolDigit(digit)) return;
        for (auto& extension : extensions_) extension->CloseTools();
    }
    for (auto& extension : extensions_)
        if (extension->OnCommand(id)) return;
    if (id == kViewSkyCommand) {
        viewport_.GetRenderer().SetSkyVisible(!viewport_.GetRenderer().SkyVisible());
        UpdateMenuChecks();
        return;
    }
    if (id == kViewAnimationsCommand) {
        viewport_.GetRenderer().SetAnimationsEnabled(!viewport_.GetRenderer().AnimationsEnabled());
        UpdateMenuChecks();
        return;
    }
    if (id == kViewHideBackgroundCommand || id == kViewHideDistantLodCommand) {
        WorldRenderer& renderer = viewport_.GetRenderer();
        const bool background = id == kViewHideBackgroundCommand;
        const bool hidden = background ? !renderer.BackgroundSceneryHidden() : !renderer.DistantLodHidden();
        if (background) renderer.SetBackgroundSceneryHidden(hidden);
        else renderer.SetDistantLodHidden(hidden);
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
            ERROR_SUCCESS) {
            const DWORD stored = hidden ? 1 : 0;
            ::RegSetValueExW(key, background ? L"HideBackgroundScenery" : L"HideDistantLod", 0, REG_DWORD,
                             (const BYTE*)&stored, sizeof(stored));
            ::RegCloseKey(key);
        }
        UpdateMenuChecks();
        return;
    }
    if (id == kViewGameLookCommand) {
        WorldRenderer& renderer = viewport_.GetRenderer();
        renderer.SetGameLook(!renderer.GameLook());
        UpdateMenuChecks();
        return;
    }
    if (id == MeshTools::kBakeCommand) {
        bakeOpen_ = true;
        return;
    }

    if (id >= ID_TRAVEL_ZONE && id < ID_TRAVEL_ZONE + 100) {
        TravelToZone((size_t)(id - ID_TRAVEL_ZONE));
        return;
    }
    if (id >= ID_TRAVEL_DISTRICT && id < ID_TRAVEL_DISTRICT + 100) {
        TravelToDistrict((size_t)(id - ID_TRAVEL_DISTRICT));
        return;
    }

    switch (id) {
        case ID_FILE_BLENDER: {
            const std::wstring blender = BlenderLink::ChooseBlender(window_, kSettingsKey);
            if (!blender.empty()) SetStatusText(2, L"Blender: " + blender);
            break;
        }

        case ID_FILE_OPEN_GAME:  RunSetup(); break;
        case ID_FILE_EXIT:       ::PostMessageW(window_, WM_CLOSE, 0, 0); break;

        case ID_WORLD_CLOSE:     CloseRegion(); break;
        case ID_WORLD_NEW:       NewWorld(); break;
        case ID_WORLD_OPEN:      OpenWorld(); break;
        case ID_WORLD_SAVE:      SaveWorld(); break;
        case ID_WORLD_SAVE_AS:   SaveWorldAs(); break;
        case ID_WORLD_PUBLISH:   PublishWorld(); break;
        case ID_WORLD_LOAD_ALL:  LoadAllSections(); break;
        case ID_WORLD_FRAME:
            if (world_) viewport_.FrameBounds(world_->TrackBounds());
            else        viewport_.FrameWorld();
            break;

        case ID_VIEW_TEXTURED:     SetRenderMode(RenderMode::Textured); break;
        case ID_VIEW_UNLIT:        SetRenderMode(RenderMode::Unlit); break;
        case ID_VIEW_VERTEX_COLOR: SetRenderMode(RenderMode::VertexColor); break;
        case ID_VIEW_WIREFRAME:    SetRenderMode(RenderMode::Wireframe); break;

        case ID_VIEW_FRUSTUM_CULL:
            viewport_.GetRenderer().SetCullingEnabled(!viewport_.GetRenderer().CullingEnabled());
            UpdateMenuChecks();
            break;

        case ID_FILE_EXPORT:
            ExportSelectedObjects();
            break;

        case ID_FILE_IMPORT:
            ImportEditedObjects();
            break;

        case ID_OBJECT_BLENDER:
            EditInBlender();
            break;

        case ID_OBJECT_NEW_BLENDER:
            NewObjectInBlender();
            break;

        case ID_OBJECT_NEW_OBJ:
            NewObjectFromObj();
            break;

        case ID_OBJECT_DELETE_MINE:
            DeleteMyObjects();
            break;

        case ID_GIZMO_MOVE:
        case ID_GIZMO_ROTATE:
        case ID_GIZMO_SCALE:
            viewport_.SetGizmoMode(id == ID_GIZMO_MOVE ? GizmoMode::Move
                                   : id == ID_GIZMO_ROTATE ? GizmoMode::Rotate : GizmoMode::Scale);
            UpdateMenuChecks();
            SetStatusText(2, id == ID_GIZMO_MOVE ? L"Gizmo: move -- arrows along an axis, the square across the ground"
                             : id == ID_GIZMO_ROTATE ? L"Gizmo: rotate -- drag a ring; Ctrl turns in 15 degree steps"
                                                     : L"Gizmo: scale -- drag a box out or in");
            break;

        case ID_FILE_OVERLAYS:
            ShowEditedMeshes();
            break;

        case IDC_SAVE_CAMERA:
            SaveCameraPosition();
            break;

        case IDC_GO_CAMERA:
            RestoreCameraPosition();
            break;

        case IDC_LAUNCH_GAME:
            LaunchGame();
            break;

        case ID_VIEW_STREAMING: {
            WorldRenderer& renderer = viewport_.GetRenderer();
            renderer.SetStreamingEnabled(!renderer.StreamingEnabled());
            UpdateMenuChecks();
            SetStatusText(2, renderer.StreamingEnabled()
                                 ? L"Streaming sections around the camera"
                                 : L"Drawing every section at once");
            break;
        }

        case ID_VIEW_ALTERNATE:
            viewport_.GetRenderer().SetAlternateSectionsVisible(
                !viewport_.GetRenderer().AlternateSectionsVisible());
            UpdateMenuChecks();
            break;

        case ID_VIEW_LIBRARY:
            viewport_.GetRenderer().SetLibraryVisible(
                !viewport_.GetRenderer().LibraryVisible());
            UpdateMenuChecks();
            break;

        case ID_VIEW_SCENERY:
            viewport_.GetRenderer().SetSceneryVisible(
                !viewport_.GetRenderer().SceneryVisible());
            UpdateMenuChecks();
            break;

        case ID_FILE_EDITS:        ShowRegionEditList(); break;
        case IDC_EDITS_REFRESH:    if (world_) edits_.Refresh(world_.get(), modelsFolder_); break;
        case IDC_EDITS_REVERT:     RevertTickedEdits(); break;
        case IDC_PROP_SNAP: {
            bool taken = false;
            for (auto& extension : extensions_)
                if (!taken) taken = extension->MoveSelectionToView();
            if (!taken) SnapSelectedPlacement();
            break;
        }
        case IDC_PROP_REMOVE:
        case ID_OBJECT_DELETE: {
            bool taken = false;
            for (auto& extension : extensions_)
                if (!taken) taken = extension->DeleteSelection();
            if (!taken) DeleteSelectedPlacement();
            break;
        }
        case IDC_PROP_BLENDER:
            EditInBlender();
            break;
        case ID_OBJECT_PLACE:      PlaceObject(); break;
        case ID_OBJECT_PLACE_MINE: PlaceObject(true); break;
        case ID_OBJECT_PROPERTIES: EditSelectedPlacement(); break;
        case ID_OBJECT_SNAP:       SnapSelectedPlacement(); break;
        case ID_OBJECT_COPY:       CopySelection(); break;
        case ID_OBJECT_PASTE:      PasteClipboard(); break;
        case ID_OBJECT_SAVE:       SavePlacements(); break;
        case ID_OBJECT_GRAB:
            viewport_.BeginTransform(Viewport::TransformMode::Move);
            break;
        case ID_OBJECT_ROTATE:
            viewport_.BeginTransform(Viewport::TransformMode::Rotate);
            break;

        case ID_VIEW_COLLISION:
            viewport_.GetRenderer().SetCollisionVisible(
                !viewport_.GetRenderer().CollisionVisible());
            UpdateMenuChecks();
            break;

        case ID_VIEW_BACKDROP:
            viewport_.GetRenderer().SetBackdropVisible(
                !viewport_.GetRenderer().BackdropVisible());
            UpdateMenuChecks();
            break;

        case ID_VIEW_BACKFACE_CULL:
            viewport_.GetRenderer().SetBackfaceCullingEnabled(
                !viewport_.GetRenderer().BackfaceCullingEnabled());
            UpdateMenuChecks();
            break;

        // MINIMAP PREVIEW (temporary)
        case ID_MINIMAP_GENERATE:
            GenerateMinimapBuildings();
            break;

        case ID_MINIMAP_SHOW:
            if (!viewport_.GetRenderer().HasMinimapBuildings()) {
                GenerateMinimapBuildings();
                break;
            }
            viewport_.GetRenderer().SetMinimapPreviewVisible(
                !viewport_.GetRenderer().MinimapPreviewVisible());
            UpdateMenuChecks();
            break;

        case ID_MINIMAP_ORIGINALS:
            viewport_.GetRenderer().SetOriginalBuildingsVisible(
                !viewport_.GetRenderer().OriginalBuildingsVisible());
            UpdateMenuChecks();
            break;

        case ID_VIEW_TEXTURES:
            textures_.Show(instance_, window_, world_.get());
            break;


        case ID_HELP_ABOUT:
            ShowAbout();
            break;

        case ID_HELP_SUPPORT:
            ::ShellExecuteW(window_, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;

        case ID_HELP_README: {
            wchar_t path[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring readme = path;
            readme = readme.substr(0, readme.find_last_of(L"\\/")) + L"\\README.txt";
            if ((INT_PTR)::ShellExecuteW(window_, L"open", readme.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
                ::MessageBoxW(window_, (L"There is no README.txt beside the editor:\n" + readme).c_str(),
                              kWindowTitle, MB_ICONINFORMATION | MB_OK);
            break;
        }

        default:
            break;
    }
}

// The setup wizard: the game folder, and the plugins the editor's work needs
// in the game (ui/SetupWizard.cpp). Cancelling it changes nothing.
void MainWindow::RunSetup() {
    if (game_.Running()) {
        ::MessageBoxW(window_, L"Close the game first: its plugins cannot be replaced while it runs.",
                      kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }
    std::wstring folder = gameFolder_;
    if (!RunSetupWizard(instance_, window_, folder)) {
        // Asked once: a player who closes it on the first run has File > Game
        // Folder and Plugins for later.
        if (StoredSetupVersion() < kSetupVersion) StoreSetupVersion(kSetupVersion);
        return;
    }
    StoreSetupVersion(kSetupVersion);
    StoreGameFolder(folder);
    if (folder != gameFolder_ || !world_) SetGameFolder(folder);
}

void MainWindow::ShowAbout() {
    const int answer = ::MessageBoxW(
        window_,
        L"FRS World Editor 0.1.0\n"
        L"A map editor for Need for Speed Underground 2.\n\n"
        L"Created by Chrystian Farias.\n\n"
        L"Licensed under Creative Commons Attribution-NonCommercial 4.0 (CC BY-NC 4.0): you may "
        L"share and adapt it, crediting Chrystian Farias, and not for commercial purposes -- it may "
        L"not be sold.\n\n"
        L"Uses the Win32 property grid by David MacDermot (CPOL). The package includes Ultimate ASI "
        L"Loader by ThirteenAG (MIT). Need for Speed is a trademark of Electronic Arts; this is a fan "
        L"project, not affiliated with EA.\n\n"
        L"If the editor helps you, you can buy me a coffee:\n"
        L"https://buymeacoffee.com/chrystianfarias\n\n"
        L"Open that page now?",
        L"About FRS World Editor", MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON2);
    if (answer == IDYES) ::ShellExecuteW(window_, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Worlds
// ---------------------------------------------------------------------------

// Underground 1 regions open read-only: the plugins that would hand the game
// an edit are written for Underground 2. True (and says so) when the open
// region is one.
bool MainWindow::RefuseReadOnlyWorld() {
    if (!world_ || !world_->IsUnderground1()) return false;
    ::MessageBoxW(window_, L"Need for Speed Underground regions are read-only for now: the editor can show and export them, but nothing it writes would reach the game.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
    return true;
}

void MainWindow::UpdateTitle() {
    std::wstring title = kWindowTitle;
    if (world_) {
        title += L" - " + world_->Region();
        if (world_->IsUnderground1())
            title += L" (Underground, track " + std::to_wstring(world_->TrackNumber()) + L", read-only)";
    }
    if (!gameFolder_.empty()) title += L" [" + worldName_ + L"]";
    ::SetWindowTextW(window_, title.c_str());
}

bool MainWindow::CanSwitchWorld() {
    if (!gameFolder_.empty() && !game_.Running()) return true;
    ::MessageBoxW(window_, gameFolder_.empty() ? L"Pick the game folder first (File > Game Folder and Plugins)."
                                               : L"Close the game first: it reads the open world's files.",
                  kWindowTitle, MB_ICONINFORMATION | MB_OK);
    return false;
}

// A world is one file (io/WorldFile.h), kept wherever the person likes;
// <game>\models is where the game reads the open one from.

std::wstring MainWindow::WorldDisplayName() const {
    if (worldPath_.empty()) return L"untitled world";
    const size_t slash = worldPath_.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? worldPath_ : worldPath_.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

void MainWindow::RememberWorldFile() {
    worldSignature_ = io::WorldSignature(modelsFolder_);
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        ::RegSetValueExW(key, L"WorldFile", 0, REG_SZ, (const BYTE*)worldPath_.c_str(),
                         (DWORD)((worldPath_.size() + 1) * sizeof(wchar_t)));
        ::RegSetValueExA(key, "WorldSignature", 0, REG_SZ, (const BYTE*)worldSignature_.c_str(),
                         (DWORD)(worldSignature_.size() + 1));
        ::RegCloseKey(key);
    }
    worldName_ = WorldDisplayName();
    UpdateTitle();
}

bool MainWindow::ConfirmWorldFileSaved() {
    if (modelsFolder_.empty() || io::WorldSignature(modelsFolder_) == worldSignature_) return true;
    if (worldPath_.empty() && !io::WorldHasFiles(modelsFolder_)) return true;   // nothing edited yet
    const std::wstring question =
        Format(L"The edits of \"%s\" have changed since the world file was last saved.\n\n"
               L"Save the world file now?", WorldDisplayName().c_str());
    const int answer = ::MessageBoxW(window_, question.c_str(), kWindowTitle, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (answer == IDCANCEL) return false;
    if (answer == IDNO) return true;
    if (worldPath_.empty()) {
        SaveWorldAs();
        return !worldPath_.empty() && io::WorldSignature(modelsFolder_) == worldSignature_;
    }
    return WriteWorldFile(worldPath_);
}

bool MainWindow::WriteWorldFile(const std::wstring& path) {
    io::WorldFile file;
    std::wstring error;
    // Entries of its own an extension keeps survive a resave.
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) file.Load(path, error);
    size_t files = 0;
    if (!io::PackWorld(modelsFolder_, file, files, error) || !file.Save(path, error)) {
        ::MessageBoxW(window_, (L"The world file was not written.\n\n" + error).c_str(), kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return false;
    }
    worldPath_ = path;
    RememberWorldFile();
    LOG_INFO("world file %S written: %zu files", path.c_str(), files);
    return true;
}

bool MainWindow::InstallWorldFile(const io::WorldFile& file, const std::wstring& path) {
    const std::wstring region = world_ ? world_->Region() : std::wstring();
    Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();
    CloseRegion();

    // The world that was open is moved, never deleted: models\worlds\_before-<time>.
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const std::wstring backup = modelsFolder_ + Format(L"\\worlds\\_before-%04u%02u%02u-%02u%02u%02u", now.wYear,
                                                       now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    ::CreateDirectoryW((modelsFolder_ + L"\\worlds").c_str(), nullptr);
    std::wstring error;
    size_t files = 0;
    const bool ok = io::UnpackWorld(file, modelsFolder_, backup, files, error);
    if (ok) {
        worldPath_ = path;
        RememberWorldFile();
        LOG_INFO("world %S opened: %zu files written into models", path.empty() ? L"(new)" : path.c_str(), files);
    }
    if (!region.empty()) {
        OpenRegion(region);
        camera.SetPosition(position);
        camera.SetYawPitch(yaw, pitch);
    }
    if (!ok)
        ::MessageBoxW(window_, (L"The world could not be opened.\n\n" + error).c_str(), kWindowTitle,
                      MB_ICONWARNING | MB_OK);
    return ok;
}

void MainWindow::NewWorld() {
    if (!CanSwitchWorld() || !ConfirmUnsavedPlacements() || !ConfirmWorldFileSaved()) return;
    if (InstallWorldFile(io::WorldFile{}, L""))
        SetStatusText(2, L"New world: the game as shipped. Save World picks where its file goes.");
}

void MainWindow::OpenWorld() {
    if (!CanSwitchWorld() || !ConfirmUnsavedPlacements() || !ConfirmWorldFileSaved()) return;
    wchar_t path[MAX_PATH * 2] = {};
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"FRS worlds (*.frsworld)\0*.frsworld\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH * 2;
    dialog.lpstrTitle = L"Open World";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&dialog)) return;
    io::WorldFile file;
    std::wstring error;
    if (!file.Load(path, error)) {
        ::MessageBoxW(window_, (L"That world could not be read.\n\n" + error).c_str(), kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }
    if (InstallWorldFile(file, path)) SetStatusText(2, Format(L"World \"%s\" opened", WorldDisplayName().c_str()));
}

void MainWindow::SaveWorldAs() {
    if (modelsFolder_.empty()) return;
    wchar_t path[MAX_PATH * 2] = {};
    const std::wstring suggested = worldPath_.empty() ? L"My world.frsworld" : worldPath_;
    wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"FRS worlds (*.frsworld)\0*.frsworld\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH * 2;
    dialog.lpstrDefExt = L"frsworld";
    dialog.lpstrTitle = L"Save World As";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&dialog)) return;
    // Whatever is still only in the editor goes into the game files first.
    for (auto& extension : extensions_) extension->Save();
    if (world_ && !SavePlacements()) return;
    if (WriteWorldFile(path)) SetStatusText(2, Format(L"World saved as %s", path));
}

void MainWindow::PublishWorld() {
    if (modelsFolder_.empty() || gameFolder_.empty()) {
        SetStatusText(2, L"Choose the game folder first: a world is published from it.");
        return;
    }
    // What is still only in the editor goes into the game files first: the
    // zip is made from those, exactly as the game would read them now.
    for (auto& extension : extensions_) extension->Save();
    if (world_ && !SavePlacements()) return;

    const std::vector<io::PublishedFile> files = io::CollectPublishedFiles(modelsFolder_);
    if (files.empty()) {
        ::MessageBoxW(window_,
                      L"This world has nothing the game reads yet.\n\nEdits reach the game files when they are "
                      L"saved (Ctrl+S); roads and terrain when they are baked.",
                      kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }
    u64 bytes = 0;
    for (const io::PublishedFile& file : files) bytes += file.size;

    const wchar_t* kAuthorValue = L"PublishAuthor";
    wchar_t author[128] = {};
    DWORD authorSize = sizeof(author);
    ::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kAuthorValue, RRF_RT_REG_SZ, nullptr, author, &authorSize);

    PublishDetails details;
    details.name = WorldDisplayName();
    details.author = author;
    details.note = Format(
        L"Makes a zip that players unpack into their game folder to play this world, with no editor. "
        L"It carries what the game reads -- %zu files, %.1f MB -- and FRSWorldLoader.asi, but not the "
        L"world file: it cannot be opened for editing. Roads and terrain go in only once baked.",
        files.size(), bytes / (1024.0 * 1024.0));
    if (!AskPublishDetails(instance_, window_, details)) return;
    ::RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kAuthorValue, REG_SZ, details.author.c_str(),
                      (DWORD)((details.author.size() + 1) * sizeof(wchar_t)));

    wchar_t path[MAX_PATH * 2] = {};
    std::wstring suggested = details.name;
    for (wchar_t& c : suggested)
        if (c < 32 || std::wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    suggested += L".zip";
    wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Zip (*.zip)\0*.zip\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH * 2;
    dialog.lpstrDefExt = L"zip";
    dialog.lpstrTitle = L"Publish World";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&dialog)) return;

    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring editorFolder = exe;
    editorFolder.resize(editorFolder.find_last_of(L"\\/"));

    io::PublishRequest request;
    request.gameFolder = gameFolder_;
    request.name = details.name;
    request.author = details.author;
    request.description = details.description;
    request.editorVersion = io::FileVersionText(exe);
    // The editor's own copy (the release lays it out in game\), then the one
    // installed in the game: the newer of the two goes in.
    request.loaderCandidates = { editorFolder + L"\\game\\scripts\\FRSWorldLoader.asi",
                                 gameFolder_ + L"\\scripts\\FRSWorldLoader.asi" };
    if (::GetFileAttributesW((editorFolder + L"\\game\\dinput8.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        request.asiLoader = editorFolder + L"\\game\\dinput8.dll";
    request.zipPath = path;

    ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    io::PublishResult result;
    std::wstring error;
    if (!io::PublishWorld(request, result, error)) {
        ::MessageBoxW(window_, (L"The world was not published.\n\n" + error).c_str(), kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }

    std::wstring regions;
    for (const std::wstring& region : result.regions) regions += (regions.empty() ? L"" : L", ") + region;
    SetStatusText(2, Format(L"World \"%s\" published: %s", details.name.c_str(), path));
    const std::wstring message = Format(
        L"\"%s\" is published:\n%s\n\n%zu world files%s%s, %.1f MB, with FRSWorldLoader.asi %s.\n\n"
        L"Players unpack it into their game folder; the README inside says how. Show it in Explorer?",
        details.name.c_str(), path, result.files, regions.empty() ? L"" : L" for ", regions.c_str(),
        result.bytes / (1024.0 * 1024.0), result.loaderVersion.empty() ? L"" : result.loaderVersion.c_str());
    if (::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONINFORMATION | MB_YESNO) == IDYES) {
        const std::wstring arguments = L"/select,\"" + std::wstring(path) + L"\"";
        ::ShellExecuteW(window_, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::SwitchToWorld(const std::wstring& target, bool createNew) {
    // The same region from the same spot: worlds are usually edits of the
    // same streets, compared one against the other.
    const std::wstring region = world_ ? world_->Region() : std::wstring();
    Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();

    CloseRegion();
    std::wstring error;
    const bool switched = io::SwitchWorld(modelsFolder_, target, createNew, error);
    worldName_ = io::ActiveWorldName(modelsFolder_);
    if (!region.empty()) {
        OpenRegion(region);
        camera.SetPosition(position);
        camera.SetYawPitch(yaw, pitch);
    }
    UpdateTitle();

    if (!switched)
        ::MessageBoxW(window_, (L"The world was not changed.\n\n" + error).c_str(), kWindowTitle,
                      MB_ICONWARNING | MB_OK);
    else
        SetStatusText(2, Format(createNew ? L"New world \"%s\" started" : L"World \"%s\" opened",
                                worldName_.c_str()));
}

void MainWindow::SaveWorld() {
    if (!world_) {
        SetStatusText(2, L"No region open: nothing to save.");
        return;
    }
    if (RefuseReadOnlyWorld()) return;
    // Extensions write what they have not yet; placements wait for this, and
    // reopen the region once written.
    for (auto& extension : extensions_) extension->Save();
    if (!SavePlacements()) return;
    // Then the world file: the first time, wherever the person wants it.
    if (worldPath_.empty()) { SaveWorldAs(); return; }
    if (WriteWorldFile(worldPath_)) SetStatusText(2, Format(L"World \"%s\" saved", worldName_.c_str()));
}

std::wstring MainWindow::CameraValueName() const {
    if (!world_) return L"Camera";
    return std::wstring(L"Camera.") + (world_->IsUnderground1() ? L"U1." : L"") + world_->Region();
}

void MainWindow::SaveCameraPosition() {
    const Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();

    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    // Five floats in one value: where the eye is and where it looks. Kept in the
    // registry beside the game folder so it survives the editor being rebuilt
    // and restarted, which happens far more often than a region changes.
    const float viewpoint[5] = { position.x, position.y, position.z,
                                 camera.Yaw(), camera.Pitch() };
    ::RegSetValueExW(key, L"Camera", 0, REG_BINARY, (const BYTE*)viewpoint,
                     sizeof(viewpoint));
    // And one per region (and game): coming back to a region finds its own.
    if (world_)
        ::RegSetValueExW(key, CameraValueName().c_str(), 0, REG_BINARY, (const BYTE*)viewpoint,
                         sizeof(viewpoint));
    ::RegCloseKey(key);

    SetStatusText(2, Format(L"Camera saved at %.0f, %.0f, %.0f",
                            position.x, position.y, position.z));
    LOG_INFO("camera saved at (%.1f, %.1f, %.1f)", position.x, position.y, position.z);
}

void MainWindow::RestoreCameraPosition() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    float viewpoint[5]{};
    DWORD size = sizeof(viewpoint);
    DWORD type = 0;
    // The region's own first; the last one saved anywhere otherwise.
    LONG result = world_ ? ::RegQueryValueExW(key, CameraValueName().c_str(), nullptr, &type, (BYTE*)viewpoint, &size)
                         : ERROR_FILE_NOT_FOUND;
    if (result != ERROR_SUCCESS) {
        size = sizeof(viewpoint);
        result = ::RegQueryValueExW(key, L"Camera", nullptr, &type, (BYTE*)viewpoint, &size);
    }
    ::RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(viewpoint)) {
        SetStatusText(2, L"No camera saved yet");
        return;
    }
    // Saved in another region or game: well outside this one, the framing stays.
    if (world_) {
        const Aabb bounds = world_->TrackBounds();
        const float margin = 2000.0f;
        if (bounds.Valid() &&
            (viewpoint[0] < bounds.min.x - margin || viewpoint[0] > bounds.max.x + margin ||
             viewpoint[1] < bounds.min.y - margin || viewpoint[1] > bounds.max.y + margin)) {
            SetStatusText(2, L"The saved camera is outside this region: framed on the whole of it");
            return;
        }
    }

    viewport_.GetCamera().SetPosition({ viewpoint[0], viewpoint[1], viewpoint[2] });
    viewport_.GetCamera().SetYawPitch(viewpoint[3], viewpoint[4]);
    ::SetFocus(viewport_.Handle());

    SetStatusText(2, Format(L"Camera restored to %.0f, %.0f, %.0f",
                            viewpoint[0], viewpoint[1], viewpoint[2]));
}

void MainWindow::LaunchGame() {
    if (gameFolder_.empty()) {
        ::MessageBoxW(window_, L"Set the game folder first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }

    // Running already: the button flips between the game and the map.
    if (game_.Running() && game_.Fullscreen()) {
        SetStatusText(2, L"The game is running fullscreen, outside the view");
        return;
    }
    if (game_.Running()) {
        game_.SetShown(!game_.Shown());
        SetLaunchLabel(game_.Shown() ? L"Show map" : L"Show game");
        UpdateGameView();
        if (!game_.Shown()) ::SetFocus(viewport_.Handle());
        return;
    }

    // An old FRSFastBoot beside FRSFastTest crashes the game as it starts.
    if (const size_t retired = RetireOldFastBoot(gameFolder_)) {
        LOG_INFO("launch: retired the old FRSFastBoot (%zu files) -- FRSFastTest replaces it", retired);
        SetStatusText(2, L"The old FRSFastBoot was moved to scripts\\retired-FRSFastBoot: FRSFastTest replaces it");
    }

    // The loader puts the car at the camera, FRSFastTest takes the game there
    // without the menus: without them this is only starting the game.
    if (!WorldLoaderInstalled(gameFolder_) || !FastBootInstalled(gameFolder_)) {
        const int answer = ::MessageBoxW(
            window_,
            L"The game plugins are not installed (FRS World Loader and FRSFastTest): the game would "
            L"start at its menus and without the editor's work.\n\nRun the setup now?",
            kWindowTitle, MB_ICONQUESTION | MB_YESNOCANCEL);
        if (answer == IDYES) { RunSetup(); return; }
        if (answer == IDCANCEL) return;
    }

    // What is still only in the editor (roads, terrain) goes into the game's
    // files first, so the game starts with the world as it is edited.
    for (auto& extension : extensions_) extension->OnBeforeLaunch();

    bool elsewhere = false;
    for (auto& extension : extensions_) elsewhere = elsewhere || extension->LaunchesElsewhere();
    if (elsewhere) ::DeleteFileW((gameFolder_ + L"\\models\\launch-pose.txt").c_str());
    else WriteLaunchPose();

    // FRSFastTest reads its switch once, as the game starts. Unless the player
    // keeps it on for every start, it is on for this launch only.
    fastBootRestore_ = false;
    if (FastBootInstalled(gameFolder_) && !FastBootEnabled(gameFolder_)) {
        if (SetFastBootEnabled(gameFolder_, true)) {
            fastBootRestore_ = true;
            fastBootSince_ = ::GetTickCount();
        } else {
            LOG_WARN("launch: could not switch FRSFastTest on in %S", FastBootIni(gameFolder_).c_str());
        }
    }

    if (!game_.Launch(gameFolder_, window_)) {
        RestoreFastBoot();
        ::MessageBoxW(window_, Format(L"Could not start\n%s\\SPEED2.EXE", gameFolder_.c_str()).c_str(),
                      kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }
    SetLaunchLabel(L"Show map");
    ::SetTimer(window_, kGameViewTimer, 250, nullptr);
    SetStatusText(2, L"Game launched -- it opens over the view once its window is up");
}

// Puts FRSFastTest's switch back off once the game has read it: its window is
// up, it is gone, or long enough has passed.
void MainWindow::RestoreFastBoot(bool onlyWhenRead) {
    if (!fastBootRestore_) return;
    if (onlyWhenRead && game_.Running() && !game_.Embedded() && !game_.Fullscreen() &&
        ::GetTickCount() - fastBootSince_ < kFastBootRestoreMs)
        return;
    fastBootRestore_ = false;
    SetFastBootEnabled(gameFolder_, false);
}

// The ground under the camera, facing the way it looks: where FRSWorldLoader
// puts the car once the session is in gameplay. FRSFastTest takes the game
// straight to free roam; without it the pose waits for the player to get there.
void MainWindow::WriteLaunchPose() {
    const std::wstring path = gameFolder_ + L"\\models\\launch-pose.txt";
    ::DeleteFileW(path.c_str());
    if (!world_) return;
    const Camera& camera = viewport_.GetCamera();
    const Vec3 eye = camera.Position();
    float ground = 0.0f;
    if (!nfs::GroundBelow(*world_, eye.x, eye.y, eye.z, ground)) {
        SetStatusText(2, L"No ground under the camera: the car starts where the game puts it");
        return;
    }
    const float heading = std::fmod(Degrees(camera.Yaw()) + 360.0f, 360.0f);
    ::CreateDirectoryW((gameFolder_ + L"\\models").c_str(), nullptr);
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wt") != 0 || !file) return;
    std::fprintf(file, "pose %.3f %.3f %.3f %.2f\n", eye.x, eye.y, ground, heading);
    std::fclose(file);
    LOG_INFO("launch pose (%.1f, %.1f, %.1f) heading %.1f", eye.x, eye.y, ground, heading);
}

void MainWindow::StopGame() {
    if (!game_.Running()) return;
    SetStatusText(2, L"Closing the game...");
    game_.Stop();
    RestoreFastBoot();
    // The timer sees it gone and gives the view back (UpdateGameView).
    UpdateGameView();
    ::SetFocus(viewport_.Handle());
    SetStatusText(2, L"Game closed");
}

void MainWindow::UpdateGameView() {
    RestoreFastBoot(true);
    if (!game_.Running()) {
        const bool watching = ::KillTimer(window_, kGameViewTimer) != 0;
        if (!watching && !game_.Embedded()) return;
        // Gone without being asked, with an error: everything worth reading
        // to find out why, in a window of its own (ui/CrashReport.h).
        DWORD exitCode = 0;
        if (game_.EndedOnItsOwn(exitCode) && exitCode != 0) {
            LOG_WARN("game: ended on its own with exit code 0x%08lX (%S)", exitCode,
                     crash::ExitCodeName(exitCode).c_str());
            crash::Show(window_, crash::BuildReport(gameFolder_, game_.Launched(), exitCode), gameFolder_);
            SetStatusText(2, L"The game crashed -- its logs are in the window that opened");
        }
        game_.Release();
        SetLaunchLabel(L"Launch game");
        return;
    }
    RECT area{};
    ::GetWindowRect(viewport_.Handle(), &area);
    // With the panels drawn in the viewport, only its middle is the view.
    if (shell_ && gameArea_.right - gameArea_.left > 64 && gameArea_.bottom - gameArea_.top > 64) area = gameArea_;
    game_.Update(area);
    RestoreFastBoot(true);
    if (game_.Fullscreen()) {
        ::KillTimer(window_, kGameViewTimer);
        SetLaunchLabel(L"Launch game");
        SetStatusText(2, L"The game runs fullscreen, so it cannot open in the view: set a windowed mode "
                         L"(NFSU2ExtraOptions WindowedMode)");
    }
}

void MainWindow::ShowEditedMeshes() {
    if (modelsFolder_.empty()) {
        ::MessageBoxW(window_, L"Set the game folder first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }

    // Removing an edit changes what the world looks like, so the region is
    // reloaded from the same callback rather than leaving the viewport showing
    // a shape whose file is gone.
    overlays_.Show(instance_, window_, modelsFolder_, [this]() {
        if (world_) OpenRegion(world_->Region());
    });
}

// Wrapped so a throw from deep in the import reaches a message rather than
// killing the editor. Reading someone else's file is exactly where a bad
// assumption turns into an exception, and a window that simply vanishes says
// nothing about which one.
void MainWindow::ImportEditedObjects() {
    try {
        ImportEditedObjectsImpl();
    } catch (const std::exception& error) {
        LOG_ERROR("import threw: %s", error.what());
        ::MessageBoxW(window_,
                      Format(L"The import failed: %S\n\nNothing was written.",
                             error.what()).c_str(),
                      kWindowTitle, MB_ICONERROR | MB_OK);
    } catch (...) {
        LOG_ERROR("import threw something that is not a std::exception");
        ::MessageBoxW(window_, L"The import failed. The log has what there is.",
                      kWindowTitle, MB_ICONERROR | MB_OK);
    }
}

void MainWindow::ImportEditedCollision(const std::wstring& path, bool quick) {
    const std::wstring modelsFolder = gameFolder_ + L"\\models";

    CollisionImportPlan plan;
    const bool planned = PlanCollisionImport(*world_, path, modelsFolder, plan,
                                             quick && !blenderBases_.empty() ? &blenderBases_ : nullptr);

    std::wstring message = Format(L"Read %zu collision objects from the OBJ.\n\n%s",
                                  plan.objectsRead, plan.report.c_str());
    if (!planned) {
        if (quick) {
            SetStatusText(2, L"Blender: the collision could not be put back -- the log has why");
            LOG_WARN("collision quick edit: %S", plan.report.c_str());
            return;
        }
        message += L"Nothing was written.";
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    const std::wstring region = world_->Region();
    const size_t written = ApplyCollisionImport(plan, modelsFolder, region);

    // Reopened so the viewport shows the collision the game will be handed --
    // from the same viewpoint, and with the collision still showing.
    const Camera camera = viewport_.GetCamera();
    OpenRegion(region);
    if (quick) {
        viewport_.GetCamera().SetPosition(camera.Position());
        viewport_.GetCamera().SetYawPitch(camera.Yaw(), camera.Pitch());
        if (!viewport_.GetRenderer().CollisionVisible()) ::SendMessageW(window_, WM_COMMAND, ID_VIEW_COLLISION, 0);
        SetStatusText(2, written == plan.spans.size()
                             ? Format(L"Blender: collision loaded into %zu sections", written)
                             : std::wstring(L"Blender: some sections could not be written -- the log has why"));
        LOG_INFO("collision quick edit:\n%S", plan.report.c_str());
        return;
    }

    if (written == plan.spans.size()) {
        message += Format(L"Wrote %zu section overlays into\n%s\n", written, modelsFolder.c_str());
        if (!plan.superseded.empty())
            message += Format(L"and replaced %zu older ones for the same sections, whose edits "
                              L"are carried in the new ones.\n", plan.superseded.size());
        message += L"\nThe game's own files are untouched. The region has been reloaded.";
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONINFORMATION | MB_OK);
    } else {
        message += L"Some overlays could not be written; the older ones were kept. The log has why.";
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
    }
}

void MainWindow::ImportEditedObjectsImpl() {
    if (!world_ || tracksFolder_.empty()) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }

    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = window_;
    dialog.lpstrFilter = L"Wavefront OBJ\0*.obj\0All files\0*.*\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"Import an OBJ edited in Blender";
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameW(&dialog)) return;

    if (IsCollisionObj(path)) {
        ImportEditedCollision(path);
        return;
    }

    ImportObj(path, false);
}

void MainWindow::ImportObj(const std::wstring& path, bool quick, const Vec3* newOrigin) {
    if (!world_) return;
    if (RefuseReadOnlyWorld()) return;

    // Everything that needs the region is read first, because the region has to
    // be closed before the file can be written: the editor keeps the stream in a
    // read-only mapping, and Windows will not hand out a writable handle beside
    // it. This was the whole of an import that reported success and changed
    // nothing.
    ImportPlan plan;
    const bool planned = PlanImport(*world_, path, plan, newOrigin);

    std::wstring message =
        Format(L"Read %zu objects from the OBJ.\n\n", plan.objectsRead);

    if (!plan.rebuiltReport.empty())
        message += Format(L"%zu changed shape enough to need their section rebuilt, "
                          L"%zu are new objects:\n\n%s\n",
                          plan.rebuilt, plan.created.size(), plan.rebuiltReport.c_str());

    if (!plan.report.empty())
        message += Format(L"%zu could not be put back, %zu had no manifest entry:\n\n%s\n",
                          plan.countMismatch, plan.unknownName, plan.report.c_str());

    if (!planned) {
        message += L"Nothing was written.";
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    const std::wstring region = world_->Region();

    ImportSummary summary;
    const bool wrote = ApplyImport(plan, gameFolder_, region, summary);

    // Reopening is what puts the edit on screen: the overlays are read as the
    // region is parsed, so the viewport ends up with the same geometry the game
    // will be handed.
    const Camera camera = viewport_.GetCamera();
    OpenRegion(region);

    if (quick) {
        // Reopening frames the whole track, which throws away the view of the
        // thing being edited -- on every save.
        viewport_.GetCamera().SetPosition(camera.Position());
        viewport_.GetCamera().SetYawPitch(camera.Yaw(), camera.Pitch());

        if (!wrote) {
            message += L"The overlays could not be written. The log has why.";
            ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
            return;
        }
        std::wstring status = Format(L"Blender: loaded %zu edited meshes", summary.objectsWritten);
        if (plan.rebuilt) status += Format(L", %zu sections rebuilt", plan.rebuilt);
        if (summary.objectsCreated)
            status += Format(L", %zu new objects", summary.objectsCreated);
        if (!plan.report.empty())
            status += Format(L" -- %zu left alone (see the log)",
                             plan.countMismatch + plan.unknownName);
        if (!plan.report.empty()) LOG_WARN("quick edit: %S", plan.report.c_str());
        SetStatusText(2, status);
        return;
    }

    if (wrote) {
        message += Format(L"Wrote %zu edited meshes into\n%s\n\n"
                          L"The game's own files are untouched. The region has been "
                          L"reloaded, so the viewport is showing the edit.",
                          summary.objectsWritten, summary.modelsFolder.c_str());
        if (summary.objectsCreated)
            message += Format(L"\n\nThe %zu new objects are recorded in the .json beside the "
                              L"OBJ, so importing it again edits them rather than adding "
                              L"them twice.", summary.objectsCreated);
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONINFORMATION | MB_OK);
    } else {
        message += L"The overlays could not be written. The log has why.";
        ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
    }
}

void MainWindow::EditInBlender() {
    if (!world_ || tracksFolder_.empty()) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }

    // Collision polygons selected: those go to Blender instead.
    if (!viewport_.GetRenderer().CollisionSelection().empty()) {
        EditCollisionInBlender();
        return;
    }
    blenderCollision_ = false;

    const auto& selection = viewport_.GetRenderer().Selection();
    if (selection.empty() || !selection.front().object) {
        ::MessageBoxW(window_,
                      L"Select the objects to edit first.\n\n"
                      L"Click an object to select it, and Ctrl-click to add more.",
                      kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }

    const std::wstring blender = BlenderLink::FindBlender(window_, kSettingsKey);
    if (blender.empty()) return;

    // Named after what is being edited, so Blender's title bar and the folder
    // both say it: the first model in the selection, and how many came with it.
    std::wstring label = world_->Region() + L"-";
    for (char c : selection.front().object->name) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        label.push_back(plain ? (wchar_t)c : L'_');
    }
    if (selection.size() > 1) label += Format(L"-and-%zu", selection.size() - 1);

    const std::wstring folder = BlenderLink::NewSessionFolder(label);
    if (folder.empty()) {
        ::MessageBoxW(window_, L"No folder could be made for the edit.", kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }
    const std::wstring objPath = folder + L"\\" + label + L".obj";

    ExportSummary summary;
    if (!ExportSelection(*world_, selection, objPath, world_->Region(), summary)) {
        ::MessageBoxW(window_, L"The selection could not be written. The log has why.",
                      kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    std::wstring error;
    if (!blender_.Start(blender, objPath, error)) {
        ::MessageBoxW(window_, error.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    SetStatusText(2, Format(L"Editing %zu meshes in Blender -- save there (Ctrl+S) to load "
                            L"them here. New objects added there are placed too.",
                            summary.objects));
}

void MainWindow::EditCollisionInBlender() {
    const auto& selection = viewport_.GetRenderer().CollisionSelection();
    const std::wstring blender = BlenderLink::FindBlender(window_, kSettingsKey);
    if (blender.empty()) return;

    const std::wstring label = world_->Region() + Format(L"-collision-%zu", selection.size());
    const std::wstring folder = BlenderLink::NewSessionFolder(label);
    if (folder.empty()) {
        ::MessageBoxW(window_, L"No folder could be made for the edit.", kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }
    const std::wstring objPath = folder + L"\\" + label + L".obj";
    CollisionExportSummary summary;
    if (!ExportCollision(*world_, selection, objPath, world_->Region(), summary)) {
        ::MessageBoxW(window_, L"The collision could not be written. The log has why.", kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }

    // The packs as they are now, by the object names the export gave them:
    // every save of this session is laid on them (PlanCollisionImport).
    blenderBases_.clear();
    for (const CollisionPick& pick : selection) {
        const nfs::LoadedSection& section = *world_->Loaded()[pick.section];
        const std::string name = "collision_" + (section.info ? section.info->name : std::string("unknown")) +
                                 (pick.pack ? "_" + std::to_string(pick.pack) : std::string());
        if (!blenderBases_.count(name)) blenderBases_[name] = section.collision[pick.pack];
    }

    // The world around it, 40 units each way, to model against; and a mark
    // that tells the Blender script this is collision.
    Aabb area = summary.bounds;
    area.min = area.min - Vec3{ 40.0f, 40.0f, 20.0f };
    area.max = area.max + Vec3{ 40.0f, 40.0f, 30.0f };
    ExportReferenceMeshes(*world_, area, summary.origin, folder + L"\\reference.obj");
    if (FILE* mark = _wfopen((objPath + L".collision").c_str(), L"w")) std::fclose(mark);

    std::wstring error;
    if (!blender_.Start(blender, objPath, error)) {
        ::MessageBoxW(window_, error.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }
    blenderCollision_ = true;
    SetStatusText(2, Format(L"Editing %zu collision polygons in Blender -- save there (Ctrl+S) to load them here",
                            summary.polygons));
}

void MainWindow::NewObjectInBlender() {
    if (!world_ || tracksFolder_.empty()) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }
    Vec3 point;
    if (!ViewCentrePoint(point)) {
        ::MessageBoxW(window_, L"Point the middle of the view at the ground where the new "
                               L"object should stand.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }

    const std::wstring blender = BlenderLink::FindBlender(window_, kSettingsKey);
    if (blender.empty()) return;

    const std::wstring label = world_->Region() + L"-new";
    const std::wstring folder = BlenderLink::NewSessionFolder(label);
    if (folder.empty()) {
        ::MessageBoxW(window_, L"No folder could be made for the edit.", kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }
    const std::wstring objPath = folder + L"\\" + label + L".obj";

    // An empty scene and a manifest with nothing in it but where Blender's
    // origin is in the world: the point the view is centred on. Everything
    // modelled there comes back as a new object standing where it was made.
    std::string region(world_->Region().begin(), world_->Region().end());
    FILE* file = nullptr;
    if (::_wfopen_s(&file, objPath.c_str(), L"w") == 0 && file) {
        std::fprintf(file, "# FRS World Editor: new objects for %s\n", region.c_str());
        std::fprintf(file, "# the origin is (%.2f, %.2f, %.2f) in the game, Z up in Blender\n",
                     point.x, point.y, point.z);
        std::fclose(file);
    }
    const std::wstring manifestPath = folder + L"\\" + label + L".json";
    if (::_wfopen_s(&file, manifestPath.c_str(), L"w") != 0 || !file) {
        ::MessageBoxW(window_, L"The edit folder could not be written.", kWindowTitle,
                      MB_ICONWARNING | MB_OK);
        return;
    }
    std::fprintf(file, "{\n  \"region\": \"%s\",\n  \"space\": \"world\",\n", region.c_str());
    std::fprintf(file, "  \"axes\": \"obj is Y-up: obj(x,y,z) = game(x, -z, y)\",\n");
    std::fprintf(file, "  \"origin\": [%.4f, %.4f, %.4f],\n", point.x, point.y, point.z);
    std::fprintf(file, "  \"objects\": [\n  ]\n}\n");
    std::fclose(file);

    std::wstring error;
    if (!blender_.Start(blender, objPath, error)) {
        ::MessageBoxW(window_, error.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }
    SetStatusText(2, Format(L"New object in Blender: its origin is the view centre (%.0f, %.0f, "
                            L"%.0f). Save there (Ctrl+S) to place what you model.",
                            point.x, point.y, point.z));
}

void MainWindow::DeleteMyObjects() {
    if (!world_) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }
    // The edits are read from disk, so placements not written yet go first.
    if (!ConfirmUnsavedPlacements()) return;

    std::vector<u32> hashes;
    if (!ChooseMyObjectsToDelete(instance_, window_, *world_, hashes)) return;
    if (::MessageBoxW(window_,
                      Format(L"Delete %zu of your objects from %s?\n\nEach goes from every "
                             L"section it was placed in. This cannot be undone, though the "
                             L"OBJ and .blend files you made them from are left alone.",
                             hashes.size(), world_->Region().c_str()).c_str(),
                      kWindowTitle, MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
        return;

    edits_.Refresh(world_.get(), modelsFolder_);
    std::wstring summary;
    if (!edits_.RevertModels(window_, hashes, summary)) return;

    Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();
    OpenRegion(world_->Region());
    camera.SetPosition(position);
    camera.SetYawPitch(yaw, pitch);
    ::MessageBoxW(window_, (summary + L"\nThe region has been reloaded.").c_str(), kWindowTitle,
                  MB_ICONINFORMATION | MB_OK);
}

void MainWindow::NewObjectFromObj() {
    if (!world_ || tracksFolder_.empty()) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return;
    }
    Vec3 point;
    if (!ViewCentrePoint(point)) {
        ::MessageBoxW(window_, L"Point the middle of the view at the ground where the new "
                               L"object should stand.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }

    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = window_;
    dialog.lpstrFilter = L"Wavefront OBJ\0*.obj\0All files\0*.*\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"New object from an OBJ (Y up, placed at the view centre)";
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&dialog)) return;

    try {
        ImportObj(path, false, &point);
    } catch (const std::exception& error) {
        LOG_ERROR("new object import threw: %s", error.what());
        ::MessageBoxW(window_,
                      Format(L"The import failed: %S\n\nNothing was written.", error.what()).c_str(),
                      kWindowTitle, MB_ICONERROR | MB_OK);
    }
}

void MainWindow::PollBlender() {
    if (!blender_.Active()) return;

    // Checked before asking whether Blender is still open, so a save made just
    // before closing it is not lost.
    if (blender_.PollSaved()) {
        LOG_INFO("quick edit: Blender saved, importing %S", blender_.ObjPath().c_str());
        try {
            if (blenderCollision_) ImportEditedCollision(blender_.ObjPath(), true);
            else ImportObj(blender_.ObjPath(), true);
        } catch (const std::exception& error) {
            LOG_ERROR("quick edit import threw: %s", error.what());
            ::MessageBoxW(window_,
                          Format(L"The import failed: %S\n\nNothing was written.",
                                 error.what()).c_str(),
                          kWindowTitle, MB_ICONERROR | MB_OK);
        }
        return;
    }

    if (!blender_.Running()) {
        blender_.Stop();
        blenderCollision_ = false;
        blenderBases_.clear();
        SetStatusText(2, L"Blender closed; the quick edit is finished");
    }
}

// MINIMAP PREVIEW (temporary)
//
// Reduces every building the viewport draws to the minimap description,
// writes it as JSON and shows it in place of the originals.
void MainWindow::GenerateMinimapBuildings() {
    if (!world_) return;
    WorldRenderer& renderer = viewport_.GetRenderer();

    const std::wstring& region = world_->Region();
    wchar_t path[MAX_PATH];
    ::_snwprintf_s(path, _TRUNCATE, L"%s-minimap-buildings.json",
                   region.empty() ? L"world" : region.c_str());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = window_;
    dialog.lpstrFilter = L"JSON *.json All files *.* ";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"Save minimap buildings";
    dialog.lpstrDefExt = L"json";
    dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetSaveFileNameW(&dialog)) return;

    ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    minimap::GenerateSummary summary;
    const std::vector<minimap::Building> buildings =
        minimap::BuildAll(renderer.PlacedBuildings(), summary);
    const bool written = minimap::WriteJson(buildings, path);
    renderer.SetMinimapBuildings(buildings);
    renderer.SetMinimapPreviewVisible(true);
    renderer.SetOriginalBuildingsVisible(false);
    UpdateMenuChecks();

    SetStatusText(2, Format(L"Minimap buildings: %zu (%zu prisms, %zu stacked, %zu boxes, %zu meshes)",
                            summary.Total(), summary.prisms, summary.parts, summary.boxes,
                            summary.meshes));
    if (!written)
        ::MessageBoxW(window_, L"The buildings could not be written. The log has why.",
                      kWindowTitle, MB_ICONWARNING | MB_OK);
}

void MainWindow::ExportSelectedCollision() {
    const auto& selection = viewport_.GetRenderer().CollisionSelection();
    if (!world_ || selection.empty()) return;

    const std::wstring& region = world_->Region();
    wchar_t path[MAX_PATH];
    ::_snwprintf_s(path, _TRUNCATE, L"%s-collision-%zu.obj",
                   region.empty() ? L"world" : region.c_str(), selection.size());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = window_;
    dialog.lpstrFilter = L"Wavefront OBJ\0*.obj\0All files\0*.*\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"Export collision for Blender";
    dialog.lpstrDefExt = L"obj";
    dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetSaveFileNameW(&dialog)) return;

    CollisionExportSummary summary;
    if (!ExportCollision(*world_, selection, path, region, summary)) {
        ::MessageBoxW(window_, L"The collision could not be written. The log has why.",
                      kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    SetStatusText(2, Format(L"Exported collision: %zu polygons in %zu sections",
                            summary.polygons, summary.sections));

    const std::wstring message =
        Format(L"Wrote %zu collision polygons (%zu of them barriers) from %zu sections.\n\n"
               L"%s\n"
               L"and its .mtl and .json manifest beside it.\n\n"
               L"Each section is one object and each polygon one face. The material "
               L"names carry the polygon's attribute bytes, so keep them on the faces "
               L"you edit. Barriers are drawn as walls; only their bottom edge counts.",
               summary.polygons, summary.barriers, summary.sections,
               summary.objPath.c_str());
    ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONINFORMATION | MB_OK);
}

void MainWindow::ExportSelectedObjects() {
    if (!viewport_.GetRenderer().CollisionSelection().empty()) {
        ExportSelectedCollision();
        return;
    }

    const auto& selection = viewport_.GetRenderer().Selection();
    if (selection.empty()) {
        ::MessageBoxW(window_,
                      L"Nothing is selected.\n\n"
                      L"Click an object to select it, and Ctrl-click to add more.\n"
                      L"With the collision shown (Ctrl+K), clicks select collision "
                      L"polygons instead; Shift-click takes a section's whole collision.",
                      kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!world_) return;

    const std::wstring& region = world_->Region();

    // A name that says what it holds without anyone having to type one: the
    // region, and how many pieces went in.
    wchar_t path[MAX_PATH];
    ::_snwprintf_s(path, _TRUNCATE, L"%s-selection-%zu.obj",
                   region.empty() ? L"world" : region.c_str(), selection.size());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner   = window_;
    dialog.lpstrFilter = L"Wavefront OBJ\0*.obj\0All files\0*.*\0";
    dialog.lpstrFile   = path;
    dialog.nMaxFile    = MAX_PATH;
    dialog.lpstrTitle  = L"Export selection for Blender";
    dialog.lpstrDefExt = L"obj";
    dialog.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetSaveFileNameW(&dialog)) return;

    ExportSummary summary;
    if (!ExportSelection(*world_, selection, path, region, summary)) {
        ::MessageBoxW(window_, L"The selection could not be written. The log has why.",
                      kWindowTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    SetStatusText(2, Format(L"Exported %zu meshes, %zu triangles, %zu textures",
                            summary.objects, summary.triangles, summary.textures));

    std::wstring message =
        Format(L"Wrote %zu meshes, %zu triangles and %zu textures.\n\n"
               L"%s\n"
               L"and its .mtl, .json manifest and textures folder beside it.\n\n"
               L"Vertices are in world space, so the pieces open in Blender standing "
               L"where the game has them. Keep the object names as they are and the "
               L"manifest can put them back.",
               summary.objects, summary.triangles, summary.textures,
               summary.objPath.c_str());

    if (summary.collapsed)
        message += Format(L"\n\n%zu of the selected placements reuse a mesh that was "
                          L"already written, so they are not in the file. The game keeps "
                          L"one copy of a mesh, so editing it changes every place it "
                          L"stands.",
                          summary.collapsed);

    ::MessageBoxW(window_, message.c_str(), kWindowTitle, MB_ICONINFORMATION | MB_OK);
}

void MainWindow::SetGameFolder(const std::wstring& folder) {
    CloseRegion();

    gameFolder_   = folder;
    tracksFolder_ = folder + L"\\TRACKS";
    viewport_.GetRenderer().SetGameFolder(folder);
    modelsFolder_ = folder + L"\\models";
    // The world file last opened or saved, and how <game>\models stood then.
    {
        wchar_t path[MAX_PATH * 2] = {};
        char signature[64] = {};
        DWORD size = sizeof(path);
        if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WorldFile", RRF_RT_REG_SZ, nullptr, path, &size) ==
            ERROR_SUCCESS)
            worldPath_ = path;
        size = sizeof(signature);
        if (::RegGetValueA(HKEY_CURRENT_USER, "Software\\FRSWorldEditor", "WorldSignature", RRF_RT_REG_SZ, nullptr,
                           signature, &size) == ERROR_SUCCESS)
            worldSignature_ = signature;
        if (!worldPath_.empty() && ::GetFileAttributesW(worldPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("world file %S is gone; the world in models is kept as untitled", worldPath_.c_str());
            worldPath_.clear();
        }
    }
    worldName_    = WorldDisplayName();
    UpdateTitle();

    ::SendMessageW(regionCombo_, CB_RESETCONTENT, 0, 0);

    const auto regions = nfs::World::AvailableRegions(tracksFolder_);
    for (const std::wstring& region : regions)
        ::SendMessageW(regionCombo_, CB_ADDSTRING, 0, (LPARAM)region.c_str());

    LOG_INFO("game folder set, %zu regions found", regions.size());
    SetStatusText(0, Format(L"%zu regions available", regions.size()));

    if (!regions.empty()) {
        ::SendMessageW(regionCombo_, CB_SETCURSEL, 0, 0);
        OpenRegion(regions.front());
    }
}

void MainWindow::OpenRegion(std::wstring region) {
    if (!ConfirmUnsavedPlacements()) return;
    CloseRegion();

    auto world = std::make_unique<nfs::World>();

    // Set before opening: the edited meshes are folded in as each section is
    // parsed, so what the viewport draws is what the game will be handed.
    world->SetOverlayFolder(modelsFolder_);

    if (!world->Open(tracksFolder_, region)) {
        ::MessageBoxW(window_, Format(L"Could not open region %s.", region.c_str()).c_str(),
                      kWindowTitle, MB_ICONERROR | MB_OK);
        return;
    }

    world_ = std::move(world);
    UpdateTitle();

    // The whole point of opening a region is to look at it, so parse and upload
    // everything right away rather than leaving an empty viewport behind.
    LoadAllSections();
}

void MainWindow::CloseRegion() {
    for (auto& extension : extensions_) extension->OnRegionClosed();
    properties_.ForgetPictures();
    ForgetShellThumbnails();
    catalog_.SetWorld(nullptr);
    edits_.Refresh(nullptr, modelsFolder_);
    textures_.SetWorld(nullptr);
    zones_.clear();
    BuildTravelMenu();
    ::SendMessageW(sectionTree_, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    viewport_.SetWorld(nullptr);
    world_.reset();
    UpdateTitle();
    SetStatusText(1, L"");
    SetStatusText(2, L"");
}

void MainWindow::LoadAllSections() {
    if (!world_) return;

    HCURSOR previous = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    SetStatusText(0, L"Parsing sections...");
    ::UpdateWindow(statusBar_);

    const DWORD parseStart = ::GetTickCount();
    world_->LoadAllSections();
    const DWORD parseMs = ::GetTickCount() - parseStart;

    SetStatusText(0, L"Uploading to the GPU...");
    ::UpdateWindow(statusBar_);

    const DWORD uploadStart = ::GetTickCount();
    viewport_.SetWorld(world_.get());
    const DWORD uploadMs = ::GetTickCount() - uploadStart;

    PopulateSectionTree();
    thumbnails_.SetRegion(world_->Region());
    catalog_.SetWorld(world_.get());
    QueueThumbnailWarmUp();
    viewport_.FrameBounds(world_->TrackBounds());
    edits_.Refresh(world_.get(), modelsFolder_);

    if (std::getenv("FRS_EDITS_SELFTEST")) SelfTestSectionEdits(*world_);

    ::SetCursor(previous);

    size_t objects = 0, triangles = 0;
    for (const auto& section : world_->Loaded()) {
        objects   += section->ObjectCount();
        triangles += section->TriangleCount();
    }

    SetStatusText(0, Format(L"%zu sections  %zu objects  %zu instances  %zu triangles",
                            world_->Loaded().size(), objects,
                            world_->InstanceCount(), triangles));

    textures_.SetWorld(world_.get());

    // Back to wherever the work was left off, rather than to a framing of the
    // whole city. Loading a region happens after every rebuild and after every
    // import, so flying back to the same street each time is most of the cost
    // of a change.
    RestoreCameraPosition();

    // The city's named zones, from the region's free-roam graph.
    std::vector<nfs::FreeRoamZone> freeRoam;
    nfs::ReadFreeRoamZones(gameFolder_, world_->Region(), freeRoam);
    zones_ = nfs::ReadCityZones(freeRoam);
    BuildTravelMenu();

    for (auto& extension : extensions_) extension->OnRegionLoaded();
    // What the extensions list in the Library is known only now.
    catalog_.Refresh();
    PopulateSectionTree();

    LOG_INFO("region loaded: parse %lu ms, upload %lu ms", parseMs, uploadMs);
}

void MainWindow::PopulateSectionTree() {
    rebuildingTree_ = true;
    ::SendMessageW(sectionTree_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(sectionTree_, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    mineHashes_.clear();
    if (!world_) {
        ::SendMessageW(sectionTree_, WM_SETREDRAW, TRUE, 0);
        rebuildingTree_ = false;
        return;
    }
    sceneSignature_ = SceneSignature();

    // The scene's own filter in the panels; the shared box in the old window.
    std::wstring needle = sceneFilter_;
    if (!shell_) {
        wchar_t search[128] = {};
        ::GetWindowTextW(sideSearch_, search, 128);
        needle = search;
    }
    for (wchar_t& c : needle) c = (wchar_t)::towlower(c);
    const auto matches = [&](const std::wstring& text) {
        if (needle.empty()) return true;
        std::wstring lower = text;
        for (wchar_t& c : lower) c = (wchar_t)::towlower(c);
        return lower.find(needle) != std::wstring::npos;
    };
    const auto insert = [&](HTREEITEM parent, const std::wstring& text, intptr_t param) {
        TVINSERTSTRUCTW item{};
        item.hParent      = parent;
        item.hInsertAfter = TVI_LAST;
        item.item.mask    = TVIF_TEXT | TVIF_PARAM;
        item.item.pszText = (LPWSTR)text.c_str();
        item.item.lParam  = (LPARAM)param;
        return (HTREEITEM)::SendMessageW(sectionTree_, TVM_INSERTITEMW, 0, (LPARAM)&item);
    };
    const auto rename = [&](HTREEITEM node, const std::wstring& text) {
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = node;
        item.pszText = (LPWSTR)text.c_str();
        ::SendMessageW(sectionTree_, TVM_SETITEMW, 0, (LPARAM)&item);
    };
    // A branch is dropped when the search leaves nothing in it, and opened
    // when the search is what put something there.
    const auto finish = [&](HTREEITEM branch, size_t count, bool open) {
        if (!branch) return;
        if (count == 0 && !needle.empty()) {
            ::SendMessageW(sectionTree_, TVM_DELETEITEM, 0, (LPARAM)branch);
            return;
        }
        if (open || !needle.empty()) ::SendMessageW(sectionTree_, TVM_EXPAND, TVE_EXPAND, (LPARAM)branch);
    };

    const auto& loaded = world_->Loaded();

    // The city by district and zone: a click flies there.
    if (!zones_.empty()) {
        HTREEITEM branch = insert(TVI_ROOT, L"Zones", kZoneBranchItem);
        size_t shown = 0;
        const auto& districts = nfs::DistrictOrder();
        for (size_t d = 0; d < districts.size(); ++d) {
            HTREEITEM group = nullptr;
            for (size_t z = 0; z < zones_.size(); ++z) {
                if (zones_[z].district != districts[d]) continue;
                if (!matches(zones_[z].name) && !matches(districts[d])) continue;
                if (!group) group = insert(branch, districts[d], -(kDistrictItemBase + (intptr_t)d));
                insert(group, zones_[z].name, -(kZoneItemBase + (intptr_t)z));
                ++shown;
            }
        }
        finish(branch, shown, true);
    }

    // What has been put in the world, by model: the rows people come back to.
    {
        std::map<std::string, std::vector<std::pair<size_t, size_t>>> byModel;
        size_t total = 0;
        for (size_t s = 0; s < loaded.size(); ++s)
            for (size_t p = 0; p < loaded[s]->placements.size(); ++p) {
                byModel[loaded[s]->placements[p].instance.modelName].push_back({ s, p });
                ++total;
            }
        HTREEITEM branch = insert(TVI_ROOT, Format(L"Placed objects (%zu)", total), kPlacedBranchItem);
        size_t shown = 0;
        for (const auto& [model, where] : byModel) {
            const std::wstring name = Widen(model);
            const bool modelMatches = matches(name);
            HTREEITEM group = nullptr;
            size_t inGroup = 0;
            for (const auto& [s, p] : where) {
                const Mat4& m = loaded[s]->placements[p].instance.transform;
                const std::wstring text = Format(L"%s  (%.0f, %.0f)", Widen(loaded[s]->info->name).c_str(),
                                                 m.m[3][0], m.m[3][1]);
                if (!modelMatches && !matches(text)) continue;
                const intptr_t param = -(kPlacedItemBase + (intptr_t)s * 65536 + (intptr_t)p);
                if (!group) group = insert(branch, name, param);
                insert(group, text, param);
                ++inGroup;
            }
            if (group) {
                rename(group, Format(L"%s (%zu)%s", name.c_str(), inGroup,
                                     model.rfind("FRS_", 0) == 0 ? L"  - yours" : L""));
                shown += inGroup;
            }
        }
        finish(branch, shown, true);
    }

    // The objects made in this editor: click to go to one, double-click to
    // place another at the view centre.
    {
        HTREEITEM branch = insert(TVI_ROOT, L"My objects", kMineBranchItem);
        size_t shown = 0;
        for (const nfs::SolidObject* model : world_->AllModels()) {
            if (!model || model->name.rfind("FRS_", 0) != 0) continue;
            const std::wstring name = Widen(model->name);
            if (!matches(name)) continue;
            size_t placed = 0;
            for (const auto& section : loaded)
                for (const auto& placement : section->placements)
                    if (placement.instance.modelHash == model->hash) ++placed;
            insert(branch, Format(L"%s  (%zu placed)", name.c_str(), placed),
                   -(kMineItemBase + (intptr_t)mineHashes_.size()));
            mineHashes_.push_back(model->hash);
            ++shown;
        }
        rename(branch, Format(L"My objects (%zu)  - double-click to place", mineHashes_.size()));
        finish(branch, shown, true);
    }

    // The extensions' branches.
    for (size_t e = 0; e < extensions_.size(); ++e) {
        SceneTreeBuilder tree;
        tree.insert = [&, e](HTREEITEM parent, const std::wstring& text, intptr_t code) {
            return insert(parent, text, code < 0 ? kExtensionNoItem
                                                 : -(kExtensionItemBase + (intptr_t)e * kExtensionItemSpan + code));
        };
        tree.matches = matches;
        tree.finish = finish;
        extensions_[e]->PopulateTree(tree);
    }

    // The sections last and closed: a place to jump to, not to read.
    {
        HTREEITEM branch = insert(TVI_ROOT, Format(L"Sections (%zu)", loaded.size()), (intptr_t)-1);
        size_t shown = 0;
        for (size_t i = 0; i < loaded.size(); ++i) {
            const nfs::LoadedSection& section = *loaded[i];
            const std::wstring text = Format(L"%s  -  %zu obj, %zu tris", Widen(section.info->name).c_str(),
                                             section.ObjectCount(), section.TriangleCount());
            if (!matches(text)) continue;
            insert(branch, text, (intptr_t)i);
            ++shown;
        }
        finish(branch, shown, false);
    }

    ::SendMessageW(sectionTree_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(sectionTree_, nullptr, TRUE);
    rebuildingTree_ = false;
}

size_t MainWindow::SceneSignature() const {
    if (!world_) return 0;
    size_t signature = 0;
    for (const auto& extension : extensions_) signature = signature * 1000003u + extension->SceneSignature();
    for (const auto& section : world_->Loaded())
        for (const auto& placement : section->placements)
            signature = signature * 31u + placement.instance.modelHash;
    return signature;
}

void MainWindow::ShowSideTab(int tab) {
    sideTab_ = tab >= 0 && tab <= (shell_ ? 3 : 2) ? tab : 0;   // 3: Online, drawn only
    focusPanel_ = sideTab_;     // in the panels, that one comes to the front
    if (sideTab_ == 3) return;
    const bool library = sideTab_ == 1;
    const bool edits = sideTab_ == 2;
    if (!shell_) {
        ::ShowWindow(sectionTree_, sideTab_ == 0 ? SW_SHOW : SW_HIDE);
        for (HWND control : { libShelf_, libUsed_, libList_ })
            ::ShowWindow(control, library ? SW_SHOW : SW_HIDE);
        ::ShowWindow(sideSearch_, edits ? SW_HIDE : SW_SHOW);
        edits_.Show(edits);
    }
    TabCtrl_SetCurSel(sideTabs_, sideTab_);
    if (edits) return;
    // The search box is shared: what is typed applies to the tab now showing.
    if (library) catalog_.OnCommand(sideSearch_, EN_CHANGE);
    else PopulateSectionTree();
}

void MainWindow::FocusPlacement(size_t section, size_t index) {
    if (!world_ || section >= world_->Loaded().size()) return;
    const nfs::LoadedSection& loaded = *world_->Loaded()[section];
    if (index >= loaded.placements.size()) return;
    const nfs::PlacedInstance& placed = loaded.placements[index].instance;
    Aabb around = placed.modelBounds.Valid() ? Transform(placed.modelBounds, placed.transform) : Aabb{};
    const Vec3 at{ placed.transform.m[3][0], placed.transform.m[3][1], placed.transform.m[3][2] };
    // Enough of the street around it to see where it stands.
    around.Add({ at.x - 12.0f, at.y - 12.0f, at.z });
    around.Add({ at.x + 12.0f, at.y + 12.0f, at.z + 8.0f });
    viewport_.FrameBounds(around);
    viewport_.GetRenderer().SelectPlacement((u32)section, (int)index);
}

void MainWindow::TravelToBounds(const Vec2& minimum, const Vec2& maximum, const std::wstring& name) {
    if (!world_) return;
    const Vec2 centre{ (minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f };
    float ground = 0.0f;
    if (!nfs::GroundHeight(*world_, centre.x, centre.y, ground)) ground = 20.0f;

    // Over the zone and a little short of it, looking down across it: close
    // enough to see streets, high enough to see where they go. A zone is
    // several hundred units to three kilometres across, so the height follows
    // its size, within reason.
    const float across = std::max(maximum.x - minimum.x, maximum.y - minimum.y);
    const float height = std::clamp(across * 0.28f, 90.0f, 520.0f);
    const Vec3 eye{ centre.x, centre.y - height * 0.9f, ground + height };
    Camera aim;
    aim.LookAt(eye, Vec3{ centre.x, centre.y, ground });
    viewport_.FlyTo(eye, aim.Yaw(), aim.Pitch());
    ::SetFocus(viewport_.Handle());
    SetStatusText(2, L"Travelling to " + name);
}

void MainWindow::TravelToZone(size_t index) {
    if (index >= zones_.size()) return;
    const nfs::CityZone& zone = zones_[index];
    TravelToBounds(zone.boundsMin, zone.boundsMax, zone.name + L", " + zone.district);
}

void MainWindow::TravelToDistrict(size_t index) {
    const auto& districts = nfs::DistrictOrder();
    if (index >= districts.size()) return;
    Vec2 minimum{ 1e30f, 1e30f }, maximum{ -1e30f, -1e30f };
    for (const nfs::CityZone& zone : zones_) {
        if (zone.district != districts[index]) continue;
        minimum = { std::min(minimum.x, zone.boundsMin.x), std::min(minimum.y, zone.boundsMin.y) };
        maximum = { std::max(maximum.x, zone.boundsMax.x), std::max(maximum.y, zone.boundsMax.y) };
    }
    if (minimum.x > maximum.x) return;
    TravelToBounds(minimum, maximum, districts[index]);
}

// Camera > Travel To, filled from the zones of the region that is open: a
// submenu per district, the district itself first and then its zones.
void MainWindow::BuildTravelMenu() {
    if (!menu_) return;
    HMENU camera = nullptr;
    for (int i = 0; i < ::GetMenuItemCount(menu_); ++i) {
        HMENU sub = ::GetSubMenu(menu_, i);
        if (sub && ::GetMenuState(sub, IDC_SAVE_CAMERA, MF_BYCOMMAND) != (UINT)-1) camera = sub;
    }
    if (!camera) return;
    // The previous region's list goes first.
    for (int i = ::GetMenuItemCount(camera) - 1; i >= 0; --i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(camera, (UINT)i, text, 64, MF_BYPOSITION);
        if (std::wstring(text).rfind(L"&Travel To", 0) == 0) {
            ::DeleteMenu(camera, (UINT)i, MF_BYPOSITION);   // destroys the submenus with it
            if ((UINT)i < (UINT)::GetMenuItemCount(camera) &&
                ::GetMenuItemID(camera, i) == 0 && ::GetMenuState(camera, (UINT)i, MF_BYPOSITION) & MF_SEPARATOR)
                ::DeleteMenu(camera, (UINT)i, MF_BYPOSITION);
        }
    }
    if (zones_.empty()) return;

    HMENU travel = ::CreatePopupMenu();
    const auto& districts = nfs::DistrictOrder();
    for (size_t d = 0; d < districts.size(); ++d) {
        HMENU district = nullptr;
        for (size_t z = 0; z < zones_.size() && z < 100; ++z) {
            if (zones_[z].district != districts[d]) continue;
            if (!district) {
                district = ::CreatePopupMenu();
                ::AppendMenuW(district, MF_STRING, ID_TRAVEL_DISTRICT + d, (L"All of " + districts[d]).c_str());
                ::AppendMenuW(district, MF_SEPARATOR, 0, nullptr);
            }
            ::AppendMenuW(district, MF_STRING, ID_TRAVEL_ZONE + z, zones_[z].name.c_str());
        }
        if (district) ::AppendMenuW(travel, MF_POPUP, (UINT_PTR)district, districts[d].c_str());
    }
    ::InsertMenuW(camera, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)travel, L"&Travel To");
    ::InsertMenuW(camera, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::DrawMenuBar(window_);
}

void MainWindow::FocusMine(size_t index) {
    if (!world_ || index >= mineHashes_.size()) return;
    const auto& loaded = world_->Loaded();
    for (size_t s = 0; s < loaded.size(); ++s)
        for (size_t p = 0; p < loaded[s]->placements.size(); ++p)
            if (loaded[s]->placements[p].instance.modelHash == mineHashes_[index]) {
                FocusPlacement(s, p);
                return;
            }
    SetStatusText(2, L"Not placed anywhere yet -- double-click it to place it at the view centre");
}

int MainWindow::ExtensionTreeMark(intptr_t param) const {
    if (param > -kExtensionItemBase) return -1;
    const intptr_t code = -param - kExtensionItemBase;
    const size_t extension = (size_t)(code / kExtensionItemSpan);
    return extension < extensions_.size() ? extensions_[extension]->TreeItemMark(code % kExtensionItemSpan) : -1;
}

void MainWindow::OnSectionSelected() {
    if (!world_ || rebuildingTree_) return;

    HTREEITEM selected = (HTREEITEM)::SendMessageW(sectionTree_, TVM_GETNEXTITEM,
                                                   TVGN_CARET, 0);
    if (!selected) return;

    TVITEMW item{};
    item.mask  = TVIF_PARAM;
    item.hItem = selected;
    if (!::SendMessageW(sectionTree_, TVM_GETITEMW, 0, (LPARAM)&item)) return;

    const auto index = (intptr_t)item.lParam;
    if (index == kPlacedBranchItem || index == kMineBranchItem || index == kZoneBranchItem ||
        index == kExtensionNoItem)
        return;
    if (index <= -kExtensionItemBase) {
        const intptr_t code = -index - kExtensionItemBase;
        const size_t extension = (size_t)(code / kExtensionItemSpan);
        if (extension < extensions_.size()) extensions_[extension]->OnTreeItem(code % kExtensionItemSpan);
        return;
    }
    if (index <= -kDistrictItemBase) {
        TravelToDistrict((size_t)(-index - kDistrictItemBase));
        return;
    }
    if (index <= -kZoneItemBase) {
        TravelToZone((size_t)(-index - kZoneItemBase));
        return;
    }
    if (index <= -kMineItemBase) {
        FocusMine((size_t)(-index - kMineItemBase));
        return;
    }
    if (index <= -kPlacedItemBase) {
        const intptr_t code = -index - kPlacedItemBase;
        FocusPlacement((size_t)(code / 65536), (size_t)(code % 65536));
        return;
    }
    if (index < 0) {                     // the root node frames the whole region
        viewport_.FrameBounds(world_->TrackBounds());
        return;
    }

    const auto& loaded = world_->Loaded();
    if ((size_t)index >= loaded.size()) return;

    const nfs::LoadedSection& section = *loaded[(size_t)index];
    const Aabb focus = world_->SectionBounds(section);
    if (focus.Valid()) viewport_.FrameBounds(focus);

    SetStatusText(2, Format(L"Section %s", Widen(section.info->name).c_str()));
}

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------

void MainWindow::SetRenderMode(RenderMode mode) {
    viewport_.GetRenderer().SetMode(mode);
    UpdateMenuChecks();
}

void MainWindow::UpdateMenuChecks() {
    if (!menu_) return;

    const GizmoMode gizmo = viewport_.GetGizmoMode();
    const int gizmoId = gizmo == GizmoMode::Move ? ID_GIZMO_MOVE
                        : gizmo == GizmoMode::Rotate ? ID_GIZMO_ROTATE : ID_GIZMO_SCALE;
    // While an extension's tool is on, none of the three is the tool in use.
    bool otherTool = false;
    for (auto& extension : extensions_) otherTool = otherTool || extension->ToolActive();
    if (otherTool)
        for (int id : { ID_GIZMO_MOVE, ID_GIZMO_ROTATE, ID_GIZMO_SCALE })
            ::CheckMenuItem(menu_, id, MF_BYCOMMAND | MF_UNCHECKED);
    else
        ::CheckMenuRadioItem(menu_, ID_GIZMO_MOVE, ID_GIZMO_SCALE, gizmoId, MF_BYCOMMAND);
    // The toolbar shows the same choice, whichever of the three made it.
    for (int id : { ID_GIZMO_MOVE, ID_GIZMO_ROTATE, ID_GIZMO_SCALE })
        ::SendMessageW(toolbar_, TB_CHECKBUTTON, id, MAKELPARAM(!otherTool && id == gizmoId, 0));

    const RenderMode mode = viewport_.GetRenderer().Mode();
    for (int id : { ID_VIEW_TEXTURED, ID_VIEW_UNLIT, ID_VIEW_VERTEX_COLOR, ID_VIEW_WIREFRAME })
        ::CheckMenuItem(menu_, id, MF_BYCOMMAND | MF_UNCHECKED);

    const int activeId = mode == RenderMode::Textured    ? ID_VIEW_TEXTURED :
                         mode == RenderMode::Unlit       ? ID_VIEW_UNLIT :
                         mode == RenderMode::VertexColor ? ID_VIEW_VERTEX_COLOR
                                                         : ID_VIEW_WIREFRAME;
    ::CheckMenuItem(menu_, activeId, MF_BYCOMMAND | MF_CHECKED);

    ::CheckMenuItem(menu_, ID_VIEW_FRUSTUM_CULL,
                    MF_BYCOMMAND | (viewport_.GetRenderer().CullingEnabled() ? MF_CHECKED
                                                                             : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_ALTERNATE,
                    MF_BYCOMMAND | (viewport_.GetRenderer().AlternateSectionsVisible()
                                        ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_STREAMING,
                    MF_BYCOMMAND | (viewport_.GetRenderer().StreamingEnabled()
                                        ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_LIBRARY,
                    MF_BYCOMMAND | (viewport_.GetRenderer().LibraryVisible() ? MF_CHECKED
                                                                             : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_SCENERY,
                    MF_BYCOMMAND | (viewport_.GetRenderer().SceneryVisible() ? MF_CHECKED
                                                                             : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_BACKDROP,
                    MF_BYCOMMAND | (viewport_.GetRenderer().BackdropVisible() ? MF_CHECKED
                                                                              : MF_UNCHECKED));
    // MINIMAP PREVIEW (temporary)
    ::CheckMenuItem(menu_, ID_MINIMAP_SHOW,
                    MF_BYCOMMAND | (viewport_.GetRenderer().MinimapPreviewVisible() ? MF_CHECKED
                                                                                    : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_MINIMAP_ORIGINALS,
                    MF_BYCOMMAND | (viewport_.GetRenderer().OriginalBuildingsVisible() ? MF_CHECKED
                                                                                       : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_COLLISION,
                    MF_BYCOMMAND | (viewport_.GetRenderer().CollisionVisible() ? MF_CHECKED
                                                                               : MF_UNCHECKED));
    ::CheckMenuItem(menu_, ID_VIEW_BACKFACE_CULL,
                    MF_BYCOMMAND | (viewport_.GetRenderer().BackfaceCullingEnabled()
                                        ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, kViewSkyCommand,
                    MF_BYCOMMAND | (viewport_.GetRenderer().SkyVisible() ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, kViewAnimationsCommand,
                    MF_BYCOMMAND | (viewport_.GetRenderer().AnimationsEnabled() ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, kViewGameLookCommand,
                    MF_BYCOMMAND | (viewport_.GetRenderer().GameLook() ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, kViewHideBackgroundCommand,
                    MF_BYCOMMAND | (viewport_.GetRenderer().BackgroundSceneryHidden() ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu_, kViewHideDistantLodCommand,
                    MF_BYCOMMAND | (viewport_.GetRenderer().DistantLodHidden() ? MF_CHECKED : MF_UNCHECKED));
    for (auto& extension : extensions_) extension->UpdateMenuChecks(menu_);
}

void MainWindow::SetStatusText(int part, const std::wstring& text) {
    if (statusBar_) ::SendMessageW(statusBar_, SB_SETTEXTW, (WPARAM)part, (LPARAM)text.c_str());
}

void MainWindow::UpdateStatusBar() {
    const Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();

    const int zone = nfs::ZoneAt(zones_, position.x, position.y);
    SetStatusText(1, Format(L"%s%sX %.0f  Y %.0f  Z %.0f   speed %.0f",
                            zone >= 0 ? zones_[(size_t)zone].name.c_str() : L"",
                            zone >= 0 ? L"   " : L"",
                            position.x, position.y, position.z, camera.Speed()));

    const auto& collision = viewport_.GetRenderer().CollisionSelection();
    const auto& selection = viewport_.GetRenderer().Selection();
    u32 placedSection = 0;
    int placedIndex = -1;
    std::wstring extensionText;
    bool extensionSelected = false;
    for (const auto& extension : extensions_)
        if (!extensionSelected) extensionSelected = extension->StatusText(extensionText);
    if (extensionSelected) {
        SetStatusText(0, extensionText);
    } else if (world_ && viewport_.SelectedPlacement(placedSection, placedIndex) &&
        placedSection < world_->Loaded().size() &&
        (size_t)placedIndex < world_->Loaded()[placedSection]->placements.size()) {
        const nfs::LoadedSection& section = *world_->Loaded()[placedSection];
        const nfs::PlacedInstance& placed = section.placements[(size_t)placedIndex].instance;
        Vec3 at;
        float heading, pitch, roll, scale;
        nfs::DecomposePlacement(placed.transform, at, heading, pitch, roll, scale);
        SetStatusText(0, Format(L"Placed %s in %s  at %.2f, %.2f, %.2f  heading %.1f%s   "
                                L"[G move  R rotate  Ctrl+G ground  F2 edit  Del delete]",
                                Widen(placed.modelName).c_str(),
                                section.info ? Widen(section.info->name).c_str() : L"?",
                                at.x, at.y, at.z, heading,
                                section.placementsChanged ? L"  (not written)" : L""));
    } else if (!collision.empty() && world_) {
        std::set<std::pair<u32, u32>> packs;
        size_t barriers = 0;
        const auto& loaded = world_->Loaded();
        for (const CollisionPick& pick : collision) {
            packs.insert({ pick.section, pick.pack });
            if (pick.section < loaded.size() && pick.pack < loaded[pick.section]->collision.size() &&
                pick.polygon < loaded[pick.section]->collision[pick.pack].polygons.size())
                barriers += loaded[pick.section]->collision[pick.pack].polygons[pick.polygon].barrier;
        }
        SetStatusText(0, Format(L"%zu collision polygons selected (%zu barriers) in %zu sections",
                                collision.size(), barriers, packs.size()));
    } else if (selection.size() == 1) {
        const PickResult& one = selection.front();
        SetStatusText(0, Format(L"%s  [%s, %s]",
                                Widen(one.object->name).c_str(),
                                Widen(one.sectionName).c_str(),
                                one.fromInstance ? L"instance" : L"own matrix"));
    } else if (!selection.empty()) {
        u32 triangles = 0;
        for (const PickResult& picked : selection)
            triangles += picked.object ? picked.object->TriangleCount() : 0;
        SetStatusText(0, Format(L"%zu objects selected, %u triangles",
                                selection.size(), triangles));
    }

    const RenderStats& stats = viewport_.GetRenderer().Stats();
    SetStatusText(2, Format(L"%u/%u sect  %u obj  %u/%u inst  %u tris  %u draws  %.1f ms",
                            stats.sectionsVisible, stats.sectionsTotal,
                            stats.objectsVisible,
                            stats.instancesVisible, stats.instancesTotal,
                            stats.triangles, stats.drawCalls,
                            stats.frameMilliseconds));
}

void MainWindow::Tick(float deltaSeconds) {
    // Under the game the map is not seen: no point drawing it.
    // Under the game the map is not seen -- but the panels round it are,
    // and they are drawn by the viewport.
    if (shell_ || !(game_.Embedded() && game_.Shown())) viewport_.Tick(deltaSeconds);

    // The status bar only needs to keep up with the eye, not with the frame rate.
    lastStatusUpdate_ += deltaSeconds;
    if (lastStatusUpdate_ >= 0.1) {
        lastStatusUpdate_ = 0.0;
        UpdateStatusBar();
        UpdateProperties();
    }

    // The scene tree follows what is placed; the library draws the pictures
    // it has been asked for, a few milliseconds a frame so the view stays
    // smooth, and frees the default-pool targets so a resize can reset.
    scenePoll_ += deltaSeconds;
    if (scenePoll_ >= 0.5) {
        scenePoll_ = 0.0;
        if (world_ && SceneSignature() != sceneSignature_) PopulateSectionTree();
        // The Edits tab carries the count, so it is seen without opening it.
        static size_t shownCount = (size_t)-1;
        if (edits_.Count() != shownCount) {
            shownCount = edits_.Count();
            std::wstring text = L"Edits (" + std::to_wstring(shownCount) + L")";
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = text.data();
            TabCtrl_SetItem(sideTabs_, 2, &item);
        }
    }
    // The Library's pictures: always on show in the panels, in the old window only on its tab.
    if ((shell_ || sideTab_ == 1) && catalog_.DrawPending(8)) viewport_.GetRenderer().ReleaseThumbnailTargets();
    else if (!thumbsToWarm_.empty()) WarmThumbnails(6);

    // Twice a second is quick enough to feel immediate after a save in Blender.
    blenderPoll_ += deltaSeconds;
    if (blenderPoll_ >= 0.5) {
        blenderPoll_ = 0.0;
        PollBlender();
    }
    for (auto& extension : extensions_) extension->Tick(deltaSeconds);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

std::wstring MainWindow::LoadStoredGameFolder() const {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};

    wchar_t buffer[MAX_PATH] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG result = ::RegQueryValueExW(key, L"GameFolder", nullptr, &type,
                                           (LPBYTE)buffer, &size);
    ::RegCloseKey(key);

    return (result == ERROR_SUCCESS && type == REG_SZ) ? buffer : std::wstring{};
}

DWORD MainWindow::StoredSetupVersion() const {
    DWORD value = 0, size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"SetupVersion", RRF_RT_REG_DWORD, nullptr, &value,
                       &size) != ERROR_SUCCESS)
        return 0;
    return value;
}

void MainWindow::StoreSetupVersion(DWORD version) const {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    ::RegSetValueExW(key, L"SetupVersion", 0, REG_DWORD, (const BYTE*)&version, sizeof(version));
    ::RegCloseKey(key);
}

void MainWindow::StoreGameFolder(const std::wstring& folder) const {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                          KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    ::RegSetValueExW(key, L"GameFolder", 0, REG_SZ, (const BYTE*)folder.c_str(),
                     (DWORD)((folder.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Scenery placement
// ---------------------------------------------------------------------------

bool MainWindow::HasUnsavedPlacements() const {
    if (!world_) return false;
    for (const auto& section : world_->Loaded())
        if (section->placementsChanged || section->collisionChanged) return true;
    return false;
}

bool MainWindow::ConfirmUnsavedPlacements() {
    if (!HasUnsavedPlacements()) return true;
    // Asked from inside a save already under way (a save reopens the region,
    // and opening a region asks): go on with it rather than ask again.
    static bool asking = false;
    if (asking) {
        LOG_WARN("unsaved edits: asked again while answering; going on with the save under way");
        return true;
    }
    struct Asking {
        bool& flag;
        explicit Asking(bool& f) : flag(f) { flag = true; }
        ~Asking() { flag = false; }
    } guard(asking);
    // Nothing of an Underground 1 region can be written: the edits just go.
    if (world_->IsUnderground1()) {
        for (const auto& section : world_->Loaded()) {
            section->placementsChanged = false;
            section->collisionChanged = false;
        }
        return true;
    }
    const int answer = ::MessageBoxW(
        window_,
        L"There are placed objects or collision edits that have not been written to the game.\n\n"
        L"Write them now?",
        kWindowTitle, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) return SavePlacements();
    for (const auto& section : world_->Loaded()) {
        section->placementsChanged = false;
        section->collisionChanged = false;
    }
    return true;
}

bool MainWindow::PlacementPoint(Vec3& point, size_t& target) {
    if (!world_) {
        ::MessageBoxW(window_, L"Open a region first.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return false;
    }

    // Where the view is centred: the first surface under the middle of the
    // viewport, or a little way ahead of the camera when there is none.
    RECT client{};
    ::GetClientRect(viewport_.Handle(), &client);
    const Ray ray = viewport_.RayAt(client.right / 2, client.bottom / 2);
    PickResult hit;
    point = ray.origin + ray.direction * 20.0f;
    if (viewport_.GetRenderer().Pick(ray, hit) && hit.distance < 2000.0f)
        point = ray.origin + ray.direction * hit.distance;

    target = world_->SectionForPlacement(point);
    if (target == SIZE_MAX) {
        ::MessageBoxW(window_, L"No section here can take scenery.", kWindowTitle,
                      MB_ICONINFORMATION | MB_OK);
        return false;
    }
    return true;
}

void MainWindow::PlaceObject(bool onlyMine) {
    Vec3 point;
    size_t target = 0;
    if (!PlacementPoint(point, target)) return;

    u32 hash = 0;
    bool copy = false;
    thumbnails_.SetRegion(world_->Region());
    const bool chosen = ChooseModelToPlace(
        instance_, window_, *world_, target, hash, copy, onlyMine,
        [this](const nfs::SolidObject& model, std::vector<u32>& pixels) {
            return thumbnails_.Get(viewport_.GetRenderer(), model, pixels);
        });
    // Default-pool targets would stop the device being reset on a resize.
    viewport_.GetRenderer().ReleaseThumbnailTargets();
    if (!chosen) return;
    PlaceModel(hash, copy, target, point);
}

void MainWindow::PlaceModelAtView(u32 hash) {
    Vec3 point;
    size_t target = 0;
    if (!PlacementPoint(point, target)) return;
    const nfs::SolidObject* model = world_->FindModel(hash);
    if (!model) return;

    const auto& loaded = world_->Loaded();
    const size_t home = world_->ModelHome(hash);
    const bool library = home < loaded.size() && world_->IsLibrary(*loaded[home]);
    const bool here = loaded[target]->HoldsModel(hash);
    // Stored in another positioned section: copied into this one when the
    // placements are written.
    PlaceModel(hash, !library && !here, target, point);
}

bool MainWindow::GroundUnderCursor(Vec3& point) {
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::ScreenToClient(viewport_.Handle(), &cursor);
    RECT client{};
    ::GetClientRect(viewport_.Handle(), &client);
    if (!::PtInRect(&client, cursor)) return false;

    // The first surface under the pointer, never the object being dragged --
    // it would otherwise climb its own roof one frame at a time.
    const Ray ray = viewport_.RayAt(cursor.x, cursor.y);
    WorldRenderer& renderer = viewport_.GetRenderer();
    if (dragPlaced_) renderer.SetPickExclusion(dragSection_, dragPlacement_);
    PickResult hit;
    const bool found = renderer.Pick(ray, hit) && hit.distance < 4000.0f;
    renderer.SetPickExclusion(0, -1);
    if (!found) return false;
    point = ray.origin + ray.direction * hit.distance;

    // Onto the collision when there is some there: it is what the car drives
    // on, and the visible surface of a road can sit a hand's width above it.
    float ground = 0.0f;
    if (nfs::GroundHeight(*world_, point.x, point.y, ground) &&
        std::fabs(ground - point.z) < 5.0f)
        point.z = ground;
    return true;
}

void MainWindow::BeginLibraryDrag(u32 hash) {
    if (!world_ || !world_->FindModel(hash)) return;
    libDragging_ = true;
    dragHash_ = hash;
    dragPlaced_ = false;
    dragPlacement_ = -1;
    dragAddedImport_ = false;
    ::SetCapture(window_);
    ::SetFocus(window_);      // for Escape
    ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
    SetStatusText(2, L"Drag onto the map and let go where it should stand -- Escape cancels");
}

void MainWindow::UpdateLibraryDrag() {
    Vec3 point;
    if (!GroundUnderCursor(point)) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
        return;
    }
    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));

    // One of an extension's rows: it makes and moves its own.
    if (dragExtension_) {
        dragExtension_->UpdateDrag(point);
        return;
    }

    if (!dragPlaced_) {
        const size_t target = world_->SectionForPlacement(point);
        if (target == SIZE_MAX) return;
        nfs::LoadedSection* section = world_->MutableLoaded(target);
        const auto& loaded = world_->Loaded();
        const size_t home = world_->ModelHome(dragHash_);
        const bool library = home < loaded.size() && world_->IsLibrary(*loaded[home]);
        const bool here = loaded[target]->HoldsModel(dragHash_);
        const bool already = section && std::find(section->importedModels.begin(),
                                                  section->importedModels.end(),
                                                  dragHash_) != section->importedModels.end();
        PlaceModel(dragHash_, !library && !here, target, point);
        u32 placedSection = 0;
        int placed = -1;
        if (!viewport_.SelectedPlacement(placedSection, placed)) return;
        dragPlaced_ = true;
        dragSection_ = placedSection;
        dragPlacement_ = placed;
        dragAddedImport_ = !library && !here && !already;
        return;
    }

    const nfs::LoadedSection* section = world_->Loaded()[dragSection_].get();
    if (dragPlacement_ < 0 || (size_t)dragPlacement_ >= section->placements.size()) return;
    Mat4 transform = section->placements[(size_t)dragPlacement_].instance.transform;
    transform.m[3][0] = point.x;
    transform.m[3][1] = point.y;
    transform.m[3][2] = point.z;
    viewport_.SetPlacementTransform(dragSection_, dragPlacement_, transform);
}

void MainWindow::RemovePlacement(u32 sectionIndex, int placement, u32 hash) {
    nfs::LoadedSection* section = world_ ? world_->MutableLoaded(sectionIndex) : nullptr;
    if (!section || placement < 0 || (size_t)placement >= section->placements.size()) return;
    section->placements.erase(section->placements.begin() + placement);
    section->placementsChanged = true;
    viewport_.GetRenderer().ClearSelection();
    viewport_.GetRenderer().RebuildInstances(*world_);
    (void)hash;
}

void MainWindow::EndLibraryDrag(bool keep) {
    if (!libDragging_) return;
    libDragging_ = false;              // first: releasing the capture comes back here
    if (::GetCapture() == window_) ::ReleaseCapture();
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));

    if (dragExtension_) {
        EditorExtension* extension = dragExtension_;
        dragExtension_ = nullptr;
        Vec3 point;
        const bool onGround = keep && GroundUnderCursor(point);
        extension->EndDrag(onGround ? &point : nullptr);
        if (onGround) ::SetFocus(viewport_.Handle());
        return;
    }
    if (!dragPlaced_) {
        SetStatusText(2, L"");
        return;
    }

    // Let go outside the view: the drag is called off.
    Vec3 point;
    if (keep && !GroundUnderCursor(point)) keep = false;

    nfs::LoadedSection* section = world_->MutableLoaded(dragSection_);
    if (!keep) {
        RemovePlacement(dragSection_, dragPlacement_, dragHash_);
        if (dragAddedImport_ && section) {
            auto& imports = section->importedModels;
            imports.erase(std::remove(imports.begin(), imports.end(), dragHash_), imports.end());
        }
        SetStatusText(2, L"Placement cancelled");
        return;
    }

    // Carried onto another section's ground: it belongs to that section now,
    // the same hand-over the move tool makes.
    viewport_.RehomePlacement(dragSection_, dragPlacement_);
    ::SetFocus(viewport_.Handle());
    SetStatusText(2, L"Placed -- G moves it, R turns it, Ctrl+S saves the world");
}

void MainWindow::PlaceModel(u32 hash, bool copy, size_t target, const Vec3& point) {
    const nfs::SolidObject* model = world_ ? world_->FindModel(hash) : nullptr;
    nfs::LoadedSection* section = world_ ? world_->MutableLoaded(target) : nullptr;
    if (!model || !section || !section->info) return;

    nfs::PlacedInstance placed;
    placed.modelHash = hash;
    placed.modelName = model->name;
    placed.modelBounds = model->bounds;
    placed.triangles = model->TriangleCount();
    if (model->bounds.Valid())
        placed.radius = Length(model->bounds.max - model->bounds.min) * 0.5f;

    // Facing the camera, standing where the view is centred.
    const float heading = Degrees(viewport_.GetCamera().Yaw()) + 180.0f;
    placed.transform = nfs::ComposePlacement(point, heading, 0.0f, 0.0f, 1.0f);

    // What the game already uses for this model -- its flags and its entry in
    // the model table -- is copied from a shipped instance when there is one,
    // since not every field of either is understood.
    for (const auto& other : world_->Loaded()) {
        const nfs::Scenery& scenery = other->scenery;
        bool done = false;
        for (const nfs::SceneryInstance& instance : scenery.instances) {
            if (scenery.ModelHash(instance) != hash) continue;
            placed.flags = (u16)instance.flags;
            placed.infoTemplate = scenery.infos[instance.infoIndex].raw;
            done = true;
            break;
        }
        if (done) break;
    }

    if (copy && std::find(section->importedModels.begin(), section->importedModels.end(), hash) ==
                    section->importedModels.end())
        section->importedModels.push_back(hash);
    section->placements.push_back({ section->info->number, placed });
    section->placementsChanged = true;
    viewport_.GetRenderer().RebuildInstances(*world_);

    // Selected, so it can be moved straight away.
    viewport_.GetRenderer().SelectPlacement((u32)target, (int)section->placements.size() - 1);

    LOG_INFO("placed %s in section %s at (%.2f, %.2f, %.2f)", model->name.c_str(),
             section->info->name.c_str(), point.x, point.y, point.z);
}

void MainWindow::EditSelectedPlacement() {
    u32 section = 0;
    int placement = -1;
    if (!world_ || !viewport_.SelectedPlacement(section, placement)) {
        ::MessageBoxW(window_, L"Select an object you placed first (Objects > Place Object).",
                      kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }
    nfs::LoadedSection* loaded = world_->MutableLoaded(section);
    const nfs::PlacedInstance& placed = loaded->placements[(size_t)placement].instance;

    PlacementEdit edit;
    edit.description = Format(L"%s\nin section %s", Widen(placed.modelName).c_str(),
                              loaded->info ? Widen(loaded->info->name).c_str() : L"?");
    edit.transform = placed.transform;
    edit.apply = [this, section, placement](const Mat4& transform) {
        viewport_.SetPlacementTransform(section, placement, transform);
        viewport_.Tick(0.0f);   // the dialog's own loop does not draw frames
    };
    edit.snap = [this, section, placement](Mat4& transform) {
        float height = 0.0f;
        const Vec3 at{ transform.m[3][0], transform.m[3][1], transform.m[3][2] };
        if (!viewport_.GroundBelow(at, section, placement, height)) return false;
        transform.m[3][2] = height;
        return true;
    };
    if (EditPlacement(instance_, window_, edit)) viewport_.RehomePlacement(section, placement);
}

void MainWindow::SnapSelectedPlacement() {
    u32 section = 0;
    int placement = -1;
    if (!world_ || !viewport_.SelectedPlacement(section, placement)) return;
    Mat4 transform = world_->Loaded()[section]->placements[(size_t)placement].instance.transform;
    float height = 0.0f;
    if (!viewport_.GroundBelow({ transform.m[3][0], transform.m[3][1], transform.m[3][2] },
                               section, placement, height)) {
        SetStatusText(2, L"There is no ground under it.");
        return;
    }
    transform.m[3][2] = height;
    viewport_.SetPlacementTransform(section, placement, transform);
}

void MainWindow::DeleteSelectedPlacement() {
    if (!world_) return;
    auto chosen = viewport_.SelectedPlacements();

    // The map's own objects are removed rather than deleted: their records
    // stay, matrix zeroed, so nothing that counts records moves.
    size_t removed = 0;
    for (const PickResult& picked : viewport_.GetRenderer().Selection()) {
        if (!picked.IsNative()) continue;
        nfs::LoadedSection* loaded = world_->MutableLoaded(picked.loadedSection);
        if (!loaded || (size_t)picked.native >= loaded->scenery.instances.size()) continue;
        const nfs::SceneryInstance& instance = loaded->scenery.instances[(size_t)picked.native];
        const auto key = std::make_pair(instance.sceneryNumber, instance.record);
        if (std::find(loaded->removedNatives.begin(), loaded->removedNatives.end(), key) ==
            loaded->removedNatives.end())
            loaded->removedNatives.push_back(key);
        loaded->placementsChanged = true;
        ++removed;
    }

    if (chosen.empty() && removed == 0) {
        SetStatusText(2, L"Select scenery objects to delete first.");
        return;
    }
    // Last first within a section, so the indices still to go stay valid.
    std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    });
    for (const auto& [section, placement] : chosen) {
        nfs::LoadedSection* loaded = world_->MutableLoaded(section);
        if (!loaded || (size_t)placement >= loaded->placements.size()) continue;
        loaded->placements.erase(loaded->placements.begin() + placement);
        loaded->placementsChanged = true;
    }
    viewport_.GetRenderer().ClearSelection();
    viewport_.GetRenderer().RebuildInstances(*world_);
    SetStatusText(2, Format(L"Deleted %zu placed objects, removed %zu of the map's own "
                            L"(not written yet: Ctrl+Shift+S)", chosen.size(), removed));
}

void MainWindow::CopySelection() {
    if (!world_) return;
    // An extension's selection is copied by the extension, on its own.
    for (auto& extension : extensions_)
        if (extension->Copy()) {
            clipboard_.clear();
            for (auto& other : extensions_)
                if (other != extension) other->ClearClipboard();
            return;
        }
    for (auto& extension : extensions_) extension->ClearClipboard();
    clipboard_.clear();
    const auto& loaded = world_->Loaded();
    for (const PickResult& picked : viewport_.GetRenderer().Selection()) {
        if (picked.loadedSection >= loaded.size()) continue;
        const nfs::LoadedSection& section = *loaded[picked.loadedSection];

        if (picked.IsPlacement() && (size_t)picked.placement < section.placements.size()) {
            clipboard_.push_back(section.placements[(size_t)picked.placement].instance);
            continue;
        }
        if (!picked.IsNative() || (size_t)picked.native >= section.scenery.instances.size()) continue;

        // One of the map's own: its record, as a placement to be.
        const nfs::Scenery& scenery = section.scenery;
        const nfs::SceneryInstance& instance = scenery.instances[(size_t)picked.native];
        if (instance.infoIndex >= scenery.infos.size()) continue;
        nfs::PlacedInstance placed;
        placed.modelHash = scenery.ModelHash(instance);
        placed.modelName = scenery.infos[instance.infoIndex].name;
        placed.transform = instance.transform;
        placed.flags = (u16)instance.flags;
        placed.infoTemplate = scenery.infos[instance.infoIndex].raw;
        if (const nfs::SolidObject* model = world_->FindModel(placed.modelHash)) {
            placed.modelBounds = model->bounds;
            placed.triangles = model->TriangleCount();
        }
        clipboard_.push_back(std::move(placed));
    }
    SetStatusText(2, clipboard_.empty()
                         ? std::wstring(L"Nothing to copy: select scenery objects first.")
                         : Format(L"Copied %zu objects. Ctrl+V puts them at the view centre.",
                                  clipboard_.size()));
}

void MainWindow::PasteClipboard() {
    if (world_)
        for (auto& extension : extensions_)
            if (extension->Paste()) return;
    if (!world_ || clipboard_.empty()) {
        SetStatusText(2, L"Nothing to paste: copy objects with Ctrl+C first.");
        return;
    }

    RECT client{};
    ::GetClientRect(viewport_.Handle(), &client);
    const Ray ray = viewport_.RayAt(client.right / 2, client.bottom / 2);
    PickResult hit;
    Vec3 point = ray.origin + ray.direction * 20.0f;
    if (viewport_.GetRenderer().Pick(ray, hit) && hit.distance < 2000.0f)
        point = ray.origin + ray.direction * hit.distance;

    // The group's pivot -- the middle of the copied objects in plan, at the
    // height of the lowest -- lands on the point, so the layout keeps its
    // shape and the group stands on what the view is centred on.
    Vec3 pivot{};
    float lowest = 1e30f;
    for (const nfs::PlacedInstance& placed : clipboard_) {
        pivot.x += placed.transform.m[3][0];
        pivot.y += placed.transform.m[3][1];
        lowest = std::min(lowest, placed.transform.m[3][2]);
    }
    pivot = pivot * (1.0f / (float)clipboard_.size());
    pivot.z = lowest;
    const Vec3 shift = point - pivot;

    std::vector<std::pair<u32, int>> pasted;
    size_t copiedModels = 0, skipped = 0;
    for (nfs::PlacedInstance placed : clipboard_) {
        placed.transform.m[3][0] += shift.x;
        placed.transform.m[3][1] += shift.y;
        placed.transform.m[3][2] += shift.z;
        const Vec3 at{ placed.transform.m[3][0], placed.transform.m[3][1], placed.transform.m[3][2] };
        const size_t target = world_->SectionForPlacement(at);
        nfs::LoadedSection* section = target == SIZE_MAX ? nullptr : world_->MutableLoaded(target);
        if (!section || !section->info) { ++skipped; continue; }

        // A model outside the library goes with its copies.
        const size_t home = world_->ModelHome(placed.modelHash);
        const bool library = home < world_->Loaded().size() && world_->IsLibrary(*world_->Loaded()[home]);
        if (!library && !section->HoldsModel(placed.modelHash)) {
            section->importedModels.push_back(placed.modelHash);
            ++copiedModels;
        }
        section->placements.push_back({ section->info->number, placed });
        section->placementsChanged = true;
        pasted.push_back({ (u32)target, (int)section->placements.size() - 1 });
    }

    viewport_.GetRenderer().RebuildInstances(*world_);
    viewport_.GetRenderer().SelectPlacements(pasted);
    SetStatusText(2, Format(L"Pasted %zu objects%s%s (not written yet)", pasted.size(),
                            copiedModels ? L", models copied into their sections" : L"",
                            skipped ? L", some had no section to go into" : L""));
    LOG_INFO("pasted %zu objects around (%.1f, %.1f, %.1f), %zu models to copy in, %zu skipped",
             pasted.size(), point.x, point.y, point.z, copiedModels, skipped);
}

bool MainWindow::SavePlacements() {
    if (!world_) return false;
    if (RefuseReadOnlyWorld()) return false;
    if (!HasUnsavedPlacements()) {
        SetStatusText(2, L"No placements to write.");
        return true;
    }

    PlacementSavePlan plan;
    const bool planned = PlanPlacementSave(*world_, modelsFolder_, plan);
    if (!planned && plan.ok && plan.spans.empty()) {
        // Only placements that were never written, and none are left -- or
        // collision changed and changed back.
        for (const auto& section : world_->Loaded()) {
            section->placementsChanged = false;
            section->collisionChanged = false;
            section->changedCollisionPacks.clear();
        }
        SetStatusText(2, L"No placements to write.");
        return true;
    }
    if (!planned) {
        ::MessageBoxW(window_,
                      Format(L"The placements could not be written:\n\n%s\nNothing was changed.",
                             plan.report.c_str()).c_str(),
                      kWindowTitle, MB_ICONWARNING | MB_OK);
        return false;
    }

    const std::wstring region = world_->Region();
    const size_t written = WriteSectionSpans(plan.spans, plan.superseded, modelsFolder_, region);
    if (written != plan.spans.size()) {
        ::MessageBoxW(window_, L"Some overlays could not be written; the older ones were kept. "
                               L"The log has why.", kWindowTitle, MB_ICONWARNING | MB_OK);
        return false;
    }
    // Everything is on disk now. The collision flags must clear as well: the
    // reopen below asks about unsaved edits, and a flag left up would ask
    // again, save again, reopen again, without end.
    for (const auto& section : world_->Loaded()) {
        section->placementsChanged = false;
        section->collisionChanged = false;
        section->changedCollisionPacks.clear();
    }
    for (auto& extension : extensions_) extension->OnSaved();

    // Reopened so the viewport shows exactly what the game will read, from
    // the same viewpoint.
    Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();
    OpenRegion(region);
    camera.SetPosition(position);
    camera.SetYawPitch(yaw, pitch);

    SetStatusText(2, Format(L"Placements written: %zu section overlays", written));
    LOG_INFO("placements written:\n%S", plan.report.c_str());
    return true;
}

void MainWindow::ShowRegionEditList() {
    if (!world_) return;
    edits_.Refresh(world_.get(), modelsFolder_);
}

void MainWindow::RevertTickedEdits() {
    if (!world_) return;
    // The list is read from what is on disk, so pending changes go first. A
    // save reopens the region, which leaves the list pointing at sections
    // that are gone: it is made again, and the same rows ticked again.
    if (HasUnsavedPlacements()) {
        const std::vector<std::wstring> ticked = edits_.TickedKeys();
        if (!ConfirmUnsavedPlacements()) return;
        if (!world_) return;
        edits_.Refresh(world_.get(), modelsFolder_);
        edits_.TickKeys(ticked);
    }

    std::wstring summary;
    if (!edits_.RevertTicked(window_, summary)) return;

    Camera& camera = viewport_.GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();
    OpenRegion(world_->Region());
    camera.SetPosition(position);
    camera.SetYawPitch(yaw, pitch);

    ::MessageBoxW(window_, (summary + L"\nThe region has been reloaded.").c_str(), kWindowTitle,
                  MB_ICONINFORMATION | MB_OK);
}

void MainWindow::UpdateProperties() {
    using Row = PropertiesPanel::Row;
    PropertiesPanel::View view;
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value,
                             const wchar_t* description) {
        Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        view.rows.push_back(std::move(row));
    };

    if (world_)
        for (auto& extension : extensions_)
            if (extension->FillProperties(view)) {
                view.mode = PropertiesPanel::Mode::Extension;
                properties_.Show(view);
                return;
            }

    const auto& selection = viewport_.GetRenderer().Selection();
    if (!world_ || selection.empty()) {
        properties_.Show(view);
        return;
    }
    if (selection.size() > 1) {
        size_t placed = 0;
        for (const PickResult& picked : selection) placed += picked.IsPlacement();
        view.mode = PropertiesPanel::Mode::Several;
        view.title = Format(L"%zu objects selected", selection.size());
        add(L"Selection", L"Objects", std::to_wstring(selection.size()), L"How many are selected.");
        add(L"Selection", L"Placed by the editor", std::to_wstring(placed),
            L"G moves and R turns these together, about the middle of the group.");
        add(L"Selection", L"From the map", std::to_wstring(selection.size() - placed),
            L"The map's own objects stay where they are under G and R; Delete removes them.");
        add(L"Selection", L"Copy / paste", L"Ctrl+C, Ctrl+V",
            L"Copies every selected object and puts the copies at the view centre, keeping the layout.");
        properties_.Show(view);
        return;
    }

    const PickResult& picked = selection.front();
    const auto& loaded = world_->Loaded();
    const nfs::LoadedSection* section =
        picked.loadedSection < loaded.size() ? loaded[picked.loadedSection].get() : nullptr;
    const nfs::SolidObject* model = picked.object;
    const size_t home = model ? world_->ModelHome(model->hash) : SIZE_MAX;

    view.title = model ? Widen(model->name) : L"?";
    view.transform = picked.transform;
    if (picked.IsPlacement() && section && (size_t)picked.placement < section->placements.size()) {
        view.mode = PropertiesPanel::Mode::Placement;
        view.transform = section->placements[(size_t)picked.placement].instance.transform;
    } else if (picked.IsNative()) {
        view.mode = PropertiesPanel::Mode::Native;
    } else {
        view.mode = PropertiesPanel::Mode::Library;
    }

    const wchar_t* source = view.mode == PropertiesPanel::Mode::Placement ? L"Placed by the editor"
                          : view.mode == PropertiesPanel::Mode::Native    ? L"Part of the map"
                                                                          : L"Model library (header matrix)";
    add(L"Object", L"Model", model ? Widen(model->name) : L"?", L"The solid object this places.");
    add(L"Object", L"Source", source,
        L"Placed objects can be edited and deleted. The map's own can be copied, removed, or moved -- "
        L"G, R, the gizmo or these fields; moving one replaces it with a placement of the same model.");
    add(L"Object", L"Section", section && section->info ? Widen(section->info->name) : L"?",
        L"The streaming section that places it: the game draws it while that section is loaded.");
    add(L"Object", L"Model stored in",
        home < loaded.size() && loaded[home]->info ? Widen(loaded[home]->info->name) : L"?",
        L"The section holding the mesh. Library sections are always loaded; another section's "
        L"model is copied along when placed elsewhere.");
    add(L"Object", L"Hash", model ? Format(L"%08X", model->hash) : L"",
        L"The hash the scenery tables name the model by.");
    add(L"Object", L"Triangles", model ? std::to_wstring(model->TriangleCount()) : L"", L"");
    add(L"Object", L"Textures", model ? std::to_wstring(model->textureHashes.size()) : L"", L"");
    if (view.mode == PropertiesPanel::Mode::Placement && section)
        add(L"Object", L"Written", section->placementsChanged ? L"No -- Ctrl+Shift+S writes it" : L"Yes",
            L"Whether the game will see it yet.");

    // The map's own objects can be moved too: the first change makes one a
    // placement (Viewport::DetachNatives).
    PropertiesPanel::AddTransformRows(view, view.mode == PropertiesPanel::Mode::Placement ||
                                                view.mode == PropertiesPanel::Mode::Native);

    if (view.mode == PropertiesPanel::Mode::Native && section &&
        (size_t)picked.native < section->scenery.instances.size()) {
        const nfs::SceneryInstance& instance = section->scenery.instances[(size_t)picked.native];
        add(L"Scenery record", L"Scenery chunk", std::to_wstring(instance.sceneryNumber),
            L"The number in the header of the scenery chunk holding the record.");
        add(L"Scenery record", L"Record", std::to_wstring(instance.record),
            L"Its index among that chunk's records. Removing it keeps the record, matrix zeroed, "
            L"so no index moves.");
        add(L"Scenery record", L"Flags", Format(L"%04X", instance.flags),
            L"Stored per instance; what the bits select is not known yet.");
    }
    properties_.Show(view);
}

void MainWindow::ApplyPropertyFields(bool rehome) {
    u32 section = 0;
    int placement = -1;
    if (!world_) return;
    Mat4 transform;
    if (!properties_.Read(transform)) return;
    // One of the map's own objects edited in the fields: it becomes a placement first.
    if (!viewport_.SelectedPlacement(section, placement)) viewport_.DetachNatives();
    if (!viewport_.SelectedPlacement(section, placement)) return;
    viewport_.SetPlacementTransform(section, placement, transform);
    if (rehome) viewport_.RehomePlacement(section, placement);
}

// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------

EditorExtension* MainWindow::ExtensionOf(const void* owner) const {
    if (!owner) return nullptr;
    for (const auto& extension : extensions_)
        if (extension.get() == owner) return extension.get();
    return nullptr;
}

bool MainWindow::ViewCentrePoint(Vec3& point) {
    // The first surface under the middle of the viewport, as for placing an
    // object; a point needs ground under it, so a miss is not a place.
    RECT client{};
    ::GetClientRect(viewport_.Handle(), &client);
    const Ray ray = viewport_.RayAt(client.right / 2, client.bottom / 2);
    PickResult hit;
    if (!viewport_.GetRenderer().Pick(ray, hit) || hit.distance > 2000.0f) return false;
    point = ray.origin + ray.direction * hit.distance;
    // Onto the collision when there is some there: it is what the car drives on.
    float ground = 0.0f;
    if (nfs::GroundHeight(*world_, point.x, point.y, ground) &&
        std::fabs(ground - point.z) < 5.0f)
        point.z = ground;
    return true;
}

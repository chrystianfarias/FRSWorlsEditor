#pragma once

#include <windows.h>
#include <d3d9.h>

#include <functional>

#include "PhosphorIcons.h"

struct ImFont;

// The editor's panels, drawn with Dear ImGui (third_party/imgui, docking
// branch) over the 3D view, in the viewport's own Direct3D 9 device.
//
// One dock space covers the view; its middle passes clicks through to the
// world, and panels dock round its edges, as tabs, or float. Layout and
// sizes are kept in %LOCALAPPDATA%\FRSWorldEditor\imgui.ini.
//
// Text is Segoe UI, read from the system's fonts folder; icons are Phosphor
// (phosphoricons.com, MIT), built into the program.
class ImGuiLayer {
public:
    bool Init(HWND window, IDirect3DDevice9* device);
    void Shutdown();
    bool Ready() const { return ready_; }

    // Every message of the window, before the window's own handling.
    void HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // Starts a frame and lays out the dock space; panels are drawn between
    // this and Render.
    // `bars` draws what takes the viewport's edges -- menu, toolbar, status
    // bar -- before the dock space fills what is left.
    void BeginFrame(const std::function<void()>& bars = {});
    void Render();

    // The device's default-pool objects go before a reset and come back after.
    void DeviceLost();
    void DeviceReset();

    // Whether ImGui is using the mouse or the keyboard right now -- a panel
    // under the pointer, a field being typed in -- so the 3D view leaves them.
    bool WantsMouse() const;
    bool WantsKeyboard() const;
    bool WantsText() const;

    static ImFont* BoldFont() { return bold_; }

    // Icons for labels: Phosphor (third_party/phosphor), merged into the
    // text font. Every icon is in PhosphorIcons.h as ICON_PH_<NAME>.
    static constexpr const char* kIconSettings = ICON_PH_GEAR;
    static constexpr const char* kIconDelete   = ICON_PH_TRASH;
    static constexpr const char* kIconEdit     = ICON_PH_PENCIL_SIMPLE;
    static constexpr const char* kIconDown     = ICON_PH_ARROW_LINE_DOWN;
    static constexpr const char* kIconShield   = ICON_PH_SHIELD;
    static constexpr const char* kIconMove     = ICON_PH_ARROWS_OUT_CARDINAL;
    static constexpr const char* kIconInfo     = ICON_PH_INFO;
    static constexpr const char* kIconList     = ICON_PH_LIST;
    static constexpr const char* kIconSort     = ICON_PH_SORT_ASCENDING;
    static constexpr const char* kIconCube     = ICON_PH_CUBE;
    static constexpr const char* kIconSliders  = ICON_PH_SLIDERS_HORIZONTAL;

private:
    void ApplyTheme(float scale);
    void BuildDefaultLayout(unsigned int dockspace);

    bool ready_ = false;
    bool layoutChecked_ = false;
    char iniPath_[MAX_PATH] = {};
    static ImFont* bold_;
};

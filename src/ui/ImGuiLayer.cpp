#include "ui/ImGuiLayer.h"

#include "core/Log.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam,
                                                             LPARAM lParam);

ImFont* ImGuiLayer::bold_ = nullptr;

namespace {

bool FileExists(const char* path) {
    const DWORD attributes = ::GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FontsFolder() {
    char windows[MAX_PATH] = {};
    ::GetWindowsDirectoryA(windows, MAX_PATH);
    return std::string(windows) + "\\Fonts\\";
}

ImVec4 Rgb(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

// Adds a text face with the Phosphor icons merged into it.
ImFont* AddFace(ImGuiIO& io, const std::string& text, float size) {
    ImFont* font = nullptr;
    if (FileExists(text.c_str())) {
        ImFontConfig config;
        config.OversampleH = 2;
        font = io.Fonts->AddFontFromFileTTF(text.c_str(), size, &config, io.Fonts->GetGlyphRangesDefault());
    }
    if (!font) font = io.Fonts->AddFontDefault();
    static const ImWchar range[] = { ICON_PH_MIN, ICON_PH_MAX, 0 };
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.FontDataOwnedByAtlas = false;   // the program's own bytes
    merge.PixelSnapH = true;
    merge.GlyphOffset = ImVec2(0.0f, size * 0.16f);
    merge.GlyphMinAdvanceX = size;
    io.Fonts->AddFontFromMemoryTTF((void*)kPhosphorFont, (int)kPhosphorFontSize, size * 1.05f, &merge, range);
    return font;
}

} // namespace

bool ImGuiLayer::Init(HWND window, IDirect3DDevice9* device) {
    if (ready_ || !window || !device) return ready_;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingWithShift = false;

    // The layout lives with the editor's other per-user files.
    char base[MAX_PATH] = {};
    if (::GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) {
        std::string folder = std::string(base) + "\\FRSWorldEditor";
        ::CreateDirectoryA(folder.c_str(), nullptr);
        std::snprintf(iniPath_, sizeof(iniPath_), "%s\\imgui.ini", folder.c_str());
        io.IniFilename = iniPath_;
    } else {
        io.IniFilename = nullptr;
    }

    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    const std::string fonts = FontsFolder();
    const float size = 16.0f * scale;
    AddFace(io, fonts + "segoeui.ttf", size);
    const std::string semibold = FileExists((fonts + "seguisb.ttf").c_str()) ? fonts + "seguisb.ttf" : fonts + "segoeuib.ttf";
    bold_ = AddFace(io, semibold, size * 1.08f);
    if (!bold_) bold_ = io.Fonts->Fonts[0];

    ApplyTheme(scale);

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(device)) {
        LOG_ERROR("ImGui could not start on the viewport");
        ImGui::DestroyContext();
        return false;
    }
    ready_ = true;
    LOG_INFO("ImGui %s ready (scale %.2f)", IMGUI_VERSION, scale);
    return true;
}

void ImGuiLayer::Shutdown() {
    if (!ready_) return;
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ready_ = false;
}

void ImGuiLayer::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ready_) ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
}

void ImGuiLayer::ApplyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(8, 5);
    style.CellPadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 14;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.WindowRounding = 6;
    style.ChildRounding = 4;
    style.FrameRounding = 4;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 3;
    style.TabRounding = 4;
    style.TabBarOverlineSize = 2;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1;
    style.DockingSeparatorSize = 2;

    ImVec4* c = style.Colors;
    const ImVec4 accent = Rgb(0x3696DB);
    c[ImGuiCol_Text]                  = Rgb(0xDCDDDE);
    c[ImGuiCol_TextDisabled]          = Rgb(0x80838A);
    c[ImGuiCol_WindowBg]              = Rgb(0x1E1F22, 0.97f);
    c[ImGuiCol_ChildBg]               = Rgb(0x1A1B1E, 0.0f);
    c[ImGuiCol_PopupBg]               = Rgb(0x232428, 0.98f);
    c[ImGuiCol_Border]                = Rgb(0x34363C);
    c[ImGuiCol_BorderShadow]          = Rgb(0x000000, 0.0f);
    c[ImGuiCol_FrameBg]               = Rgb(0x2B2D31);
    c[ImGuiCol_FrameBgHovered]        = Rgb(0x34363C);
    c[ImGuiCol_FrameBgActive]         = Rgb(0x3A3D44);
    c[ImGuiCol_TitleBg]               = Rgb(0x18191C);
    c[ImGuiCol_TitleBgActive]         = Rgb(0x1B1C1F);
    c[ImGuiCol_TitleBgCollapsed]      = Rgb(0x18191C);
    c[ImGuiCol_MenuBarBg]             = Rgb(0x18191C);
    c[ImGuiCol_ScrollbarBg]           = Rgb(0x1E1F22, 0.0f);
    c[ImGuiCol_ScrollbarGrab]         = Rgb(0x3A3D44);
    c[ImGuiCol_ScrollbarGrabHovered]  = Rgb(0x4A4E57);
    c[ImGuiCol_ScrollbarGrabActive]   = Rgb(0x5A5F6A);
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = Rgb(0x4DB0E6);
    c[ImGuiCol_Button]                = Rgb(0x2F3136);
    c[ImGuiCol_ButtonHovered]         = Rgb(0x3C3F46);
    c[ImGuiCol_ButtonActive]          = Rgb(0x4A4E57);
    c[ImGuiCol_Header]                = Rgb(0x2B2D31);
    c[ImGuiCol_HeaderHovered]         = Rgb(0x34363C);
    c[ImGuiCol_HeaderActive]          = Rgb(0x3A3D44);
    c[ImGuiCol_Separator]             = Rgb(0x34363C);
    c[ImGuiCol_SeparatorHovered]      = accent;
    c[ImGuiCol_SeparatorActive]       = accent;
    c[ImGuiCol_ResizeGrip]            = Rgb(0x34363C, 0.0f);
    c[ImGuiCol_ResizeGripHovered]     = Rgb(0x3696DB, 0.5f);
    c[ImGuiCol_ResizeGripActive]      = accent;
    c[ImGuiCol_Tab]                   = Rgb(0x18191C);
    c[ImGuiCol_TabHovered]            = Rgb(0x34363C);
    c[ImGuiCol_TabSelected]           = Rgb(0x1E1F22);
    c[ImGuiCol_TabSelectedOverline]   = accent;
    c[ImGuiCol_TabDimmed]             = Rgb(0x18191C);
    c[ImGuiCol_TabDimmedSelected]     = Rgb(0x1E1F22);
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(0x3696DB, 0.4f);
    c[ImGuiCol_DockingPreview]        = Rgb(0x3696DB, 0.45f);
    c[ImGuiCol_DockingEmptyBg]        = Rgb(0x000000, 0.0f);
    c[ImGuiCol_PlotLines]             = accent;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_TableHeaderBg]         = Rgb(0x25262B);
    c[ImGuiCol_TableBorderStrong]     = Rgb(0x34363C);
    c[ImGuiCol_TableBorderLight]      = Rgb(0x2B2D31);
    c[ImGuiCol_TableRowBg]            = Rgb(0x000000, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = Rgb(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg]        = Rgb(0x3696DB, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accent;
    c[ImGuiCol_NavCursor]             = accent;
    c[ImGuiCol_ModalWindowDimBg]      = Rgb(0x000000, 0.5f);

    style.ScaleAllSizes(scale);
}

void ImGuiLayer::BuildDefaultLayout(unsigned int dockspace) {
    // The first run, or a layout file without the dock space: properties on
    // the right, a quarter of the view wide.
    // A layout from before the Explorer panel existed is rebuilt too.
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    ImGuiWindowSettings* explorer = ImGui::FindWindowSettingsByID(ImHashStr("###Explorer"));
    // ... and one from before the Library, Online and Edits were panels of their own.
    ImGuiWindowSettings* library = ImGui::FindWindowSettingsByID(ImHashStr("###Library"));
    if (node && node->IsSplitNode() && explorer && library) return;
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
    ImGuiID centre = dockspace;
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.27f, nullptr, &centre);
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.27f, nullptr, &centre);
    ImGuiID leftTop = left;
    const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.5f, nullptr, &leftTop);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.24f, nullptr, &centre);
    ImGui::DockBuilderDockWindow("###Properties", right);
    ImGui::DockBuilderDockWindow("###Explorer", leftTop);
    ImGui::DockBuilderDockWindow("###Library", leftBottom);
    ImGui::DockBuilderDockWindow("###Online", leftBottom);
    ImGui::DockBuilderDockWindow("###Edits", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void ImGuiLayer::BeginFrame(const std::function<void()>& bars) {
    if (!ready_) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (bars) bars();
    const ImGuiID dockspace = ImGui::GetID("EditorDockSpace");
    if (!layoutChecked_) {
        layoutChecked_ = true;
        BuildDefaultLayout(dockspace);
    }
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
}

void ImGuiLayer::Render() {
    if (!ready_) return;
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiLayer::DeviceLost() {
    if (ready_) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void ImGuiLayer::DeviceReset() {
    if (ready_) ImGui_ImplDX9_CreateDeviceObjects();
}

bool ImGuiLayer::WantsMouse() const { return ready_ && ImGui::GetIO().WantCaptureMouse; }
bool ImGuiLayer::WantsKeyboard() const { return ready_ && ImGui::GetIO().WantCaptureKeyboard; }
bool ImGuiLayer::WantsText() const { return ready_ && ImGui::GetIO().WantTextInput; }

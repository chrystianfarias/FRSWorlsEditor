#include "ui/PropertiesPanel.h"

#include "core/Types.h"
#include "nfs/SceneryEdit.h"
#include "ui/ImGuiLayer.h"
#include "ui/Resource.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>

namespace {

const wchar_t* kFieldNames[8] = { L"", L"X", L"Y", L"Z (height)", L"Heading", L"Pitch", L"Roll", L"Scale" };
const wchar_t* kFieldHelp[8] = {
    L"",
    L"World position along X, in game units.",
    L"World position along Y, in game units.",
    L"Height of the object's origin. Ctrl+G or Snap to ground drops it onto what is below.",
    L"Turn about the vertical, in degrees. R in the view turns it with the mouse.",
    L"Tilt about Y, in degrees.",
    L"Tilt about X, in degrees.",
    L"Uniform scale, 0.01 to 3.9: the file keeps the matrix in fixed point.",
};

constexpr size_t kBufferSize = 256;
constexpr int kListPicture = 64;       // beside a choice in a drop-down
constexpr int kCellPicture = 40;       // beside the chosen value in its row

PropertiesPanel::ImageProvider g_images;
unsigned g_imagesGeneration = 0;   // counts SetImageProvider calls

bool SameRows(const std::vector<PropertiesPanel::Row>& a, const std::vector<PropertiesPanel::Row>& b,
              bool ignoreFields) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].group != b[i].group || a[i].name != b[i].name || a[i].editable != b[i].editable ||
            a[i].field != b[i].field || a[i].choices != b[i].choices || a[i].preview != b[i].preview)
            return false;
        if ((!ignoreFields || a[i].field == 0) && a[i].value != b[i].value) return false;
    }
    return true;
}

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

bool ParseNumber(const std::wstring& value, float& out) {
    std::wstring text = value;
    for (wchar_t& c : text) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    out = std::wcstof(text.c_str(), &end);
    return end != text.c_str() && text.find_first_not_of(L" 0123456789.+-eE") == std::wstring::npos;
}

void Fill(std::string& buffer, const std::wstring& value) {
    const std::string text = Utf8(value);
    buffer.assign(kBufferSize, '\0');
    std::memcpy(buffer.data(), text.data(), std::min(text.size(), kBufferSize - 1));
}

// The provider's DIB, as a texture ImGui can draw.
IDirect3DTexture9* MakeTexture(IDirect3DDevice9* device, HBITMAP bitmap) {
    BITMAP info{};
    if (!device || !bitmap || !::GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0)
        return nullptr;
    const int width = info.bmWidth, height = std::abs(info.bmHeight);
    std::vector<u32> pixels((size_t)width * height);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = ::GetDC(nullptr);
    const int lines = ::GetDIBits(dc, bitmap, 0, (UINT)height, pixels.data(), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (lines <= 0) return nullptr;

    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture((UINT)width, (UINT)height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture,
                                     nullptr)))
        return nullptr;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        u32* row = (u32*)((u8*)locked.pBits + (size_t)y * locked.Pitch);
        for (int x = 0; x < width; ++x) row[x] = pixels[(size_t)y * width + x] | 0xFF000000u;
    }
    texture->UnlockRect(0);
    return texture;
}

} // namespace

void PropertiesPanel::SetImageProvider(ImageProvider provider) {
    g_images = std::move(provider);
    ++g_imagesGeneration;
}

void PropertiesPanel::ForgetPictures() {
    for (auto& [bitmap, texture] : textures_)
        if (texture) texture->Release();
    textures_.clear();
}

void PropertiesPanel::AddTransformRows(View& view, bool editable) {
    Vec3 position;
    float heading, pitch, roll, scale;
    nfs::DecomposePlacement(view.transform, position, heading, pitch, roll, scale);
    const float values[8] = { 0, position.x, position.y, position.z, heading, pitch, roll, scale };
    for (int field = 1; field <= 7; ++field) {
        wchar_t text[32];
        ::swprintf_s(text, L"%.3f", values[field]);
        Row row;
        row.group = L"Transform";
        row.name = kFieldNames[field];
        row.value = text;
        row.description = kFieldHelp[field];
        row.editable = editable;
        row.field = field;
        view.rows.push_back(std::move(row));
    }
}

void PropertiesPanel::Create(HWND parent, HINSTANCE instance, HFONT font) {
    (void)instance;
    (void)font;
    parent_ = parent;
    Show(View{});
}

void PropertiesPanel::Post(int id) const {
    if (parent_) ::PostMessageW(parent_, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

void PropertiesPanel::Show(const View& view) {
    const bool sameObject = haveShown_ && view.mode == shown_.mode && view.title == shown_.title &&
                            view.removed == shown_.removed;
    if (sameObject && SameRows(view.rows, shown_.rows, false)) {
        shown_.actions = view.actions;
        return;
    }

    if (sameObject && SameRows(view.rows, shown_.rows, true)) {
        // Only the transform moved: the values change in place, except in a
        // row someone is typing into.
        for (size_t i = 0; i < view.rows.size(); ++i) {
            if ((int)i == activeRow_) continue;
            values_[i] = view.rows[i].value;
            Fill(buffers_[i], values_[i]);
        }
        const std::wstring typing = activeRow_ >= 0 ? values_[(size_t)activeRow_] : std::wstring();
        shown_ = view;
        if (activeRow_ >= 0) shown_.rows[(size_t)activeRow_].value = typing;
        return;
    }

    shown_ = view;
    haveShown_ = true;
    activeRow_ = -1;
    values_.resize(view.rows.size());
    buffers_.resize(view.rows.size());
    for (size_t i = 0; i < view.rows.size(); ++i) {
        values_[i] = view.rows[i].value;
        Fill(buffers_[i], values_[i]);
    }
}

void PropertiesPanel::Commit(size_t row) {
    changed_ = true;
    lastField_ = shown_.rows[row].field;
    shown_.rows[row].value = values_[row];
    Post(IDC_PROP_GRID);
}

bool PropertiesPanel::OnCommand(int id) {
    if (id != IDC_PROP_GRID) return false;
    if (changed_) {
        changed_ = false;
        if (onChange_) onChange_();
    }
    return true;
}

void* PropertiesPanel::Picture(IDirect3DDevice9* device, const std::wstring& name, int size) {
    if (!g_images || name.empty()) return nullptr;
    if (picturesGeneration_ != g_imagesGeneration) {
        ForgetPictures();
        picturesGeneration_ = g_imagesGeneration;
    }
    HBITMAP bitmap = g_images(name, size);
    if (!bitmap) return nullptr;
    auto found = textures_.find(bitmap);
    if (found != textures_.end()) return found->second;
    IDirect3DTexture9* texture = MakeTexture(device, bitmap);
    // A miss is remembered too, so a bad picture is not retried every frame.
    textures_[bitmap] = texture;
    if (textures_.size() > 512) {
        for (auto& [handle, cached] : textures_)
            if (cached && cached != texture) cached->Release();
        textures_.clear();
        textures_[bitmap] = texture;
    }
    return texture;
}

void PropertiesPanel::DrawRow(size_t index, IDirect3DDevice9* device) {
    const Row& row = shown_.rows[index];
    ImGui::PushID((int)index);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    float number = 0.0f;
    const bool numeric = row.editable && row.choices.empty() && ParseNumber(values_[index], number);
    if (numeric) {
        // Drag the name to change the number.
        const float speed = row.field == 7 ? 0.005f : std::fabs(number) < 2.0f ? 0.01f : 0.05f;
        DragLabel("##name", Utf8(row.name).c_str(), 0, ImGui::GetContentRegionAvail().x, index, speed, true);
    } else {
        ImGui::AlignTextToFramePadding();
        if (row.editable) ImGui::TextUnformatted(Utf8(row.name).c_str());
        else ImGui::TextDisabled("%s", Utf8(row.name).c_str());
    }
    if (ImGui::IsItemHovered()) hoverHelp_ = row.description;

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string value = Utf8(values_[index]);

    if (row.editable && !row.choices.empty()) {
        if (row.preview)
            if (void* picture = Picture(device, values_[index], kCellPicture)) {
                ImGui::Image((ImTextureID)picture, ImVec2((float)kCellPicture, (float)kCellPicture));
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kCellPicture - ImGui::GetFrameHeight()) * 0.5f);
                ImGui::SetNextItemWidth(-FLT_MIN);
            }
        if (ImGui::BeginCombo("##value", value.c_str(), ImGuiComboFlags_HeightLargest)) {
            for (size_t c = 0; c < row.choices.size(); ++c) {
                const std::string choice = Utf8(row.choices[c]);
                const bool selected = row.choices[c] == values_[index];
                ImGui::PushID((int)c);
                bool picked = false;
                if (row.preview) {
                    const float side = (float)kListPicture;
                    if (void* picture = Picture(device, row.choices[c], kListPicture)) {
                        const ImVec2 at = ImGui::GetCursorPos();
                        picked = ImGui::Selectable("##choice", selected, 0, ImVec2(0, side));
                        ImGui::SetCursorPos(at);
                        ImGui::Image((ImTextureID)picture, ImVec2(side, side));
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(at.y + (side - ImGui::GetTextLineHeight()) * 0.5f);
                        ImGui::TextUnformatted(choice.c_str());
                    } else {
                        picked = ImGui::Selectable(choice.c_str(), selected);
                    }
                } else {
                    picked = ImGui::Selectable(choice.c_str(), selected);
                }
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
                if (picked && !selected) {
                    values_[index] = row.choices[c];
                    Fill(buffers_[index], values_[index]);
                    Commit(index);
                }
            }
            ImGui::EndCombo();
        }
    } else if (row.editable) {
        ValueInput(index, numeric || (row.field >= 1 && row.field <= 7));
    } else {
        if (row.preview)
            if (void* picture = Picture(device, values_[index], kCellPicture)) {
                ImGui::Image((ImTextureID)picture, ImVec2((float)kCellPicture, (float)kCellPicture));
                ImGui::SameLine();
            }
        ImGui::TextWrapped("%s", value.c_str());
    }
    if (ImGui::IsItemHovered()) hoverHelp_ = row.description;
    ImGui::PopID();
}

void PropertiesPanel::ValueInput(size_t row, bool numeric) {
    ImGui::InputText("##value", buffers_[row].data(), kBufferSize,
                     numeric ? ImGuiInputTextFlags_CharsScientific : 0);
    if (ImGui::IsItemActive()) activeRow_ = (int)row;
    else if (activeRow_ == (int)row && !dragging_) activeRow_ = -1;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        values_[row] = Wide(buffers_[row].c_str());
        Commit(row);
    }
}

void PropertiesPanel::DragLabel(const char* id, const char* text, unsigned badge, float width, size_t row,
                                float speed, bool live) {
    const ImGuiIO& io = ImGui::GetIO();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, width), height));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    if (badge) {
        // A coloured tag, brighter under the pointer.
        ImVec4 colour = ImGui::ColorConvertU32ToFloat4(badge);
        if (hovered || active) { colour.x = std::min(1.0f, colour.x * 1.25f); colour.y = std::min(1.0f, colour.y * 1.25f); colour.z = std::min(1.0f, colour.z * 1.25f); }
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::ColorConvertFloat4ToU32(colour),
                            ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
        draw->AddText(ImVec2(at.x + (width - textSize.x) * 0.5f, at.y + (height - textSize.y) * 0.5f),
                      IM_COL32(255, 255, 255, 235), text);
    } else {
        const ImU32 colour = ImGui::GetColorU32(hovered || active ? ImGuiCol_Text : ImGuiCol_Text, hovered || active ? 1.0f : 0.92f);
        draw->PushClipRect(at, ImVec2(at.x + width, at.y + height), true);
        draw->AddText(ImVec2(at.x, at.y + (height - textSize.y) * 0.5f), colour, text);
        draw->PopClipRect();
    }

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && io.MouseDelta.x != 0.0f) {
        float value = 0.0f;
        if (ParseNumber(values_[row], value)) {
            const float factor = io.KeyShift ? 10.0f : io.KeyCtrl ? 0.1f : 1.0f;
            value += io.MouseDelta.x * speed * factor;
            if (shown_.rows[row].field == 7) value = std::clamp(value, 0.02f, 3.9f);
            wchar_t text2[32];
            ::swprintf_s(text2, L"%.3f", value);
            values_[row] = text2;
            Fill(buffers_[row], values_[row]);
            shown_.rows[row].value = values_[row];
            activeRow_ = (int)row;
            dragging_ = true;
            // While dragging, the view follows -- a few times a second for
            // anything but the transform, whose apply is cheap.
            const double now = ImGui::GetTime();
            if (live && now - lastLiveCommit_ > (shown_.rows[row].field ? 0.0 : 0.1)) {
                lastLiveCommit_ = now;
                Commit(row);
            }
        }
    }
    if (ImGui::IsItemDeactivated() && dragging_) {
        dragging_ = false;
        Commit(row);
        activeRow_ = -1;
    }
}

void PropertiesPanel::DrawVector(const char* label, const size_t rows[3], const char* const axes[3],
                                 const unsigned colours[3], float speed) {
    ImGui::PushID(label);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered())
        hoverHelp_ = L"Drag a coloured tag to change that value (Shift faster, Ctrl finer), or type it.";
    ImGui::TableSetColumnIndex(1);
    const float spacing = 3.0f;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * 2) / 3.0f;
    const float tag = ImGui::GetFrameHeight() * 0.72f;
    for (int k = 0; k < 3; ++k) {
        const size_t row = rows[k];
        ImGui::PushID(k);
        DragLabel("##tag", axes[k], colours[k], tag, row, speed, true);
        if (ImGui::IsItemHovered()) hoverHelp_ = shown_.rows[row].description;
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(std::max(10.0f, width - tag));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, ImGui::GetStyle().FramePadding.y));
        ValueInput(row, true);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) hoverHelp_ = shown_.rows[row].description;
        ImGui::PopID();
        if (k < 2) ImGui::SameLine(0.0f, spacing);
    }
    ImGui::PopID();
}

void PropertiesPanel::Draw(IDirect3DDevice9* device) {
    const std::string title = std::string(ImGuiLayer::kIconSliders) + "  Properties###Properties";
    if (!ImGui::Begin(title.c_str())) {
        ImGui::End();
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    hoverHelp_.clear();

    // --- heading and the grouping switch -------------------------------------
    ImGui::PushFont(ImGuiLayer::BoldFont());
    const std::string heading = shown_.title.empty() ? std::string("Nothing selected") : Utf8(shown_.title);
    const float switches = ImGui::GetFrameHeight() * 2 + style.ItemSpacing.x;
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - switches - style.ItemSpacing.x);
    ImGui::TextUnformatted(heading.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - switches);
    const auto toggle = [&](const char* icon, bool on, const char* tip) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_HeaderActive]);
        const bool clicked = ImGui::Button(icon, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
    if (toggle(ImGuiLayer::kIconList, !sorted_, "By group")) sorted_ = false;
    ImGui::SameLine();
    if (toggle(ImGuiLayer::kIconSort, sorted_, "By name")) sorted_ = true;
    ImGui::Separator();

    // --- the footer's room: description and buttons -------------------------------
    // The buttons go two to a row, the extension's own under a heading per
    // group (shown in the order they first appear).
    const float buttonHeight = ImGui::GetFrameHeight();
    const float helpHeight = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    std::vector<std::wstring> groups;
    for (const View::Action& a : shown_.actions)
        if (std::find(groups.begin(), groups.end(), a.group) == groups.end()) groups.push_back(a.group);
    const bool ownButtons = shown_.mode == Mode::Extension && !shown_.actions.empty();
    float buttonRows = 2.0f;
    if (ownButtons) {
        buttonRows = 1.0f;
        for (const std::wstring& g : groups) {
            size_t n = 0;
            for (const View::Action& a : shown_.actions) n += a.group == g;
            buttonRows += (float)((n + 1) / 2) + (g.empty() ? 0.0f : 1.0f);
        }
    }
    const float footer = helpHeight + buttonRows * (buttonHeight + style.ItemSpacing.y) + style.ItemSpacing.y * 4;

    // --- the rows ------------------------------------------------------------------
    if (ImGui::BeginChild("##rows", ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y - footer)))) {
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        // Where the transform rows are: they draw as Position and Rotation
        // lines of three, and Scale on its own.
        size_t fieldRow[8];
        bool haveField[8] = {};
        for (size_t i = 0; i < shown_.rows.size(); ++i) {
            const int field = shown_.rows[i].field;
            if (field >= 1 && field <= 7) { fieldRow[field] = i; haveField[field] = true; }
        }
        const bool position = haveField[1] && haveField[2] && haveField[3];
        const bool rotation = haveField[4] && haveField[5] && haveField[6];
        static const char* const kXyz[3] = { "X", "Y", "Z" };
        static const char* const kTurn[3] = { "H", "P", "R" };   // heading (about Z), pitch (Y), roll (X)
        static const unsigned kXyzColours[3] = { IM_COL32(196, 64, 64, 255), IM_COL32(72, 160, 72, 255),
                                                  IM_COL32(64, 112, 210, 255) };
        static const unsigned kTurnColours[3] = { IM_COL32(64, 112, 210, 255), IM_COL32(72, 160, 72, 255),
                                                  IM_COL32(196, 64, 64, 255) };

        const auto table = [&](const char* id, const std::vector<size_t>& rows) {
            if (!ImGui::BeginTable(id, 2, tableFlags)) return;
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.27f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.73f);
            for (size_t index : rows) {
                const int field = shown_.rows[index].field;
                if (position && field >= 1 && field <= 3) {
                    if (field == 1) {
                        const size_t three[3] = { fieldRow[1], fieldRow[2], fieldRow[3] };
                        DrawVector("Position", three, kXyz, kXyzColours, 0.05f);
                    }
                    continue;
                }
                if (rotation && field >= 4 && field <= 6) {
                    if (field == 4) {
                        const size_t three[3] = { fieldRow[4], fieldRow[5], fieldRow[6] };
                        DrawVector("Rotation", three, kTurn, kTurnColours, 0.5f);
                    }
                    continue;
                }
                DrawRow(index, device);
            }
            ImGui::EndTable();
        };
        if (shown_.rows.empty()) {
            ImGui::TextDisabled("Select something in the view.");
        } else if (sorted_) {
            std::vector<size_t> order(shown_.rows.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            // The transform keeps its own order, first.
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const int fa = shown_.rows[a].field, fb = shown_.rows[b].field;
                const bool ta = fa >= 1 && fa <= 7, tb = fb >= 1 && fb <= 7;
                if (ta || tb) return ta && tb ? fa < fb : ta;
                return ::_wcsicmp(shown_.rows[a].name.c_str(), shown_.rows[b].name.c_str()) < 0;
            });
            table("##sorted", order);
        } else {
            std::vector<std::wstring> groups;
            std::map<std::wstring, std::vector<size_t>> members;
            for (size_t i = 0; i < shown_.rows.size(); ++i) {
                const std::wstring& group = shown_.rows[i].group;
                if (!members.count(group)) groups.push_back(group);
                members[group].push_back(i);
            }
            for (const std::wstring& group : groups) {
                const std::string label = Utf8(group.empty() ? std::wstring(L"Properties") : group);
                if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    table(("##" + label).c_str(), members[group]);
            }
        }
    }
    ImGui::EndChild();

    // --- what the row under the pointer means ------------------------------------
    if (ImGui::BeginChild("##help", ImVec2(0, helpHeight), ImGuiChildFlags_None)) {
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", Utf8(hoverHelp_).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // --- actions ---------------------------------------------------------------------
    const Mode mode = shown_.mode;
    const bool extension = mode == Mode::Extension;
    const bool mesh = mode == Mode::Placement || mode == Mode::Native || mode == Mode::Library || mode == Mode::Several;
    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    // A button half the width; every second one goes beside the one before.
    int column = 0;
    const auto button = [&](const std::string& text, bool enabled, int id, const std::wstring& help) {
        if (column == 1) ImGui::SameLine();
        ImGui::BeginDisabled(!enabled);
        ImGui::PushID(id);
        if (ImGui::Button(text.c_str(), ImVec2(half, buttonHeight))) Post(id);
        ImGui::PopID();
        ImGui::EndDisabled();
        if (!help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) hoverHelp_ = help;
        column ^= 1;
    };
    const auto icon = [](const char* glyph, const std::string& label) { return std::string(glyph) + "  " + label; };
    ImGui::Spacing();
    button(icon(ImGuiLayer::kIconDown,
                extension && !shown_.moveLabel.empty() ? Utf8(shown_.moveLabel) : std::string("Snap to ground")),
           mode == Mode::Placement || (extension && !shown_.moveLabel.empty()), IDC_PROP_SNAP, L"");
    const bool canRemove = mode == Mode::Placement || mode == Mode::Native || mode == Mode::Several ||
                           (extension && !shown_.removeLabel.empty());
    button(icon(ImGuiLayer::kIconDelete,
                mode == Mode::Native ? std::string("Remove from the world")
                : extension && !shown_.removeLabel.empty() ? Utf8(shown_.removeLabel) : std::string("Delete")),
           canRemove && !shown_.removed, IDC_PROP_REMOVE, L"");
    if (ownButtons) {
        // The extension's own, in place of the mesh buttons, which do not
        // apply to what it shows.
        for (const std::wstring& group : groups) {
            column = 0;
            if (!group.empty()) ImGui::SeparatorText(Utf8(group).c_str());
            for (const View::Action& a : shown_.actions)
                if (a.group == group) button(Utf8(a.label), true, a.command, a.help);
        }
    } else {
        button(icon(ImGuiLayer::kIconEdit, "Edit in Blender"), mesh, IDC_PROP_BLENDER, L"");
        button(icon(ImGuiLayer::kIconShield, "Make collision"), mesh, IDC_PROP_COLLISION, L"");
    }

    ImGui::End();
}

bool PropertiesPanel::ReadText(int field, std::wstring& text) const {
    for (size_t i = 0; i < shown_.rows.size(); ++i)
        if (shown_.rows[i].field == field) {
            text = values_[i];
            return true;
        }
    return false;
}

bool PropertiesPanel::ReadNumber(int field, float& value) const {
    std::wstring text;
    if (!ReadText(field, text)) return false;
    for (wchar_t& c : text) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    value = std::wcstof(text.c_str(), &end);
    return end != text.c_str();
}

bool PropertiesPanel::Read(Mat4& transform) const {
    float values[8] = {};
    for (int field = 1; field <= 7; ++field)
        if (!ReadNumber(field, values[field])) return false;
    if (values[7] <= 0.01f || values[7] > 3.9f) return false;
    transform = nfs::ComposePlacement({ values[1], values[2], values[3] }, values[4], values[5],
                                      values[6], values[7]);
    return true;
}

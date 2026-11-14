// Markers: the shops and race starts of the region, and the ones placed from
// the Library -- working copies of the game's shops and garage, and race
// starts the chosen career races are moved to -- handed to the game through
// <game>\models\files (io/MarkerWrite) and the loader (loader/src/Markers.cpp).

#include "ui/MarkersExtension.h"

#include "core/Log.h"
#include "io/MarkerEffects.h"
#include "io/MarkerWrite.h"
#include "io/SectionEdits.h"
#include "nfs/Markers.h"
#include "nfs/TextureDecode.h"
#include "nfs/World.h"
#include "render/CollisionMesh.h"
#include "render/ModelThumbnails.h"
#include "render/RenderExtension.h"
#include "render/WorldRenderer.h"
#include "ui/MainWindow.h"
#include "ui/Resource.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

// What the markers reach in the editor's window; MainWindow names it a friend.
struct MarkersAccess {
    explicit MarkersAccess(MainWindow& window) : w(window) {}
    MainWindow& w;

    HWND Window() const { return w.window_; }
    HMENU Menu() const { return w.menu_; }
    HACCEL& Accelerators() const { return w.accelerators_; }
    Viewport& View() const { return w.viewport_; }
    WorldRenderer& Renderer() const { return w.viewport_.GetRenderer(); }
    PropertiesPanel& Properties() const { return w.properties_; }
    ModelCatalog& Catalog() const { return w.catalog_; }
    nfs::World* World() const { return w.world_.get(); }
    const std::wstring& GameFolder() const { return w.gameFolder_; }
    const std::wstring& ModelsFolder() const { return w.modelsFolder_; }
    void Status(const std::wstring& text) const { w.SetStatusText(2, text); }
    void RebuildTree() const { w.PopulateSectionTree(); }
    void UpdateMenuChecks() const { w.UpdateMenuChecks(); }
    void UpdateProperties() const { w.UpdateProperties(); }
    bool ViewCentre(Vec3& point) const { return w.ViewCentrePoint(point); }
    bool ConfirmUnsaved() const { return w.ConfirmUnsavedPlacements(); }
    void ShowLibrary() const { w.ShowSideTab(1); }
    void ReopenRegion() const {
        if (w.world_) w.OpenRegion(w.world_->Region());
    }
    void Message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) const {
        ::MessageBoxW(w.window_, text.c_str(), L"FRS World Editor - Markers", icon | MB_OK);
    }
};

namespace {

constexpr int ID_MARKERS_FIRST     = ID_EXTENSION_FIRST + 200;
constexpr int ID_VIEW_MARKERS      = ID_MARKERS_FIRST + 0;
constexpr int ID_MARKER_LIBRARY    = ID_MARKERS_FIRST + 1;
constexpr int ID_MARKER_DELETE     = ID_MARKERS_FIRST + 2;
constexpr int ID_MARKER_TO_VIEW    = ID_MARKERS_FIRST + 3;
constexpr int ID_MARKER_WRITE      = ID_MARKERS_FIRST + 4;
constexpr int ID_MARKER_PICK_RACES = ID_MARKERS_FIRST + 5;

std::wstring FormatText(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring WidenText(const std::string& text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)needed, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), needed);
    return out;
}

std::wstring NumberText(float value) {
    wchar_t number[32];
    ::swprintf_s(number, L"%.3f", value);
    return number;
}

void AddField(PropertiesPanel::View& view, const wchar_t* group, const wchar_t* name, std::wstring value,
              const wchar_t* description, int id, bool editable = true) {
    PropertiesPanel::Row row;
    row.group = group;
    row.name = name;
    row.value = std::move(value);
    row.description = description;
    row.editable = editable;
    row.field = id;
    view.rows.push_back(std::move(row));
}

void AddInfo(PropertiesPanel::View& view, const wchar_t* group, const wchar_t* name, std::wstring value,
             const wchar_t* description) {
    AddField(view, group, name, std::move(value), description, 0, false);
}

// The submenu that holds `id`, the innermost one.
HMENU SubmenuHolding(HMENU menu, UINT id) {
    for (int i = 0; i < ::GetMenuItemCount(menu); ++i) {
        HMENU sub = ::GetSubMenu(menu, i);
        if (!sub) continue;
        if (::GetMenuState(sub, id, MF_BYCOMMAND) != (UINT)-1) {
            HMENU inner = SubmenuHolding(sub, id);
            return inner ? inner : sub;
        }
    }
    return nullptr;
}

// The Library shelf the placeable markers sit on.
const wchar_t kShelf[] = L"Markers";

u32 BinHash(const char* text) {
    u32 h = 0xFFFFFFFF;
    for (; *text; ++text) h = h * 33 + (u8)*text;
    return h;
}

// A kind's picture for the Library: the game's own icon for it, fitted into
// the square on the catalogue's dark ground, or a diamond of the kind's
// colour when the region has not loaded the pack it is in. A race start's is
// the minimap's white circuit sign, so it is drawn in the race colour.
bool DrawKindPicture(const nfs::World* world, nfs::MarkerKind kind, std::vector<u32>& out) {
    const u32 size = ModelThumbnails::kSize;
    const u32 ground = 0xFF26282C;
    const u32 rgb = nfs::MarkerKindColour(kind);
    out.assign((size_t)size * size, ground);

    std::vector<u32> pixels;
    u32 width = 0, height = 0;
    const char* icon = nfs::MarkerKindIcon(kind);
    const nfs::Texture* texture = world && icon ? world->FindDisplayTexture(BinHash(icon)) : nullptr;
    if (texture && nfs::DecodeTexture(*texture, 0, pixels, width, height) && width && height) {
        const bool tint = kind == nfs::MarkerKind::RaceStart;
        // Some of the game's pictures carry no alpha at all; those are opaque.
        bool anyAlpha = false;
        for (u32 p : pixels) anyAlpha |= (p >> 24) != 0;
        if (!anyAlpha)
            for (u32& p : pixels) p |= 0xFF000000u;
        // Fitted with a margin, keeping its shape; sampled bilinearly.
        const float room = size * 0.84f;
        const float scale = std::min(room / width, room / height);
        const float w = width * scale, h = height * scale;
        const float x0 = (size - w) * 0.5f, y0 = (size - h) * 0.5f;
        const auto texel = [&](int x, int y) {
            x = std::clamp(x, 0, (int)width - 1);
            y = std::clamp(y, 0, (int)height - 1);
            return pixels[(size_t)y * width + (size_t)x];
        };
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) {
                const float u = (x + 0.5f - x0) / scale - 0.5f, v = (y + 0.5f - y0) / scale - 0.5f;
                if (u < -0.5f || v < -0.5f || u > width - 0.5f || v > height - 0.5f) continue;
                const int iu = (int)std::floor(u), iv = (int)std::floor(v);
                const float fu = u - iu, fv = v - iv;
                float channel[4] = {};
                const u32 corners[4] = { texel(iu, iv), texel(iu + 1, iv), texel(iu, iv + 1), texel(iu + 1, iv + 1) };
                const float weights[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 4; ++c) channel[c] += weights[k] * ((corners[k] >> (8 * c)) & 0xFF);
                float b = channel[0], g = channel[1], r = channel[2];
                const float a = channel[3] / 255.0f;
                if (tint) {
                    b *= (rgb & 0xFF) / 255.0f;
                    g *= ((rgb >> 8) & 0xFF) / 255.0f;
                    r *= ((rgb >> 16) & 0xFF) / 255.0f;
                }
                u32& dst = out[(size_t)y * size + x];
                const auto mixed = [&](float source, int shift) {
                    return (u32)std::lround(source * a + ((dst >> shift) & 0xFF) * (1.0f - a)) << shift;
                };
                dst = 0xFF000000u | mixed(r, 16) | mixed(g, 8) | mixed(b, 0);
            }
        return true;
    }

    // A diamond in the kind's colour.
    const float centre = size * 0.5f, reach = size * 0.34f;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const float d = std::fabs(x + 0.5f - centre) + std::fabs(y + 0.5f - centre);
            if (d <= reach) out[(size_t)y * size + x] = 0xFF000000u | (d > reach - 3.0f ? 0x101010u : rgb);
        }
    return true;
}

// Marker rows past the transform's seven.
constexpr int kMarkerFieldName   = 20;
constexpr int kMarkerFieldRadius = 21;
constexpr int kMarkerFieldOpens  = 24;
constexpr int kMarkerFieldDoorForward = 31;
constexpr int kMarkerFieldDoorRight   = 32;
constexpr int kMarkerFieldDoorUp      = 33;
constexpr int kMarkerFieldDoorTurn    = 34;

// A beacon's shape, in the marker's own frame: a glowing column standing on
// the zone, an icon floating over it, and the zone's footprint on the ground.
// The column is what the game's shop glow looks like from down the street, so
// it is what makes a marker findable here too.
constexpr float kColumnRadius = 2.5f;
constexpr float kColumnHeight = 28.0f;
constexpr float kIconHeight   = 7.0f;
constexpr float kIconSize     = 2.2f;
constexpr float kGroundLift   = 0.25f;
constexpr int   kColumnSides  = 16;

// The box a click has to land in, a little wider than the column so a thin
// beacon far away can still be hit.
Aabb MarkerPickBox(const nfs::Marker& marker) {
    Aabb box;
    const Vec3& p = marker.position;
    box.Add({ p.x - kColumnRadius * 1.6f, p.y - kColumnRadius * 1.6f, p.z - 0.5f });
    box.Add({ p.x + kColumnRadius * 1.6f, p.y + kColumnRadius * 1.6f, p.z + kColumnHeight });
    return box;
}

D3DCOLOR Argb(u32 rgb, u32 alpha) { return (D3DCOLOR)((alpha << 24) | (rgb & 0xFFFFFF)); }

class MarkersExtension : public EditorExtension, public RenderExtension {
public:
    explicit MarkersExtension(MainWindow& window) : app_(window) {}

    void OnCreate() override {
        app_.Renderer().AddExtension(this);
        AddMenus();
    }

    bool OnCommand(int id) override {
        switch (id) {
            case ID_VIEW_MARKERS:
                visible_ = !visible_;
                if (!visible_ && Selected() >= 0) app_.Renderer().ClearSelection();
                app_.UpdateMenuChecks();
                return true;
            case ID_MARKER_PICK_RACES: PickRaces(); return true;
            case ID_MARKER_LIBRARY:
                app_.ShowLibrary();
                app_.Catalog().ShowShelf(kShelf);
                app_.Status(L"Drag a marker onto the map, or double-click one to place it at the view centre");
                return true;
            case ID_MARKER_DELETE:   DeleteSelection(); return true;
            case ID_MARKER_TO_VIEW:  MoveSelectionToView(); return true;
            case ID_MARKER_WRITE:    WriteMarkersToGame(false); return true;
            default:                 return false;
        }
    }

    void UpdateMenuChecks(HMENU menu) override {
        ::CheckMenuItem(menu, ID_VIEW_MARKERS, MF_BYCOMMAND | (visible_ ? MF_CHECKED : MF_UNCHECKED));
    }

    void OnRegionLoaded() override {
        nfs::World* world = app_.World();
        if (!world) return;
        markers_.Load(app_.GameFolder(), world->Region());
        markers_.PlaceOnGround(*world);
        RefreshPreviews();
        app_.Catalog().Refresh();

        // FRS_FOCUS_MARKER=<name> opens on that marker, so a check of how one is
        // drawn needs no flying about.
        if (const char* name = std::getenv("FRS_FOCUS_MARKER"))
            for (size_t i = 0; i < markers_.Markers().size(); ++i)
                if (markers_.Markers()[i].name == name) Focus(i);
    }

    void OnRegionClosed() override {
        markers_.Clear();
        app_.Catalog().Refresh();
    }

    // ---- the Library: the game's beacons, to place

    void CatalogRows(std::vector<ModelCatalog::Row>& rows) override {
        // Only where the region has a free-roam graph for them to go in.
        if (markers_.Zones().empty()) return;
        for (nfs::MarkerKind kind : nfs::kPlaceableKinds) {
            const nfs::Marker* shop = kind == nfs::MarkerKind::RaceStart ? nullptr : markers_.FirstShopOfKind(kind);
            if (kind != nfs::MarkerKind::RaceStart && !shop) continue;   // nothing of the kind to copy
            ModelCatalog::Row row;
            row.name = nfs::MarkerKindName(kind);
            row.home = shop ? L"a copy of " + WidenText(shop->name) : L"the career's races";
            row.group = kShelf;
            row.owner = static_cast<EditorExtension*>(this);
            row.key = std::to_string((int)kind);
            const MarkersExtension* self = this;
            row.picture = [self, kind](std::vector<u32>& bgra) {
                return DrawKindPicture(self->app_.World(), kind, bgra);
            };
            rows.push_back(std::move(row));
        }
    }

    void ActivateRow(const ModelCatalog::Row& row) override {
        Vec3 point;
        if (!app_.World() || !app_.ViewCentre(point)) {
            app_.Status(L"Point the middle of the view at the street where the marker should stand.");
            return;
        }
        const int index = Place(KindOfRow(row), point);
        if (index >= 0) Placed(index);
    }

    bool DescribeRow(const ModelCatalog::Row& row, std::wstring& text) override {
        const nfs::MarkerKind kind = KindOfRow(row);
        text = kind == nfs::MarkerKind::RaceStart
                   ? std::wstring(L"Race start: choose in Properties the career races that start here -- drag "
                                  L"onto the map or double-click to place it at the view centre")
                   : FormatText(L"%s: a working shop, %s -- drag onto the map or double-click to place it at "
                                L"the view centre", row.name.c_str(), row.home.c_str());
        return true;
    }

    bool BeginDrag(const ModelCatalog::Row& row) override {
        dragKind_ = KindOfRow(row);
        dragIndex_ = -1;
        return true;
    }

    // Made as the pointer enters the view, standing on the ground under it,
    // facing the camera.
    void UpdateDrag(const Vec3& ground) override {
        if (dragIndex_ < 0) dragIndex_ = Place(dragKind_, ground);
        else if (nfs::Marker* marker = markers_.Mutable((size_t)dragIndex_)) marker->position = ground;
        if (dragIndex_ >= 0) Select(dragIndex_);
        RefreshPreviews();
    }

    void EndDrag(const Vec3* ground) override {
        const int index = dragIndex_;
        dragIndex_ = -1;
        if (index < 0) return;
        if (ground) {
            if (nfs::Marker* marker = markers_.Mutable((size_t)index)) marker->position = *ground;
            Placed(index);
        } else {
            markers_.RemovePlaced((size_t)index);
            app_.Renderer().ClearSelection();
            SavePlacedMarkers();
            app_.Status(L"Placement cancelled");
        }
    }

    void Save() override { SavePlacedMarkers(); }

    // Testing in the game is testing what is placed: the markers go into the
    // game's files on every launch, without asking or reopening the region.
    void OnBeforeLaunch() override {
        if (!app_.World() || markers_.Zones().empty()) return;
        bool written = markers_.PlacedCount() > 0;
        for (const std::wstring& file : io::MarkerFiles(app_.GameFolder(), app_.World()->Region()))
            written = written || ::GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (written) WriteMarkersToGame(true);
    }

    size_t SceneSignature() const override { return markers_.Markers().size(); }

    void PopulateTree(SceneTreeBuilder& tree) override {
        // Placed first, then the shops, then the race starts.
        if (markers_.Markers().empty()) return;
        HTREEITEM root = tree.insert(TVI_ROOT, FormatText(L"Markers (%zu)", markers_.Markers().size()), -1);
        size_t shown = 0;
        const auto& markers = markers_.Markers();
        for (int pass = 0; pass < 3; ++pass) {
            for (size_t i = 0; i < markers.size(); ++i) {
                const nfs::Marker& marker = markers[i];
                const int group = marker.IsPlaced() ? 0 : marker.kind == nfs::MarkerKind::RaceStart ? 2 : 1;
                if (group != pass) continue;
                std::wstring text = FormatText(L"%s  -  %s", WidenText(marker.name).c_str(),
                                               nfs::MarkerKindName(marker.GameKind()));
                if (marker.IsPlaced()) text += L" (placed)";
                if (!tree.matches(text)) continue;
                tree.insert(root, text, (intptr_t)i);
                ++shown;
            }
        }
        tree.finish(root, shown, false);
    }

    void OnTreeItem(intptr_t code) override { Focus((size_t)code); }

    bool StatusText(std::wstring& text) const override {
        const int index = Selected();
        if (index < 0 || (size_t)index >= markers_.Markers().size()) return false;
        const nfs::Marker& marker = markers_.Markers()[(size_t)index];
        text = FormatText(L"%s %s  at %.1f, %.1f, %.1f%s", nfs::MarkerKindName(marker.GameKind()),
                          WidenText(marker.name).c_str(), marker.position.x, marker.position.y, marker.position.z,
                          marker.IsPlaced() ? L"   [G move  R rotate  Del delete, edit in Properties]" : L"");
        return true;
    }

    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;

    bool DeleteSelection() override {
        const int index = Selected();
        if (index < 0) return false;
        if (!markers_.RemovePlaced((size_t)index)) {
            app_.Status(L"Only markers placed in the editor can be deleted; the game's own stay.");
            return true;
        }
        app_.Renderer().ClearSelection();
        SavePlacedMarkers();
        return true;
    }

    bool MoveSelectionToView() override {
        const int index = Selected();
        if (index < 0) return false;
        nfs::Marker* marker = markers_.Mutable((size_t)index);
        if (!marker || !marker->IsPlaced()) return true;
        Vec3 point;
        if (!app_.ViewCentre(point)) {
            app_.Status(L"There is nothing under the middle of the view.");
            return true;
        }
        marker->position = point;
        markers_.MarkPlacedChanged();
        SavePlacedMarkers();
        return true;
    }

    bool GetPoint(Vec3& position, float& heading) override {
        const int index = Selected();
        if (index < 0 || (size_t)index >= markers_.Markers().size() ||
            !markers_.Markers()[(size_t)index].IsPlaced())
            return false;
        position = markers_.Markers()[(size_t)index].position;
        heading = markers_.Markers()[(size_t)index].heading;
        return true;
    }

    bool SetPoint(const Vec3& position, float heading, bool commit) override {
        const int index = Selected();
        nfs::Marker* marker = index >= 0 ? markers_.Mutable((size_t)index) : nullptr;
        if (!marker) return false;
        marker->position = position;
        marker->heading = heading;
        RefreshPreviews();   // the door and the effects follow as it moves
        if (commit) {
            markers_.MarkPlacedChanged();
            SavePlacedMarkers();
        }
        return true;
    }

    // ---- RenderExtension

    void DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) override;

    bool Pick(const Ray& ray, int& index, float& distance) const override {
        if (!visible_) return false;
        index = -1;
        const auto& markers = markers_.Markers();
        for (size_t i = 0; i < markers.size(); ++i) {
            const float hit = IntersectRayAabb(ray, MarkerPickBox(markers[i]));
            if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
            index = (int)i;
            distance = hit;
        }
        return index >= 0;
    }

    // A beacon wins when nothing solid stands in front of it: markers are
    // drawn over the world, and one under the cursor is what is aimed at.
    float PickTolerance() const override { return 0.0f; }

private:
    int Selected() const { return app_.Renderer().ExtensionSelection(this); }
    void Select(int index) { app_.Renderer().SelectExtension(this, index); }

    void Focus(size_t index) {
        if (index >= markers_.Markers().size()) return;
        const Vec3& p = markers_.Markers()[index].position;
        // Enough of the street around it to see where it stands.
        Aabb around;
        around.Add({ p.x - 30.0f, p.y - 30.0f, p.z });
        around.Add({ p.x + 30.0f, p.y + 30.0f, p.z + 30.0f });
        app_.View().FrameBounds(around);
        Select((int)index);
    }

    // View > Show > Markers, the Markers menu before Help, and their keys.
    void AddMenus() {
        HMENU menu = app_.Menu();
        if (!menu) return;
        if (HMENU show = SubmenuHolding(menu, ID_VIEW_COLLISION)) {
            ::AppendMenuW(show, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(show, MF_STRING, ID_VIEW_MARKERS, L"&Markers\tCtrl+M");
        }
        HMENU markers = ::CreatePopupMenu();
        ::AppendMenuW(markers, MF_STRING, ID_MARKER_LIBRARY, L"&Place a Marker (Library)\tCtrl+Shift+M");
        ::AppendMenuW(markers, MF_STRING, ID_MARKER_TO_VIEW, L"&Move Selected Marker to View Centre");
        ::AppendMenuW(markers, MF_STRING, ID_MARKER_DELETE, L"&Delete Selected Marker");
        ::AppendMenuW(markers, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(markers, MF_STRING, ID_MARKER_WRITE, L"&Write Markers to Game");
        const UINT help = (UINT)std::max(0, ::GetMenuItemCount(menu) - 1);
        ::InsertMenuW(menu, help, MF_BYPOSITION | MF_POPUP, (UINT_PTR)markers, L"&Markers");
        ::DrawMenuBar(app_.Window());

        HACCEL& table = app_.Accelerators();
        const int count = table ? ::CopyAcceleratorTableW(table, nullptr, 0) : 0;
        std::vector<ACCEL> keys((size_t)count);
        if (count) ::CopyAcceleratorTableW(table, keys.data(), count);
        keys.push_back({ FVIRTKEY | FCONTROL, 'M', (WORD)ID_VIEW_MARKERS });
        keys.push_back({ FVIRTKEY | FCONTROL | FSHIFT, 'M', (WORD)ID_MARKER_LIBRARY });
        HACCEL replaced = ::CreateAcceleratorTableW(keys.data(), (int)keys.size());
        if (!replaced) return;
        if (table) ::DestroyAcceleratorTable(table);
        table = replaced;
    }

    void SavePlacedMarkers() {
        RefreshPreviews();
        if (!markers_.PlacedChanged()) return;
        if (!markers_.SavePlaced())
            app_.Message(L"Could not write " + markers_.PlacedPath(), MB_ICONWARNING);
        else
            app_.Status(FormatText(L"%zu placed markers saved -- Markers > Write Markers to Game "
                                   L"hands them to the game", markers_.PlacedCount()));
        // Rebuilding the tree can move its caret, which would re-frame the
        // camera; keep the marker that was being edited.
        const int selected = Selected();
        app_.RebuildTree();
        if (selected >= 0) Select(selected);
    }

    // What the placed shops will look like in the game, worked out by the
    // same code that writes them: the garage door, drawn by the renderer as
    // the region's own doors are (nfs::World::SetPreviewAnimations), and where
    // the icon, ring and smoke emitters stand, drawn in DrawOverlay.
    void RefreshPreviews() {
        nfs::World* world = app_.World();
        effects_.assign(markers_.Markers().size(), {});
        if (!world) return;
        const auto& shipped = world->ObjectAnimations().Placements();
        std::vector<nfs::WorldAnimationPlacement> doors;
        for (size_t i = 0; i < markers_.Markers().size(); ++i) {
            const nfs::Marker& marker = markers_.Markers()[i];
            if (!marker.IsPlaced() || marker.IsRaceStart()) continue;
            effects_[i] = PreviewMarkerEffects(*world, markers_, marker);
            const nfs::Marker* shop = markers_.FindShop(marker.opens);
            if (!shop) continue;
            for (const nfs::WorldAnimationPlacement& placement : shipped) {
                if (placement.key != shop->hash) continue;
                nfs::WorldAnimationPlacement door = placement;
                door.matrix = nfs::PlacedDoorMatrix(placement.matrix, *shop, marker);
                const size_t section = world->SectionForPlacement(marker.position);
                if (section < world->Loaded().size() && world->Loaded()[section]->info)
                    door.section = (int)world->Loaded()[section]->info->number;
                doors.push_back(door);
                break;
            }
        }
        world->SetPreviewAnimations(std::move(doors));
    }
    std::vector<std::vector<MarkerEffectPreview>> effects_;   // by marker

    // `quiet`: from a launch -- no questions, no reopening, the status line
    // and the log say how it went.
    void WriteMarkersToGame(bool quiet);

    // The race browser, picking the selected race start's races.
    void PickRaces() {
        const int index = Selected();
        const nfs::Marker* marker = index >= 0 ? &markers_.Markers()[(size_t)index] : nullptr;
        if (!marker || !marker->IsPlaced() || !marker->IsRaceStart()) return;
        // By name: the selection may move while the list is open.
        pickFor_ = marker->name;
        picked_ = marker->races;
        pickSearch_[0] = 0;
        picking_ = true;
    }

    // The race picker: every race of the career, ticked to start here.
    void DrawHud(float x, float y, float width, float height) override {
        (void)x; (void)y; (void)width; (void)height;
        if (!picking_) return;
        ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
        bool open = true;
        const std::string title = "Races starting at " + pickFor_ + "###markerraces";
        if (!ImGui::Begin(title.c_str(), &open)) {
            ImGui::End();
            if (!open) picking_ = false;
            return;
        }
        ImGui::TextWrapped("Tick the career races that start here. Each is moved from its own start to this one "
                           "when the markers are written. The game offers a race only once the save has reached "
                           "its stage (S1_ to S5_); the prologue's (STAGE_) are never offered in free roam.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##racesearch", "Search", pickSearch_, sizeof(pickSearch_));
        std::string query = pickSearch_;
        for (char& c : query) c = (char)std::toupper((unsigned char)c);

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##racelist", ImVec2(0, -footer), true);
        for (const std::string& race : markers_.CareerRaces()) {
            if (!query.empty() && race.find(query) == std::string::npos) continue;
            bool on = std::find(picked_.begin(), picked_.end(), race) != picked_.end();
            const bool prologue = race.rfind("STAGE_", 0) == 0;
            const std::string label = prologue ? race + "   (prologue)" : race;
            if (prologue) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Checkbox(label.c_str(), &on)) {
                if (on) picked_.push_back(race);
                else picked_.erase(std::remove(picked_.begin(), picked_.end(), race), picked_.end());
            }
            if (prologue) ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        const bool done = ImGui::Button("Done", ImVec2(90, 0));
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(90, 0));
        ImGui::SameLine();
        ImGui::TextDisabled("%zu chosen", picked_.size());
        ImGui::End();
        if (done) SetRaces(pickFor_, picked_);
        if (done || cancel || !open) picking_ = false;
    }

    // Known races only, each once; one taken from another placed start
    // leaves it, since a race starts in one place.
    void SetRaces(const std::string& name, const std::vector<std::string>& chosen) {
        const auto& known = markers_.CareerRaces();
        std::string list;
        for (const std::string& race : chosen) list += race + " ";
        LOG_INFO("markers: races chosen for %s: %s(%zu career races known)", name.c_str(), list.c_str(),
                 known.size());
        std::vector<std::string> races;
        for (const std::string& race : chosen)
            if (std::find(known.begin(), known.end(), race) != known.end() &&
                std::find(races.begin(), races.end(), race) == races.end())
                races.push_back(race);
        if (races.size() != chosen.size())
            LOG_WARN("markers: %zu of the chosen races are not in the career's list and were left out",
                     chosen.size() - races.size());
        int index = -1;
        for (size_t i = 0; i < markers_.Markers().size(); ++i) {
            nfs::Marker* other = markers_.Mutable(i);
            if (!other->IsPlaced() || !other->IsRaceStart()) continue;
            if (other->name == name) { index = (int)i; continue; }
            for (const std::string& race : races)
                other->races.erase(std::remove(other->races.begin(), other->races.end(), race), other->races.end());
        }
        if (index < 0) return;   // deleted while the browser was open
        markers_.Mutable((size_t)index)->races = races;
        markers_.MarkPlacedChanged();
        SavePlacedMarkers();
        Select(index);
        app_.UpdateProperties();
        app_.Status(FormatText(L"%s: %zu race(s) start here", WidenText(name).c_str(), races.size()));
    }

    static nfs::MarkerKind KindOfRow(const ModelCatalog::Row& row) {
        return (nfs::MarkerKind)std::atoi(row.key.c_str());
    }

    // A beacon of `kind` standing at `point`, facing the camera; -1 when
    // there is no region.
    int Place(nfs::MarkerKind kind, const Vec3& point) {
        if (!app_.World()) return -1;
        const float heading = std::fmod(Degrees(app_.View().GetCamera().Yaw()) + 180.0f + 360.0f, 360.0f);
        return (int)markers_.AddPlaced(kind, point, heading);
    }

    void Placed(int index) {
        SavePlacedMarkers();
        Select(index);
        const nfs::Marker& marker = markers_.Markers()[(size_t)index];
        app_.Status(marker.IsRaceStart()
                        ? FormatText(L"%s placed -- choose its races in Properties, then Markers > Write "
                                     L"Markers to Game", WidenText(marker.name).c_str())
                        : FormatText(L"%s placed -- G moves it, R turns it; Markers > Write Markers to Game "
                                     L"hands it to the game", WidenText(marker.name).c_str()));
        LOG_INFO("marker %s placed at (%.2f, %.2f, %.2f)", marker.name.c_str(), marker.position.x,
                 marker.position.y, marker.position.z);
        // A race start is nothing until it has races: the browser opens on it.
        if (marker.IsRaceStart()) PickRaces();
    }

    MarkersAccess app_;
    nfs::MarkerSet markers_;
    bool visible_ = true;
    std::string writtenSignature_;   // the marker file as last written to the game
    bool picking_ = false;           // the race picker is open
    std::string pickFor_;            // for the race start of this name
    std::vector<std::string> picked_;
    char pickSearch_[64] = {};
    nfs::MarkerKind dragKind_ = nfs::MarkerKind::BodyShop;
    int dragIndex_ = -1;
};

bool MarkersExtension::FillProperties(PropertiesPanel::View& view) {
    const int markerIndex = Selected();
    if (markerIndex < 0 || (size_t)markerIndex >= markers_.Markers().size()) return false;
    const nfs::Marker& marker = markers_.Markers()[(size_t)markerIndex];
    const bool placed = marker.IsPlaced();
    view.title = WidenText(marker.name);
    view.moveLabel = placed ? L"Move to view centre" : L"";
    view.removeLabel = placed ? L"Delete marker" : L"";
    const auto field = [&view, placed](const wchar_t* group, const wchar_t* name, std::wstring value,
                                       const wchar_t* description, int id) {
        AddField(view, group, name, std::move(value), description, id, placed && id != 0);
    };
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value,
                             const wchar_t* description) { AddInfo(view, group, name, std::move(value), description); };

    field(L"Marker", L"Name", WidenText(marker.name),
          placed ? L"The placed marker's name: letters, digits and underscores."
                 : L"The name the career data gives it.", kMarkerFieldName);
    add(L"Marker", L"Kind", nfs::MarkerKindName(marker.GameKind()), L"What the game does here.");
    if (placed && marker.IsRaceStart()) {
        const wchar_t* about =
            L"A career race that starts here: it is moved from its own start to this one when the "
            L"markers are written. In the game the marker offers the ones the save has reached -- "
            L"S1_ to S5_ is the career stage a race opens in. Choose races... picks them.";
        if (marker.races.empty()) add(L"Races", L"Race", L"none -- Choose races... (below)", about);
        for (size_t i = 0; i < marker.races.size(); ++i)
            add(L"Races", FormatText(L"Race %zu", i + 1).c_str(), WidenText(marker.races[i]), about);
        view.actions.push_back({ L"Choose races...", ID_MARKER_PICK_RACES, L"Races",
                                 L"Opens the list of the career's races, to tick the ones that start here." });
    } else if (placed) {
        std::wstring shops;
        for (const nfs::Marker& other : markers_.Markers())
            if (!other.IsPlaced() && other.kind == marker.style) {
                if (!shops.empty()) shops += L", ";
                shops += WidenText(other.name);
            }
        field(L"Game", L"Copy of", WidenText(marker.opens),
              (L"The shipped shop this one is a working copy of: what it sells, its prompt and the "
               L"career stage it opens in. One of: " + shops + L".").c_str(),
              kMarkerFieldOpens);
        const wchar_t* door = L"The garage door stands where it stands at the shop copied, turned with the "
                              L"marker; this moves it from there, in units along the marker's facing, to its "
                              L"right and up, or turns it, in degrees. The view shows it as the game will.";
        field(L"Door", L"Forward", NumberText(marker.doorForward), door, kMarkerFieldDoorForward);
        field(L"Door", L"Right", NumberText(marker.doorRight), door, kMarkerFieldDoorRight);
        field(L"Door", L"Up", NumberText(marker.doorUp), door, kMarkerFieldDoorUp);
        field(L"Door", L"Turn", NumberText(marker.doorTurn), door, kMarkerFieldDoorTurn);
    }
    field(L"Zone", L"X", NumberText(marker.position.x), L"Centre of the zone, where the beacon stands.", 1);
    field(L"Zone", L"Y", NumberText(marker.position.y), L"Centre of the zone, where the beacon stands.", 2);
    field(L"Zone", L"Z (height)", NumberText(marker.position.z),
          placed ? L"The zones are flat; this is only where the editor draws it."
                 : L"The zones are flat; the editor stands it on the collision below.", 3);
    field(L"Zone", L"Heading", NumberText(marker.heading), L"The way the zone faces, in degrees.", 4);
    field(L"Zone", L"Radius", NumberText(marker.radius), L"How close the car has to come.", kMarkerFieldRadius);
    if (!placed && marker.zone >= 0 && (size_t)marker.zone < markers_.Zones().size()) {
        const nfs::FreeRoamZone& zone = markers_.Zones()[(size_t)marker.zone];
        add(L"Record", L"Hash", FormatText(L"%08X", zone.hash), L"What the career data names the zone by.");
        add(L"Record", L"Zone type", FormatText(L"0x%02X", zone.type), L"0x0D race trigger, 0x0E shop.");
        add(L"Record", L"Offset", FormatText(L"0x%X", zone.offset), L"Of the record in PathsFreeRoam.bin.");
        add(L"Record", L"Polygon", std::to_wstring(zone.polygon.size()) + L" corners",
            L"The footprint the car has to enter.");
        add(L"Record", L"Cell", FormatText(L"%d, %d", zone.cellX, zone.cellY),
            L"Whole units at +0x34. The centre on race triggers; somewhere else on shops.");
    }
    if (!marker.sceneFile.empty()) {
        add(L"Shop", L"Scene", WidenText(marker.sceneFile), L"The front-end scene the game opens.");
        add(L"Shop", L"Type", FormatText(L"0x%X", marker.shopType), L"+0x50 of the career record.");
        add(L"Shop", L"Stage word", FormatText(L"0x%X", marker.stage),
            L"The record's last word. Its high byte follows the career stage it opens in.");
    }
    for (size_t i = 0; i < marker.races.size(); ++i)
        add(L"Races", FormatText(L"Race %zu", i + 1).c_str(), WidenText(marker.races[i]),
            L"A race that starts from this trigger.");
    return true;
}

bool MarkersExtension::ApplyProperties() {
    const int index = Selected();
    if (index < 0) return false;
    nfs::Marker* marker = markers_.Mutable((size_t)index);
    if (!marker || !marker->IsPlaced()) return true;
    PropertiesPanel& properties = app_.Properties();

    float x, y, z, heading, radius;
    if (properties.ReadNumber(1, x) && properties.ReadNumber(2, y) && properties.ReadNumber(3, z))
        marker->position = { x, y, z };
    if (properties.ReadNumber(4, heading)) marker->heading = heading;
    if (properties.ReadNumber(kMarkerFieldRadius, radius) && radius > 0.5f) marker->radius = radius;
    float door;
    if (properties.ReadNumber(kMarkerFieldDoorForward, door)) marker->doorForward = door;
    if (properties.ReadNumber(kMarkerFieldDoorRight, door)) marker->doorRight = door;
    if (properties.ReadNumber(kMarkerFieldDoorUp, door)) marker->doorUp = door;
    if (properties.ReadNumber(kMarkerFieldDoorTurn, door)) marker->doorTurn = door;

    std::wstring text;
    if (properties.ReadText(kMarkerFieldName, text)) {
        // One token in the file and a plausible game identifier: letters,
        // digits and underscores, upper case.
        std::string name;
        for (wchar_t c : text)
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_') name += (char)c;
            else if (c >= L'a' && c <= L'z') name += (char)(c - L'a' + L'A');
        bool taken = false;
        for (size_t i = 0; i < markers_.Markers().size(); ++i)
            taken |= (int)i != index && markers_.Markers()[i].name == name;
        if (!name.empty() && !taken) marker->name = name;
    }
    if (properties.ReadText(kMarkerFieldOpens, text)) {
        std::string name;
        for (wchar_t c : text) name += (char)::towupper(c);
        // A placed shop stays a shop of the kind it was placed as.
        const nfs::Marker* shop = markers_.FindShop(name);
        if (shop && shop->kind == marker->style) marker->opens = name;
    }
    markers_.MarkPlacedChanged();
    SavePlacedMarkers();
    return true;
}

void MarkersExtension::WriteMarkersToGame(bool quiet) {
    nfs::World* world = app_.World();
    if (!world) return;
    if (!quiet && !app_.ConfirmUnsaved()) return;
    markers_.MarkPlacedChanged();
    SavePlacedMarkers();

    // A launch with nothing changed since the last write leaves the files as
    // they are, and the region open.
    std::string signature;
    for (wchar_t c : world->Region()) signature += (char)c;   // L4RA: plain ASCII
    signature += '\n';
    if (FILE* file = _wfopen(markers_.PlacedPath().c_str(), L"rb")) {
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) signature.append(chunk, got);
        std::fclose(file);
    }
    if (quiet && signature == writtenSignature_) return;
    writtenSignature_ = signature;

    std::wstring report, message;
    bool ok = true;

    // Shops copied from a template: the career, zones, positions and doors.
    if (io::WriteMarkersToGame(app_.GameFolder(), world->Region(), markers_, message)) report += message + L"\n";
    else { report += L"Shops: " + message + L"\n"; ok = false; }

    // The icon, ring and smoke, in the sections the markers stand in.
    MarkerEffectsPlan effects;
    std::wstring error;
    if (!PlanMarkerEffects(*world, app_.ModelsFolder(), markers_, effects, error)) {
        report += L"Effects: " + error + L"\n";
        ok = false;
    } else if (effects.sections) {
        const size_t written = WriteSectionSpans(effects.spans, effects.superseded, app_.ModelsFolder(),
                                                 world->Region());
        if (written != effects.spans.size()) {
            report += L"Effects: some section overlays could not be written; the log has why.\n";
            ok = false;
        }
        report += FormatText(L"%zu effects in %zu sections\n", effects.effects, effects.sections);
    }
    report += effects.report;

    LOG_INFO("markers written to the game:\n%S", report.c_str());

    // Reopened so the editor reads back what the game will -- the effects are
    // section overlays, and a later save from the old state would drop them.
    Camera& camera = app_.View().GetCamera();
    const Vec3 position = camera.Position();
    const float yaw = camera.Yaw(), pitch = camera.Pitch();
    app_.ReopenRegion();
    camera.SetPosition(position);
    camera.SetYawPitch(yaw, pitch);

    if (!quiet) app_.Message(report, ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    else if (ok) app_.Status(L"Markers written to the game for this launch");
    else app_.Message(L"The markers could not all be written for this launch:\n\n" + report, MB_ICONWARNING);
}

void MarkersExtension::DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
    if (!visible_ || markers_.Markers().empty()) return;

    struct Vertex { float x, y, z; D3DCOLOR colour; };
    std::vector<Vertex> glow, solid, lines;

    const int selectedMarker = Selected();
    const Vec3 eye = camera.Position();
    const auto& markers = markers_.Markers();
    for (size_t i = 0; i < markers.size(); ++i) {
        const nfs::Marker& marker = markers[i];
        const bool selected = (int)i == selectedMarker;
        const u32 rgb = marker.DrawColour();
        const Vec3& p = marker.position;

        // The column: brightest at the foot, fading out upwards. Additive, so
        // the colour glows over the night city instead of painting it.
        const float height = selected ? kColumnHeight * 1.4f : kColumnHeight;
        const D3DCOLOR foot = Argb(rgb, selected ? 0xF0 : 0xB0);
        const D3DCOLOR top  = Argb(rgb, 0x00);
        for (int s = 0; s < kColumnSides; ++s) {
            const float a0 = 2.0f * kPi * s / kColumnSides;
            const float a1 = 2.0f * kPi * (s + 1) / kColumnSides;
            const float x0 = p.x + std::cos(a0) * kColumnRadius, y0 = p.y + std::sin(a0) * kColumnRadius;
            const float x1 = p.x + std::cos(a1) * kColumnRadius, y1 = p.y + std::sin(a1) * kColumnRadius;
            glow.push_back({ x0, y0, p.z, foot });
            glow.push_back({ x1, y1, p.z, foot });
            glow.push_back({ x1, y1, p.z + height, top });
            glow.push_back({ x0, y0, p.z, foot });
            glow.push_back({ x1, y1, p.z + height, top });
            glow.push_back({ x0, y0, p.z + height, top });
        }

        // The icon: an upright diamond turned to face the camera, standing in
        // for the shop's picture until the game's own icons are read.
        Vec3 side = { -(eye.y - p.y), eye.x - p.x, 0.0f };
        const float sideLength = std::sqrt(side.x * side.x + side.y * side.y);
        side = sideLength > 1e-3f ? side * (1.0f / sideLength) : Vec3{ 1.0f, 0.0f, 0.0f };
        const float size = selected ? kIconSize * 1.3f : kIconSize;
        Vec3 centre = { p.x, p.y, p.z + kIconHeight };

        // A placed shop's effects where the game will put them: the icon on
        // its pole, the ring on the ground, the smoke rising.
        const std::vector<MarkerEffectPreview>* effects = i < effects_.size() ? &effects_[i] : nullptr;
        if (effects)
            for (const MarkerEffectPreview& effect : *effects) {
                const Vec3& e = effect.position;
                if (effect.name.find("_IconA") != std::string::npos) {
                    centre = e;
                    lines.push_back({ e.x, e.y, p.z, Argb(rgb, 0xFF) });   // the pole it hangs on
                    lines.push_back({ e.x, e.y, e.z - size, Argb(rgb, 0xFF) });
                } else if (effect.name.find("_Ring") != std::string::npos) {
                    for (int s = 0; s < 32; ++s) {
                        const float a0 = 2.0f * kPi * s / 32, a1 = 2.0f * kPi * (s + 1) / 32;
                        lines.push_back({ e.x + std::cos(a0) * 3.0f, e.y + std::sin(a0) * 3.0f, e.z + kGroundLift,
                                          Argb(rgb, 0xFF) });
                        lines.push_back({ e.x + std::cos(a1) * 3.0f, e.y + std::sin(a1) * 3.0f, e.z + kGroundLift,
                                          Argb(rgb, 0xFF) });
                    }
                } else {
                    // Smoke: a thin soft column from the emitter up.
                    const float r = 0.8f;
                    for (int s = 0; s < 8; ++s) {
                        const float a0 = 2.0f * kPi * s / 8, a1 = 2.0f * kPi * (s + 1) / 8;
                        const float x0 = e.x + std::cos(a0) * r, y0 = e.y + std::sin(a0) * r;
                        const float x1 = e.x + std::cos(a1) * r, y1 = e.y + std::sin(a1) * r;
                        const D3DCOLOR low = Argb(rgb, 0x90), high = Argb(rgb, 0x00);
                        glow.push_back({ x0, y0, e.z, low });
                        glow.push_back({ x1, y1, e.z, low });
                        glow.push_back({ x1, y1, e.z + 9.0f, high });
                        glow.push_back({ x0, y0, e.z, low });
                        glow.push_back({ x1, y1, e.z + 9.0f, high });
                        glow.push_back({ x0, y0, e.z + 9.0f, high });
                    }
                }
            }
        const Vec3 up    = { centre.x, centre.y, centre.z + size };
        const Vec3 down  = { centre.x, centre.y, centre.z - size };
        const Vec3 left  = centre - side * size;
        const Vec3 right = centre + side * size;
        const D3DCOLOR iconColour = Argb(rgb, 0xFF);
        for (const Vec3& v : { up, left, right, down, right, left })
            solid.push_back({ v.x, v.y, v.z, iconColour });
        const D3DCOLOR edge = selected ? 0xFFFFFFFF : 0xFF000000;
        const Vec3 diamond[4] = { up, right, down, left };
        for (int k = 0; k < 4; ++k) {
            const Vec3& a = diamond[k];
            const Vec3& b = diamond[(k + 1) % 4];
            lines.push_back({ a.x, a.y, a.z, edge });
            lines.push_back({ b.x, b.y, b.z, edge });
        }

        // The footprint: the zone's polygon when it has one, else its radius.
        const D3DCOLOR outline = selected ? 0xFFFFFFFF : Argb(rgb, 0xFF);
        std::vector<Vec3> ring;
        if (marker.polygon.size() >= 3) {
            for (const Vec2& corner : marker.polygon) ring.push_back({ corner.x, corner.y, p.z });
        } else {
            for (int s = 0; s < 24; ++s) {
                const float a = 2.0f * kPi * s / 24;
                ring.push_back({ p.x + std::cos(a) * marker.radius, p.y + std::sin(a) * marker.radius, p.z });
            }
        }
        for (size_t k = 0; k < ring.size(); ++k) {
            const Vec3& a = ring[k];
            const Vec3& b = ring[(k + 1) % ring.size()];
            lines.push_back({ a.x, a.y, a.z + kGroundLift, outline });
            lines.push_back({ b.x, b.y, b.z + kGroundLift, outline });
        }

        // Which way the zone faces, as a short line on the ground.
        const float h = Radians(marker.heading);
        lines.push_back({ p.x, p.y, p.z + kGroundLift, outline });
        lines.push_back({ p.x + std::cos(h) * (kColumnRadius + 4.0f),
                          p.y + std::sin(h) * (kColumnRadius + 4.0f), p.z + kGroundLift, outline });
    }

    renderer.BeginOverlay(device);

    // Solid parts first, into the depth buffer.
    if (!solid.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(solid.size() / 3), solid.data(), sizeof(Vertex));
    if (!lines.empty())
        device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2), lines.data(), sizeof(Vertex));

    // Then the glow, tested against the world but not written, so columns do
    // not cut into each other.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    if (!glow.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(glow.size() / 3), glow.data(), sizeof(Vertex));

    renderer.EndOverlay(device);
}

} // namespace

std::unique_ptr<EditorExtension> CreateMarkersExtension(MainWindow& window) {
    return std::make_unique<MarkersExtension>(window);
}

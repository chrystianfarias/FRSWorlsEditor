// Roads: procedural road networks drawn with Bezier segments between nodes,
// and baked into the game's own models (roads/RoadNetwork.h, roads/RoadBake.h).
//
// Working with them follows the truck-sim map editors: drop a node, pull a
// road out of it, drop the end on another node to join them -- three or more
// roads on one node make a junction -- or on the middle of a road to make a
// T. Each segment carries its lanes, sidewalks, walls and the game textures
// they are painted with, and the mesh follows every change. The game only
// sees the roads once they are baked.

#include "ui/RoadsExtension.h"

#include "core/Log.h"
#include "io/CollisionEdit.h"
#include "io/TextureImport.h"
#include "nfs/SceneryEdit.h"
#include "nfs/TextureDecode.h"
#include "nfs/World.h"
#include "render/CollisionMesh.h"
#include "render/RenderExtension.h"
#include "roads/RoadBake.h"
#include "roads/RoadNetwork.h"
#include "roads/RoadRoutes.h"
#include "terrain/GroundDeform.h"
#include "terrain/Terrain.h"
#include "io/SelectionExport.h"
#include "ui/CollisionTools.h"
#include "ui/EditorExtension.h"
#include "ui/MainWindow.h"
#include "ui/Resource.h"
#include "ui/UndoRegistry.h"
#include "ui/Viewport.h"

#include "imgui.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <set>
#include <unordered_map>

// What the roads reach in the editor's window; MainWindow names it a friend.
struct RoadsAccess {
    explicit RoadsAccess(MainWindow& window) : w(window) {}
    MainWindow& w;

    HWND Window() const { return w.window_; }
    HMENU Menu() const { return w.menu_; }
    HWND Toolbar() const { return w.toolbar_; }
    HACCEL& Accelerators() const { return w.accelerators_; }
    Viewport& View() const { return w.viewport_; }
    WorldRenderer& Renderer() const { return w.viewport_.GetRenderer(); }
    PropertiesPanel& Properties() const { return w.properties_; }
    EditsPanel& Edits() const { return w.edits_; }
    nfs::World* World() const { return w.world_.get(); }
    const std::wstring& ModelsFolder() const { return w.modelsFolder_; }
    const std::wstring& GameFolder() const { return w.gameFolder_; }
    void Status(const std::wstring& text) const { w.SetStatusText(2, text); }
    void RebuildTree() const { w.PopulateSectionTree(); }
    void UpdateMenuChecks() const { w.UpdateMenuChecks(); }
    bool ViewCentre(Vec3& point) const { return w.ViewCentrePoint(point); }
    bool ConfirmUnsaved() const { return w.ConfirmUnsavedPlacements(); }
    void ImportObj(const std::wstring& path, const Vec3& origin) const { w.ImportObj(path, true, &origin); }
    void ImportEdited(const std::wstring& path) const { w.ImportObj(path, true, nullptr); }
    CollisionTools* Collision() const { return w.collisionTools_; }
    bool SaveEdits() const { return w.SavePlacements(); }
    ModelThumbnails& Thumbnails() const { return w.thumbnails_; }
    void ReopenRegionKeepingView() const {
        if (!w.world_) return;
        Camera& camera = w.viewport_.GetCamera();
        const Vec3 position = camera.Position();
        const float yaw = camera.Yaw(), pitch = camera.Pitch();
        w.OpenRegion(w.world_->Region());
        camera.SetPosition(position);
        camera.SetYawPitch(yaw, pitch);
    }
    void Message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) const {
        ::MessageBoxW(w.window_, text.c_str(), L"FRS World Editor - Roads", icon | MB_OK);
    }
};

namespace {

// How far ahead a pulled-out road's new end is put.
constexpr float kPullLength = 30.0f;

constexpr int ID_ROADS_FIRST        = ID_EXTENSION_FIRST + 100;
constexpr int ID_VIEW_ROADS         = ID_ROADS_FIRST + 0;
constexpr int ID_ROAD_NEW           = ID_ROADS_FIRST + 1;
constexpr int ID_ROAD_EXTEND        = ID_ROADS_FIRST + 2;
constexpr int ID_ROAD_SPLIT         = ID_ROADS_FIRST + 3;
constexpr int ID_ROAD_DELETE        = ID_ROADS_FIRST + 4;
constexpr int ID_ROAD_AUTO_HEADING  = ID_ROADS_FIRST + 5;
constexpr int ID_ROAD_BAKE          = ID_ROADS_FIRST + 6;
constexpr int ID_ROAD_UNBAKE        = ID_ROADS_FIRST + 7;
constexpr int ID_TERRAIN_NEW        = ID_ROADS_FIRST + 20;
constexpr int ID_TERRAIN_BRUSH      = ID_ROADS_FIRST + 21;   // + (int)terrain::Brush, 0..5
constexpr int ID_GROUND_FIT         = ID_ROADS_FIRST + 30;
constexpr int ID_GROUND_DISCARD     = ID_ROADS_FIRST + 31;
constexpr int ID_TERRAIN_OUTLINE    = ID_ROADS_FIRST + 32;
constexpr int ID_TERRAIN_ALONG      = ID_ROADS_FIRST + 33;
constexpr int ID_ROAD_SPLIT_WAYS    = ID_ROADS_FIRST + 35;
constexpr int ID_VIEW_ROAD_GRAPH    = ID_ROADS_FIRST + 36;
constexpr int ID_ROAD_FACE_JUNCTION = ID_ROADS_FIRST + 37;
constexpr int ID_ROAD_LANE_ADD      = ID_ROADS_FIRST + 38;
constexpr int ID_ROAD_LANE_DROP     = ID_ROADS_FIRST + 39;
constexpr int ID_ROAD_LANE_SPLIT    = ID_ROADS_FIRST + 40;
constexpr int ID_ROAD_DRAW          = ID_ROADS_FIRST + 41;
constexpr int ID_ROAD_INSERT_MIDDLE = ID_ROADS_FIRST + 42;
constexpr int ID_ROAD_DRAW_FROM     = ID_ROADS_FIRST + 43;

// A terrain patch's selection id: the patch's id with this bit.
constexpr int kTerrainBit = 0x40000000;
// A node of the game's road graph: (route id << 12 | node index) with this bit.
constexpr int kGraphBit = 0x04000000;

// Properties rows.
enum Field {
    kNodeX = 1, kNodeY, kNodeZ, kNodeHeading, kNodeCrosswalks, kHandleLength, kHandleDirection,
    kLanesForward = 20, kLanesBackward, kLaneWidth, kLaneCentre, kLaneInner, kTextureLength,
    kSidewalkWidth, kSidewalkHeight, kSidewalkTexture,
    kWallLeft, kWallRight, kWallHeight, kWallTexture, kWallTextureLength,
    kPoleModel = 50, kPoleSpacing, kPoleSides, kPoleInset, kPoleTurn,
    kAmbient = 60, kLampColor, kLampRadius, kLampStrength,
    kBrush = 80, kBrushRadius, kBrushStrength, kBrushLayer, kLayerCount, kConform, kShoulder, kSink,
    kTerrainAmbient, kMargin,
    kBridge = 90, kDeckThickness, kPillarSpacing, kPillarWidth, kDeckTexture, kPillarTexture,
    kBankMode = 96, kBankMax, kDesignSpeed, kBankAngle,
    kTraffic = 150, kGps, kSpeedLimit,
    kPillarCount = 170, kPillarModel, kPillarModelTurn,
    kTaperLength = 180,
    kMedianType = 130, kMedianWidth, kMedianHeight, kMedianTexture, kWalkLeft, kWalkRight,
    kWallStyle, kWallModel, kWallModelSpacing, kWallModelTurn,
    kLayerTexture = 100,     // + layer * 4: texture, tile, surface
    kLayerTile = 101, kLayerSurface = 102,
};

// A curve handle's selection id: the segment's id with the end's bit.
constexpr int kHandleAtStart = 0x10000000;
constexpr int kHandleAtEnd   = 0x20000000;
constexpr int kHandleBits    = kHandleAtStart | kHandleAtEnd;

const wchar_t* const kSideNames[5] = { L"right", L"left", L"both", L"alternating", L"median" };
const wchar_t* const kMedianNames[5] = { L"none", L"painted", L"concrete", L"grass", L"barrier" };
const wchar_t* const kWallStyleNames[3] = { L"flat", L"guardrail", L"model" };

// A picture for the properties panel: `bgra` (top row first) fitted into a
// `box`-pixel square on the panel's colour.
HBITMAP MakePicture(const std::vector<u32>& bgra, u32 width, u32 height, int box) {
    if (!width || !height || bgra.size() < (size_t)width * height) return nullptr;
    const float scale = std::min((float)box / width, (float)box / height);
    const int w = std::max(1, (int)(width * scale)), h = std::max(1, (int)(height * scale));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;
    u32* out = (u32*)bits;
    // Box-filtered down, so a lane's thin markings survive.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 x0 = (u32)(x / scale), x1 = std::max(x0 + 1, std::min(width, (u32)((x + 1) / scale)));
            const u32 y0 = (u32)(y / scale), y1 = std::max(y0 + 1, std::min(height, (u32)((y + 1) / scale)));
            u32 r = 0, g = 0, b = 0, n = 0;
            for (u32 sy = y0; sy < y1; ++sy)
                for (u32 sx = x0; sx < x1; ++sx) {
                    const u32 c = bgra[(size_t)sy * width + sx];
                    r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; ++n;
                }
            out[(size_t)y * w + x] = n ? ((r / n) << 16) | ((g / n) << 8) | (b / n) : 0;
        }
    return bitmap;
}

u32 ParseColour(const std::wstring& text, u32 fallback) {
    std::wstring digits;
    for (wchar_t c : text)
        if (::iswxdigit(c)) digits += c;
    if (digits.empty() || digits.size() > 6) return fallback;
    return (u32)std::wcstoul(digits.c_str(), nullptr, 16) & 0xFFFFFF;
}

std::wstring Fmt(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    ::_vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text)
        if (c > 0x20 && c < 0x7F) out += (char)::towupper(c);
    return out;
}

std::wstring Number(float value) { return Fmt(L"%.3f", value); }

void AddRow(PropertiesPanel::View& view, const wchar_t* group, const wchar_t* name, std::wstring value,
            const wchar_t* description, int field, std::vector<std::wstring> choices = {}, bool preview = false) {
    PropertiesPanel::Row row;
    row.preview = preview;
    row.group = group;
    row.name = name;
    row.value = std::move(value);
    row.description = description;
    row.editable = field != 0;
    row.field = field;
    row.choices = std::move(choices);
    view.rows.push_back(std::move(row));
}

Vec3 HorizontalDirection(const Vec3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    return length > 1e-5f ? Vec3{ v.x / length, v.y / length, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
}

// A batch the viewport draws: one texture, world-space vertices.
struct DrawBatch {
    u32 texture = 0;
    std::vector<nfs::SolidVertex> vertices;
    std::vector<u16> indices;
    u32 owner = 0;            // segment or node id, for picking
};

class RoadsExtension : public EditorExtension, public RenderExtension, public ViewportTool, public UndoParticipant {
public:
    explicit RoadsExtension(MainWindow& window) : app_(window) {}
    ~RoadsExtension() override { undo::Unregister(this); }

    // ---- undo (ui/UndoRegistry.h)
    //
    // Every change to the roads, the terrain patches and the sculpting of the
    // game's ground is one step: the whole of that state as it was before,
    // which is small next to what it describes. Ctrl+Z takes back the most
    // recent step of the editor, this one's or another part's.

    struct UndoState {
        roads::Network network;
        terrain::Terrain terrain;
        terrain::GroundDeform ground;
        std::set<std::string> fitted;
        bool fitGround = true;
        u64 stamp = 0;
        std::wstring label;
        // A bake or its removal: the world's models folder as it was before,
        // and how it looked right after, to tell whether anything else wrote
        // there since.
        std::wstring filesBackup;
        u64 filesAfter = 0;
    };

    UndoState Capture() const {
        UndoState state;
        state.network = network_;
        state.terrain = terrain_;
        state.ground = ground_;
        state.fitted = fitted_;
        state.fitGround = fitGround_;
        return state;
    }

    // After a change is made: what was there before becomes a step.
    void Commit(const wchar_t* label) {
        if (!historyReady_) return;
        UndoState before = std::move(settled_);
        before.stamp = undo::NextStamp();
        before.label = label;
        undo_.push_back(std::move(before));
        if (undo_.size() > 100) {
            DropBackup(undo_.front());
            undo_.erase(undo_.begin());
        }
        settled_ = Capture();
    }

    // ---- what a bake changes on disk, for taking it back

    // The world's own files: <game>\\models without the other worlds.
    static bool WorldFile(const std::filesystem::path& relative) {
        const std::wstring first = relative.begin() == relative.end() ? L"" : relative.begin()->wstring();
        return _wcsicmp(first.c_str(), L"worlds") != 0;
    }

    u64 ModelsSignature() const {
        namespace fs = std::filesystem;
        std::error_code ec;
        u64 hash = 1469598103934665603ull;
        const fs::path root = app_.ModelsFolder();
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path relative = fs::relative(it->path(), root, ec);
            if (!WorldFile(relative)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const std::wstring name = relative.wstring();
            for (wchar_t c : name) hash = (hash ^ (u64)c) * 1099511628211ull;
            hash = (hash ^ (u64)it->file_size(ec)) * 1099511628211ull;
            hash = (hash ^ (u64)it->last_write_time(ec).time_since_epoch().count()) * 1099511628211ull;
        }
        return hash;
    }

    // A copy of the world's files, under %LOCALAPPDATA%\\FRSWorldEditor\\undo.
    std::wstring BackupModels() const {
        namespace fs = std::filesystem;
        wchar_t localAppData[MAX_PATH] = {};
        if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) return {};
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        const fs::path backup = fs::path(localAppData) / L"FRSWorldEditor" / L"undo" /
                                Fmt(L"bake-%04u%02u%02u-%02u%02u%02u-%03u", now.wYear, now.wMonth, now.wDay, now.wHour,
                                    now.wMinute, now.wSecond, now.wMilliseconds);
        std::error_code ec;
        const fs::path root = app_.ModelsFolder();
        fs::create_directories(backup, ec);
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) return {};
            const fs::path relative = fs::relative(it->path(), root, ec);
            if (!WorldFile(relative)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) fs::create_directories(backup / relative, ec);
            else if (it->is_regular_file(ec)) fs::copy_file(it->path(), backup / relative, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                LOG_WARN("undo: %S could not be copied", it->path().c_str());
                return {};
            }
        }
        return backup.wstring();
    }

    // Puts the world's files back as the copy has them.
    bool RestoreModels(const std::wstring& from) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = app_.ModelsFolder();
        if (!fs::exists(from, ec)) return false;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const fs::path relative = entry.path().filename();
            if (!WorldFile(relative)) continue;
            fs::remove_all(entry.path(), ec);
        }
        fs::copy(from, root, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) LOG_WARN("undo: the world's files could not all be put back: %s", ec.message().c_str());
        return !ec;
    }

    void DropBackup(const UndoState& state) {
        if (state.filesBackup.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(state.filesBackup, ec);
    }

    // A bake or a removal as one step: what the roads were, and the files.
    struct DiskStep {
        RoadsExtension* self;
        UndoState before;
        bool armed = false;
        explicit DiskStep(RoadsExtension* owner, const wchar_t* label) : self(owner) {
            before = self->Capture();
            before.label = label;
            before.filesBackup = self->BackupModels();
            armed = !before.filesBackup.empty();
            if (!armed) LOG_WARN("undo: no copy of the world's files could be made; this step cannot be undone");
        }
        ~DiskStep() {
            if (!armed) return;
            before.stamp = undo::NextStamp();
            before.filesAfter = self->ModelsSignature();
            self->undo_.push_back(std::move(before));
            self->settled_ = self->Capture();
        }
    };

    void ResetHistory() {
        for (const UndoState& state : undo_) DropBackup(state);
        undo_.clear();
        settled_ = Capture();
        historyReady_ = true;
    }

    u64 LatestUndoStamp() const override { return undo_.empty() ? 0 : undo_.back().stamp; }

    bool UndoLatest() override {
        if (undo_.empty()) return false;
        if (!undo_.back().filesBackup.empty()) {
            // A bake: its files go back first, then the region is read again.
            const UndoState& step = undo_.back();
            if (ModelsSignature() != step.filesAfter &&
                ::MessageBoxW(app_.Window(),
                              (L"Undo \"" + step.label + L"\"?\n\nThe world's files changed after it (something else was "
                               L"written since). Undoing puts all of them back as they were before it.").c_str(),
                              L"FRS World Editor - Roads", MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
                return true;
            if (!RestoreModels(step.filesBackup)) {
                app_.Message(L"The world's files could not all be put back; the log has why.", MB_ICONWARNING);
                return true;
            }
            app_.ReopenRegionKeepingView();
        }
        UndoState state = std::move(undo_.back());
        undo_.pop_back();
        DropBackup(state);
        const std::wstring label = state.label;
        network_ = std::move(state.network);
        terrain_ = std::move(state.terrain);
        ground_ = std::move(state.ground);
        fitted_ = std::move(state.fitted);
        fitGround_ = state.fitGround;
        if (light_) network_.SetLightSources(light_);
        network_.SetDirty();
        meshRevision_ = 0;
        terrainBuilt_ = 0;
        groundFieldRevision_ = 0;
        terrainBatches_.clear();
        stroking_ = strokeGround_ = false;
        drawing_ = false;
        outline_.clear();
        // What a selection pointed at may be gone.
        const int selected = Selected();
        if (selected >= 0 && !network_.FindNode((u32)(selected & ~kHandleBits)) &&
            !network_.FindSegment((u32)(selected & ~kHandleBits)) &&
            !terrain_.Find((u32)(selected & ~kTerrainBit)))
            app_.Renderer().ClearSelection();
        SaveNetwork();
        SaveTerrain();
        SaveGround();
        SaveFitted();
        settled_ = Capture();
        app_.RebuildTree();
        app_.UpdateMenuChecks();
        app_.Status(L"Undone: " + label);
        return true;
    }

    void OnCreate() override {
        app_.Renderer().AddExtension(this);
        app_.View().AddTool(this);
        network_.SetGroundQuery([this](float x, float y, float top, float& height) {
            return GroundBelow(x, y, top, height);
        });
        network_.SetModelBounds([this](const std::string& name, Vec3& min, Vec3& max) {
            const auto found = modelsByName_.find(Widen(name));
            if (found == modelsByName_.end() || !found->second->bounds.Valid()) return false;
            min = found->second->bounds.min;
            max = found->second->bounds.max;
            return true;
        });
        // The baked models stand where the procedural roads are drawn; the
        // editor shows the description, which is what is being edited.
        app_.Renderer().SetHiddenModels([this](const nfs::SolidObject& model) {
            return visible_ && (roads::IsBakedRoadModel(model.name) || faceHidden_.count(model.hash) != 0);
        });
        AddMenu();
        AddAccelerators();
        PropertiesPanel::SetImageProvider([this](const std::wstring& name, int size) { return Picture(name, size); });
        undo::Register(this);
    }

    bool OnCommand(int id) override {
        // Ctrl+Z: ours when our step is the editor's most recent one.
        if (id == CollisionTools::kUndoCommand) {
            if (undo::Latest() != this) return false;
            return UndoLatest();
        }
        switch (id) {
            case ID_VIEW_ROADS:
                visible_ = !visible_;
                if (!visible_ && Selected() >= 0) app_.Renderer().ClearSelection();
                app_.UpdateMenuChecks();
                return true;
            case ID_ROAD_NEW:          NewRoad(); return true;
            case ID_ROAD_EXTEND:       Extend(); return true;
            case ID_ROAD_SPLIT:        Split(); return true;
            case ID_ROAD_DELETE:       DeleteSelection(); return true;
            case ID_ROAD_AUTO_HEADING: AutoHeading(); return true;
            case ID_ROAD_BAKE:         Bake(); return true;
            case ID_ROAD_UNBAKE:       Unbake(); return true;
            case ID_TERRAIN_NEW:       NewTerrain(); return true;
            case ID_TERRAIN_OUTLINE:   StartOutline(); return true;
            case ID_TERRAIN_ALONG:     TerrainAlongRoad(); return true;
            case ID_ROAD_FACE_JUNCTION: StartFaceWizard(); return true;
            case ID_ROAD_SPLIT_WAYS:   SplitCarriageways(); return true;
            case ID_ROAD_LANE_ADD:     ChangeLanesAfterNode(1); return true;
            case ID_ROAD_LANE_DROP:    ChangeLanesAfterNode(-1); return true;
            case ID_ROAD_LANE_SPLIT:   SplitOffOuterLane(); return true;
            case ID_ROAD_DRAW:         if (drawTool_) CloseDrawTool(); else StartDrawTool(0); return true;
            case ID_ROAD_DRAW_FROM:
                if (const roads::Node* node = SelectedNode()) StartDrawTool(node->id);
                return true;
            case ID_ROAD_INSERT_MIDDLE:
                if (const roads::Segment* s = SelectedSegment()) {
                    const u32 id = s->id;
                    if (const u32 middle = network_.SplitSegment(id, 0.5f)) {
                        if (roads::Node* n = network_.FindNode(middle)) n->hasHeading = false;
                        Changed(true);
                        Select((int)middle);
                        app_.Status(Fmt(L"A node put in the middle of road %u", id));
                    }
                }
                return true;
            case ID_VIEW_ROAD_GRAPH:
                graphView_ = !graphView_;
                graphBuilt_ = 0;
                app_.UpdateMenuChecks();
                app_.Status(graphView_ ? L"Road graph shown: white routes (yellow: the nearest), green the ones the "
                                         L"editor adds; blue lanes forward, orange backward, red the road's edges; "
                                         L"links green (all), purple (GPS and racers only), grey (closed); click a node"
                                       : L"Road graph hidden");
                return true;
            case ID_GROUND_FIT:
                fitGround_ = !fitGround_;
                groundFieldRevision_ = 0;
                app_.UpdateMenuChecks();
                Commit(L"fit the game's ground to the roads");
                return true;
            case ID_GROUND_DISCARD:
                ground_.Clear();
                SaveGround();
                RefreshGround(nullptr);
                Commit(L"discard the ground sculpting");
                app_.Status(L"Unbaked sculpting of the game's ground thrown away");
                return true;
            default:
                if (id >= ID_TERRAIN_BRUSH && id < ID_TERRAIN_BRUSH + terrain::kBrushCount) {
                    SetBrush((terrain::Brush)(id - ID_TERRAIN_BRUSH));
                    return true;
                }
                return false;
        }
    }

    void UpdateMenuChecks(HMENU menu) override {
        ::CheckMenuItem(menu, ID_VIEW_ROADS, MF_BYCOMMAND | (visible_ ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_VIEW_ROAD_GRAPH, MF_BYCOMMAND | (graphView_ ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_ROAD_DRAW, MF_BYCOMMAND | (drawTool_ ? MF_CHECKED : MF_UNCHECKED));
        SyncToolbar();
        ::CheckMenuItem(menu, ID_GROUND_FIT, MF_BYCOMMAND | (fitGround_ ? MF_CHECKED : MF_UNCHECKED));
        for (int b = 0; b < terrain::kBrushCount; ++b)
            ::CheckMenuItem(menu, ID_TERRAIN_BRUSH + b, MF_BYCOMMAND | ((int)brush_ == b ? MF_CHECKED : MF_UNCHECKED));
    }

    void OnRegionLoaded() override {
        nfs::World* world = app_.World();
        if (!world) return;
        network_.Load(NetworkPath());
        terrain_.Load(TerrainPath());
        terrainBuilt_ = 0;
        ground_.Load(GroundPath());
        LoadFitted();
        CollectGroundModels(*world);
        SetGameLight(*world);
        groundFieldRevision_ = 0;
        meshRevision_ = 0;
        CollectTextureChoices(*world);
        if (world->Region() != historyRegion_) {
            historyRegion_ = world->Region();
            ResetHistory();
        } else {
            settled_ = Capture();
        }
    }

    void OnRegionClosed() override {
        network_.Clear();
        terrain_.Clear();
        terrainBatches_.clear();
        stroking_ = false;
        strokeGround_ = false;
        if (wizard_ != kWizardOff) EndFaceWizard(nullptr);
        faceRemainder_.clear();
        faceHidden_.clear();
        groundModels_.clear();
        ground_.Clear();
        batches_.clear();
        poles_.clear();
        meshRevision_ = 0;
        DropPictures();
        texturesByName_.clear();
        modelsByName_.clear();
    }

    // ---- the Edits tab
    //
    // Every source item of the roads and the terrain is a row there: each
    // road, each junction made into a game road, each stretch cut out of one,
    // each terrain patch, the ground sculpting not yet baked -- and, once
    // baked, what the game holds of them. Reverting an item takes it out of
    // the source and, when the roads are baked, bakes again, so its models,
    // street lights, collision and routes leave the game with it. Reverting
    // the baked row takes everything baked out of the game.

    enum class EditKind : size_t { Segment = 1, GameJunction, Cut, Patch, Ground, Baked, FaceJunction };
    static size_t EditIndex(EditKind kind, size_t i) { return ((size_t)kind << 24) | (i & 0xFFFFFF); }

    size_t BakedModels() const {
        size_t count = 0;
        if (const nfs::World* world = app_.World())
            for (const nfs::SolidObject* model : world->AllModels()) count += model && roads::IsBakedRoadModel(model->name);
        return count;
    }

    void EditRows(std::vector<EditsPanel::ExtraRow>& rows) override {
        const auto add = [&](EditKind kind, size_t i, std::wstring what, std::wstring detail, const wchar_t* label) {
            EditsPanel::ExtraRow row;
            row.owner = static_cast<EditorExtension*>(this);
            row.index = EditIndex(kind, i);
            row.kind = label;
            row.what = std::move(what);
            row.detail = std::move(detail);
            rows.push_back(std::move(row));
        };
        const size_t baked = BakedModels();
        const wchar_t* state = baked ? L"" : L"  (not baked)";
        const auto& segments = network_.Segments();
        for (size_t i = 0; i < segments.size(); ++i) {
            const roads::Segment& sgm = segments[i];
            add(EditKind::Segment, i, Fmt(L"Road %u", sgm.id),
                Fmt(L"%d+%d lanes, %.0f long%s%s%s", sgm.profile.lanesForward, sgm.profile.lanesBackward,
                    SegmentLength(sgm), sgm.profile.bridge ? L", bridge" : L"",
                    sgm.profile.keepGround ? L", over a game road" : L"", state),
                L"Road");
        }
        const auto& junctions = network_.GameJunctions();
        for (size_t i = 0; i < junctions.size(); ++i)
            add(EditKind::GameJunction, i, Fmt(L"Junction at %.0f, %.0f", junctions[i].centre.x, junctions[i].centre.y),
                Fmt(L"into the game's road, %.1f wide%s", junctions[i].halfRoad * 2.0f, state), L"Road junction");
        const auto& faceJunctions = network_.FaceJunctions();
        for (size_t i = 0; i < faceJunctions.size(); ++i) {
            const roads::FaceJunction& fj = faceJunctions[i];
            Vec3 at{};
            for (const roads::GameFace& f : fj.faces) at = at + f.p[0];
            if (!fj.faces.empty()) at = at * (1.0f / fj.faces.size());
            add(EditKind::FaceJunction, i, Fmt(L"Junction at %.0f, %.0f", at.x, at.y),
                Fmt(L"%zu faces of %S replaced%s", fj.faces.size(), fj.model.c_str(), state), L"Road junction");
        }
        const auto& cuts = network_.StoredCuts();
        for (size_t i = 0; i < cuts.size(); ++i)
            add(EditKind::Cut, i, Fmt(L"Cut at %.0f, %.0f", cuts[i].centre.x, cuts[i].centre.y),
                Fmt(L"%.0f of the game's road replaced%s", cuts[i].halfLength * 2.0f, state), L"Road cut");
        const auto& patches = terrain_.Patches();
        for (size_t i = 0; i < patches.size(); ++i)
            add(EditKind::Patch, i, Fmt(L"Terrain %u", patches[i].id),
                Fmt(L"%d x %d cells of %.1f%s", patches[i].columns, patches[i].rows, patches[i].cell, state), L"Terrain");
        if (!ground_.Empty()) add(EditKind::Ground, 0, L"Game ground sculpting", L"not baked yet", L"Terrain");
        if (baked)
            add(EditKind::Baked, 0, Fmt(L"%zu road and terrain models", baked),
                L"in the game, with their street lights, collision and routes", L"Roads baked");
    }

    void RevertRows(const std::vector<size_t>& chosen) override {
        // From the last of each kind, so the indices before stay true.
        std::vector<size_t> indices = chosen;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        bool unbake = false, changed = false;
        std::vector<u32> segments, patches;
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
            const EditKind kind = (EditKind)(*it >> 24);
            const size_t i = *it & 0xFFFFFF;
            switch (kind) {
                case EditKind::Segment:
                    if (i < network_.Segments().size()) segments.push_back(network_.Segments()[i].id);
                    break;
                case EditKind::GameJunction: network_.RemoveGameJunction(i); changed = true; break;
                case EditKind::Cut:          network_.RemoveCut(i); changed = true; break;
                case EditKind::FaceJunction: network_.RemoveFaceJunction(i); changed = true; break;
                case EditKind::Patch:
                    if (i < terrain_.Patches().size()) patches.push_back(terrain_.Patches()[i].id);
                    break;
                case EditKind::Ground:       ground_.Clear(); changed = true; break;
                case EditKind::Baked:        unbake = true; break;
            }
        }
        for (u32 id : segments) network_.RemoveSegment(id);
        for (u32 id : patches) terrain_.Remove(id);
        changed |= !segments.empty() || !patches.empty();
        app_.Renderer().ClearSelection();
        if (changed) {
            terrainBatches_.clear();
            SaveTerrain();
            SaveGround();
            RefreshGround(nullptr);
            Changed(true);          // saves the network and records the step
        }
        if (unbake) {
            Unbake();
        } else if (changed && BakedModels()) {
            // What the game holds follows the source: bake again.
            app_.Status(L"Reverted -- baking again so the game follows");
            Bake();
        }
        app_.Edits().RefreshExtra();
    }

    void Save() override {
        SaveNetwork();
        SaveTerrain();
    }

    // ---- the terrain brush and the area outline (ViewportTool)

    static bool AreaBrush(terrain::Brush brush) {
        return brush == terrain::Brush::AddArea || brush == terrain::Brush::EraseArea;
    }

    // Any surface under the cursor: a patch, the game's ground, the world.
    bool AnythingUnder(int x, int y, Vec3& point) const {
        u32 patch = 0;
        if (TerrainUnder(x, y, patch, point)) return true;
        const Ray ray = app_.View().RayAt(x, y);
        PickResult hit;
        if (app_.Renderer().Pick(ray, hit)) {
            point = ray.origin + ray.direction * hit.distance;
            return true;
        }
        // Nothing: the plane of the last point drawn, or of the view centre.
        const float z = !outline_.empty() ? outline_.back().z : brushPoint_.z;
        if (std::fabs(ray.direction.z) < 1e-4f) return false;
        const float t = (z - ray.origin.z) / ray.direction.z;
        if (t <= 0.0f) return false;
        point = ray.origin + ray.direction * t;
        return true;
    }

    bool MouseDown(int x, int y, WPARAM keys) override {
        (void)keys;
        if (!visible_) return false;
        if (wizard_ != kWizardOff) return WizardMouseDown(x, y);
        if (drawTool_) return DrawToolMouseDown(x, y);
        if (drawing_) {
            Vec3 point;
            if (AnythingUnder(x, y, point)) outline_.push_back(point);
            return true;
        }
        if (brush_ == terrain::Brush::None) return false;
        u32 patch = 0;
        Vec3 point;
        if (AreaBrush(brush_)) {
            // Grows or trims the selected patch, wherever the cursor is.
            terrain::Patch* selected = SelectedPatchMutable();
            if (!selected) {
                app_.Status(L"Select a terrain patch first (click it), then paint its area");
                return false;
            }
            if (!AnythingUnder(x, y, point)) return true;
            stroking_ = true;
            strokeGround_ = false;
            strokePatch_ = selected->id;
            brushPoint_ = point;
            brushHit_ = true;
            return true;
        }
        if (!TerrainUnder(x, y, patch, point)) {
            // The game's own ground.
            if (brush_ == terrain::Brush::Paint || !GameGroundUnder(x, y, point)) return false;
            stroking_ = true;
            strokeGround_ = true;
            brushPoint_ = point;
            brushHit_ = true;
            flattenLevel_ = point.z;
            return true;
        }
        stroking_ = true;
        strokeGround_ = false;
        strokePatch_ = patch;
        brushPoint_ = point;
        brushHit_ = true;
        flattenLevel_ = point.z;
        Select(kTerrainBit | (int)patch);
        return true;
    }

    bool MouseDoubleClick(int x, int y, WPARAM keys) override {
        if (!drawing_) return false;
        MouseDown(x, y, keys);
        FinishOutline();
        return true;
    }

    bool MouseUp(int x, int y, WPARAM keys) override {
        (void)x; (void)y; (void)keys;
        if (wizard_ == kWizardOff) return false;
        wizardPainting_ = false;
        return true;
    }

    void MouseMove(int x, int y) override {
        cursorX_ = x;
        cursorY_ = y;
        if (wizard_ != kWizardOff) { WizardMouseMove(x, y); return; }
        if (drawTool_) {
            drawHover_ = DrawHitAt(x, y);
            return;
        }
        if (drawing_) {
            hoverValid_ = AnythingUnder(x, y, hover_);
            return;
        }
        if (brush_ == terrain::Brush::None) { brushHit_ = false; return; }
        u32 patch = 0;
        if (AreaBrush(brush_)) {
            brushHit_ = AnythingUnder(x, y, brushPoint_);
            return;
        }
        brushHit_ = TerrainUnder(x, y, patch, brushPoint_) ||
                    (brush_ != terrain::Brush::Paint && GameGroundUnder(x, y, brushPoint_));
    }

    bool KeyDown(WPARAM key, bool repeat) override {
        (void)repeat;
        if (wizard_ != kWizardOff && WizardKey(key)) return true;
        if (drawTool_) {
            if (key == VK_ESCAPE) {
                if (drawFrom_ || drawPending_) EndDrawChain();
                else CloseDrawTool();
                return true;
            }
            if (key == VK_RETURN) {
                EndDrawChain();
                return true;
            }
        }
        if (drawing_) {
            if (key == VK_ESCAPE) {
                drawing_ = false;
                outline_.clear();
                app_.Status(L"Terrain outline cancelled");
                return true;
            }
            if (key == VK_BACK) {
                if (!outline_.empty()) outline_.pop_back();
                return true;
            }
            if (key == VK_RETURN) {
                FinishOutline();
                return true;
            }
            return false;
        }
        if (brush_ == terrain::Brush::None) return false;
        if (key == VK_ESCAPE) { SetBrush(terrain::Brush::None); return true; }
        if (key == VK_OEM_4) { brushRadius_ = std::max(1.0f, brushRadius_ / 1.2f); return true; }    // [
        if (key == VK_OEM_6) { brushRadius_ = std::min(400.0f, brushRadius_ * 1.2f); return true; }  // ]
        return false;
    }

    void Tick(float deltaSeconds) override {
        if (!stroking_) return;
        if (!(::GetKeyState(VK_LBUTTON) & 0x8000)) {
            stroking_ = false;
            if (strokeGround_) {
                strokeGround_ = false;
                SaveGround();
                Commit(L"sculpt the game's ground");
                app_.Status(L"Game ground sculpted -- shown here now; Roads > Bake writes it into the region");
                return;
            }
            SaveTerrain();
            Commit(L"terrain brush");
            app_.Status(Fmt(L"Terrain %s -- Roads > Bake hands it to the game", terrain::BrushName(brush_)));
            return;
        }
        if (strokeGround_) {
            Vec3 point;
            if (GameGroundUnder(cursorX_, cursorY_, point)) brushPoint_ = point;
            const float dt = std::min(deltaSeconds, 0.1f);
            const Aabb changed = ground_.Apply(brush_, brushPoint_, brushRadius_, brushStrength_ * dt, flattenLevel_,
                                               [this](float x, float y) { return BaseGroundHeight(x, y); });
            if (changed.Valid()) RefreshGround(&changed);
            return;
        }
        terrain::Patch* patch = terrain_.Find(strokePatch_);
        if (!patch) { stroking_ = false; return; }
        Vec3 point;
        if (AreaBrush(brush_)) {
            if (AnythingUnder(cursorX_, cursorY_, point)) brushPoint_ = point;
            terrain_.Apply(*patch, brush_, brushPoint_, brushRadius_, 0.0f, 0.0f, paintLayer_,
                           [this, z = brushPoint_.z](float x, float y) { return NewGroundHeight(x, y, z); });
            dirtyPatch_ = patch->id;
            return;
        }
        u32 under = 0;
        if (TerrainUnder(cursorX_, cursorY_, under, point)) {
            brushPoint_ = point;
            brushHit_ = true;
        }
        if (!brushHit_) return;
        const float dt = std::min(deltaSeconds, 0.1f);
        terrain_.Apply(*patch, brush_, brushPoint_, brushRadius_, brushStrength_ * dt, flattenLevel_, paintLayer_);
        dirtyPatch_ = patch->id;
    }

    // Where new ground starts: on the game's ground or a patch under it, else
    // at the nearest road's height, else at `fallback`.
    float NewGroundHeight(float x, float y, float fallback) const {
        float height = 0.0f, outside = 0.0f;
        if (GroundBelow(x, y, fallback + 40.0f, height) && height > fallback - 60.0f) return height;
        if (!field_.Empty() && field_.Query(x, y, 200.0f, height, outside)) return height - 0.15f;
        return fallback;
    }

    void StartOutline() {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        visible_ = true;
        SetBrush(terrain::Brush::None);
        drawing_ = true;
        outline_.clear();
        ::SetFocus(app_.View().Handle());
        app_.Status(L"Draw the terrain's outline: click points on the ground; Enter or a double click closes it, "
                    L"Backspace takes the last point back, Esc cancels");
    }

    void FinishOutline() {
        drawing_ = false;
        if (outline_.size() < 3) {
            outline_.clear();
            app_.Status(L"An outline needs three points or more");
            return;
        }
        Vec2 min{ 1e30f, 1e30f }, max{ -1e30f, -1e30f };
        float z = 0.0f;
        for (const Vec3& p : outline_) {
            min = { std::min(min.x, p.x), std::min(min.y, p.y) };
            max = { std::max(max.x, p.x), std::max(max.y, p.y) };
            z += p.z;
        }
        z /= (float)outline_.size();
        const std::vector<Vec3> outline = outline_;
        const auto inside = [&outline](float x, float y) {
            bool in = false;
            for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
                const Vec3& a = outline[i];
                const Vec3& b = outline[j];
                if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
            }
            return in;
        };
        const u32 id = terrain_.AddWhere(min, max, 4.0f, inside,
                                         [this, z](float x, float y) { return NewGroundHeight(x, y, z); });
        outline_.clear();
        if (!id) {
            app_.Status(L"The outline encloses no whole cell (4 units); draw it larger");
            return;
        }
        NewPatchMade(id);
    }

    // Ground along the selected road and every road joined to it, `margin`
    // beyond their edges.
    void TerrainAlongRoad() {
        u32 start = 0;
        if (const roads::Segment* s = SelectedSegment()) start = s->id;
        else if (const roads::Node* n = SelectedNode()) {
            const std::vector<u32> around = network_.SegmentsAt(n->id);
            if (!around.empty()) start = around.front();
        }
        if (!start) {
            app_.Message(L"Select a road (click it) first: the terrain is made along it and every road joined to it.");
            return;
        }
        // The roads reachable from it.
        std::set<u32> chosen{ start };
        std::vector<u32> todo{ start };
        while (!todo.empty()) {
            const roads::Segment* s = network_.FindSegment(todo.back());
            todo.pop_back();
            if (!s) continue;
            for (u32 node : { s->a, s->b })
                for (u32 next : network_.SegmentsAt(node))
                    if (chosen.insert(next).second) todo.push_back(next);
        }
        roads::RoadField along;
        along.Build(network_, [&](const roads::Segment& s) { return chosen.count(s.id) && !s.profile.bridge; });
        if (along.Empty()) {
            app_.Status(L"Only bridges there: no ground to make along them");
            return;
        }
        Aabb box;
        for (u32 id : chosen) {
            const roads::Segment* s = network_.FindSegment(id);
            const roads::Curve c = network_.CurveOf(*s);
            for (int k = 0; k <= 16; ++k) box.Add(c.At(k / 16.0f));
        }
        const float margin = terrainMargin_;
        const float reach = margin + 30.0f;
        const Vec2 min{ box.min.x - reach, box.min.y - reach }, max{ box.max.x + reach, box.max.y + reach };
        const float z = box.Center().z;
        const u32 id = terrain_.AddWhere(
            min, max, 4.0f,
            [&](float x, float y) {
                float h = 0.0f, outside = 0.0f;
                return along.Query(x, y, margin, h, outside) && outside < margin;
            },
            [this, z](float x, float y) { return NewGroundHeight(x, y, z); });
        if (!id) {
            app_.Status(L"No ground could be made along that road");
            return;
        }
        NewPatchMade(id);
    }

    void NewPatchMade(u32 id) {
        if (terrain::Patch* patch = terrain_.Find(id))
            if (!groundTextures_.empty()) patch->layers[0].texture = Narrow(groundTextures_.front());
        terrainBuilt_ = 0;
        SaveTerrain();
        Commit(L"new terrain");
        app_.RebuildTree();
        Select(kTerrainBit | (int)id);
        app_.Status(L"Terrain made: sculpt it with the brushes, grow or trim it with Add/Erase Area, move it with "
                    L"the gizmo; Roads > Bake hands it to the game");
    }

    // ---- the scene tree

    size_t SceneSignature() const override { return network_.Segments().size() * 7919 + network_.Nodes().size(); }

    void PopulateTree(SceneTreeBuilder& tree) override {
        if (network_.Segments().empty()) return;
        HTREEITEM root = tree.insert(TVI_ROOT, Fmt(L"Roads (%zu)", network_.Segments().size()), -1);
        size_t shown = 0;
        for (const roads::Segment& s : network_.Segments()) {
            const std::wstring text = Fmt(L"Road %u  -  %d+%d lanes, %.0f long", s.id, s.profile.lanesForward,
                                          s.profile.lanesBackward, SegmentLength(s));
            if (!tree.matches(text)) continue;
            tree.insert(root, text, (intptr_t)s.id);
            ++shown;
        }
        tree.finish(root, shown, false);
    }

    void OnTreeItem(intptr_t code) override {
        const roads::Segment* s = network_.FindSegment((u32)code);
        if (!s) return;
        const roads::Node* a = network_.FindNode(s->a);
        const roads::Node* b = network_.FindNode(s->b);
        Aabb box;
        box.Add(a->position);
        box.Add(b->position);
        app_.View().FrameBounds(Pad(box, 20.0f));
        Select((int)s->id);
    }

    // ---- the selection

    bool StatusText(std::wstring& text) const override {
        if (const nfs::routes::Node* n = SelectedGraphNode()) {
            const int id = Selected() & ~kGraphBit;
            text = Fmt(L"Road graph: route %d node %d at %.1f, %.1f, %.1f%s", id >> 12, id & 0xFFF, n->x, n->y,
                       n->height8 / 8.0f, ourRoutes_.count((u16)(id >> 12)) ? L" (made by the editor)" : L"");
            return true;
        }
        if (const terrain::Patch* patch = SelectedPatch()) {
            text = Fmt(L"Terrain patch %u: %d x %d cells of %.1f   [brush %s, size %.0f: Terrain menu or Properties; "
                       L"hold the left button to sculpt   [ ] size   Del delete]",
                       patch->id, patch->columns, patch->rows, patch->cell, terrain::BrushName(brush_), brushRadius_);
            return true;
        }
        if (const roads::Segment* s = SelectedHandle()) {
            text = Fmt(L"Curve handle of road %u at its %s end   [gizmo moves it: its direction turns the node, its "
                       L"length shapes the curve   Del makes it automatic]",
                       s->id, (Selected() & kHandleAtStart) ? L"first" : L"last");
            return true;
        }
        if (const roads::Node* node = SelectedNode()) {
            const size_t count = network_.SegmentsAt(node->id).size();
            text = Fmt(L"Road node %u at %.1f, %.1f, %.1f  (%s)   [gizmo: arrows/square move (drop on a node to "
                       L"join, on a road to make a T), 2 then the ring turns   Ctrl+Shift+R pull a road out   Del delete]",
                       node->id, node->position.x, node->position.y, node->position.z,
                       count >= 3 ? L"junction" : count == 2 ? L"through" : L"road end");
            return true;
        }
        if (const roads::Segment* s = SelectedSegment()) {
            text = Fmt(L"Road %u: %d+%d lanes, %.0f long   [edit in Properties   Roads > Split Road Here   "
                       L"Del delete   Roads > Bake to hand it to the game]",
                       s->id, s->profile.lanesForward, s->profile.lanesBackward, SegmentLength(*s));
            return true;
        }
        return false;
    }

    bool FillProperties(PropertiesPanel::View& view) override;
    bool ApplyProperties() override;

    bool DeleteSelection() override {
        const int id = Selected();
        if (id < 0) return false;
        if (id & kGraphBit) return true;   // the game's graph is shown, not edited
        if (const terrain::Patch* patch = SelectedPatch()) {
            terrain_.Remove(patch->id);
            terrainBatches_.clear();
            app_.Renderer().ClearSelection();
            SaveTerrain();
            Commit(L"delete terrain");
            app_.RebuildTree();
            return true;
        }
        if (roads::Segment* s = SelectedHandleMutable()) {
            // A handle is not deleted, it goes back to automatic.
            if (id & kHandleAtStart) { s->handleA = 0.0f; s->liftSetA = false; }
            else { s->handleB = 0.0f; s->liftSetB = false; }
            Changed(true);
            return true;
        }
        if (network_.FindNode((u32)id)) network_.RemoveNode((u32)id);
        else if (network_.FindSegment((u32)id)) network_.RemoveSegment((u32)id);
        app_.Renderer().ClearSelection();
        Changed(true);
        return true;
    }

    // The properties panel's first button pulls a road out of a node.
    bool MoveSelectionToView() override {
        if (Selected() < 0) return false;
        Extend();
        return true;
    }

    bool GetPoint(Vec3& position, float& heading) override {
        if (const terrain::Patch* patch = SelectedPatch()) {
            // The gizmo stands on the patch's middle and moves it whole.
            position = PatchCentre(*patch);
            heading = 0.0f;
            if (!patchDragging_) patchDragStart_ = position;
            return true;
        }
        if (const roads::Segment* s = SelectedHandle()) {
            position = HandlePoint(*s, (Selected() & kHandleAtStart) != 0);
            heading = 0.0f;
            return true;
        }
        const roads::Node* node = SelectedNode();
        if (!node) return false;
        position = node->position;
        heading = node->hasHeading ? node->heading : AutoHeadingOf(*node);
        // The heading a drag starts from. An automatic heading follows the
        // node as it moves, so only a turn away from this one fixes it.
        if (!dragging_ || dragNode_ != node->id) {
            dragHeading_ = heading;
            dragNode_ = node->id;
        }
        return true;
    }

    bool SetPoint(const Vec3& position, float heading, bool commit) override {
        if (commit && wizard_ == kWizardPreview) wizardRefresh_ = true;   // once the move is in
        if (terrain::Patch* patch = SelectedPatchMutable()) {
            (void)heading;
            patchDragging_ = !commit;
            const Vec3 now = PatchCentre(*patch);
            terrain_.Move(*patch, position - now);
            if (commit) {
                SaveTerrain();
                Commit(L"move terrain");
                app_.Status(L"Terrain patch moved");
            }
            return true;
        }
        if (roads::Segment* s = SelectedHandleMutable()) {
            // The handle's direction is the node's, so a road through the
            // node stays smooth; its length is this end's own.
            const bool atStart = (Selected() & kHandleAtStart) != 0;
            roads::Node* node = network_.FindNode(atStart ? s->a : s->b);
            if (!node) return true;
            const Vec3 offset{ position.x - node->position.x, position.y - node->position.y, 0.0f };
            const float length = Length(offset);
            if (length > 0.25f) {
                node->hasHeading = true;
                node->heading = Degrees(std::atan2(offset.y, offset.x));
                (atStart ? s->handleA : s->handleB) = std::max(0.5f, length);
            }
            // Up or down: the road leaves the node on that slope -- a ramp.
            // A road going on through the node takes the same slope, so the
            // two meet smoothly.
            const Vec3 current = HandlePoint(*s, atStart);
            if (std::fabs(position.z - current.z) > 1e-3f) {
                const float lift = position.z - node->position.z;
                (atStart ? s->liftSetA : s->liftSetB) = true;
                (atStart ? s->liftA : s->liftB) = lift;
                const float reach = std::max(0.5f, Length(Vec3{ current.x - node->position.x,
                                                                current.y - node->position.y, 0.0f }));
                const float slope = lift / reach;
                const std::vector<u32> around = network_.SegmentsAt(node->id);
                if (around.size() == 2)
                    for (u32 otherId : around) {
                        roads::Segment* other = network_.FindSegment(otherId);
                        if (!other || other == s) continue;
                        const bool otherStart = other->a == node->id;
                        const Vec3 h = HandlePoint(*other, otherStart);
                        const float otherReach = std::max(0.5f, Length(Vec3{ h.x - node->position.x,
                                                                             h.y - node->position.y, 0.0f }));
                        (otherStart ? other->liftSetA : other->liftSetB) = true;
                        (otherStart ? other->liftA : other->liftB) = -slope * otherReach;
                    }
            }
            network_.SetDirty();
            if (commit) Changed(true);
            return true;
        }
        roads::Node* node = SelectedNodeMutable();
        if (!node) return false;
        dragging_ = !commit;
        node->position = position;
        float turn = std::fabs(std::fmod(heading - dragHeading_ + 540.0f, 360.0f) - 180.0f);
        if (turn > 0.05f) {
            node->hasHeading = true;
            node->heading = heading;
        }
        network_.UpdateFollowers();
        network_.SetDirty();
        if (commit) {
            Snap(node->id);
            Changed(true);
        }
        return true;
    }

    // ---- RenderExtension

    void DrawWithWorld(IDirect3DDevice9* device, WorldRenderer& renderer) override {
        if (!visible_) return;
        Rebuild();
        // The street lights, drawn as the world's own models are.
        for (const roads::Pole& pole : poles_) {
            const GpuSection* gpu = nullptr;
            const GpuObject* object = nullptr;
            if (!renderer.FindGpuModel(TextureHash(pole.model), gpu, object)) continue;
            renderer.BeginModels(device, *gpu);
            const D3DMATRIX world = ToD3DMatrix(PoleTransform(pole));
            device->SetTransform(D3DTS_WORLD, &world);
            renderer.DrawModel(device, *object, false);
        }
        if (batches_.empty() && terrainBatches_.empty() && faceRemainder_.empty()) return;
        renderer.BeginUserMesh(device);
        const auto draw = [&](const DrawBatch& batch) {
            renderer.BindUserTexture(device, batch.texture);
            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)batch.vertices.size(),
                                           (UINT)(batch.indices.size() / 3), batch.indices.data(), D3DFMT_INDEX16,
                                           batch.vertices.data(), sizeof(nfs::SolidVertex));
        };
        for (const DrawBatch& batch : batches_) draw(batch);
        for (const DrawBatch& batch : faceRemainder_) draw(batch);
        for (const auto& [id, list] : terrainBatches_)
            for (const DrawBatch& batch : list) draw(batch);
    }

    void DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) override;

    bool Pick(const Ray& ray, int& index, float& distance) const override {
        if (graphView_ && PickGraphNode(ray, index)) {
            distance = 0.0f;
            return true;
        }
        if (!visible_) return false;
        index = -1;
        // The curve handles on show come first, then the nodes: small, and
        // drawn over everything.
        for (const auto& [segment, atStart] : VisibleHandles()) {
            const roads::Segment* s = network_.FindSegment(segment);
            if (!s) continue;
            const Vec3 p = HandlePoint(*s, atStart);
            Aabb box;
            box.Add(p - Vec3{ 1.2f, 1.2f, 1.2f });
            box.Add(p + Vec3{ 1.2f, 1.2f, 1.2f });
            const float hit = IntersectRayAabb(ray, box);
            if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
            index = (int)segment | (atStart ? kHandleAtStart : kHandleAtEnd);
            distance = hit;
        }
        if (index >= 0) {
            distance = 0.0f;
            return true;
        }
        for (const roads::Node& node : network_.Nodes()) {
            const float size = NodeSize(node);
            Aabb box;
            box.Add(node.position - Vec3{ size, size, 0.5f });
            box.Add(node.position + Vec3{ size, size, size * 2.0f });
            const float hit = IntersectRayAabb(ray, box);
            if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
            index = (int)node.id;
            distance = hit;
        }
        if (index >= 0) {
            distance = 0.0f;     // drawn on top, so it wins over the world
            return true;
        }
        for (const DrawBatch& batch : batches_) {
            for (size_t k = 0; k + 2 < batch.indices.size(); k += 3) {
                const float hit = IntersectRayTriangle(ray, batch.vertices[batch.indices[k]].position,
                                                       batch.vertices[batch.indices[k + 1]].position,
                                                       batch.vertices[batch.indices[k + 2]].position);
                if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
                index = (int)batch.owner;
                distance = hit;
            }
        }
        for (const auto& [id, list] : terrainBatches_)
            for (const DrawBatch& batch : list)
                for (size_t k = 0; k + 2 < batch.indices.size(); k += 3) {
                    const float hit = IntersectRayTriangle(ray, batch.vertices[batch.indices[k]].position,
                                                           batch.vertices[batch.indices[k + 1]].position,
                                                           batch.vertices[batch.indices[k + 2]].position);
                    if (hit < 0.0f || (index >= 0 && hit >= distance)) continue;
                    index = kTerrainBit | (int)id;
                    distance = hit;
                }
        if (index >= 0) lastPick_ = ray.origin + ray.direction * distance;
        return index >= 0;
    }

    float PickTolerance() const override { return 1.0f; }

private:
    int Selected() const { return app_.Renderer().ExtensionSelection(this); }
    void Select(int id) { app_.Renderer().SelectExtension(this, id); }

    const roads::Node* SelectedNode() const {
        const int id = Selected();
        return id >= 0 ? network_.FindNode((u32)id) : nullptr;
    }
    roads::Node* SelectedNodeMutable() {
        const int id = Selected();
        return id >= 0 ? network_.FindNode((u32)id) : nullptr;
    }
    const roads::Segment* SelectedSegment() const {
        const int id = Selected();
        return id >= 0 && !(id & kHandleBits) ? network_.FindSegment((u32)id) : nullptr;
    }
    const roads::Segment* SelectedHandle() const {
        const int id = Selected();
        return id >= 0 && (id & kHandleBits) ? network_.FindSegment((u32)(id & ~kHandleBits)) : nullptr;
    }
    roads::Segment* SelectedHandleMutable() {
        const int id = Selected();
        return id >= 0 && (id & kHandleBits) ? network_.FindSegment((u32)(id & ~kHandleBits)) : nullptr;
    }

    Vec3 HandlePoint(const roads::Segment& s, bool atStart) const {
        const roads::Curve curve = network_.CurveOf(s);
        return atStart ? curve.p1 : curve.p2;
    }

    // The handles drawn and pickable: those of the selected road, of the
    // road whose handle is selected, and of every road at the selected node.
    std::vector<std::pair<u32, bool>> VisibleHandles() const {
        std::vector<std::pair<u32, bool>> out;
        if (const roads::Segment* s = SelectedSegment()) {
            out.push_back({ s->id, true });
            out.push_back({ s->id, false });
        } else if (const roads::Segment* h = SelectedHandle()) {
            out.push_back({ h->id, true });
            out.push_back({ h->id, false });
        } else if (const roads::Node* node = SelectedNode()) {
            for (u32 id : network_.SegmentsAt(node->id)) {
                const roads::Segment& s = *network_.FindSegment(id);
                out.push_back({ id, s.a == node->id });
            }
        }
        return out;
    }

    // ---- the bake as the game starts
    //
    // What the last bake (or unbake) was made from, as a fingerprint of the
    // roads' and the terrain's files, kept beside them. When the game is
    // started from the editor and the source has moved on since, it is baked
    // first, so the game always has the roads as they are in the editor. A
    // source that was unbaked on purpose and not changed since stays out.

    std::wstring BakeStampPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\roads-baked-" + region + L".txt";
    }

    std::string SourceStamp() const {
        u64 hash = 1469598103934665603ull;
        const auto mix = [&](const std::string& bytes) {
            for (unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ull; }
            hash ^= 0xFF;
            hash *= 1099511628211ull;
        };
        // The bake's own version: raised when what a bake writes changes
        // (2: ground collision faces up; 3: loose road ends join the game's routes; 4: lane slots as the game fills them; 5: dead ends marked; 6: pieces kept in sections loaded all along them), so the next start bakes again.
        mix("bake 7");
        for (const std::wstring& path : { NetworkPath(), TerrainPath() }) {
            std::ifstream in(path, std::ios::binary);
            mix(in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string());
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)hash);
        return text;
    }

    void WriteBakeStamp() {
        if (!app_.World() || app_.ModelsFolder().empty()) return;
        std::ofstream out(BakeStampPath(), std::ios::binary | std::ios::trunc);
        out << SourceStamp() << "\n";
    }

    bool NeedsBake() const {
        if (!app_.World() || app_.ModelsFolder().empty()) return false;
        if (!ground_.Empty()) return true;                 // sculpted ground not yet in the game
        std::ifstream in(BakeStampPath(), std::ios::binary);
        std::string stamp;
        if (in) in >> stamp;
        if (!in.is_open()) {
            // Never baked: only when there is something to bake.
            return !network_.Segments().empty() || !terrain_.Patches().empty() || !network_.GameJunctions().empty();
        }
        return stamp != SourceStamp();
    }

    void OnBeforeLaunch() override {
        SaveNetwork();
        SaveTerrain();
        if (!NeedsBake()) return;
        LOG_INFO("roads: the roads or terrain changed since the last bake -- baking before the game starts");
        app_.Status(L"Baking the roads and terrain before the game starts...");
        Bake();
    }

    std::wstring NetworkPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\roads-" + region + L".txt";
    }

    void SaveNetwork() {
        if (!app_.World() || app_.ModelsFolder().empty()) return;
        if (network_.Nodes().empty() && ::GetFileAttributesW(NetworkPath().c_str()) == INVALID_FILE_ATTRIBUTES)
            return;
        if (!network_.Save(NetworkPath()))
            app_.Status(L"Could not write " + NetworkPath());
    }

    // After an edit: saved at once, the tree and the status brought up to date.
    void Changed(bool save) {
        network_.UpdateFollowers();
        network_.SetDirty();
        if (save) {
            SaveNetwork();
            Commit(L"road edit");
            app_.Edits().RefreshExtra();
        }
        const int selected = Selected();
        app_.RebuildTree();
        if (selected >= 0 && (network_.FindNode((u32)selected) || network_.FindSegment((u32)selected)))
            Select(selected);
        app_.Status(L"Roads changed -- Roads > Bake Roads into the Game (Ctrl+Shift+B) hands them to the game");
    }

    float SegmentLength(const roads::Segment& s) const {
        const roads::Curve curve = network_.CurveOf(s);
        float length = 0.0f;
        Vec3 previous = curve.p0;
        for (int i = 1; i <= 32; ++i) {
            const Vec3 p = curve.At(i / 32.0f);
            length += Length(p - previous);
            previous = p;
        }
        return length;
    }

    float NodeSize(const roads::Node& node) const {
        float widest = 2.0f;
        for (u32 id : network_.SegmentsAt(node.id))
            widest = std::max(widest, network_.FindSegment(id)->profile.HalfRoad() * 0.35f);
        return std::min(widest, 4.0f);
    }

    float AutoHeadingOf(const roads::Node& node) const {
        for (u32 id : network_.SegmentsAt(node.id)) {
            const roads::Segment& s = *network_.FindSegment(id);
            const roads::Curve curve = network_.CurveOf(s);
            const Vec3 d = HorizontalDirection(s.a == node.id ? curve.Tangent(0.0f) : curve.Tangent(1.0f) * -1.0f);
            return Degrees(std::atan2(d.y, d.x));
        }
        return 0.0f;
    }

    // The profile a new road takes: the one it grows out of, or the last edited.
    roads::RoadProfile ProfileAt(u32 node) const {
        const std::vector<u32> around = network_.SegmentsAt(node);
        if (!around.empty()) return network_.FindSegment(around.front())->profile;
        return lastProfile_;
    }

    void NewRoad() {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        Vec3 point;
        if (!app_.ViewCentre(point)) {
            app_.Message(L"Point the middle of the view at the ground where the road should start.");
            return;
        }
        visible_ = true;
        const u32 start = network_.AddNode(point);
        Pull(start);
    }

    // A new node a road's length ahead of `from`, joined to it and selected,
    // for the gizmo to put where it belongs.
    void Pull(u32 from) {
        const roads::Node* origin = network_.FindNode(from);
        if (!origin) return;
        const roads::RoadProfile profile = ProfileAt(from);

        // Straight on from a road end; otherwise the way the camera looks.
        Vec3 direction{};
        const std::vector<u32> around = network_.SegmentsAt(from);
        if (around.size() == 1) {
            const roads::Segment& s = *network_.FindSegment(around.front());
            const roads::Curve curve = network_.CurveOf(s);
            direction = HorizontalDirection(s.a == from ? curve.Tangent(0.0f) * -1.0f : curve.Tangent(1.0f));
        } else {
            direction = HorizontalDirection(app_.View().GetCamera().Forward());
        }
        Vec3 at = origin->position + direction * kPullLength;
        float ground = 0.0f;
        if (app_.View().GroundBelow({ at.x, at.y, at.z + 30.0f }, 0, -1, ground) && std::fabs(ground - at.z) < 25.0f)
            at.z = ground;

        const u32 end = network_.AddNode(at);
        network_.AddSegment(from, end, profile);
        Select((int)end);
        Changed(true);
        app_.Status(L"New road: drag the gizmo's arrows or square to place its end (drop it on a node or a road "
                    L"to join), the ring to turn it; Ctrl+Shift+R pulls the next one");
    }

    // At the end of a two-way road: its two directions go on as two one-way
    // roads, each starting exactly over its own half of the road -- held
    // there by the end node, which they follow -- and each pulled out a
    // little apart, to be laid where the highway's carriageways run.
    void SplitCarriageways() {
        const roads::Node* end = SelectedNode();
        const std::vector<u32> around = end ? network_.SegmentsAt(end->id) : std::vector<u32>{};
        if (!end || around.size() != 1) {
            app_.Message(L"Select the end node of a two-way road (a node with one road) first.");
            return;
        }
        const roads::Segment road = *network_.FindSegment(around.front());
        const roads::RoadProfile& p = road.profile;
        if (p.lanesForward < 1 || p.lanesBackward < 1) {
            app_.Message(L"That road runs one way only; a two-way road can be split into its two directions.");
            return;
        }
        const u32 endId = end->id;
        const bool atEnd = road.b == endId;
        const roads::Curve curve = network_.CurveOf(road);
        // The road's own direction at this end, and the way it goes on.
        const Vec3 along = HorizontalDirection(atEnd ? curve.Tangent(1.0f) : curve.Tangent(0.0f));
        const Vec3 onward = atEnd ? along : along * -1.0f;
        const Vec3 right{ along.y, -along.x, 0.0f };
        roads::Node* node = network_.FindNode(endId);
        node->hasHeading = true;
        node->heading = Degrees(std::atan2(along.y, along.x));

        // Each half's middle, across the road (right of its direction).
        const float h = p.HalfRoad();
        const float back = -h + p.lanesBackward * p.laneWidth * 0.5f;
        const float fore = p.MedianLeft() + p.MedianWidth() + p.lanesForward * p.laneWidth * 0.5f;

        roads::RoadProfile oneWayBack = p, oneWayFore = p;
        for (roads::RoadProfile* q : { &oneWayBack, &oneWayFore }) {
            q->medianType = 0;
            q->medianTexture.clear();
            if (q->poleSides == 4) q->poleSides = 0;
        }
        oneWayBack.lanesForward = 0;
        oneWayBack.sidewalkRight = false;      // its inner side
        oneWayBack.wallRight = false;
        oneWayFore.lanesBackward = 0;
        oneWayFore.sidewalkLeft = false;
        oneWayFore.wallLeft = false;

        const float reach = 40.0f, spread = 8.0f;
        struct Half { float offset; float side; const roads::RoadProfile* profile; };
        const Half halves[2] = { { back, -1.0f, &oneWayBack }, { fore, 1.0f, &oneWayFore } };
        u32 first = 0;
        for (const Half& half : halves) {
            const Vec3 start = node->position + right * half.offset;
            const u32 joint = network_.AddNode(start);
            node = network_.FindNode(endId);          // AddNode may move the nodes
            roads::Node* j = network_.FindNode(joint);
            j->follow = endId;
            j->followOffset = half.offset;
            j->hasHeading = true;
            j->heading = node->heading;
            const Vec3 distant = start + onward * reach + right * (half.side * spread);
            const u32 tip = network_.AddNode({ distant.x, distant.y, start.z });
            // Drawn the same way as the road, so its lanes keep their direction.
            if (atEnd) network_.AddSegment(joint, tip, *half.profile);
            else network_.AddSegment(tip, joint, *half.profile);
            if (!first) first = tip;
        }
        Changed(true);
        Select((int)first);
        app_.Status(L"The two directions now go on as two one-way roads: move their far ends with the gizmo, and "
                    L"pull more road out of them (Ctrl+Shift+R). They stay joined to this road's end.");
    }

    // ---- highway lanes at a node
    //
    // A road's lanes change at a node: one more after it (a lane gained, the
    // extra lane widening out of nothing over the taper length), one fewer (a
    // lane dropped, narrowing away before the node), or the outer lane leaving
    // as a road of its own -- an exit -- while the rest go on. Lanes counted
    // in the direction of travel away from the node; the outer one is on the
    // right.

    // A profile as seen travelling the other way along its road.
    static roads::RoadProfile Flipped(roads::RoadProfile p) {
        std::swap(p.lanesForward, p.lanesBackward);
        std::swap(p.sidewalkLeft, p.sidewalkRight);
        std::swap(p.wallLeft, p.wallRight);
        return p;
    }

    // The road going on from the selected node: with two, the one drawn
    // leaving it; alone, the only one. Null (with a message) when none.
    roads::Segment* RoadOnFrom(const roads::Node& node, bool& leaving) {
        const std::vector<u32> around = network_.SegmentsAt(node.id);
        if (around.empty() || around.size() > 2) {
            app_.Message(L"Select a node with one or two roads (not a junction).");
            return nullptr;
        }
        roads::Segment* chosen = network_.FindSegment(around.back());
        for (u32 id : around) {
            roads::Segment* s = network_.FindSegment(id);
            if (s && s->a == node.id) { chosen = s; break; }
        }
        leaving = chosen && chosen->a == node.id;
        return chosen;
    }

    void ChangeLanesAfterNode(int change) {
        const roads::Node* node = SelectedNode();
        if (!node) {
            app_.Message(L"Select a node first: the lanes change on the road going on from it.");
            return;
        }
        bool leaving = false;
        roads::Segment* road = RoadOnFrom(*node, leaving);
        if (!road) return;
        int& lanes = leaving ? road->profile.lanesForward : road->profile.lanesBackward;
        if (lanes + change < 1) {
            app_.Message(L"That road has one lane that way: it cannot lose it (split it off, or delete the road).");
            return;
        }
        const float halfBefore = road->profile.HalfRoad();
        lanes = std::min(8, lanes + change);
        const std::vector<u32> around = network_.SegmentsAt(node->id);
        const bool through = around.size() == 2;
        if (through) {
            // The lane comes or goes on the outer (right) side only: the
            // other edge stays in line with the road before the node.
            const roads::Segment* before = network_.FindSegment(around[0] == road->id ? around[1] : around[0]);
            const bool sameWay = (road->a == node->id) != (before->a == node->id);
            const float otherShift = sameWay ? before->shift : -before->shift;
            const float otherHalf = before->profile.HalfRoad();
            const float half = road->profile.HalfRoad();
            road->shift = leaving ? otherShift + half - otherHalf : otherShift + otherHalf - half;
        } else {
            road->shift += (road->profile.HalfRoad() - halfBefore) * (leaving ? 1.0f : -1.0f);
        }
        Changed(true);
        Select((int)road->id);
        app_.Status(Fmt(L"Road %u now has %d lanes going on from the node%s", road->id, lanes,
                        through ? (change > 0 ? L": the new lane widens in over the taper length"
                                              : L": the dropped lane narrows away before the node")
                                : L""));
    }

    void SplitOffOuterLane() {
        const roads::Node* selected = SelectedNode();
        if (!selected) {
            app_.Message(L"Select a node first: the outer lane of the road going on from it leaves there.");
            return;
        }
        const u32 nodeId = selected->id;
        const std::vector<u32> around = network_.SegmentsAt(nodeId);
        bool leaving = false;
        roads::Segment* found = RoadOnFrom(*selected, leaving);
        if (!found) return;
        const roads::Segment road = *found;
        const bool alone = around.size() == 1;
        // With one road the lanes go on past its end; with two, the road
        // going on is rebuilt from the split.
        const roads::Curve curve = network_.CurveOf(road);
        Vec3 away;
        bool flip = false;
        if (alone) {
            const bool endsHere = road.b == nodeId;
            away = HorizontalDirection(endsHere ? curve.Tangent(1.0f) : curve.Tangent(0.0f) * -1.0f);
            flip = !endsHere;
        } else {
            away = HorizontalDirection(leaving ? curve.Tangent(0.0f) : curve.Tangent(1.0f) * -1.0f);
            flip = !leaving;
        }
        const roads::RoadProfile p = flip ? Flipped(road.profile) : road.profile;
        if (p.lanesForward < 2) {
            app_.Message(L"The road has a single lane going on from that node: there is no outer lane to split off.");
            return;
        }
        const Vec3 right{ away.y, -away.x, 0.0f };
        roads::RoadProfile main = p, exit = p;
        main.lanesForward -= 1;
        main.sidewalkRight = false;      // the side the exit leaves from
        exit.lanesForward = 1;
        exit.lanesBackward = 0;
        exit.medianType = 0;
        exit.medianTexture.clear();
        exit.sidewalkLeft = false;
        // Walls on both sides of both when the road had one there: between
        // them they start where the two roads have parted (the gore).
        main.wallRight = p.wallRight;
        exit.wallLeft = p.wallRight;
        if (exit.poleSides == 4) exit.poleSides = 0;
        const float half = p.HalfRoad();
        const float shiftAway = flip ? -road.shift : road.shift;   // the road's cross-section off its curve
        const float mainOffset = -half + main.HalfRoad() + shiftAway;
        const float exitOffset = half - exit.HalfRoad() + shiftAway;

        roads::Node* node = network_.FindNode(nodeId);
        node->hasHeading = true;
        node->heading = Degrees(std::atan2(away.y, away.x));
        const auto follower = [&](float offset) {
            const Vec3 start = network_.FindNode(nodeId)->position + right * offset;
            const u32 id = network_.AddNode(start);
            roads::Node* n = network_.FindNode(id);
            n->follow = nodeId;
            n->followOffset = offset;
            n->hasHeading = true;
            n->heading = network_.FindNode(nodeId)->heading;
            return id;
        };
        const u32 mainStart = follower(mainOffset);
        const u32 exitStart = follower(exitOffset);
        const Vec3 mainAt = network_.FindNode(mainStart)->position;
        const Vec3 exitAt = network_.FindNode(exitStart)->position;
        if (alone) {
            const Vec3 tip = mainAt + away * 60.0f;
            network_.AddSegment(mainStart, network_.AddNode({ tip.x, tip.y, mainAt.z }), main);
        } else {
            // The road going on now starts from the main lanes' side.
            const u32 farEnd = leaving ? road.b : road.a;
            if (roads::Segment* r = network_.AddSegment(mainStart, farEnd, main)) {
                r->handleA = leaving ? road.handleA : road.handleB;
                r->handleB = leaving ? road.handleB : road.handleA;
                r->liftSetA = leaving ? road.liftSetA : road.liftSetB;
                r->liftA = leaving ? road.liftA : road.liftB;
                r->liftSetB = leaving ? road.liftSetB : road.liftSetA;
                r->liftB = leaving ? road.liftB : road.liftA;
            }
            network_.RemoveSegment(road.id);
        }
        // The exit: away and out to the right.
        const Vec3 exitTip = exitAt + away * 60.0f + right * 14.0f;
        const u32 tip = network_.AddNode({ exitTip.x, exitTip.y, exitAt.z });
        network_.AddSegment(exitStart, tip, exit);
        Changed(true);
        Select((int)tip);
        app_.Status(L"The outer lane leaves as a road of its own: move its end with the gizmo and pull more road out "
                    L"of it (Ctrl+Shift+R); drop an end on another node to join them.");
    }

    void Extend() {
        u32 from = 0;
        if (const roads::Node* node = SelectedNode()) {
            from = node->id;
        } else if (const roads::Segment* s = SelectedSegment()) {
            // From whichever end is nearer the view centre.
            Vec3 centre;
            const roads::Node* a = network_.FindNode(s->a);
            const roads::Node* b = network_.FindNode(s->b);
            from = (app_.ViewCentre(centre) && Length(a->position - centre) < Length(b->position - centre)) ? s->a
                                                                                                         : s->b;
        } else {
            app_.Status(L"Select a road node first (or Roads > New Road at View Centre)");
            return;
        }
        Pull(from);
    }

    void Split() {
        const roads::Segment* s = SelectedSegment();
        if (!s) {
            app_.Status(L"Click a road where it should be split first.");
            return;
        }
        float t = 0.5f, distance = 0.0f;
        network_.NearestOnSegment(*s, lastPick_, t, distance);
        const u32 node = network_.SplitSegment(s->id, t);
        if (!node) {
            app_.Status(L"Too close to the end of the road to split it there.");
            return;
        }
        Select((int)node);
        Changed(true);
    }

    void AutoHeading() {
        roads::Node* node = SelectedNodeMutable();
        if (!node) return;
        node->hasHeading = false;
        Changed(true);
    }

    // Where a dragged node lands: on another node they become one; on the
    // middle of a road, the road is split there and joined.
    // The node a dragged one would be joined to if let go now, or 0.
    u32 SnapTarget(u32 moved) const {
        const roads::Node* node = network_.FindNode(moved);
        if (!node) return 0;
        const Vec3 at = node->position;
        std::set<u32> neighbours;
        for (u32 id : network_.SegmentsAt(moved)) {
            const roads::Segment& s = *network_.FindSegment(id);
            neighbours.insert(s.a == moved ? s.b : s.a);
        }
        float snap = 3.0f;
        for (u32 id : network_.SegmentsAt(moved))
            snap = std::max(snap, network_.FindSegment(id)->profile.HalfRoad() * 0.75f);
        u32 target = 0;
        float best = snap;
        for (const roads::Node& other : network_.Nodes()) {
            if (other.id == moved || neighbours.count(other.id)) continue;
            if (other.follow == moved || node->follow == other.id) continue;
            const float d = Length(Vec3{ other.position.x - at.x, other.position.y - at.y, 0.0f });
            if (d < best && std::fabs(other.position.z - at.z) < 4.0f) { best = d; target = other.id; }
        }
        return target;
    }

    void Snap(u32 moved) {
        const roads::Node* node = network_.FindNode(moved);
        if (!node) return;
        const Vec3 at = node->position;
        std::set<u32> neighbours;
        for (u32 id : network_.SegmentsAt(moved)) {
            const roads::Segment& s = *network_.FindSegment(id);
            neighbours.insert(s.a == moved ? s.b : s.a);
        }
        float snap = 3.0f;
        for (u32 id : network_.SegmentsAt(moved))
            snap = std::max(snap, network_.FindSegment(id)->profile.HalfRoad() * 0.75f);

        u32 target = 0;
        float best = snap;
        for (const roads::Node& other : network_.Nodes()) {
            if (other.id == moved || neighbours.count(other.id)) continue;
            if (other.follow == moved || node->follow == other.id) continue;
            const float d = Length(Vec3{ other.position.x - at.x, other.position.y - at.y, 0.0f });
            if (d < best && std::fabs(other.position.z - at.z) < 4.0f) { best = d; target = other.id; }
        }
        if (target) {
            network_.MergeNodes(moved, target);
            Select((int)target);
            pulling_ = 0;
            LOG_INFO("roads: node %u joined to node %u", moved, target);
            return;
        }

        // A road it was dropped on, away from that road's ends.
        u32 hitSegment = 0;
        float hitT = 0.0f, hitDistance = 1e30f;
        for (const roads::Segment& s : network_.Segments()) {
            if (s.a == moved || s.b == moved) continue;
            float t = 0.0f, d = 0.0f;
            network_.NearestOnSegment(s, at, t, d);
            if (d < s.profile.HalfRoad() && d < hitDistance && t > 0.05f && t < 0.95f &&
                std::fabs(network_.CurveOf(s).At(t).z - at.z) < 4.0f) {
                hitSegment = s.id;
                hitT = t;
                hitDistance = d;
            }
        }
        if (hitSegment) {
            const u32 middle = network_.SplitSegment(hitSegment, hitT);
            if (middle) {
                network_.MergeNodes(moved, middle);
                roads::Node* joined = network_.FindNode(middle);
                if (joined) joined->hasHeading = false;   // a junction: roads leave it straight
                Select((int)middle);
                LOG_INFO("roads: node %u joined into road %u", moved, hitSegment);
            }
        }
        pulling_ = 0;
    }

    // A mesh's triangles as the viewport draws them.
    static void AddBatches(const roads::MeshPiece& piece, u32 owner, std::vector<DrawBatch>& out,
                           RoadsExtension& self) {
        for (const roads::MeshGroup& group : piece.groups) {
            if (group.vertices.empty() || group.vertices.size() > 65535) continue;
            DrawBatch batch;
            batch.texture = self.TextureHash(group.texture);
            batch.owner = owner;
            batch.vertices.reserve(group.vertices.size());
            for (const roads::MeshVertex& v : group.vertices) {
                nfs::SolidVertex vertex;
                vertex.position = v.position;
                vertex.normal = v.normal;
                vertex.color = v.color;
                vertex.uv = v.uv;
                batch.vertices.push_back(vertex);
            }
            for (u32 i : group.indices) batch.indices.push_back((u16)i);
            out.push_back(std::move(batch));
        }
    }

    void RebuildTerrain(const terrain::Patch& patch) {
        std::vector<DrawBatch>& list = terrainBatches_[patch.id];
        list.clear();
        for (const roads::MeshPiece& piece : terrain_.BuildMesh(patch, &field_, poles_, network_.Light()))
            AddBatches(piece, kTerrainBit | patch.id, list, *this);
    }

    void Rebuild() {
        RebuildRoads();
        if (groundFieldRevision_ != meshRevision_) {
            groundFieldRevision_ = meshRevision_;
            cutsCache_ = network_.Cuts();
            BuildGroundField();
            RefreshGround(nullptr);
        }
        // Ground follows the roads: all of it when they change, the patch
        // being sculpted while a brush works.
        if (terrainBuilt_ != terrain_.Revision() || terrainRoads_ != meshRevision_) {
            if (stroking_ && dirtyPatch_ && terrainRoads_ == meshRevision_) {
                if (const terrain::Patch* patch = terrain_.Find(dirtyPatch_)) RebuildTerrain(*patch);
            } else {
                terrainBatches_.clear();
                for (const terrain::Patch& patch : terrain_.Patches()) RebuildTerrain(patch);
            }
            terrainBuilt_ = terrain_.Revision();
            terrainRoads_ = meshRevision_;
            dirtyPatch_ = 0;
        }
    }

    void RebuildRoads() {
        if (meshRevision_ == network_.Revision()) return;
        meshRevision_ = network_.Revision();
        field_.Build(network_, [](const roads::Segment& segment) { return !segment.profile.bridge; });
        batches_.clear();
        poles_.clear();
        BuildFaceRemainder();
        const std::vector<roads::MeshPiece> pieces = network_.BuildMesh(&poles_);
        for (const roads::MeshPiece& piece : pieces) {
            for (const roads::MeshGroup& group : piece.groups) {
                if (group.vertices.empty() || group.vertices.size() > 65535) continue;
                DrawBatch batch;
                batch.texture = TextureHash(group.texture);
                batch.owner = piece.segment ? piece.segment : piece.node;
                batch.vertices.reserve(group.vertices.size());
                for (const roads::MeshVertex& v : group.vertices) {
                    nfs::SolidVertex out;
                    out.position = v.position;
                    out.normal = v.normal;
                    out.color = v.color;
                    out.uv = v.uv;
                    batch.vertices.push_back(out);
                }
                for (u32 i : group.indices) batch.indices.push_back((u16)i);
                batches_.push_back(std::move(batch));
            }
        }
    }

    u32 TextureHash(const std::string& name) {
        auto found = hashes_.find(name);
        if (found != hashes_.end()) return found->second;
        std::string upper = name;
        for (char& c : upper) c = (char)::toupper((unsigned char)c);
        const u32 hash = TextureNameHash(upper);
        hashes_[name] = hash;
        return hash;
    }

    // Region textures offered in the drop-downs, most used first.
    void CollectTextureChoices(const nfs::World& world) {
        texturesByName_.clear();
        for (const nfs::Texture* texture : world.AllTextures())
            if (texture) texturesByName_[Widen(texture->name)] = texture;
        modelsByName_.clear();
        poleModels_.clear();
        std::vector<std::pair<u32, std::wstring>> lights;
        for (const nfs::SolidObject* model : world.AllModels()) {
            if (!model) continue;
            const std::wstring name = Widen(model->name);
            modelsByName_[name] = model;
            const bool light = model->name.find("LIGHT") != std::string::npos ||
                               model->name.find("LAMP") != std::string::npos ||
                               model->name.find("POLE") != std::string::npos ||
                               model->name.find("POST") != std::string::npos;
            if (light && !model->IsTerrain() && !roads::IsBakedRoadModel(model->name))
                lights.push_back({ model->TriangleCount(), name });
        }
        std::sort(lights.begin(), lights.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        {
            std::map<std::string, u32> ground;
            for (const nfs::SolidObject* model : world.AllModels())
                if (model && model->IsTerrain())
                    for (u32 hash : model->textureHashes)
                        if (const nfs::Texture* texture = world.FindTexture(hash)) ++ground[texture->name];
            std::vector<std::pair<u32, std::string>> ranked;
            for (const auto& [name, uses] : ground) ranked.push_back({ uses, name });
            std::sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            groundTextures_.clear();
            for (const auto& [uses, name] : ranked) groundTextures_.push_back(Widen(name));
            for (const char* extra : { "RDP_INTERSECTMIDTILE", "ARC_SIDEWALKPAVE1", "RDP_GRAVEL_A_CK" })
                if (std::find(groundTextures_.begin(), groundTextures_.end(), Widen(extra)) == groundTextures_.end())
                    groundTextures_.push_back(Widen(extra));
            if (groundTextures_.size() > 150) groundTextures_.resize(150);
        }
        wallModels_.clear();
        wallModels_.push_back(L"-");
        for (const nfs::SolidObject* model : world.AllModels()) {
            if (!model || model->IsTerrain() || roads::IsBakedRoadModel(model->name)) continue;
            const std::string& n = model->name;
            if (n.find("RAIL") != std::string::npos || n.find("GUARD") != std::string::npos ||
                n.find("FENCE") != std::string::npos || n.find("BARRIER") != std::string::npos ||
                n.find("WALL") != std::string::npos)
                wallModels_.push_back(Widen(n));
        }
        pillarModels_.clear();
        pillarModels_.push_back(L"-");
        for (const nfs::SolidObject* model : world.AllModels()) {
            if (!model || model->IsTerrain() || roads::IsBakedRoadModel(model->name)) continue;
            const std::string& n = model->name;
            if (n.find("PILLAR") != std::string::npos || n.find("SUPPORT") != std::string::npos ||
                n.find("COLUMN") != std::string::npos || n.find("PIER") != std::string::npos ||
                n.find("STANCHION") != std::string::npos) {
                // Only models built around their own origin: one built in
                // place in the city would stand far off the bridge.
                const Aabb& box = model->bounds;
                if (box.Valid() && std::fabs(box.min.x + box.max.x) < 40.0f && std::fabs(box.min.y + box.max.y) < 40.0f)
                    pillarModels_.push_back(Widen(n));
            }
        }
        std::sort(pillarModels_.begin() + 1, pillarModels_.end());
        pillarModels_.erase(std::unique(pillarModels_.begin() + 1, pillarModels_.end()), pillarModels_.end());
        std::sort(wallModels_.begin() + 1, wallModels_.end());
        wallModels_.erase(std::unique(wallModels_.begin() + 1, wallModels_.end()), wallModels_.end());
        if (wallModels_.size() > 300) wallModels_.resize(300);
        poleModels_.push_back(L"-");
        for (const auto& light : lights)
            if (poleModels_.empty() || poleModels_.back() != light.second) poleModels_.push_back(light.second);
        if (poleModels_.size() > 300) poleModels_.resize(300);
        lanes_.clear();
        walks_.clear();
        walls_.clear();
        std::vector<std::pair<u32, std::string>> all;
        for (const nfs::Texture* texture : world.AllTextures())
            if (texture) all.push_back({ world.TextureUsage(texture->hash), texture->name });
        std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        const auto has = [](const std::string& name, std::initializer_list<const char*> words) {
            for (const char* word : words)
                if (name.find(word) != std::string::npos) return true;
            return false;
        };
        for (const auto& [uses, name] : all) {
            if (!uses) continue;
            if (name.rfind("RDP_", 0) == 0) lanes_.push_back(Widen(name));
            if (has(name, { "SIDEWALK", "PAVE", "CONCRET", "CURB", "GRAVEL" })) walks_.push_back(Widen(name));
            if (has(name, { "WALL", "BARRIER", "CONCRET", "FENCE", "BRICK" })) walls_.push_back(Widen(name));
        }
        const auto cap = [](std::vector<std::wstring>& list) { if (list.size() > 120) list.resize(120); };
        cap(lanes_);
        cap(walks_);
        cap(walls_);
    }

    // Pictures of textures and models, for the properties panel.
    HBITMAP Picture(const std::wstring& name, int size) {
        if (name.empty() || name == L"-") return nullptr;
        const std::wstring key = name + L"|" + std::to_wstring(size);
        const auto cached = pictures_.find(key);
        if (cached != pictures_.end()) return cached->second;
        HBITMAP picture = nullptr;
        if (const auto texture = texturesByName_.find(name); texture != texturesByName_.end()) {
            std::vector<u32> bgra;
            u32 width = 0, height = 0;
            if (nfs::DecodeTexture(*texture->second, 0, bgra, width, height))
                picture = MakePicture(bgra, width, height, size);
        } else if (const auto model = modelsByName_.find(name); model != modelsByName_.end()) {
            std::vector<u32> bgra;
            if (app_.Thumbnails().Get(app_.Renderer(), *model->second, bgra))
                picture = MakePicture(bgra, ModelThumbnails::kSize, ModelThumbnails::kSize, size);
        } else {
            return nullptr;   // not a picture; not remembered, the region may not be open yet
        }
        pictures_[key] = picture;
        return picture;
    }

    void DropPictures() {
        for (auto& [key, picture] : pictures_)
            if (picture) ::DeleteObject(picture);
        pictures_.clear();
    }

    static Mat4 PoleTransform(const roads::Pole& pole) {
        return nfs::ComposePlacement(pole.position, pole.heading, 0.0f, 0.0f, 1.0f);
    }

    std::wstring PoleRecordPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\roads-" + region + L"-poles.txt";
    }
    bool BakePoles(const std::vector<roads::Pole>& poles, size_t& added, size_t& removed);

    // ---- terrain

    std::wstring TerrainPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\terrain-" + region + L".txt";
    }

    void SaveTerrain() {
        if (!app_.World() || app_.ModelsFolder().empty()) return;
        if (terrain_.Patches().empty() && ::GetFileAttributesW(TerrainPath().c_str()) == INVALID_FILE_ATTRIBUTES)
            return;
        if (!terrain_.Save(TerrainPath())) app_.Status(L"Could not write " + TerrainPath());
    }

    static Vec3 PatchCentre(const terrain::Patch& patch) {
        float sum = 0.0f;
        for (float h : patch.heights) sum += h;
        return { patch.origin.x + patch.columns * patch.cell * 0.5f, patch.origin.y + patch.rows * patch.cell * 0.5f,
                 patch.heights.empty() ? 0.0f : sum / (float)patch.heights.size() };
    }

    const terrain::Patch* SelectedPatch() const {
        const int id = Selected();
        return id >= 0 && (id & kTerrainBit) ? terrain_.Find((u32)(id & ~kTerrainBit)) : nullptr;
    }
    terrain::Patch* SelectedPatchMutable() {
        const int id = Selected();
        return id >= 0 && (id & kTerrainBit) ? terrain_.Find((u32)(id & ~kTerrainBit)) : nullptr;
    }

    bool TerrainUnder(int x, int y, u32& patch, Vec3& point) const {
        const Ray ray = app_.View().RayAt(x, y);
        float best = 1e30f;
        bool found = false;
        for (const auto& [id, list] : terrainBatches_)
            for (const DrawBatch& batch : list)
                for (size_t k = 0; k + 2 < batch.indices.size(); k += 3) {
                    const float hit = IntersectRayTriangle(ray, batch.vertices[batch.indices[k]].position,
                                                           batch.vertices[batch.indices[k + 1]].position,
                                                           batch.vertices[batch.indices[k + 2]].position);
                    if (hit < 0.0f || hit >= best) continue;
                    best = hit;
                    patch = id;
                    found = true;
                }
        if (found) point = ray.origin + ray.direction * best;
        return found;
    }

    void SetBrush(terrain::Brush brush) {
        brush_ = brush;
        stroking_ = false;
        brushHit_ = false;
        app_.UpdateMenuChecks();
        if (brush == terrain::Brush::None) {
            app_.Status(L"Terrain brush off");
        } else {
            app_.Status(Fmt(L"Terrain brush: %s -- hold the left button over a patch; [ and ] change the size "
                            L"(%.0f), Esc puts it away", terrain::BrushName(brush), brushRadius_));
            ::SetFocus(app_.View().Handle());
        }
    }

    void NewTerrain() {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        Vec3 point;
        if (!app_.ViewCentre(point)) {
            app_.Message(L"Point the middle of the view at the ground where the terrain should go.");
            return;
        }
        visible_ = true;
        const u32 id = terrain_.Add(point, 128.0f, 4.0f);
        struct CommitLater { RoadsExtension* self; ~CommitLater() { self->SaveTerrain(); self->Commit(L"new terrain"); } } later{ this };
        if (terrain::Patch* patch = terrain_.Find(id)) {
            // The first layer: a texture the region's own ground uses most.
            if (!groundTextures_.empty()) patch->layers[0].texture = Narrow(groundTextures_.front());
        }
        SaveTerrain();
        Select(kTerrainBit | (int)id);
        if (brush_ == terrain::Brush::None) SetBrush(terrain::Brush::Raise);
        app_.RebuildTree();
        Select(kTerrainBit | (int)id);
    }

    // The game's road textures: its road surfaces, and the lane markings among them.
    static bool IsRoadTexture(const std::string& name) { return name.rfind("RDP_", 0) == 0; }
    static bool IsLaneTexture(const std::string& name) {
        return name.rfind("RDP_LANE", 0) == 0 || name.rfind("RDP_ROADSTRIP", 0) == 0;
    }

    // ---- the game's own ground

    struct GroundModel {
        const nfs::SolidObject* model = nullptr;
        Mat4 toLocal;
        Aabb bounds;
        std::vector<Vec3> world;     // its vertices where the game has them now
        bool shown = false;          // drawn moved
    };

    // ---- the road drawing tool
    //
    // Click by click, as in a road editor: on open ground a node, joined by
    // a road to the one before; on a node, a road into it (and the drawing
    // goes on from there); on a road away from its nodes, a node in it --
    // the road is split there and the new road joins it. Each click is a
    // step of its own for Ctrl+Z. Enter ends the road being drawn, Esc too
    // and then closes the tool.

    void StartDrawTool(u32 from) {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        if (wizard_ != kWizardOff) EndFaceWizard(nullptr);
        visible_ = true;
        SetBrush(terrain::Brush::None);
        drawing_ = false;
        drawTool_ = true;
        drawFrom_ = from;
        drawPending_ = false;
        drawProfile_ = lastProfile_;
        if (from)
            for (u32 id : network_.SegmentsAt(from))
                if (const roads::Segment* s = network_.FindSegment(id)) { drawProfile_ = s->profile; break; }
        ::SetFocus(app_.View().Handle());
        SyncToolbar();
        app_.Status(L"Drawing roads: click open ground for nodes, a node to join it, a road to put a node in it. "
                    L"Enter ends the road, Esc closes the tool.");
    }

    void EndDrawChain() {
        drawFrom_ = 0;
        drawPending_ = false;
    }

    void CloseDrawTool() {
        drawTool_ = false;
        EndDrawChain();
        SyncToolbar();
        app_.Status(L"Road drawing closed");
    }

    // What the cursor is over for the tool: a node, a road (and where along
    // it), or open ground.
    struct DrawHit {
        enum Kind { None, Node, Road, Ground } kind = None;
        u32 node = 0, segment = 0;
        float t = 0.0f;
        Vec3 point;
    };

    DrawHit DrawHitAt(int x, int y) const {
        DrawHit hit;
        const Ray ray = app_.View().RayAt(x, y);
        // A node near the ray.
        float bestMiss = 1e30f;
        for (const roads::Node& node : network_.Nodes()) {
            if (node.follow) continue;
            const Vec3 d = node.position - ray.origin;
            const float t = std::max(0.0f, Dot(d, ray.direction));
            const float miss = Length(d - ray.direction * t);
            if (miss < std::max(NodeSize(node) * 1.3f, t * 0.015f) && miss < bestMiss) {
                bestMiss = miss;
                hit.kind = DrawHit::Node;
                hit.node = node.id;
                hit.point = node.position;
            }
        }
        if (hit.kind == DrawHit::Node) return hit;
        // A road's surface.
        float nearest = -1.0f;
        u32 owner = 0;
        for (const DrawBatch& batch : batches_)
            for (size_t k = 0; k + 2 < batch.indices.size(); k += 3) {
                const float d = IntersectRayTriangle(ray, batch.vertices[batch.indices[k]].position,
                                                     batch.vertices[batch.indices[k + 1]].position,
                                                     batch.vertices[batch.indices[k + 2]].position);
                if (d < 0.0f || (nearest >= 0.0f && d >= nearest)) continue;
                nearest = d;
                owner = batch.owner;
            }
        if (nearest >= 0.0f) {
            const Vec3 at = ray.origin + ray.direction * nearest;
            if (const roads::Segment* s = network_.FindSegment(owner)) {
                float t = 0.0f, distance = 0.0f;
                network_.NearestOnSegment(*s, at, t, distance);
                if (t > 0.03f && t < 0.97f) {
                    hit.kind = DrawHit::Road;
                    hit.segment = s->id;
                    hit.t = t;
                    hit.point = network_.CurveOf(*s).At(t);
                    return hit;
                }
            }
        }
        Vec3 ground;
        if (AnythingUnder(x, y, ground)) {
            hit.kind = DrawHit::Ground;
            hit.point = ground;
        }
        return hit;
    }

    // The road being drawn, from the last node (or the first click) to `to`.
    bool DrawRoadTo(u32 to) {
        if (drawPending_) {
            const u32 start = network_.AddNode(drawStart_);
            drawPending_ = false;
            drawFrom_ = start;
        }
        if (!drawFrom_ || drawFrom_ == to) return false;
        for (u32 id : network_.SegmentsAt(drawFrom_)) {
            const roads::Segment* s = network_.FindSegment(id);
            if (s && (s->a == to || s->b == to)) return false;     // already joined
        }
        return network_.AddSegment(drawFrom_, to, drawProfile_) != nullptr;
    }

    bool DrawToolMouseDown(int x, int y) {
        const DrawHit hit = DrawHitAt(x, y);
        const bool drawingRoad = drawFrom_ || drawPending_;
        switch (hit.kind) {
            case DrawHit::Node:
                if (!drawingRoad) {
                    // Start from it, with the profile of a road it has.
                    StartDrawTool(hit.node);
                    Select((int)hit.node);
                    return true;
                }
                if (DrawRoadTo(hit.node)) {
                    Changed(true);
                    app_.Status(Fmt(L"Joined to node %u -- go on from it, or Enter to end the road", hit.node));
                }
                drawFrom_ = hit.node;
                Select((int)hit.node);
                return true;
            case DrawHit::Road: {
                const u32 middle = network_.SplitSegment(hit.segment, hit.t);
                if (!middle) return true;
                if (roads::Node* joined = network_.FindNode(middle)) joined->hasHeading = false;
                if (drawingRoad) {
                    DrawRoadTo(middle);
                    EndDrawChain();
                    app_.Status(Fmt(L"A node put in road %u and the new road joined to it", hit.segment));
                } else {
                    drawFrom_ = middle;    // and on from it, if the next click is elsewhere
                    app_.Status(Fmt(L"A node put in road %u: click on to draw a road out of it, Enter to stop",
                                    hit.segment));
                }
                Changed(true);
                Select((int)middle);
                return true;
            }
            case DrawHit::Ground: {
                if (!drawingRoad) {
                    drawPending_ = true;
                    drawStart_ = hit.point;
                    app_.Status(L"Road started: click where it goes next");
                    return true;
                }
                const u32 node = network_.AddNode(hit.point);
                DrawRoadTo(node);
                drawFrom_ = node;
                Changed(true);
                Select((int)node);
                return true;
            }
            default: return true;
        }
    }

    // The tool's panel over the view.
    void DrawToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##drawroads", nullptr, flags)) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImGui::TextColored(accent, "Draw roads");
            ImGui::SameLine();
            ImGui::TextDisabled("%d+%d lanes, %.1f wide", drawProfile_.lanesForward, drawProfile_.lanesBackward,
                                drawProfile_.HalfWidth() * 2.0f);
            ImGui::Separator();
            if (drawFrom_ || drawPending_)
                ImGui::TextUnformatted("Click where the road goes next. On a node it joins it; on a road it makes a node there.");
            else
                ImGui::TextUnformatted("Click open ground to start a road, a node to draw from it, a road to put a node in it.");
            ImGui::TextDisabled("Green: a node to join   Cyan: a node goes into that road   White: a new node");
            ImGui::Spacing();
            ImGui::BeginDisabled(!drawFrom_ && !drawPending_);
            if (ImGui::Button("End this road  (Enter)")) EndDrawChain();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close the tool  (Esc)")) CloseDrawTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // ---- the face junction wizard
    //
    // A junction made of the game's own faces, in three steps: the game's
    // road mesh to edit, the faces of it in the new road's way, and the end
    // node of the procedural road that joins there. From then on the result
    // is worked out on the spot and drawn as a wireframe over the untouched
    // game mesh -- faces can still be added or taken away, or another node
    // chosen -- until Enter makes it part of the roads.

    enum WizardStep { kWizardOff = 0, kWizardModel, kWizardFaces, kWizardNode, kWizardPreview };
    static constexpr float kUnderRoad = 8.0f;   // how far back under the new road the game's ground goes

    void StartFaceWizard() {
        if (!app_.World() || groundModels_.empty()) {
            app_.Message(L"Open a region first.");
            return;
        }
        visible_ = true;
        SetBrush(terrain::Brush::None);
        drawing_ = false;
        wizard_ = kWizardModel;
        wizardModel_ = -1;
        wizardFaces_.clear();
        wizardNode_ = 0;
        wizardHover_ = -1;
        wizardPiece_ = {};
        ::SetFocus(app_.View().Handle());
        WizardStatus();
    }

    void WizardStatus() {
        switch (wizard_) {
            case kWizardModel:
                app_.Status(L"Junction from game faces, 1 of 3: click the game's road mesh to edit (Esc cancels)");
                break;
            case kWizardFaces:
                app_.Status(Fmt(L"Junction from game faces, 2 of 3: click or drag over the faces in the new road's way "
                                L"(%zu chosen; a chosen one unchooses). Enter: next, Backspace: back, Esc: cancel",
                                wizardFaces_.size()));
                break;
            case kWizardNode:
                app_.Status(L"Junction from game faces, 3 of 3: click the end node of the procedural road that joins "
                            L"here. Backspace: back, Esc: cancel");
                break;
            case kWizardPreview:
                app_.Status(Fmt(L"Junction preview (%zu faces): green is what replaces them, the game mesh is untouched. "
                                L"Click faces to change, Backspace for another node, Enter applies, Esc cancels",
                                wizardFaces_.size()));
                break;
            default: break;
        }
    }

    // The wizard's steps, drawn at the top of the view: where it is, what to
    // do now, and buttons for the keys.
    void DrawHud(float x, float y, float width, float height) override {
        (void)height;
        if (drawTool_ && wizard_ == kWizardOff) {
            DrawToolHud(x, y, width);
            return;
        }
        if (wizard_ == kWizardOff) return;
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##facewizard", nullptr, flags)) {
            ImGui::TextUnformatted("Junction into a game road");
            ImGui::Separator();
            const char* names[4] = { "1  Game mesh", "2  Faces in the way", "3  Road end", "4  Preview" };
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            for (int k = 0; k < 4; ++k) {
                const int step = k + 1;
                if (k) ImGui::SameLine(0.0f, 14.0f);
                if (step == wizard_) ImGui::TextColored(accent, "%s", names[k]);
                else if (step < wizard_) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", names[k]);
                else ImGui::TextDisabled("%s", names[k]);
            }
            ImGui::Spacing();
            switch (wizard_) {
                case kWizardModel:
                    ImGui::TextUnformatted("Click the game's road where the new road joins it.");
                    break;
                case kWizardFaces:
                    ImGui::Text("Click or drag over the faces in the new road's way (red).  %zu chosen", wizardFaces_.size());
                    ImGui::TextDisabled("A chosen face clicked again is let go. Kerbs beside them come along.");
                    break;
                case kWizardNode:
                    ImGui::TextUnformatted("Click the end node of the procedural road that joins here.");
                    break;
                case kWizardPreview:
                    ImGui::Text("Green: the new surface, orange: kerbs. %zu faces; click faces to change them.",
                                wizardFaces_.size());
                    ImGui::TextUnformatted("Drag the road end's gizmo to move it: the preview follows when let go.");
                    ImGui::TextDisabled("The game's mesh is not changed until the roads are baked.");
                    break;
                default: break;
            }
            ImGui::Spacing();
            if (wizard_ > kWizardModel && ImGui::Button("Back  (Backspace)")) WizardKey(VK_BACK);
            if (wizard_ == kWizardFaces || wizard_ == kWizardPreview) {
                if (wizard_ > kWizardModel) ImGui::SameLine();
                ImGui::BeginDisabled(wizardFaces_.empty());
                if (ImGui::Button(wizard_ == kWizardPreview ? "Apply  (Enter)" : "Next  (Enter)")) WizardKey(VK_RETURN);
                ImGui::EndDisabled();
            }
            if (wizard_ > kWizardModel) ImGui::SameLine();
            if (ImGui::Button("Cancel  (Esc)")) WizardKey(VK_ESCAPE);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    void EndFaceWizard(const wchar_t* status) {
        wizard_ = kWizardOff;
        wizardPainting_ = false;
        wizardFaces_.clear();
        wizardPiece_ = {};
        if (status) app_.Status(status);
    }

    // The nearest of the game's ground triangles along a ray, of one mesh
    // (`only`) or any.
    bool RayGround(const Ray& ray, int only, int& model, size_t& triangle) const {
        float best = -1.0f;
        for (size_t m = 0; m < groundModels_.size(); ++m) {
            if (only >= 0 && (int)m != only) continue;
            const GroundModel& g = groundModels_[m];
            Aabb box = g.bounds;
            box.Add(g.bounds.min - Vec3{ 0.5f, 0.5f, 0.5f });
            box.Add(g.bounds.max + Vec3{ 0.5f, 0.5f, 0.5f });
            if (IntersectRayAabb(ray, box) < 0.0f) continue;
            const auto& indices = g.model->indices;
            for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                const float hit = IntersectRayTriangle(ray, g.world[indices[k]], g.world[indices[k + 1]],
                                                       g.world[indices[k + 2]]);
                if (hit < 0.0f || (best >= 0.0f && hit >= best)) continue;
                best = hit;
                model = (int)m;
                triangle = k / 3;
            }
        }
        return best >= 0.0f;
    }

    std::string TriangleTexture(const GroundModel& g, size_t triangle) const {
        const nfs::SolidObject& model = *g.model;
        const size_t index = triangle * 3;
        for (const nfs::SolidMaterial& material : model.materials) {
            if (index < material.firstIndex || index >= (size_t)material.firstIndex + material.indexCount) continue;
            const u32 hash = material.textureIndex < model.textureHashes.size() ? model.textureHashes[material.textureIndex] : 0;
            const nfs::Texture* texture = hash && app_.World() ? app_.World()->FindTexture(hash) : nullptr;
            return texture ? texture->name : std::string();
        }
        return {};
    }

    static roads::UvMap MapOf(const roads::GameFace& f) {
        roads::UvMap map;
        const Vec3 &a = f.p[0], &b = f.p[1], &c = f.p[2];
        const float det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::fabs(det) < 1e-6f) return map;
        const auto gradient = [&](float fa, float fb, float fc) {
            const float d1 = fb - fa, d2 = fc - fa;
            return Vec2{ (d1 * (c.y - a.y) - d2 * (b.y - a.y)) / det, (d2 * (b.x - a.x) - d1 * (c.x - a.x)) / det };
        };
        map.gu = gradient(f.uv[0].x, f.uv[1].x, f.uv[2].x);
        map.gv = gradient(f.uv[0].y, f.uv[1].y, f.uv[2].y);
        map.origin = a;
        map.uv = f.uv[0];
        map.valid = true;
        return map;
    }

    roads::GameFace FaceOf(const GroundModel& g, size_t triangle) const {
        roads::GameFace f;
        const auto& indices = g.model->indices;
        for (int k = 0; k < 3; ++k) {
            const u32 i = indices[triangle * 3 + k];
            f.p[k] = g.world[i];
            f.uv[k] = g.model->vertices[i].uv;
        }
        f.texture = TriangleTexture(g, triangle);
        return f;
    }

    static float FacePlanArea(const roads::GameFace& f) {
        return std::fabs((f.p[1].x - f.p[0].x) * (f.p[2].y - f.p[0].y) - (f.p[2].x - f.p[0].x) * (f.p[1].y - f.p[0].y)) * 0.5f;
    }

    // The junction as chosen so far.
    roads::FaceJunction MakeFaceJunction() const {
        roads::FaceJunction j;
        j.node = wizardNode_;
        if (wizardModel_ < 0 || wizardModel_ >= (int)groundModels_.size()) return j;
        const GroundModel& g = groundModels_[(size_t)wizardModel_];
        j.model = g.model->name;
        const auto& indices = g.model->indices;
        const size_t count = indices.size() / 3;
        std::set<size_t> chosen = wizardFaces_;
        // The game's ground under the new road's first metres goes too, so
        // nothing runs on underneath where the road rises from the game's.
        {
            Vec3 into;
            const roads::Segment* segment = nullptr;
            const roads::Node* node = network_.FindNode(wizardNode_);
            if (node && network_.EndDirection(wizardNode_, into, segment)) {
                const Vec3 right{ into.y, -into.x, 0.0f };
                const float half = segment->profile.HalfWidth();
                for (size_t t = 0; t < count; ++t) {
                    const Vec3 c = (g.world[indices[t * 3]] + g.world[indices[t * 3 + 1]] + g.world[indices[t * 3 + 2]]) *
                                   (1.0f / 3.0f);
                    const Vec3 d{ c.x - node->position.x, c.y - node->position.y, 0.0f };
                    const float along = Dot(d, into), across = Dot(d, right);
                    if (along < 0.0f && along > -kUnderRoad && std::fabs(across) <= half &&
                        c.z < node->position.z + 0.5f && c.z > node->position.z - 3.0f)
                        chosen.insert(t);
                }
            }
        }
        // The kerbs' standing faces along the chosen ones come with them:
        // through the new road they go, elsewhere they come back as they were.
        std::vector<Vec3> corners;
        for (size_t t : chosen)
            for (int k = 0; k < 3; ++k) corners.push_back(g.world[indices[t * 3 + k]]);
        const auto shared = [&](const Vec3& p) {
            for (const Vec3& q : corners)
                if (std::fabs(p.x - q.x) < 0.01f && std::fabs(p.y - q.y) < 0.01f && std::fabs(p.z - q.z) < 0.01f) return true;
            return false;
        };
        for (size_t t = 0; t < count; ++t) {
            if (chosen.count(t)) continue;
            const roads::GameFace f = FaceOf(g, t);
            if (FacePlanArea(f) > 5e-4f) continue;
            int touching = 0;
            for (const Vec3& p : f.p) touching += shared(p);
            if (touching >= 2) chosen.insert(t);
        }
        Vec3 middle{};
        for (size_t t : chosen) {
            j.faces.push_back(FaceOf(g, t));
            middle = middle + (j.faces.back().p[0] + j.faces.back().p[1] + j.faces.back().p[2]) * (1.0f / 3.0f);
        }
        if (!j.faces.empty()) middle = middle * (1.0f / j.faces.size());

        // The game's road direction: where its lane textures' V runs (as an
        // axis, the doubled angle), from the chosen faces or around them.
        const auto direction = [&](bool nearby) {
            float sx = 0.0f, sy = 0.0f;
            const auto add = [&](const roads::GameFace& f) {
                if (!IsLaneTexture(f.texture)) return;
                const roads::UvMap map = MapOf(f);
                if (!map.valid) return;
                const float l = std::sqrt(map.gv.x * map.gv.x + map.gv.y * map.gv.y);
                if (l < 1e-5f) return;
                const float angle = std::atan2(map.gv.y, map.gv.x);
                const float w = FacePlanArea(f);
                sx += std::cos(2.0f * angle) * w;
                sy += std::sin(2.0f * angle) * w;
            };
            if (!nearby) for (const roads::GameFace& f : j.faces) add(f);
            else
                for (size_t t = 0; t < count; ++t) {
                    const Vec3& p = g.world[indices[t * 3]];
                    if (std::fabs(p.x - middle.x) > 30.0f || std::fabs(p.y - middle.y) > 30.0f) continue;
                    add(FaceOf(g, t));
                }
            if (std::fabs(sx) + std::fabs(sy) < 1e-6f) return false;
            const float angle = 0.5f * std::atan2(sy, sx);
            j.gameForward = { std::cos(angle), std::sin(angle), 0.0f };
            return true;
        };
        if (!direction(false)) direction(true);

        // The surface laid where there was none: the game's own plain road
        // texture there, carried on with its own mapping.
        float bestArea = 0.0f;
        const auto consider = [&](const roads::GameFace& f) {
            if (!IsRoadTexture(f.texture) || IsLaneTexture(f.texture)) return;
            const float area = FacePlanArea(f);
            if (area <= bestArea) return;
            bestArea = area;
            j.asphalt = f.texture;
            j.asphaltMap = MapOf(f);
        };
        for (const roads::GameFace& f : j.faces) consider(f);
        if (j.asphalt.empty())
            for (size_t t = 0; t < count; ++t) {
                const Vec3& p = g.world[indices[t * 3]];
                if (std::fabs(p.x - middle.x) > 30.0f || std::fabs(p.y - middle.y) > 30.0f) continue;
                consider(FaceOf(g, t));
            }
        return j;
    }

    void WizardPreview() {
        wizardPiece_ = {};
        if (wizard_ != kWizardPreview) return;
        network_.BuildFaceJunctionPiece(MakeFaceJunction(), wizardPiece_);
    }

    void WizardToggle(size_t triangle, bool add) {
        const bool has = wizardFaces_.count(triangle) != 0;
        if (has == add) return;
        if (add) wizardFaces_.insert(triangle);
        else wizardFaces_.erase(triangle);
        WizardPreview();
        WizardStatus();
    }

    bool WizardMouseDown(int x, int y) {
        const Ray ray = app_.View().RayAt(x, y);
        if (wizard_ == kWizardModel) {
            int model = -1;
            size_t triangle = 0;
            if (!RayGround(ray, -1, model, triangle)) {
                app_.Status(L"No game ground there: click a road of the game");
                return true;
            }
            wizardModel_ = model;
            wizard_ = kWizardFaces;
            WizardStatus();
            return true;
        }
        if (wizard_ == kWizardFaces || wizard_ == kWizardPreview) {
            // In the preview the road's end node has the gizmo: a click on
            // it moves the node, and the preview follows when let go.
            if (wizard_ == kWizardPreview && app_.View().GizmoHot()) return false;
            int model = -1;
            size_t triangle = 0;
            if (!RayGround(ray, wizardModel_, model, triangle)) return true;
            wizardPainting_ = true;
            wizardPaintAdd_ = wizardFaces_.count(triangle) == 0;
            WizardToggle(triangle, wizardPaintAdd_);
            return true;
        }
        if (wizard_ == kWizardNode) {
            u32 best = 0;
            float bestDistance = 1e30f;
            for (const roads::Node& node : network_.Nodes()) {
                if (network_.SegmentsAt(node.id).size() != 1) continue;   // a road's end only
                const Vec3 d = node.position - ray.origin;
                const float t = std::max(0.0f, Dot(d, ray.direction));
                const float miss = Length(d - ray.direction * t);
                if (miss < std::max(3.0f, t * 0.02f) && miss < bestDistance) { bestDistance = miss; best = node.id; }
            }
            if (!best) {
                app_.Status(L"No road end there: click the end node of a procedural road (the end that joins here)");
                return true;
            }
            wizardNode_ = best;
            wizard_ = kWizardPreview;
            Select((int)best);       // its gizmo moves it
            WizardPreview();
            WizardStatus();
            return true;
        }
        return true;
    }

    void WizardMouseMove(int x, int y) {
        if (wizard_ != kWizardFaces && wizard_ != kWizardPreview) return;
        const Ray ray = app_.View().RayAt(x, y);
        int model = -1;
        size_t triangle = 0;
        wizardHover_ = RayGround(ray, wizardModel_, model, triangle) ? (long long)triangle : -1;
        if (wizardPainting_ && wizardHover_ >= 0) WizardToggle((size_t)wizardHover_, wizardPaintAdd_);
    }

    bool WizardKey(WPARAM key) {
        if (key == VK_ESCAPE) {
            EndFaceWizard(L"Junction from game faces cancelled");
            return true;
        }
        if (key == VK_BACK) {
            if (wizard_ == kWizardPreview) { wizard_ = kWizardNode; wizardNode_ = 0; wizardPiece_ = {}; }
            else if (wizard_ == kWizardNode) wizard_ = kWizardFaces;
            else if (wizard_ == kWizardFaces) { wizard_ = kWizardModel; wizardFaces_.clear(); wizardModel_ = -1; }
            WizardStatus();
            return true;
        }
        if (key == VK_RETURN) {
            if (wizard_ == kWizardFaces) {
                if (wizardFaces_.empty()) { app_.Status(L"Choose at least one face first"); return true; }
                wizard_ = kWizardNode;
                WizardStatus();
            } else if (wizard_ == kWizardPreview) {
                ApplyFaceWizard();
            }
            return true;
        }
        return false;
    }

    void ApplyFaceWizard() {
        const roads::FaceJunction j = MakeFaceJunction();
        if (j.faces.empty() || !network_.FindNode(j.node)) {
            app_.Status(L"Nothing to apply: choose faces and a road end");
            return;
        }
        network_.AddFaceJunction(j);
        const size_t faces = j.faces.size();
        EndFaceWizard(nullptr);
        Changed(true);
        Select((int)j.node);
        LOG_INFO("roads: junction from %zu faces of %s at node %u (asphalt %s)", faces, j.model.c_str(), j.node,
                 j.asphalt.c_str());
        app_.Status(Fmt(L"Junction made from %zu faces of the game's road: they are replaced when the roads are baked "
                        L"(Ctrl+Z takes it back; the Edits tab lists it)", faces));
    }

    void DrawWizard(IDirect3DDevice9* device, WorldRenderer& renderer) {
        if (wizard_ == kWizardOff || wizardModel_ < 0 || wizardModel_ >= (int)groundModels_.size()) return;
        if (wizardRefresh_) {
            wizardRefresh_ = false;
            WizardPreview();
        }
        struct Vertex { float x, y, z; D3DCOLOR colour; };
        std::vector<Vertex> lines;
        const auto line = [&](const Vec3& a, const Vec3& b, D3DCOLOR c) {
            lines.push_back({ a.x, a.y, a.z, c });
            lines.push_back({ b.x, b.y, b.z, c });
        };
        const GroundModel& g = groundModels_[(size_t)wizardModel_];
        const auto& indices = g.model->indices;
        const Vec3 lift{ 0.0f, 0.0f, 0.04f };
        const auto triangle = [&](size_t t, D3DCOLOR c) {
            if (t * 3 + 2 >= indices.size()) return;
            const Vec3 a = g.world[indices[t * 3]] + lift, b = g.world[indices[t * 3 + 1]] + lift,
                       d = g.world[indices[t * 3 + 2]] + lift;
            line(a, b, c);
            line(b, d, c);
            line(d, a, c);
        };
        if (wizard_ == kWizardFaces || wizard_ == kWizardModel) {
            // The mesh being edited, faintly.
            for (size_t t = 0; t * 3 + 2 < indices.size(); ++t) triangle(t, 0x30FFFFFF);
        }
        for (size_t t : wizardFaces_) triangle(t, wizard_ == kWizardPreview ? 0x80FF4040 : 0xFFFF4040);
        if (wizardHover_ >= 0) triangle((size_t)wizardHover_, 0xFFFFFF40);
        if (wizardNode_)
            if (const roads::Node* node = network_.FindNode(wizardNode_))
                line(node->position, node->position + Vec3{ 0, 0, 6.0f }, 0xFFFFFFFF);
        // The result, as a wireframe.
        const Vec3 above{ 0.0f, 0.0f, 0.07f };
        for (const roads::MeshGroup& group : wizardPiece_.groups)
            for (size_t k = 0; k + 2 < group.indices.size(); k += 3) {
                const Vec3 a = group.vertices[group.indices[k]].position + above;
                const Vec3 b = group.vertices[group.indices[k + 1]].position + above;
                const Vec3 c = group.vertices[group.indices[k + 2]].position + above;
                const D3DCOLOR colour = roads::IsBakedRoadModel(group.texture) ? 0xFF40FF80
                                        : group.texture.rfind("RDP_", 0) == 0 ? 0xFF40FF80 : 0xFF40C0FF;
                line(a, b, colour);
                line(b, c, colour);
                line(c, a, colour);
            }
        for (const roads::CollisionWall& wall : wizardPiece_.walls)
            for (size_t k = 0; k + 1 < wall.points.size(); ++k)
                line(wall.points[k] + Vec3{ 0, 0, wall.height + 0.08f }, wall.points[k + 1] + Vec3{ 0, 0, wall.height + 0.08f },
                     0xFFFF9020);
        if (lines.empty()) return;
        renderer.BeginOverlay(device);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        const size_t batch = 60000;
        for (size_t at = 0; at < lines.size(); at += batch) {
            const size_t n = std::min(batch, lines.size() - at);
            device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(n / 2), lines.data() + at, sizeof(Vertex));
        }
        renderer.EndOverlay(device);
    }

    // The game meshes face junctions are made of are drawn without the faces
    // they replace, in the editor only: the game's own copy is left alone
    // until the bake.
    void BuildFaceRemainder() {
        faceRemainder_.clear();
        faceHidden_.clear();
        std::map<std::string, std::vector<const roads::FaceJunction*>> byModel;
        for (const roads::FaceJunction& j : network_.FaceJunctions())
            if (network_.FindNode(j.node) && !j.faces.empty()) byModel[j.model].push_back(&j);
        for (const auto& [name, junctions] : byModel) {
            const GroundModel* found = nullptr;
            for (const GroundModel& g : groundModels_)
                if (g.model->name == name) { found = &g; break; }
            if (!found) continue;
            const GroundModel& g = *found;
            std::vector<Vec3> centres;
            Aabb box;
            for (const roads::FaceJunction* j : junctions)
                for (const roads::GameFace& f : j->faces) {
                    centres.push_back((f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f));
                    box.Add(centres.back());
                }
            const auto removed = [&](const Vec3& c) {
                if (c.x < box.min.x - 0.1f || c.x > box.max.x + 0.1f || c.y < box.min.y - 0.1f || c.y > box.max.y + 0.1f)
                    return false;
                for (const Vec3& q : centres)
                    if (std::fabs(c.x - q.x) < 0.01f && std::fabs(c.y - q.y) < 0.01f && std::fabs(c.z - q.z) < 0.01f)
                        return true;
                return false;
            };
            faceHidden_.insert(g.model->hash);
            const nfs::SolidObject& model = *g.model;
            for (const nfs::SolidMaterial& material : model.materials) {
                DrawBatch batch;
                batch.texture = material.textureIndex < model.textureHashes.size() ? model.textureHashes[material.textureIndex] : 0;
                std::unordered_map<u32, u16> remap;
                const size_t last = std::min<size_t>((size_t)material.firstIndex + material.indexCount, model.indices.size());
                const auto flush = [&] {
                    if (!batch.indices.empty()) faceRemainder_.push_back(batch);
                    batch.vertices.clear();
                    batch.indices.clear();
                    remap.clear();
                };
                for (size_t k = material.firstIndex; k + 2 < last; k += 3) {
                    const u32 t[3] = { model.indices[k], model.indices[k + 1], model.indices[k + 2] };
                    if (removed((g.world[t[0]] + g.world[t[1]] + g.world[t[2]]) * (1.0f / 3.0f))) continue;
                    if (batch.vertices.size() > 65000) flush();
                    for (u32 i : t) {
                        auto it = remap.find(i);
                        if (it == remap.end()) {
                            nfs::SolidVertex v = model.vertices[i];
                            v.position = g.world[i];
                            it = remap.emplace(i, (u16)batch.vertices.size()).first;
                            batch.vertices.push_back(v);
                        }
                        batch.indices.push_back(it->second);
                    }
                }
                flush();
            }
        }
    }

    std::wstring GroundPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\ground-" + region + L".txt";
    }
    std::wstring FittedPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\ground-" + region + L"-fitted.txt";
    }
    void SaveGround() {
        if (!app_.World() || app_.ModelsFolder().empty()) return;
        if (!ground_.Save(GroundPath())) app_.Status(L"Could not write " + GroundPath());
    }

    // A road's shape, so the ground is fitted to it once: fitting again would
    // pull the slopes beside it a little further down every bake.
    std::string Fingerprint(const roads::Segment& segment) const {
        const roads::Curve curve = network_.CurveOf(segment);
        char text[256];
        std::snprintf(text, sizeof(text), "%.1f,%.1f,%.1f|%.1f,%.1f,%.1f|%.1f,%.1f|%.1f,%.1f|%.2f",
                      curve.p0.x, curve.p0.y, curve.p0.z, curve.p3.x, curve.p3.y, curve.p3.z, curve.p1.x, curve.p1.y,
                      curve.p2.x, curve.p2.y, segment.profile.HalfWidth());
        return text;
    }
    void LoadFitted() {
        fitted_.clear();
        std::ifstream in(FittedPath(), std::ios::binary);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty() && line[0] != '#') fitted_.insert(line);
    }
    void SaveFitted() {
        std::ofstream out(FittedPath(), std::ios::binary | std::ios::trunc);
        out << "# roads the game's ground was already fitted to, by shape\n";
        for (const std::string& f : fitted_) out << f << "\n";
    }

    bool Bridged(const roads::Segment& segment) const;

    void BuildGroundField() {
        groundField_.Build(network_, [this](const roads::Segment& segment) {
            return !Bridged(segment) && !segment.profile.keepGround && !fitted_.count(Fingerprint(segment));
        });
    }

    // The game's terrain meshes that can be moved: each drawn in one place
    // only, so moving its vertices moves nothing somewhere else.
    void CollectGroundModels(const nfs::World& world) {
        groundModels_.clear();
        lightGrid_.clear();
        lightGridBuilt_ = false;
        std::unordered_map<u32, int> placements;
        for (const auto& loaded : world.Loaded())
            for (const nfs::SceneryInstance& instance : loaded->scenery.instances)
                ++placements[loaded->scenery.ModelHash(instance)];
        size_t shared = 0;
        for (const auto& loaded : world.Loaded()) {
            if (!loaded->info || world.IsLibrary(*loaded)) continue;
            for (const nfs::SolidList& list : loaded->solidLists)
                for (const nfs::SolidObject& model : list.objects) {
                    if (!model.IsTerrain() || model.vertices.empty() || roads::IsBakedRoadModel(model.name)) continue;
                    if (placements[model.hash] > 1) { ++shared; continue; }
                    GroundModel g;
                    g.model = &model;
                    g.toLocal = InverseAffine(model.transform);
                    g.world.reserve(model.vertices.size());
                    for (const nfs::SolidVertex& v : model.vertices) {
                        g.world.push_back(model.transform.TransformPoint(v.position));
                        g.bounds.Add(g.world.back());
                    }
                    groundModels_.push_back(std::move(g));
                }
        }
        LOG_INFO("ground: %zu terrain meshes can be sculpted (%zu are drawn in several places and are left alone)",
                 groundModels_.size(), shared);
    }

    // The light the game baked into its ground: the vertex colours of the
    // triangle under a point, found through a grid of the terrain's triangles.
    struct TriangleRef { u32 model, first; };
    static constexpr float kLightCell = 8.0f;
    static long long LightKey(int x, int y) { return ((long long)x << 32) ^ (long long)(u32)y; }

    void BuildLightGrid() const {
        lightGrid_.clear();
        for (u32 m = 0; m < groundModels_.size(); ++m) {
            const GroundModel& g = groundModels_[m];
            const auto& indices = g.model->indices;
            for (u32 k = 0; k + 2 < indices.size(); k += 3) {
                const Vec3& a = g.world[indices[k]];
                const Vec3& b = g.world[indices[k + 1]];
                const Vec3& c = g.world[indices[k + 2]];
                const int x0 = (int)std::floor(std::min({ a.x, b.x, c.x }) / kLightCell);
                const int x1 = (int)std::floor(std::max({ a.x, b.x, c.x }) / kLightCell);
                const int y0 = (int)std::floor(std::min({ a.y, b.y, c.y }) / kLightCell);
                const int y1 = (int)std::floor(std::max({ a.y, b.y, c.y }) / kLightCell);
                if (x1 - x0 > 16 || y1 - y0 > 16) continue;   // a huge backdrop triangle
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) lightGrid_[LightKey(x, y)].push_back({ m, k });
            }
        }
        lightGridBuilt_ = true;
    }

    bool GameLightAt(const Vec3& p, u32& colour) const {
        if (!lightGridBuilt_) BuildLightGrid();
        const auto cell = lightGrid_.find(LightKey((int)std::floor(p.x / kLightCell), (int)std::floor(p.y / kLightCell)));
        if (cell == lightGrid_.end()) return false;
        float bestGap = 2.5f;
        bool found = false;
        for (const TriangleRef& ref : cell->second) {
            if (ref.model >= groundModels_.size()) continue;
            const GroundModel& g = groundModels_[ref.model];
            const auto& indices = g.model->indices;
            const u32 ia = indices[ref.first], ib = indices[ref.first + 1], ic = indices[ref.first + 2];
            const Vec3& a = g.world[ia];
            const Vec3& b = g.world[ib];
            const Vec3& c = g.world[ic];
            const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::fabs(d) < 1e-6f) continue;
            const float l1 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
            const float l2 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
            const float l3 = 1.0f - l1 - l2;
            if (l1 < -1e-3f || l2 < -1e-3f || l3 < -1e-3f) continue;
            const float z = a.z * l1 + b.z * l2 + c.z * l3;
            const float gap = std::fabs(z - p.z);
            if (gap > bestGap) continue;
            bestGap = gap;
            const u32 ca = g.model->vertices[ia].color, cb = g.model->vertices[ib].color, cc = g.model->vertices[ic].color;
            colour = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                const float v = ((ca >> shift) & 0xFF) * l1 + ((cb >> shift) & 0xFF) * l2 + ((cc >> shift) & 0xFF) * l3;
                colour |= (u32)std::clamp((int)std::lround(v), 0, 255) << shift;
            }
            found = true;
        }
        return found;
    }

    // The network's light: the game's ground under it, and the game's street
    // lights where there is none.
    void SetGameLight(const nfs::World& world) {
        auto sources = std::make_shared<roads::Network::LightSources>();
        sources->sample = [this](const Vec3& p, u32& colour) { return GameLightAt(p, colour); };
        for (const auto& loaded : world.Loaded())
            for (const nfs::SceneryInstance& instance : loaded->scenery.instances) {
                const nfs::SolidObject* model = world.FindModel(loaded->scenery.ModelHash(instance));
                if (!model) continue;
                const std::string& name = model->name;
                if (name.find("STREETLIGHT") == std::string::npos && name.find("LAMP") == std::string::npos) continue;
                roads::Pole lamp;
                lamp.model = name;
                lamp.position = { instance.transform.m[3][0], instance.transform.m[3][1], instance.transform.m[3][2] };
                lamp.lamp = lamp.position + Vec3{ 0.0f, 0.0f, 7.0f };
                lamp.color = 0xFFD9A0;
                lamp.radius = 14.0f;
                lamp.strength = 1.0f;
                sources->lamps.push_back(lamp);
            }
        LOG_INFO("roads: lit by the game's baked ground light, and %zu of its street lights where there is no "
                 "ground of its under them", sources->lamps.size());
        network_.SetLightSources(sources);
        light_ = sources;
    }

    // Where the game's ground ends up: its height, the brushes' offset, and
    // fitted to the roads it has not been fitted to yet.
    float GroundAt(float x, float y, float z) const {
        float h = z + ground_.At(x, y);
        for (const roads::GameCut& cut : cutsCache_)
            if (cut.Inside({ x, y, h }, -kGroundShoulder)) return h;
        float roadZ = 0.0f, outside = 0.0f;
        if (fitGround_ && !groundField_.Empty() && groundField_.Query(x, y, kGroundShoulder, roadZ, outside) &&
            std::fabs(h - roadZ) < 12.0f) {
            const float bed = roadZ - kGroundSink;
            if (outside <= 0.0f) h = bed;
            else {
                const float t = std::clamp(outside / kGroundShoulder, 0.0f, 1.0f);
                h = bed + (h - bed) * (t * t * (3.0f - 2.0f * t));
            }
        }
        return h;
    }

    // Draws the moved ground: every mesh in `area` (all when null).
    void RefreshGround(const Aabb* area) {
        const auto& cuts = cutsCache_;
        const bool any = !ground_.Empty() || (fitGround_ && !groundField_.Empty()) || !cuts.empty();
        for (GroundModel& g : groundModels_) {
            if (area && (g.bounds.max.x < area->min.x || g.bounds.min.x > area->max.x ||
                         g.bounds.max.y < area->min.y || g.bounds.min.y > area->max.y))
                continue;
            bool moved = false;
            std::vector<Vec3> local;
            if (any) {
                local.reserve(g.world.size());
                for (const Vec3& p : g.world) {
                    float z = GroundAt(p.x, p.y, p.z);
                    for (const roads::GameCut& cut : cuts)
                        if (cut.Inside(p, 0.05f)) z -= 0.6f;   // shown only: the bake cuts it out
                    moved |= std::fabs(z - p.z) > 0.005f;
                    local.push_back(g.toLocal.TransformPoint({ p.x, p.y, z }));
                }
            }
            if (!moved && !g.shown) continue;
            if (!moved) {
                local.clear();
                for (const nfs::SolidVertex& v : g.model->vertices) local.push_back(v.position);
            }
            app_.Renderer().UpdateModelPositions(g.model->hash, local);
            g.shown = moved;
        }
    }

    // The ground under a point as the game has it, before any offset.
    float BaseGroundHeight(float x, float y) const {
        float best = 0.0f, bestGap = 1e30f;
        const float nearZ = brushPoint_.z;
        for (const GroundModel& g : groundModels_) {
            if (x < g.bounds.min.x || x > g.bounds.max.x || y < g.bounds.min.y || y > g.bounds.max.y) continue;
            const auto& indices = g.model->indices;
            for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                const Vec3& a = g.world[indices[k]];
                const Vec3& b = g.world[indices[k + 1]];
                const Vec3& c = g.world[indices[k + 2]];
                const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (std::fabs(d) < 1e-6f) continue;
                const float l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
                const float l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
                const float l3 = 1.0f - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                const float z = a.z * l1 + b.z * l2 + c.z * l3;
                if (std::fabs(z - nearZ) < bestGap) { bestGap = std::fabs(z - nearZ); best = z; }
            }
        }
        return bestGap < 1e29f ? best : nearZ;
    }

    bool GameGroundUnder(int x, int y, Vec3& point) const {
        if (groundModels_.empty()) return false;
        const Ray ray = app_.View().RayAt(x, y);
        PickResult hit;
        if (!app_.Renderer().Pick(ray, hit) || !hit.object || !hit.object->IsTerrain()) return false;
        point = ray.origin + ray.direction * hit.distance;
        return true;
    }

    // The highest ground under `top` at (x, y): the game's terrain as it will
    // be, or a terrain patch.
    bool GroundBelow(float x, float y, float top, float& height) const {
        bool found = false;
        for (const GroundModel& g : groundModels_) {
            if (x < g.bounds.min.x || x > g.bounds.max.x || y < g.bounds.min.y || y > g.bounds.max.y) continue;
            const auto& indices = g.model->indices;
            for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                const Vec3& a = g.world[indices[k]];
                const Vec3& b = g.world[indices[k + 1]];
                const Vec3& c = g.world[indices[k + 2]];
                const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (std::fabs(d) < 1e-6f) continue;
                const float l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
                const float l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
                const float l3 = 1.0f - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                const float z = a.z * l1 + b.z * l2 + c.z * l3 + ground_.At(x, y);
                if (z < top && (!found || z > height)) { height = z; found = true; }
            }
        }
        for (const terrain::Patch& patch : terrain_.Patches()) {
            const float fx = (x - patch.origin.x) / patch.cell, fy = (y - patch.origin.y) / patch.cell;
            if (fx < 0 || fy < 0 || fx > patch.columns || fy > patch.rows) continue;
            const int i = std::min((int)fx, patch.columns - 1), j = std::min((int)fy, patch.rows - 1);
            const float u = fx - i, v = fy - j;
            const float z = (patch.H(i, j) * (1 - u) + patch.H(i + 1, j) * u) * (1 - v) +
                            (patch.H(i, j + 1) * (1 - u) + patch.H(i + 1, j + 1) * u) * v;
            if (z < top && (!found || z > height)) { height = z; found = true; }
        }
        return found;
    }

    // The game's road graph with the network's roads in it: always built from
    // the game's own file, written where the loader serves it in its place.
    bool WriteRoutes(std::wstring& summary) {
        nfs::World* world = app_.World();
        if (!world) return false;
        const std::wstring region = world->Region();
        const std::wstring shipped = app_.GameFolder() + L"\\TRACKS\\ROUTES" + region + L"\\RoutesFreeRoam.bin";
        const std::wstring folder = app_.ModelsFolder() + L"\\files\\TRACKS\\ROUTES" + region;
        const std::wstring served = folder + L"\\RoutesFreeRoam.bin";
        std::vector<u8> bytes;
        {
            std::ifstream in(shipped, std::ios::binary);
            if (!in) { summary = L"the game's road graph " + shipped + L" could not be read"; return false; }
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nfs::routes::Graph graph;
        std::wstring error;
        if (!graph.Parse(bytes, error)) { summary = L"the game's road graph: " + error; return false; }
        roads::RouteReport report;
        const bool ok = roads::AddRoutes(network_, graph, report);
        if (!ok) {
            summary = L"traffic and GPS were not written: " + report.problems;
            return false;
        }
        if (!report.routes) {
            // Nothing of ours: the game reads its own file again.
            ::DeleteFileW(served.c_str());
            summary = L"no roads for traffic";
            return true;
        }
        ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        const std::vector<u8> out = graph.Write();
        std::ofstream file(served, std::ios::binary | std::ios::trunc);
        file.write((const char*)out.data(), (std::streamsize)out.size());
        if (!file) { summary = L"could not write " + served; return false; }
        summary = Fmt(L"%zu roads in the game's road graph (%zu links among them, %zu into the game's roads)",
                      report.routes, report.links, report.gameLinks);
        if (!report.problems.empty()) summary += L"; " + report.problems;
        summary += WriteRaceRoutes(region, graph);
        return true;
    }

    // The graphs the races load instead (Routes<n>F/B.bin): each holds only
    // its race's area. The roads go into every one where they meet a road of
    // that race -- for traffic only -- with the ids they have in free roam;
    // the others are left to the game's own file.
    std::wstring WriteRaceRoutes(const std::wstring& region, const nfs::routes::Graph& freeRoam) {
        namespace fs = std::filesystem;
        const fs::path shippedFolder = fs::path(app_.GameFolder()) / L"TRACKS" / (L"ROUTES" + region);
        const fs::path servedFolder = fs::path(app_.ModelsFolder()) / L"files" / L"TRACKS" / (L"ROUTES" + region);
        std::error_code ec;
        // What earlier bakes wrote for races goes first.
        if (fs::exists(servedFolder, ec))
            for (const auto& entry : fs::directory_iterator(servedFolder, ec)) {
                const std::wstring name = entry.path().filename().wstring();
                // Only names the game ships: any other Routes<n> there was
                // put there by something else and is not the roads' to remove.
                if (name.rfind(L"Routes", 0) == 0 && name != L"RoutesFreeRoam.bin" &&
                    fs::exists(shippedFolder / name, ec))
                    fs::remove(entry.path(), ec);
            }
        if (network_.Segments().empty()) return L"";
        // The ids of the game's own free-roam routes: a race keeps links to
        // the ones it leaves out, and no new route may take them.
        std::vector<u16> shippedIds;
        {
            std::ifstream in((shippedFolder / L"RoutesFreeRoam.bin").wstring(), std::ios::binary);
            std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            nfs::routes::Graph shipped;
            std::wstring error;
            if (shipped.Parse(bytes, error))
                for (const auto& r : shipped.Routes()) shippedIds.push_back(r.header.id);
        }
        (void)freeRoam;
        size_t written = 0, races = 0;
        for (const auto& entry : fs::directory_iterator(shippedFolder, ec)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"Routes", 0) != 0 || name == L"RoutesFreeRoam.bin") continue;
            ++races;
            std::ifstream in(entry.path().wstring(), std::ios::binary);
            std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            nfs::routes::Graph graph;
            std::wstring error;
            if (!graph.Parse(bytes, error)) continue;
            for (u16 id : shippedIds) graph.Reserve(id);
            roads::RouteReport report;
            if (!roads::AddRoutes(network_, graph, report, true) || !report.gameLinks) continue;
            fs::create_directories(servedFolder, ec);
            const std::vector<u8> out = graph.Write();
            std::ofstream file((servedFolder / name).wstring(), std::ios::binary | std::ios::trunc);
            file.write((const char*)out.data(), (std::streamsize)out.size());
            if (file) ++written;
        }
        LOG_INFO("routes: the roads went into %zu of %zu race graphs", written, races);
        return Fmt(L"; traffic in %zu of %zu race graphs", written, races);
    }

    // ---- the road graph view: what the game will be handed, the game's own
    // routes with the network's added, rebuilt when the network changes.
    void RefreshGraph() {
        nfs::World* world = app_.World();
        if (!world) return;
        if (graphBuilt_ == network_.Revision() && graphRegion_ == world->Region()) return;
        if (graphRegion_ != world->Region() || graphShipped_.empty()) {
            graphShipped_.clear();
            std::ifstream in(app_.GameFolder() + L"\\TRACKS\\ROUTES" + world->Region() + L"\\RoutesFreeRoam.bin",
                             std::ios::binary);
            if (in) graphShipped_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            graphRegion_ = world->Region();
        }
        graphBuilt_ = network_.Revision();
        graph_ = nfs::routes::Graph{};
        ourRoutes_.clear();
        std::wstring error;
        if (graphShipped_.empty() || !graph_.Parse(graphShipped_, error)) return;
        std::set<u16> before;
        for (const auto& r : graph_.Routes()) before.insert(r.header.id);
        roads::RouteReport report;
        roads::AddRoutes(network_, graph_, report);
        for (const auto& r : graph_.Routes())
            if (!before.count(r.header.id)) ourRoutes_.insert(r.header.id);
    }

    const nfs::routes::Node* SelectedGraphNode() const {
        const int id = Selected();
        if (id < 0 || !(id & kGraphBit)) return nullptr;
        const int value = id & ~kGraphBit;
        const nfs::routes::Route* r = graph_.Find((u16)(value >> 12));
        const int k = value & 0xFFF;
        return r && k < (int)r->nodes.size() ? &r->nodes[(size_t)k] : nullptr;
    }

    bool PickGraphNode(const Ray& ray, int& index) const {
        float best = 1e30f;
        index = -1;
        for (const nfs::routes::Route& r : graph_.Routes())
            for (size_t k = 0; k < r.nodes.size() && k < 4096; ++k) {
                const nfs::routes::Node& n = r.nodes[k];
                const Vec3 p{ n.x, n.y, n.height8 / 8.0f + 0.6f };
                Aabb box;
                box.Add(p - Vec3{ 1.2f, 1.2f, 1.2f });
                box.Add(p + Vec3{ 1.2f, 1.2f, 1.2f });
                const float hit = IntersectRayAabb(ray, box);
                if (hit < 0.0f || hit >= best) continue;
                best = hit;
                index = kGraphBit | ((int)r.header.id << 12) | (int)k;
            }
        return index >= 0;
    }

    void DrawRouteGraph(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer);

    bool BakeGroundCollision(size_t& moved);
    bool BakeGroundModels(size_t& moved, std::wstring& error);

    void Bake();
    void Unbake();
    bool RemoveBaked(size_t& removed);
    bool BakeCollision(const std::vector<roads::MeshPiece>& pieces, size_t& added, size_t& removed);

    std::wstring CollisionRecordPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\roads-" + region + L"-collision.txt";
    }

    // The road drawing tool beside the gizmo's buttons on the toolbar, pressed
    // while it is on.
    void AddToolbarButton() {
        HWND toolbar = app_.Toolbar();
        if (!toolbar) return;
        TBBUTTON buttons[2] = {};
        buttons[0].fsStyle = BTNS_SEP;
        buttons[1].iBitmap = I_IMAGENONE;
        buttons[1].idCommand = ID_ROAD_DRAW;
        buttons[1].fsState = TBSTATE_ENABLED;
        buttons[1].fsStyle = (BYTE)(BTNS_CHECK | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
        buttons[1].iString = (INT_PTR)L"Road tool";
        ::SendMessageW(toolbar, TB_ADDBUTTONSW, 2, (LPARAM)buttons);
        ::SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
    }

    void SyncToolbar() const {
        if (HWND toolbar = app_.Toolbar()) ::SendMessageW(toolbar, TB_CHECKBUTTON, ID_ROAD_DRAW, MAKELPARAM(drawTool_ ? 1 : 0, 0));
    }

    void AddMenu() {
        AddToolbarButton();
        HMENU menu = app_.Menu();
        if (!menu) return;
        HMENU roadsMenu = ::CreatePopupMenu();
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_DRAW, L"&Draw Roads (nodes, click by click)\tCtrl+Shift+D");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_NEW, L"&New Road at View Centre\tCtrl+R");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_EXTEND, L"&Pull a Road out of the Selected Node\tCtrl+Shift+R");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_SPLIT, L"&Split the Selected Road Where Clicked");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_FACE_JUNCTION, L"&Junction into a Game Road...\tCtrl+Shift+J");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_SPLIT_WAYS, L"Split the Two &Directions at This Road End");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_LANE_ADD, L"Add a &Lane after the Selected Node");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_LANE_DROP, L"Drop a La&ne after the Selected Node");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_LANE_SPLIT, L"Split the &Outer Lane off Here (an exit)");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_AUTO_HEADING, L"Node Direction &Automatic");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_DELETE, L"&Delete Selected Node or Road");
        ::AppendMenuW(roadsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(roadsMenu, MF_STRING, ID_VIEW_ROADS, L"Show &Roads (procedural)");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_VIEW_ROAD_GRAPH, L"Show the Road &Graph (traffic and GPS)");
        ::AppendMenuW(roadsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_BAKE, L"&Bake Roads into the Game\tCtrl+Shift+B");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_UNBAKE, L"&Remove Baked Roads from the Game");
        HMENU terrainMenu = ::CreatePopupMenu();
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_OUTLINE, L"&Draw a Terrain Area...\tCtrl+Shift+T");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_ALONG, L"Terrain &Along the Selected Road");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_NEW, L"Square Patch at View &Centre");
        ::AppendMenuW(terrainMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::None, L"Brush &Off\tEsc");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::Raise, L"&Raise");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::Lower, L"&Lower");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::Smooth, L"&Smooth");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::Flatten, L"&Flatten");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::Paint, L"&Paint Texture");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::AddArea,
                      L"&Add Area (grow the selected patch)");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_BRUSH + (int)terrain::Brush::EraseArea, L"&Erase Area");
        ::AppendMenuW(terrainMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(terrainMenu, MF_STRING, ID_GROUND_FIT, L"Fit the Game's &Ground to the Roads");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_GROUND_DISCARD, L"&Discard Unbaked Ground Sculpting");
        // Before Help, the last menu.
        const UINT help = (UINT)std::max(0, ::GetMenuItemCount(menu) - 1);
        ::InsertMenuW(menu, help, MF_BYPOSITION | MF_POPUP, (UINT_PTR)roadsMenu, L"&Roads");
        ::InsertMenuW(menu, help + 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)terrainMenu, L"&Terrain");
        ::DrawMenuBar(app_.Window());
    }

    void AddAccelerators() {
        HACCEL& table = app_.Accelerators();
        const int count = table ? ::CopyAcceleratorTableW(table, nullptr, 0) : 0;
        std::vector<ACCEL> keys((size_t)count);
        if (count) ::CopyAcceleratorTableW(table, keys.data(), count);
        const ACCEL added[] = {
            { FVIRTKEY | FCONTROL, 'R', (WORD)ID_ROAD_NEW },
            { FVIRTKEY | FCONTROL | FSHIFT, 'R', (WORD)ID_ROAD_EXTEND },
            { FVIRTKEY | FCONTROL | FSHIFT, 'B', (WORD)ID_ROAD_BAKE },
            { FVIRTKEY | FCONTROL | FSHIFT, 'T', (WORD)ID_TERRAIN_OUTLINE },
            { FVIRTKEY | FCONTROL | FSHIFT, 'J', (WORD)ID_ROAD_FACE_JUNCTION },
            { FVIRTKEY | FCONTROL | FSHIFT, 'D', (WORD)ID_ROAD_DRAW },
        };
        for (const ACCEL& key : added) {
            const bool taken = std::any_of(keys.begin(), keys.end(), [&](const ACCEL& k) {
                return k.key == key.key && (k.fVirt & ~FNOINVERT) == (key.fVirt & ~FNOINVERT);
            });
            if (taken) LOG_WARN("roads: accelerator for command %u already taken, left out", key.cmd);
            else keys.push_back(key);
        }
        HACCEL replaced = ::CreateAcceleratorTableW(keys.data(), (int)keys.size());
        if (!replaced) return;
        if (table) ::DestroyAcceleratorTable(table);
        table = replaced;
    }

    RoadsAccess app_;
    roads::Network network_;
    roads::RoadProfile lastProfile_;
    std::vector<DrawBatch> batches_;
    bool graphView_ = false;
    u64 graphBuilt_ = 0;
    std::wstring graphRegion_;
    std::vector<u8> graphShipped_;
    nfs::routes::Graph graph_;
    std::set<u16> ourRoutes_;
    std::vector<UndoState> undo_;
    UndoState settled_;
    bool historyReady_ = false;
    std::wstring historyRegion_;
    std::shared_ptr<const roads::Network::LightSources> light_;
    terrain::Terrain terrain_;
    roads::RoadField field_;
    std::map<u32, std::vector<DrawBatch>> terrainBatches_;
    u64 terrainBuilt_ = 0, terrainRoads_ = 0;
    u32 dirtyPatch_ = 0;
    terrain::Brush brush_ = terrain::Brush::None;
    float brushRadius_ = 16.0f;
    float brushStrength_ = 6.0f;
    u8    paintLayer_ = 0;
    bool  stroking_ = false, brushHit_ = false;
    u32   strokePatch_ = 0;
    Vec3  brushPoint_{};
    float flattenLevel_ = 0.0f;
    int   cursorX_ = 0, cursorY_ = 0;
    std::vector<std::wstring> groundTextures_;
    bool  drawing_ = false;
    bool  drawTool_ = false;           // the road drawing tool is on
    u32   drawFrom_ = 0;               // the node the road being drawn goes on from
    bool  drawPending_ = false;        // a first click on open ground, no node yet
    Vec3  drawStart_;
    roads::RoadProfile drawProfile_;
    DrawHit drawHover_;
    int   wizard_ = 0;                 // WizardStep
    int   wizardModel_ = -1;           // in groundModels_
    std::set<size_t> wizardFaces_;     // its triangles
    u32   wizardNode_ = 0;
    long long wizardHover_ = -1;
    bool  wizardPainting_ = false, wizardPaintAdd_ = true;
    bool  wizardRefresh_ = false;      // the node was moved: work the preview out again
    roads::MeshPiece wizardPiece_;
    std::vector<DrawBatch> faceRemainder_;
    std::set<u32> faceHidden_;
    std::vector<Vec3> outline_;
    Vec3  hover_{};
    bool  hoverValid_ = false;
    float terrainMargin_ = 40.0f;
    Vec3  patchDragStart_{};
    bool  patchDragging_ = false;
    terrain::GroundDeform ground_;
    std::vector<GroundModel> groundModels_;
    mutable std::unordered_map<long long, std::vector<TriangleRef>> lightGrid_;
    mutable bool lightGridBuilt_ = false;
    std::vector<roads::GameCut> cutsCache_;
    roads::RoadField groundField_;
    u64 groundFieldRevision_ = 0;
    std::set<std::string> fitted_;
    bool fitGround_ = true;
    bool strokeGround_ = false;
    std::vector<nfs::CollisionPolygon> roadCollision_;    // what the last road bake put in
    static constexpr float kGroundShoulder = 10.0f;
    static constexpr float kGroundSink = 0.2f;
    std::vector<roads::Pole> poles_;
    std::unordered_map<std::wstring, const nfs::Texture*> texturesByName_;
    std::unordered_map<std::wstring, const nfs::SolidObject*> modelsByName_;
    std::unordered_map<std::wstring, HBITMAP> pictures_;
    std::vector<std::wstring> poleModels_;
    std::vector<std::wstring> wallModels_;
    std::vector<std::wstring> pillarModels_;
    u64 meshRevision_ = 0;
    std::unordered_map<std::string, u32> hashes_;
    std::vector<std::wstring> lanes_, walks_, walls_;
    bool visible_ = true;
    u32 pulling_ = 0;
    float dragHeading_ = 0.0f;
    bool  dragging_ = false;
    u32   dragNode_ = 0;
    mutable Vec3 lastPick_{};
};

bool RoadsExtension::Bridged(const roads::Segment& segment) const { return segment.profile.bridge; }

bool RoadsExtension::FillProperties(PropertiesPanel::View& view) {
    if (const nfs::routes::Node* n = SelectedGraphNode()) {
        const int id = Selected() & ~kGraphBit;
        const u16 routeId = (u16)(id >> 12);
        const int index = id & 0xFFF;
        const nfs::routes::Route* route = graph_.Find(routeId);
        view.title = Fmt(L"Road graph: route %u, node %d", routeId, index);
        view.moveLabel = L"";
        view.removeLabel = L"";
        AddRow(view, L"Route", L"Route", Fmt(L"%u%s", routeId, ourRoutes_.count(routeId) ? L" (the editor's)" : L""),
               L"Its id in the game's road graph.", 0);
        if (route) {
            AddRow(view, L"Route", L"Nodes", std::to_wstring(route->nodes.size()), L"", 0);
            AddRow(view, L"Route", L"Length", Number(route->header.length), L"", 0);
        }
        AddRow(view, L"Node", L"Index", std::to_wstring(index), L"0 is where the route starts.", 0);
        AddRow(view, L"Node", L"Position", Fmt(L"%.1f, %.1f, %.1f", n->x, n->y, n->height8 / 8.0f), L"", 0);
        AddRow(view, L"Node", L"Half widths", Fmt(L"left %.2f, right %.2f", n->halfLeft256 / 256.0f, n->halfRight256 / 256.0f),
               L"The road's edges either side.", 0);
        const auto perm = [&](u32 f, u32 b) {
            return std::wstring((n->flags & f) ? L"forward" : L"") + ((n->flags & f) && (n->flags & b) ? L" and " : L"") +
                   ((n->flags & b) ? L"backward" : L"") + (!(n->flags & (f | b)) ? L"no" : L"");
        };
        AddRow(view, L"Who drives it", L"GPS (player)", perm(nfs::routes::kGpsForward, nfs::routes::kGpsBackward), L"", 0);
        AddRow(view, L"Who drives it", L"Traffic", perm(nfs::routes::kTrafficForward, nfs::routes::kTrafficBackward), L"", 0);
        AddRow(view, L"Who drives it", L"Racers", perm(nfs::routes::kRacersForward, nfs::routes::kRacersBackward), L"", 0);
        const float maxHalf = std::max(n->halfLeft256, n->halfRight256) / 256.0f;
        for (int j = 0; j < 8; ++j) {
            if (n->lanes[j] == -1) continue;
            AddRow(view, L"Lanes", Fmt(L"%s lane %d", j < 4 ? L"Forward" : L"Backward", j % 4 + 1).c_str(),
                   Fmt(L"%.2f right of travel, %u m/s (%.0f km/h)", n->lanes[j] / 128.0f * maxHalf, n->speeds[2 + j],
                       n->speeds[2 + j] * 3.6f),
                   L"", 0);
        }
        const auto& signposts = graph_.Signposts();
        for (int k = 0; k < 2; ++k) {
            const u16 slot = n->slots[k];
            if (slot == nfs::routes::kNoSlot || slot >= signposts.size()) continue;
            const nfs::routes::Signpost& sp = signposts[slot];
            const auto typeName = [](u32 t) {
                switch (t) {
                    case 2: return L"follow";
                    case 3: return L"end";
                    case 4: return L"optional";
                    case 7: return L"closed";
                    default: return L"other";
                }
            };
            AddRow(view, L"Links", Fmt(L"Link %d", k + 1).c_str(),
                   Fmt(L"to route %u node %u, %s; GPS/racers %s, traffic %s%s", sp.dstRoute, sp.dstNode,
                       (sp.flags >> 18 & 1) ? L"travelling backward" : L"travelling forward", typeName(sp.flags & 0xF),
                       typeName(sp.flags >> 4 & 0xF), (sp.flags >> 23 & 1) ? L", turns round" : L""),
                   L"", 0);
        }
        return true;
    }
    if (const terrain::Patch* patch = SelectedPatch()) {
        view.title = Fmt(L"Terrain patch %u", patch->id);
        view.moveLabel = L"";
        view.removeLabel = L"Delete patch";
        std::vector<std::wstring> brushes;
        for (int b = 0; b < terrain::kBrushCount; ++b) brushes.push_back(terrain::BrushName((terrain::Brush)b));
        std::vector<std::wstring> layerNames;
        for (size_t l = 0; l < patch->layers.size(); ++l)
            layerNames.push_back(Fmt(L"%zu %s", l + 1, Widen(patch->layers[l].texture).c_str()));
        AddRow(view, L"Brush", L"Brush", terrain::BrushName(brush_),
               L"What holding the left button over the ground does: raise, lower, smooth, flatten (to the height "
               L"where the stroke began) or paint the layer below. off puts it away.", kBrush, brushes);
        AddRow(view, L"Brush", L"Size", Number(brushRadius_), L"The brush's radius; [ and ] in the view.", kBrushRadius);
        AddRow(view, L"Brush", L"Strength", Number(brushStrength_), L"Units a second, at the brush's middle.",
               kBrushStrength);
        AddRow(view, L"Brush", L"Paint layer",
               paintLayer_ < layerNames.size() ? layerNames[paintLayer_] : std::wstring(L"1"),
               L"The layer the paint brush lays.", kBrushLayer, layerNames);
        AddRow(view, L"Patch", L"Size", Fmt(L"%d x %d cells of %.1f", patch->columns, patch->rows, patch->cell),
               L"The grid. New patches are 128 units across.", 0);
        AddRow(view, L"Patch", L"Layers", std::to_wstring(patch->layers.size()),
               L"How many textures the patch can be painted with (1 to 6).", kLayerCount);
        const std::vector<std::wstring> surfaces = { L"grass", L"road", L"sidewalk" };
        for (size_t l = 0; l < patch->layers.size(); ++l) {
            const terrain::Layer& layer = patch->layers[l];
            const std::wstring group = Fmt(L"Layer %zu", l + 1);
            const int base = kLayerTexture + (int)l * 4;
            AddRow(view, group.c_str(), L"Texture", Widen(layer.texture),
                   L"The game texture; the list has the ones the region's own ground uses most.", base,
                   groundTextures_, true);
            AddRow(view, group.c_str(), L"Tile", Number(layer.tile), L"World units one repeat covers.", base + 1);
            AddRow(view, group.c_str(), L"Surface",
                   layer.surface == 0x01 ? L"road" : layer.surface == 0x10 ? L"sidewalk" : L"grass",
                   L"How the car drives on it: grip, sound, dust.", base + 2, surfaces);
        }
        AddRow(view, L"Roads", L"Margin (along roads)", Number(terrainMargin_),
               L"How far beyond a road's edge Terrain > Terrain Along the Selected Road makes ground.", kMargin);
        AddRow(view, L"Roads", L"Fit the roads", patch->conform ? L"yes" : L"no",
               L"The ground sits under every road and slopes to the sculpted height beside it.", kConform,
               { L"yes", L"no" });
        AddRow(view, L"Roads", L"Shoulder", Number(patch->shoulder),
               L"How far from a road's edge the ground takes to reach its own height.", kShoulder);
        AddRow(view, L"Roads", L"Under the road", Number(patch->sink),
               L"How far below the road surface the ground is kept.", kSink);
        AddRow(view, L"Light", L"Ambient", Fmt(L"%06X", patch->ambient & 0xFFFFFF),
               L"The night light, RRGGBB, as on the roads; the street lights add their pools.", kTerrainAmbient);
        return true;
    }
    if (const roads::Segment* s = SelectedHandle()) {
        const bool atStart = (Selected() & kHandleAtStart) != 0;
        const roads::Node* node = network_.FindNode(atStart ? s->a : s->b);
        const float length = atStart ? s->handleA : s->handleB;
        view.title = Fmt(L"Curve handle: road %u, %s end", s->id, atStart ? L"first" : L"last");
        view.moveLabel = L"";
        view.removeLabel = L"Make automatic";
        AddRow(view, L"Handle", L"Length", length > 0.0f ? Number(length) : L"auto",
               L"How far the handle reaches; auto is a third of the road. Longer makes the curve leave the node "
               L"straighter.", kHandleLength);
        AddRow(view, L"Handle", L"Direction", node && node->hasHeading ? Number(node->heading) : L"auto",
               L"The node's direction, in degrees: every road through the node shares it, so the curve stays "
               L"smooth.", kHandleDirection);
        return true;
    }
    if (const roads::Node* node = SelectedNode()) {
        const size_t count = network_.SegmentsAt(node->id).size();
        view.title = Fmt(L"Road node %u", node->id);
        view.moveLabel = L"Pull a road out";
        view.removeLabel = L"Delete node";
        view.actions.push_back({ L"Draw from here", ID_ROAD_DRAW_FROM, L"Roads",
                                 L"The road drawing tool, going on from this node with its road's profile." });
        view.actions.push_back({ L"Draw roads", ID_ROAD_DRAW, L"Roads", L"The road drawing tool (Ctrl+Shift+D)." });
        if (count == 1 || count == 2) {
            view.actions.push_back({ L"+ Lane", ID_ROAD_LANE_ADD, L"Lanes after this node",
                                     L"One more lane on the road going on from this node, on its right; it widens in "
                                     L"over the taper length." });
            view.actions.push_back({ L"- Lane", ID_ROAD_LANE_DROP, L"Lanes after this node",
                                     L"One lane fewer on the road going on from this node; the outer one narrows "
                                     L"away before it." });
            view.actions.push_back({ L"Exit", ID_ROAD_LANE_SPLIT, L"Split here",
                                     L"The outer lane leaves as a road of its own (an exit); the other lanes go on." });
        }
        if (count == 1)
            view.actions.push_back({ L"Two directions", ID_ROAD_SPLIT_WAYS, L"Split here",
                                     L"The two directions go on as two one-way roads (a divided highway)." });
        AddRow(view, L"Node", L"Kind",
               count >= 3 ? Fmt(L"junction of %zu roads", count) : count == 2 ? L"through" : L"road end",
               L"Three or more roads on one node make a junction.", 0);
        AddRow(view, L"Node", L"X", Number(node->position.x), L"Where the node stands.", kNodeX);
        AddRow(view, L"Node", L"Y", Number(node->position.y), L"Where the node stands.", kNodeY);
        AddRow(view, L"Node", L"Z (height)", Number(node->position.z), L"The road's height here.", kNodeZ);
        AddRow(view, L"Node", L"Direction", node->hasHeading ? Number(node->heading) : L"auto",
               L"Degrees the roads run through the node in (0 = +X). auto follows the neighbouring nodes; "
               L"R in the viewport turns it.",
               kNodeHeading);
        if (const roads::GameJunction* gj = network_.FindGameJunction(node->id))
            AddRow(view, L"Junction", L"Pedestrian crossings", gj->crosswalks ? L"yes" : L"no",
                   L"Crossing stripes (RDP_INTERSECT_AA_WH) across the game's road either side of the junction "
                   L"and across the mouth of the new road.", kNodeCrosswalks, { L"yes", L"no" });
        else if (count >= 3)
            AddRow(view, L"Junction", L"Pedestrian crossings", node->crosswalks ? L"yes" : L"no",
                   L"Crossing stripes (RDP_INTERSECT_AA_WH) across the end of every road at this junction.",
                   kNodeCrosswalks, { L"yes", L"no" });
        return true;
    }
    const roads::Segment* s = SelectedSegment();
    if (!s) return false;
    const roads::RoadProfile& p = s->profile;
    view.title = Fmt(L"Road %u", s->id);
    view.moveLabel = L"Pull a road out";
    view.removeLabel = L"Delete road";
    view.actions.push_back({ L"Node in the middle", ID_ROAD_INSERT_MIDDLE, L"Roads",
                             L"Splits this road in two at its middle, with a node there." });
    view.actions.push_back({ L"Draw roads", ID_ROAD_DRAW, L"Roads", L"The road drawing tool (Ctrl+Shift+D)." });
    const std::vector<std::wstring> yesNo = { L"yes", L"no" };
    AddRow(view, L"Road", L"Length", Number(SegmentLength(*s)), L"Along the curve.", 0);
    AddRow(view, L"Lanes", L"Lanes forward", std::to_wstring(p.lanesForward),
           L"Lanes on the right of the centre line, in the direction the road was drawn.", kLanesForward);
    AddRow(view, L"Lanes", L"Lanes backward", std::to_wstring(p.lanesBackward),
           L"Lanes on the left of the centre line. 0 makes a one-way road.", kLanesBackward);
    AddRow(view, L"Lanes", L"Lane width", Number(p.laneWidth), L"In world units (metres).", kLaneWidth);
    AddRow(view, L"Lanes", L"Taper length", Number(p.taperLength),
           L"Where this road meets one with fewer lanes at a node, its extra lanes narrow away over this length.",
           kTaperLength);
    AddRow(view, L"Lanes", L"Centre lane texture", Widen(p.laneCentre),
           L"The lane beside the centre line. The game's lane textures carry their marking on the left edge.",
           kLaneCentre, lanes_, true);
    AddRow(view, L"Lanes", L"Other lanes texture", Widen(p.laneInner), L"Every other lane.", kLaneInner, lanes_, true);
    AddRow(view, L"Lanes", L"Texture length", Number(p.textureLength),
           L"How much road one repeat of a lane texture covers.", kTextureLength);
    AddRow(view, L"Traffic", L"Traffic", p.traffic ? L"yes" : L"no",
           L"The game's traffic drives this road, in the directions its lanes run. Joined to the game's own "
           L"roads at the junctions made into them, written at every bake.", kTraffic, yesNo);
    AddRow(view, L"Traffic", L"GPS", p.gps ? L"yes" : L"no",
           L"The player's GPS (and the racers) may route over this road.", kGps, yesNo);
    AddRow(view, L"Traffic", L"Speed limit (km/h)", Number(p.speedLimit),
           L"The speed traffic keeps on the straights; it slows on bends by itself.", kSpeedLimit);
    AddRow(view, L"Median", L"Median", kMedianNames[std::clamp(p.medianType, 0, 4)],
           L"Between the two directions: painted stripes, a raised concrete or grass island, or a concrete "
           L"barrier. It opens out before junctions. Street lights can stand on it (Sides: median).",
           kMedianType, { kMedianNames[0], kMedianNames[1], kMedianNames[2], kMedianNames[3], kMedianNames[4] });
    if (p.medianType) {
        AddRow(view, L"Median", L"Width", Number(p.medianWidth), L"Across the road.", kMedianWidth);
        if (p.medianType >= 2)
            AddRow(view, L"Median", L"Height", Number(p.medianHeight),
                   L"Above the road: a kerb for an island, the wall for a barrier.", kMedianHeight);
        AddRow(view, L"Median", L"Texture", Widen(p.MedianTexture()), L"Its surface.", kMedianTexture, walls_, true);
    }
    AddRow(view, L"Sidewalks", L"Width", Number(p.sidewalkWidth), L"0 leaves them out.", kSidewalkWidth);
    AddRow(view, L"Sidewalks", L"Left", p.sidewalkLeft ? L"yes" : L"no", L"A sidewalk on the left edge.", kWalkLeft,
           yesNo);
    AddRow(view, L"Sidewalks", L"Right", p.sidewalkRight ? L"yes" : L"no", L"A sidewalk on the right edge.",
           kWalkRight, yesNo);
    AddRow(view, L"Sidewalks", L"Kerb height", Number(p.sidewalkHeight), L"How far they stand above the road.",
           kSidewalkHeight);
    AddRow(view, L"Sidewalks", L"Texture", Widen(p.sidewalkTexture), L"The sidewalk and kerb.", kSidewalkTexture,
           walks_, true);
    AddRow(view, L"Walls", L"Left wall", p.wallLeft ? L"yes" : L"no", L"Along the left edge.", kWallLeft, yesNo);
    AddRow(view, L"Walls", L"Right wall", p.wallRight ? L"yes" : L"no", L"Along the right edge.", kWallRight, yesNo);
    AddRow(view, L"Walls", L"Style", kWallStyleNames[std::clamp(p.wallStyle, 0, 2)],
           L"flat: a straight wall; guardrail: a steel W-beam on posts; model: a game model repeated along the "
           L"edge, turned to follow its curve. Every style collides as a wall nothing gets over.", kWallStyle,
           { kWallStyleNames[0], kWallStyleNames[1], kWallStyleNames[2] });
    if (p.wallStyle == 2) {
        AddRow(view, L"Walls", L"Model", p.wallModel.empty() ? L"-" : Widen(p.wallModel),
               L"Rails, fences and barriers of the region. Its length sets the spacing when picked.", kWallModel,
               wallModels_, true);
        AddRow(view, L"Walls", L"Spacing", Number(p.wallModelSpacing), L"From one copy to the next.",
               kWallModelSpacing);
        AddRow(view, L"Walls", L"Turn", Number(p.wallModelTurn),
               L"Degrees added to each copy, for a model that is not built along X.", kWallModelTurn);
    }
    AddRow(view, L"Walls", L"Height", Number(p.wallHeight), L"Above the road or sidewalk.", kWallHeight);
    AddRow(view, L"Walls", L"Texture", Widen(p.wallTexture), L"The wall.", kWallTexture, walls_, true);
    AddRow(view, L"Walls", L"Texture length", Number(p.wallTextureLength),
           L"How much wall one repeat of its texture covers.", kWallTextureLength);
    {
        const wchar_t* const modes[3] = { L"off", L"auto", L"fixed" };
        AddRow(view, L"Banking", L"Banking", modes[std::clamp(p.bankMode, 0, 2)],
               L"Tilt the road into its curves for speed. auto: from the curve and the design speed; fixed: one "
               L"angle. It eases to flat at junctions and road ends.", kBankMode, { modes[0], modes[1], modes[2] });
        if (p.bankMode == 1) {
            AddRow(view, L"Banking", L"Design speed (km/h)", Number(p.designSpeed),
                   L"The speed the curve is banked for: the faster, the steeper.", kDesignSpeed);
            AddRow(view, L"Banking", L"Steepest (degrees)", Number(p.bankMax), L"Never tilted past this.", kBankMax);
        } else if (p.bankMode == 2) {
            AddRow(view, L"Banking", L"Angle (degrees)", Number(p.bankAngle),
                   L"Positive lifts the right edge (in the direction the road was drawn), negative the left.",
                   kBankAngle);
        }
    }
    AddRow(view, L"Bridge", L"Bridge", p.bridge ? L"yes" : L"no",
           L"A deck on pillars: the ground is not fitted under it. Raise its nodes (gizmo Z) for the height; "
           L"the roads leading up to it make embankments.", kBridge, { L"yes", L"no" });
    if (p.bridge) {
        AddRow(view, L"Bridge", L"Deck thickness", Number(p.deckThickness), L"From the road surface down.", kDeckThickness);
        AddRow(view, L"Bridge", L"Pillar spacing", Number(p.pillarSpacing), L"Between pillars; 0 for none.",
               kPillarSpacing);
        AddRow(view, L"Bridge", L"Pillars across", Number((float)p.pillarCount),
               L"Side by side at each spacing, spread over the deck's width; 0 picks by the width.", kPillarCount);
        AddRow(view, L"Bridge", L"Pillar model", p.pillarModel.empty() ? L"-" : Widen(p.pillarModel),
               L"A pillar of the game (supports, columns, piers) instead of the square column: its top under the "
               L"deck; if it is shorter than the drop, the column carries on below it. '-' for the column.",
               kPillarModel, pillarModels_, true);
        if (!p.pillarModel.empty())
            AddRow(view, L"Bridge", L"Model turn", Number(p.pillarModelTurn),
                   L"Degrees added to each pillar, for a model not built along the road.", kPillarModelTurn);
        AddRow(view, L"Bridge", L"Pillar width", Number(p.pillarWidth), L"Square columns, this wide.", kPillarWidth);
        AddRow(view, L"Bridge", L"Deck texture", Widen(p.deckTexture), L"The deck's underside and edges.",
               kDeckTexture, walls_, true);
        AddRow(view, L"Bridge", L"Pillar texture", Widen(p.pillarTexture), L"The pillars.", kPillarTexture, walls_,
               true);
    }
    AddRow(view, L"Street lights", L"Model", p.poleModel.empty() ? L"-" : Widen(p.poleModel),
           L"The game model stood along the road (- for none). The lamps light the road's vertices.", kPoleModel,
           poleModels_, true);
    AddRow(view, L"Street lights", L"Spacing", Number(p.poleSpacing), L"Distance between poles along the road.",
           kPoleSpacing);
    AddRow(view, L"Street lights", L"Sides", kSideNames[std::clamp(p.poleSides, 0, 4)],
           L"right, left, both, or alternating from side to side.", kPoleSides,
           { kSideNames[0], kSideNames[1], kSideNames[2], kSideNames[3], kSideNames[4] });
    AddRow(view, L"Street lights", L"Inset", Number(p.poleInset),
           L"How far in from the sidewalk's outer edge -- or out from the road's edge when there is no sidewalk.",
           kPoleInset);
    AddRow(view, L"Street lights", L"Turn", Number(p.poleTurn),
           L"Degrees added to each pole's heading, for a model whose arm does not face the road.", kPoleTurn);
    AddRow(view, L"Light", L"Ambient", Fmt(L"%06X", p.ambient & 0xFFFFFF),
           L"The night light everywhere, RRGGBB. The game draws texture x colour x 2: 383C42 is its own roads' "
           L"dim blue-grey, 808080 the texture as it is.", kAmbient);
    AddRow(view, L"Light", L"Lamp colour", Fmt(L"%06X", p.lampColor & 0xFFFFFF),
           L"The street lights' colour, RRGGBB: FFD9A0 sodium, E0F0FF white.", kLampColor);
    AddRow(view, L"Light", L"Lamp radius", Number(p.lampRadius), L"How far each lamp's pool of light reaches.",
           kLampRadius);
    AddRow(view, L"Light", L"Lamp strength", Number(p.lampStrength), L"1 matches the game's own street lights.",
           kLampStrength);
    return true;
}

bool RoadsExtension::ApplyProperties() {
    if (Selected() < 0) return false;
    PropertiesPanel& properties = app_.Properties();
    if (terrain::Patch* patch = SelectedPatchMutable()) {
        std::wstring value;
        float number = 0.0f;
        if (properties.ReadText(kBrush, value))
            for (int b = 0; b < terrain::kBrushCount; ++b)
                if (value == terrain::BrushName((terrain::Brush)b) && (int)brush_ != b) SetBrush((terrain::Brush)b);
        if (properties.ReadNumber(kBrushRadius, number)) brushRadius_ = std::clamp(number, 1.0f, 400.0f);
        if (properties.ReadNumber(kBrushStrength, number)) brushStrength_ = std::clamp(number, 0.05f, 200.0f);
        if (properties.ReadText(kBrushLayer, value) && !value.empty())
            paintLayer_ = (u8)std::clamp((int)std::wcstol(value.c_str(), nullptr, 10) - 1, 0,
                                         (int)patch->layers.size() - 1);
        if (properties.ReadNumber(kLayerCount, number)) {
            const int count = std::clamp((int)std::lround(number), 1, terrain::kMaxLayers);
            while ((int)patch->layers.size() < count) patch->layers.push_back(terrain::Layer{});
            while ((int)patch->layers.size() > count) patch->layers.pop_back();
            for (u8& cell : patch->paint)
                if (cell != terrain::kEmpty) cell = std::min<u8>(cell, (u8)(count - 1));
            paintLayer_ = std::min<u8>(paintLayer_, (u8)(count - 1));
        }
        for (size_t l = 0; l < patch->layers.size(); ++l) {
            terrain::Layer& layer = patch->layers[l];
            const int base = kLayerTexture + (int)l * 4;
            if (properties.ReadText(base, value)) {
                const std::string name = Narrow(value);
                if (!name.empty()) layer.texture = name;
            }
            if (properties.ReadNumber(base + 1, number)) layer.tile = std::clamp(number, 0.25f, 500.0f);
            if (properties.ReadText(base + 2, value))
                layer.surface = value == L"road" ? 0x01 : value == L"sidewalk" ? 0x10 : 0x0F;
        }
        if (properties.ReadText(kConform, value)) patch->conform = !value.empty() && (value[0] == L'y' || value[0] == L'Y');
        if (properties.ReadNumber(kShoulder, number)) patch->shoulder = std::clamp(number, 0.0f, 200.0f);
        if (properties.ReadNumber(kMargin, number)) terrainMargin_ = std::clamp(number, 2.0f, 500.0f);
        if (properties.ReadNumber(kSink, number)) patch->sink = std::clamp(number, 0.0f, 5.0f);
        if (properties.ReadText(kTerrainAmbient, value)) patch->ambient = ParseColour(value, patch->ambient);
        terrain_.Touch();
        SaveTerrain();
        Commit(L"terrain settings");
        return true;
    }
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::wstring text;

    if (roads::Segment* handle = SelectedHandleMutable()) {
        const bool atStart = (Selected() & kHandleAtStart) != 0;
        float& length = atStart ? handle->handleA : handle->handleB;
        if (properties.ReadText(kHandleLength, text)) {
            float value = 0.0f;
            if (text.empty() || text[0] == L'a' || text[0] == L'A') length = 0.0f;
            else if (properties.ReadNumber(kHandleLength, value)) length = std::clamp(value, 0.0f, 2000.0f);
        }
        if (roads::Node* node = network_.FindNode(atStart ? handle->a : handle->b);
            node && properties.ReadText(kHandleDirection, text)) {
            float value = 0.0f;
            if (text.empty() || text[0] == L'a' || text[0] == L'A') node->hasHeading = false;
            else if (properties.ReadNumber(kHandleDirection, value)) {
                node->hasHeading = true;
                node->heading = value;
            }
        }
        Changed(true);
        return true;
    }
    if (roads::Node* node = SelectedNodeMutable()) {
        if (properties.ReadNumber(kNodeX, x) && properties.ReadNumber(kNodeY, y) && properties.ReadNumber(kNodeZ, z))
            node->position = { x, y, z };
        if (properties.ReadText(kNodeHeading, text)) {
            float heading = 0.0f;
            if (text.empty() || text[0] == L'a' || text[0] == L'A') node->hasHeading = false;
            else if (properties.ReadNumber(kNodeHeading, heading)) {
                node->hasHeading = true;
                node->heading = heading;
            }
        }
        if (properties.ReadText(kNodeCrosswalks, text)) {
            const bool on = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
            if (roads::GameJunction* gj = network_.FindGameJunction(node->id)) gj->crosswalks = on;
            else node->crosswalks = on;
        }
        Changed(true);
        return true;
    }

    const int id = Selected();
    roads::Segment* s = network_.FindSegment((u32)id);
    if (!s) return true;
    roads::RoadProfile& p = s->profile;
    float value = 0.0f;
    if (properties.ReadNumber(kLanesForward, value)) p.lanesForward = std::clamp((int)std::lround(value), 0, 8);
    if (properties.ReadNumber(kLanesBackward, value)) p.lanesBackward = std::clamp((int)std::lround(value), 0, 8);
    if (p.Lanes() == 0) p.lanesForward = 1;
    if (properties.ReadNumber(kLaneWidth, value)) p.laneWidth = std::clamp(value, 1.0f, 20.0f);
    if (properties.ReadNumber(kTextureLength, value)) p.textureLength = std::clamp(value, 0.5f, 500.0f);
    if (properties.ReadNumber(kSidewalkWidth, value)) p.sidewalkWidth = std::clamp(value, 0.0f, 20.0f);
    if (properties.ReadNumber(kSidewalkHeight, value)) p.sidewalkHeight = std::clamp(value, 0.0f, 2.0f);
    if (properties.ReadNumber(kWallHeight, value)) p.wallHeight = std::clamp(value, 0.0f, 30.0f);
    if (properties.ReadNumber(kWallTextureLength, value)) p.wallTextureLength = std::clamp(value, 0.5f, 500.0f);
    if (properties.ReadText(kBridge, text)) {
        const bool bridge = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
        if (bridge && !p.bridge) p.wallLeft = p.wallRight = true;    // railings
        p.bridge = bridge;
    }
    if (properties.ReadText(kBankMode, text))
        p.bankMode = text == L"auto" ? 1 : text == L"fixed" ? 2 : 0;
    if (properties.ReadNumber(kDesignSpeed, value)) p.designSpeed = std::clamp(value, 10.0f, 400.0f);
    if (properties.ReadNumber(kBankMax, value)) p.bankMax = std::clamp(value, 0.0f, 45.0f);
    if (properties.ReadNumber(kBankAngle, value)) p.bankAngle = std::clamp(value, -45.0f, 45.0f);
    if (properties.ReadNumber(kDeckThickness, value)) p.deckThickness = std::clamp(value, 0.0f, 20.0f);
    if (properties.ReadNumber(kPillarSpacing, value)) p.pillarSpacing = std::clamp(value, 0.0f, 500.0f);
    if (properties.ReadNumber(kPillarWidth, value)) p.pillarWidth = std::clamp(value, 0.3f, 20.0f);
    if (properties.ReadNumber(kTaperLength, value)) p.taperLength = std::clamp(value, 5.0f, 500.0f);
    if (properties.ReadText(kPillarModel, text)) {
        const std::string name = Narrow(text);
        p.pillarModel = name == "-" ? std::string() : name;
    }
    if (properties.ReadNumber(kPillarModelTurn, value)) p.pillarModelTurn = value;
    if (properties.ReadNumber(kPillarCount, value)) p.pillarCount = std::clamp((int)std::lround(value), 0, 16);
    if (properties.ReadNumber(kPoleSpacing, value)) p.poleSpacing = std::clamp(value, 2.0f, 1000.0f);
    if (properties.ReadNumber(kPoleInset, value)) p.poleInset = std::clamp(value, -20.0f, 20.0f);
    if (properties.ReadNumber(kPoleTurn, value)) p.poleTurn = value;
    if (properties.ReadNumber(kLampRadius, value)) p.lampRadius = std::clamp(value, 1.0f, 200.0f);
    if (properties.ReadNumber(kLampStrength, value)) p.lampStrength = std::clamp(value, 0.0f, 4.0f);
    if (properties.ReadText(kAmbient, text)) p.ambient = ParseColour(text, p.ambient);
    if (properties.ReadText(kLampColor, text)) p.lampColor = ParseColour(text, p.lampColor);
    if (properties.ReadText(kPoleSides, text))
        for (int k = 0; k < 5; ++k)
            if (text == kSideNames[k]) p.poleSides = k;
    if (properties.ReadText(kPoleModel, text)) {
        const std::string name = Narrow(text);
        p.poleModel = name == "-" ? std::string() : name;
    }
    const auto readTexture = [&](int field, std::string& into) {
        if (!properties.ReadText(field, text)) return;
        const std::string name = Narrow(text);
        if (!name.empty()) into = name;
    };
    readTexture(kLaneCentre, p.laneCentre);
    readTexture(kLaneInner, p.laneInner);
    readTexture(kSidewalkTexture, p.sidewalkTexture);
    readTexture(kWallTexture, p.wallTexture);
    readTexture(kDeckTexture, p.deckTexture);
    readTexture(kPillarTexture, p.pillarTexture);
    if (properties.ReadText(kMedianType, text))
        for (int k = 0; k < 5; ++k)
            if (text == kMedianNames[k]) {
                if (k != p.medianType) p.medianTexture.clear();   // the kind's own texture
                p.medianType = k;
            }
    if (properties.ReadNumber(kMedianWidth, value)) p.medianWidth = std::clamp(value, 0.3f, 60.0f);
    if (properties.ReadNumber(kMedianHeight, value)) p.medianHeight = std::clamp(value, 0.0f, 3.0f);
    if (properties.ReadText(kMedianTexture, text)) {
        const std::string name = Narrow(text);
        if (!name.empty() && name != p.MedianTexture()) p.medianTexture = name;
    }
    if (properties.ReadText(kTraffic, text)) p.traffic = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (properties.ReadText(kGps, text)) p.gps = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (properties.ReadNumber(kSpeedLimit, value)) p.speedLimit = std::clamp(value, 10.0f, 300.0f);
    if (properties.ReadText(kWalkLeft, text)) p.sidewalkLeft = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (properties.ReadText(kWalkRight, text)) p.sidewalkRight = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (properties.ReadText(kWallStyle, text))
        for (int k = 0; k < 3; ++k)
            if (text == kWallStyleNames[k]) p.wallStyle = k;
    if (properties.ReadText(kWallModel, text)) {
        const std::string name = Narrow(text);
        const std::string picked = name == "-" ? std::string() : name;
        if (picked != p.wallModel) {
            p.wallModel = picked;
            // The model's own length along X spaces the copies end to end.
            if (const auto found = modelsByName_.find(Widen(picked)); found != modelsByName_.end() &&
                                                                         found->second->bounds.Valid()) {
                const float length = found->second->bounds.max.x - found->second->bounds.min.x;
                if (length > 0.5f) p.wallModelSpacing = length;
            }
        }
    }
    if (properties.ReadNumber(kWallModelSpacing, value)) p.wallModelSpacing = std::clamp(value, 0.5f, 100.0f);
    if (properties.ReadNumber(kWallModelTurn, value)) p.wallModelTurn = value;
    if (properties.ReadText(kWallLeft, text)) p.wallLeft = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (properties.ReadText(kWallRight, text)) p.wallRight = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    lastProfile_ = p;
    Changed(true);
    return true;
}

void RoadsExtension::DrawRouteGraph(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
    RefreshGraph();
    struct Vertex { float x, y, z; D3DCOLOR colour; };
    std::vector<Vertex> lines;
    const auto line = [&](float x0, float y0, float z0, float x1, float y1, float z1, D3DCOLOR c) {
        lines.push_back({ x0, y0, z0, c });
        lines.push_back({ x1, y1, z1, c });
    };
    const Vec3 eye = camera.Position();
    const float reach = 700.0f;
    const int selected = Selected();
    const int selectedRoute = selected >= 0 && (selected & kGraphBit) ? (selected & ~kGraphBit) >> 12 : -1;

    // The route nearest the middle of the view, in yellow.
    int nearest = -1;
    {
        Vec3 centre;
        if (app_.ViewCentre(centre)) {
            float best = 30.0f;
            for (const nfs::routes::Route& r : graph_.Routes()) {
                int k = 0;
                float t = 0.0f, d = 0.0f;
                if (graph_.Project(r, centre.x, centre.y, k, t, d) && d < best) { best = d; nearest = r.header.id; }
            }
        }
    }
    const float lift = 0.6f;
    for (const nfs::routes::Route& r : graph_.Routes()) {
        const float* box = r.header.bbox;
        if (box[2] < eye.x - reach || box[0] > eye.x + reach || box[3] < eye.y - reach || box[1] > eye.y + reach) continue;
        const bool ours = ourRoutes_.count(r.header.id) != 0;
        const bool chosen = r.header.id == selectedRoute || r.header.id == nearest;
        const D3DCOLOR centreColour = chosen ? 0xFFFFE030 : ours ? 0xFF40FF80 : 0xFFF0F0F0;
        for (size_t k = 0; k < r.nodes.size(); ++k) {
            const nfs::routes::Node& n = r.nodes[k];
            const float z = n.height8 / 8.0f + lift;
            const float dx = n.dirX / 32767.0f, dy = n.dirY / 32767.0f;
            const float maxHalf = std::max(n.halfLeft256, n.halfRight256) / 256.0f;
            // The node: a short cross mark.
            line(n.x - 0.6f, n.y, z, n.x + 0.6f, n.y, z, centreColour);
            line(n.x, n.y - 0.6f, z, n.x, n.y + 0.6f, z, centreColour);
            if (n.flags >> 19 & 0x3F) {
                if ((n.flags >> 19 & 0x3F) != nfs::routes::kNoIntersection) {
                    // A traffic-light crossing: a red diamond.
                    const float s = 2.5f;
                    line(n.x - s, n.y, z, n.x, n.y + s, z, 0xFFFF3030);
                    line(n.x, n.y + s, z, n.x + s, n.y, z, 0xFFFF3030);
                    line(n.x + s, n.y, z, n.x, n.y - s, z, 0xFFFF3030);
                    line(n.x, n.y - s, z, n.x - s, n.y, z, 0xFFFF3030);
                }
            }
            if (k + 1 >= r.nodes.size()) continue;
            const nfs::routes::Node& m = r.nodes[k + 1];
            const float zm = m.height8 / 8.0f + lift;
            const float mdx = m.dirX / 32767.0f, mdy = m.dirY / 32767.0f;
            line(n.x, n.y, z, m.x, m.y, zm, centreColour);
            // The road's edges.
            const float hl = n.halfLeft256 / 256.0f, hr = n.halfRight256 / 256.0f;
            const float mhl = m.halfLeft256 / 256.0f, mhr = m.halfRight256 / 256.0f;
            line(n.x + dy * hr, n.y - dx * hr, z, m.x + mdy * mhr, m.y - mdx * mhr, zm, 0xC0FF4040);
            line(n.x - dy * hl, n.y + dx * hl, z, m.x - mdy * mhl, m.y + mdx * mhl, zm, 0xC0FF4040);
            // The lanes: blue forward, orange backward, each with an arrow the way it is driven.
            const float mMaxHalf = std::max(m.halfLeft256, m.halfRight256) / 256.0f;
            for (int j = 0; j < 8; ++j) {
                if (n.lanes[j] == -1 || m.lanes[j] == -1) continue;
                const bool back = j >= 4;
                const float o0 = n.lanes[j] / 128.0f * maxHalf, o1 = m.lanes[j] / 128.0f * mMaxHalf;
                const float sgn = back ? -1.0f : 1.0f;
                const float x0 = n.x + dy * o0 * sgn, y0 = n.y - dx * o0 * sgn;
                const float x1 = m.x + mdy * o1 * sgn, y1 = m.y - mdx * o1 * sgn;
                const bool traffic = (n.flags & (back ? nfs::routes::kTrafficBackward : nfs::routes::kTrafficForward)) != 0;
                const D3DCOLOR c = back ? (traffic ? 0xFFFF9020 : 0x80FF9020) : (traffic ? 0xFF4090FF : 0x804090FF);
                line(x0, y0, z, x1, y1, zm, c);
                // The arrow, halfway, pointing the way the lane is driven.
                const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f, mz = (z + zm) * 0.5f;
                const float ax = back ? -dx : dx, ay = back ? -dy : dy;
                const float hx = mx + ax * 1.2f, hy = my + ay * 1.2f;
                line(hx, hy, mz, hx - ax * 1.2f + ay * 0.7f, hy - ay * 1.2f - ax * 0.7f, mz, c);
                line(hx, hy, mz, hx - ax * 1.2f - ay * 0.7f, hy - ay * 1.2f + ax * 0.7f, mz, c);
            }
        }
    }
    // The links: from a node to where they lead.
    for (const nfs::routes::Signpost& sp : graph_.Signposts()) {
        const nfs::routes::Route* a = graph_.Find(sp.srcRoute);
        const nfs::routes::Route* b = graph_.Find(sp.dstRoute);
        if (!a || !b || sp.srcNode >= a->nodes.size() || sp.dstNode >= b->nodes.size()) continue;
        const nfs::routes::Node& n = a->nodes[sp.srcNode];
        const nfs::routes::Node& m = b->nodes[sp.dstNode];
        if (std::fabs(n.x - eye.x) > reach || std::fabs(n.y - eye.y) > reach) continue;
        const u32 ta = sp.flags & 0xF, tb = sp.flags >> 4 & 0xF;
        const D3DCOLOR c = ta == 7 && tb == 7 ? 0xFF808080 : (ta == 2 || ta == 4) && (tb == 2 || tb == 4) ? 0xFF30FF30 : 0xFFC040FF;
        const float z0 = n.height8 / 8.0f + lift + 0.8f, z1 = m.height8 / 8.0f + lift + 0.8f;
        if (std::fabs(n.x - m.x) + std::fabs(n.y - m.y) < 0.3f) {
            line(n.x, n.y, z0 - 0.8f, n.x, n.y, z0 + 1.5f, c);   // a link to the same spot: a short post
        } else {
            line(n.x, n.y, z0, m.x, m.y, z1, c);
        }
    }
    if (selected >= 0 && (selected & kGraphBit)) {
        if (const nfs::routes::Node* n = SelectedGraphNode()) {
            const float z = n->height8 / 8.0f + lift;
            line(n->x, n->y, z, n->x, n->y, z + 6.0f, 0xFFFFFFFF);
        }
    }
    if (lines.empty()) return;
    renderer.BeginOverlay(device);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    const size_t batch = 60000;
    for (size_t at = 0; at < lines.size(); at += batch) {
        const size_t count = std::min(batch, lines.size() - at);
        device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(count / 2), lines.data() + at, sizeof(Vertex));
    }
    renderer.EndOverlay(device);
}

void RoadsExtension::DrawOverlay(IDirect3DDevice9* device, const Camera& camera, WorldRenderer& renderer) {
    if (graphView_) DrawRouteGraph(device, camera, renderer);
    if (wizard_ != kWizardOff) DrawWizard(device, renderer);
    if (!visible_ || (network_.Nodes().empty() && brush_ == terrain::Brush::None && !drawing_ && !drawTool_)) return;
    struct Vertex { float x, y, z; D3DCOLOR colour; };
    std::vector<Vertex> solid, lines;
    const int selected = Selected();

    const auto line = [&](const Vec3& a, const Vec3& b, D3DCOLOR c) {
        lines.push_back({ a.x, a.y, a.z, c });
        lines.push_back({ b.x, b.y, b.z, c });
    };

    // Each road's centre line, a little above the surface.
    for (const roads::Segment& s : network_.Segments()) {
        const bool chosen = (int)s.id == selected;
        const roads::Curve curve = network_.CurveOf(s);
        const D3DCOLOR c = chosen ? 0xFF40E0FF : 0xA0FFD040;
        Vec3 previous = curve.At(0.0f) + Vec3{ 0, 0, 0.3f };
        for (int i = 1; i <= 48; ++i) {
            const Vec3 p = curve.At(i / 48.0f) + Vec3{ 0, 0, 0.3f };
            line(previous, p, c);
            previous = p;
        }
        if (chosen) {
            // The edges of the selected road.
            const float half = s.profile.HalfWidth();
            Vec3 left0{}, right0{};
            for (int i = 0; i <= 48; ++i) {
                const float t = i / 48.0f;
                const Vec3 d = HorizontalDirection(curve.Tangent(t));
                const Vec3 r{ d.y, -d.x, 0.0f };
                const Vec3 p = curve.At(t) + Vec3{ 0, 0, 0.35f };
                const Vec3 l = p - r * half, rr = p + r * half;
                if (i) { line(left0, l, 0xFF40E0FF); line(right0, rr, 0xFF40E0FF); }
                left0 = l;
                right0 = rr;
            }
        }
    }

    // The drawing tool: what a click would do, and the road it would draw.
    if (drawTool_ && drawHover_.kind != DrawHit::None) {
        const D3DCOLOR c = drawHover_.kind == DrawHit::Node ? 0xFF40FF60 : drawHover_.kind == DrawHit::Road ? 0xFF40E0FF
                                                                                                          : 0xFFFFFFFF;
        const Vec3 at = drawHover_.point + Vec3{ 0, 0, 0.4f };
        const float r = 1.6f;
        for (int k = 0; k < 20; ++k) {
            const float a0 = k * 6.2831853f / 20, a1 = (k + 1) * 6.2831853f / 20;
            line(at + Vec3{ r * std::cos(a0), r * std::sin(a0), 0 }, at + Vec3{ r * std::cos(a1), r * std::sin(a1), 0 }, c);
        }
        line(at, at + Vec3{ 0, 0, 3.0f }, c);
        Vec3 from{};
        bool band = false;
        if (drawPending_) { from = drawStart_; band = true; }
        else if (const roads::Node* n = network_.FindNode(drawFrom_)) { from = n->position; band = true; }
        if (band) {
            const Vec3 a = from + Vec3{ 0, 0, 0.4f };
            const Vec3 d = HorizontalDirection(at - a);
            const Vec3 side{ d.y, -d.x, 0.0f };
            const float half = drawProfile_.HalfWidth();
            line(a, at, 0xFFFFD040);
            line(a + side * half, at + side * half, 0xA0FFD040);
            line(a - side * half, at - side * half, 0xA0FFD040);
        }
    }

    // While a node is dragged: the node it joins if let go, ringed.
    if (dragging_)
        if (const roads::Node* moving = SelectedNode())
            if (const u32 target = SnapTarget(moving->id))
                if (const roads::Node* t = network_.FindNode(target)) {
                    const float r = std::max(2.0f, NodeSize(*t) * 1.6f);
                    const Vec3 c = t->position + Vec3{ 0, 0, 0.5f };
                    for (int k = 0; k < 24; ++k) {
                        const float a0 = k * 6.2831853f / 24, a1 = (k + 1) * 6.2831853f / 24;
                        line(c + Vec3{ r * std::cos(a0), r * std::sin(a0), 0 }, c + Vec3{ r * std::cos(a1), r * std::sin(a1), 0 },
                             0xFF40FF60);
                    }
                    line(moving->position + Vec3{ 0, 0, 0.5f }, c, 0xFF40FF60);
                }

    // The curve handles on show: a line from the node and a small diamond.
    for (const auto& [segmentId, atStart] : VisibleHandles()) {
        const roads::Segment* s = network_.FindSegment(segmentId);
        if (!s) continue;
        const roads::Node* node = network_.FindNode(atStart ? s->a : s->b);
        if (!node) continue;
        const bool chosen = selected == (int)(segmentId | (atStart ? kHandleAtStart : kHandleAtEnd));
        const D3DCOLOR c = chosen ? 0xFFFFFFFF : 0xFFFF40FF;
        const Vec3 h = HandlePoint(*s, atStart) + Vec3{ 0, 0, 0.4f };
        line(node->position + Vec3{ 0, 0, 0.4f }, h, c);
        const float r = 0.9f;
        const Vec3 corners[6] = { h + Vec3{ r, 0, 0 }, h + Vec3{ 0, r, 0 }, h + Vec3{ -r, 0, 0 },
                                  h + Vec3{ 0, -r, 0 }, h + Vec3{ 0, 0, r }, h + Vec3{ 0, 0, -r } };
        for (int k = 0; k < 4; ++k)
            for (int tip = 4; tip < 6; ++tip) {
                const Vec3& a = corners[k];
                const Vec3& b = corners[(k + 1) % 4];
                const Vec3& t = corners[tip];
                solid.push_back({ a.x, a.y, a.z, c });
                solid.push_back({ b.x, b.y, b.z, c });
                solid.push_back({ t.x, t.y, t.z, c });
            }
    }

    // Nodes: a small pyramid each, white when selected, orange at junctions.
    for (const roads::Node& node : network_.Nodes()) {
        const size_t count = network_.SegmentsAt(node.id).size();
        const bool chosen = (int)node.id == selected;
        const D3DCOLOR c = chosen ? 0xFFFFFFFF : count >= 3 ? 0xFFFF8020 : count == 2 ? 0xFF40C0FF : 0xFFFFE040;
        const float size = NodeSize(node);
        const Vec3 p = node.position;
        const Vec3 top = p + Vec3{ 0, 0, size * 1.6f };
        const Vec3 base[4] = { p + Vec3{ size, 0, 0.2f }, p + Vec3{ 0, size, 0.2f }, p + Vec3{ -size, 0, 0.2f },
                               p + Vec3{ 0, -size, 0.2f } };
        for (int k = 0; k < 4; ++k) {
            const Vec3& a = base[k];
            const Vec3& b = base[(k + 1) % 4];
            solid.push_back({ a.x, a.y, a.z, c });
            solid.push_back({ b.x, b.y, b.z, c });
            solid.push_back({ top.x, top.y, top.z, c });
            line(a, b, 0xFF000000);
            line(a, top, 0xFF000000);
        }
        if (chosen) {
            // The direction roads run through it.
            const float h = Radians(node.hasHeading ? node.heading : AutoHeadingOf(node));
            const Vec3 d{ std::cos(h), std::sin(h), 0.0f };
            line(p + Vec3{ 0, 0, 0.4f } - d * (size * 3.0f), p + Vec3{ 0, 0, 0.4f } + d * (size * 3.0f),
                 node.hasHeading ? 0xFFFF40FF : 0xFFFFFFFF);
        }
    }

    // The outline being drawn, closed back to its first point.
    if (drawing_) {
        std::vector<Vec3> points = outline_;
        if (hoverValid_) points.push_back(hover_);
        for (size_t k = 0; k + 1 < points.size(); ++k)
            line(points[k] + Vec3{ 0, 0, 0.4f }, points[k + 1] + Vec3{ 0, 0, 0.4f }, 0xFF60FF60);
        if (points.size() >= 3) line(points.back() + Vec3{ 0, 0, 0.4f }, points.front() + Vec3{ 0, 0, 0.4f }, 0x8060FF60);
        for (const Vec3& p : outline_) line(p, p + Vec3{ 0, 0, 3.0f }, 0xFFFFFFFF);
    }

    // The terrain brush: a ring where it would work.
    if (brush_ != terrain::Brush::None && brushHit_) {
        const D3DCOLOR c = stroking_ ? 0xFFFFFFFF : 0xFF60FF60;
        const Vec3 o = brushPoint_ + Vec3{ 0, 0, 0.3f };
        for (int k = 0; k < 48; ++k) {
            const float a0 = 2.0f * kPi * k / 48, a1 = 2.0f * kPi * (k + 1) / 48;
            line(o + Vec3{ std::cos(a0) * brushRadius_, std::sin(a0) * brushRadius_, 0 },
                 o + Vec3{ std::cos(a1) * brushRadius_, std::sin(a1) * brushRadius_, 0 }, c);
        }
        line(o, o + Vec3{ 0, 0, 3.0f }, c);
    }

    renderer.BeginOverlay(device);
    // Over everything: they are handles, and a road hides its own nodes.
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    if (!lines.empty())
        device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2), lines.data(), sizeof(Vertex));
    if (!solid.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(solid.size() / 3), solid.data(), sizeof(Vertex));
    renderer.EndOverlay(device);
}

// ---- baking --------------------------------------------------------------

bool RoadsExtension::RemoveBaked(size_t& removed) {
    removed = 0;
    nfs::World* world = app_.World();
    if (!world) return false;
    std::vector<u32> hashes;
    for (const nfs::SolidObject* model : world->AllModels())
        if (model && roads::IsBakedRoadModel(model->name) &&
            std::find(hashes.begin(), hashes.end(), model->hash) == hashes.end())
            hashes.push_back(model->hash);
    if (hashes.empty()) return true;
    app_.Edits().Refresh(world, app_.ModelsFolder());
    std::wstring summary;
    if (!app_.Edits().RevertModels(app_.Window(), hashes, summary)) return false;
    removed = hashes.size();
    LOG_INFO("roads: %zu baked road models taken out of the region", removed);
    app_.ReopenRegionKeepingView();
    return true;
}

namespace {

// A collision polygon the last bake made, as it was put in.
struct RecordedPolygon {
    Vec3 corners[4];
    bool quad = false, barrier = false;
    u8 surface = 0;
};

bool SamePolygon(const RecordedPolygon& r, const nfs::CollisionPolygon& p) {
    if (r.barrier != p.barrier || r.quad != p.quad || r.surface != p.surface) return false;
    // The pack stores corners to an eighth of a unit across and a 256th up,
    // and barriers without heights.
    for (int c = 0; c < 4; ++c) {
        if (std::fabs(r.corners[c].x - p.corners[c].x) > 0.2f) return false;
        if (std::fabs(r.corners[c].y - p.corners[c].y) > 0.2f) return false;
        if (!r.barrier && std::fabs(r.corners[c].z - p.corners[c].z) > 0.2f) return false;
    }
    return true;
}

constexpr float kRecordCell = 4.0f;

long long CellKey(float x, float y) {
    const long long cx = (long long)std::floor(x / kRecordCell);
    const long long cy = (long long)std::floor(y / kRecordCell);
    return (cx << 32) ^ (cy & 0xFFFFFFFFll);
}

Vec3 Centre(const Vec3* corners) { return (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f; }

} // namespace

// The street lights as placements of the game's models, in the section each
// stands in. The ones the last bake made are found by model and position and
// taken out first. Written by the next save, with the rest of the edits.
bool RoadsExtension::BakePoles(const std::vector<roads::Pole>& poles, size_t& added, size_t& removed) {
    added = removed = 0;
    nfs::World* world = app_.World();
    if (!world) return false;

    struct Recorded { u32 hash; Vec3 at; };
    std::vector<Recorded> old;
    {
        std::ifstream in(PoleRecordPath(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string hash;
            Recorded r{};
            fields >> hash >> r.at.x >> r.at.y >> r.at.z;
            if (!fields) continue;
            r.hash = (u32)std::strtoul(hash.c_str(), nullptr, 16);
            old.push_back(r);
        }
    }
    bool changed = false;
    for (size_t s = 0; s < world->Loaded().size() && !old.empty(); ++s) {
        nfs::LoadedSection* section = world->MutableLoaded(s);
        if (!section) continue;
        auto& list = section->placements;
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const nfs::LoadedSection::UserPlacement& placement) {
                                      const Mat4& t = placement.instance.transform;
                                      const Vec3 at{ t.m[3][0], t.m[3][1], t.m[3][2] };
                                      return std::any_of(old.begin(), old.end(), [&](const Recorded& r) {
                                          return r.hash == placement.instance.modelHash && Length(r.at - at) < 0.1f;
                                      });
                                  }),
                   list.end());
        if (list.size() != before) {
            removed += before - list.size();
            section->placementsChanged = true;
            changed = true;
        }
    }

    std::ofstream out(PoleRecordPath(), std::ios::binary | std::ios::trunc);
    out << "# street lights placed by the last road bake: model hash, x y z\n";
    const auto& loaded = world->Loaded();
    for (const roads::Pole& pole : poles) {
        const u32 hash = TextureHash(pole.model);    // the game hashes model names the same way
        const nfs::SolidObject* model = world->FindModel(hash);
        if (!model) {
            LOG_WARN("roads: street light model %s is not in the region", pole.model.c_str());
            continue;
        }
        // A section the game has loaded where it stands.
        size_t target = world->SectionForPlacementAlong({ pole.position });
        if (target == SIZE_MAX) target = world->SectionForPlacement(pole.position);
        nfs::LoadedSection* section = target < loaded.size() ? world->MutableLoaded(target) : nullptr;
        if (!section || !section->info) continue;

        nfs::PlacedInstance placed;
        placed.modelHash = hash;
        placed.modelName = model->name;
        placed.modelBounds = model->bounds;
        placed.triangles = model->TriangleCount();
        if (model->bounds.Valid()) placed.radius = Length(model->bounds.max - model->bounds.min) * 0.5f;
        placed.transform = PoleTransform(pole);
        // What the game already uses for this model, as Objects > Place does.
        for (const auto& other : loaded) {
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
        // A model kept in another positioned section is copied in on save.
        const size_t home = world->ModelHome(hash);
        const bool library = home < loaded.size() && world->IsLibrary(*loaded[home]);
        if (!library && !section->HoldsModel(hash) &&
            std::find(section->importedModels.begin(), section->importedModels.end(), hash) ==
                section->importedModels.end())
            section->importedModels.push_back(hash);
        section->placements.push_back({ section->info->number, placed });
        section->placementsChanged = true;
        changed = true;
        ++added;
        char text[160];
        std::snprintf(text, sizeof(text), "%08X %.4f %.4f %.4f\n", hash, pole.position.x, pole.position.y,
                      pole.position.z);
        out << text;
    }
    if (changed) {
        app_.Renderer().ClearSelection();
        app_.Renderer().RebuildInstances(*world);
    }
    LOG_INFO("roads: %zu street lights placed, %zu from the last bake removed", added, removed);
    return true;
}

namespace {
struct UnloggedCollision {
    collision::Editor& editor;
    explicit UnloggedCollision(collision::Editor& e) : editor(e) { editor.SetLogging(false); }
    ~UnloggedCollision() { editor.SetLogging(true); }
    UnloggedCollision(const UnloggedCollision&) = delete;
    UnloggedCollision& operator=(const UnloggedCollision&) = delete;
};
} // namespace

// The ground's collision follows the ground: every polygon of it with a
// corner the offsets or the road fitting move is replaced by a moved copy,
// and inside a cut of the game's roads it is taken out -- cut on the cut's
// edge where it crosses it. Barriers and the roads' own polygons are left
// alone, except that walls standing inside a cut go.
bool RoadsExtension::BakeGroundCollision(size_t& moved) {
    moved = 0;
    nfs::World* world = app_.World();
    CollisionTools* tools = app_.Collision();
    if (!world || !tools) return false;
    cutsCache_ = network_.Cuts();
    const auto& cuts = cutsCache_;
    if (ground_.Empty() && !(fitGround_ && !groundField_.Empty()) && cuts.empty()) return true;
    collision::Editor& editor = tools->Editor();
    if (editor.World() != world) editor.SetWorld(world);
    // The bake's polygons are the roads' own edit (the "Roads baked" row),
    // kept out of the collision change log so they are not listed twice.
    const UnloggedCollision unlogged(editor);

    const auto ours = [&](const nfs::CollisionPolygon& p) {
        for (const nfs::CollisionPolygon& r : roadCollision_) {
            bool same = r.quad == p.quad && r.barrier == p.barrier;
            for (int c = 0; c < 4 && same; ++c)
                same = std::fabs(r.corners[c].x - p.corners[c].x) < 0.2f &&
                       std::fabs(r.corners[c].y - p.corners[c].y) < 0.2f &&
                       (r.barrier || std::fabs(r.corners[c].z - p.corners[c].z) < 0.3f);
            if (same) return true;
        }
        return false;
    };

    std::vector<collision::PolygonRef> refs;
    std::vector<nfs::CollisionPolygon> replacements;
    std::vector<std::pair<std::vector<Vec3>, u8>> pieces;   // cut ground, as outlines
    std::set<std::string> seen;
    size_t removedInCuts = 0;

    // Where a new road comes into one of the game's roads through a junction
    // made of its faces, the game's edge barriers across the way are opened
    // exactly as wide as the road, and where the road has barriers of its
    // own each one is carried on to the game's, so the edge stays one closed
    // line with the road inside it. A car crossing a barrier is put back on
    // the road, so one left across the mouth would throw it back.
    struct Mouth {
        Vec3 node, u, r;
        float lo = 0, hi = 0, back = 0, front = 0;   // across (r) and along (u, into the junction)
        bool wall[2] = { false, false };              // the road's own barriers, left and right
    };
    std::vector<Mouth> mouths;
    for (const roads::FaceJunction& j : network_.FaceJunctions()) {
        const roads::Node* node = network_.FindNode(j.node);
        Vec3 into;
        const roads::Segment* segment = nullptr;
        if (!node || !network_.EndDirection(j.node, into, segment) || j.faces.empty()) continue;
        Mouth m;
        m.node = node->position;
        m.u = into;
        m.r = { into.y, -into.x, 0.0f };
        const roads::RoadProfile& p = segment->profile;
        m.lo = -p.Edge(-1);
        m.hi = p.Edge(1);
        m.back = -kUnderRoad;
        for (const roads::GameFace& f : j.faces)
            for (const Vec3& q : f.p) m.front = std::max(m.front, Dot(q - m.node, m.u));
        m.front += 2.0f;
        m.wall[0] = p.Wall(-1) && p.wallHeight >= 0.05f;
        m.wall[1] = p.Wall(1) && p.wallHeight >= 0.05f;
        mouths.push_back(m);
    }
    std::vector<nfs::CollisionPolygon> connectors;
    size_t barriersOpened = 0;
    // The part of a barrier's line inside a mouth: t0..t1 of a->b, or false.
    const auto across = [](const Mouth& m, const Vec3& a, const Vec3& b, float& t0, float& t1) {
        t0 = 0.0f;
        t1 = 1.0f;
        const Vec3 d{ b.x - a.x, b.y - a.y, 0.0f };
        const Vec3 o{ a.x - m.node.x, a.y - m.node.y, 0.0f };
        const float bounds[2][3] = { { Dot(o, m.r), Dot(d, m.r), 0 }, { Dot(o, m.u), Dot(d, m.u), 0 } };
        const float low[2] = { m.lo, m.back }, high[2] = { m.hi, m.front };
        for (int axis = 0; axis < 2; ++axis) {
            const float start = bounds[axis][0], step = bounds[axis][1];
            if (std::fabs(step) < 1e-6f) {
                if (start < low[axis] || start > high[axis]) return false;
                continue;
            }
            float ta = (low[axis] - start) / step, tb = (high[axis] - start) / step;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        return t1 - t0 > 1e-4f;
    };
    const auto& loaded = world->Loaded();
    for (size_t s = 0; s < loaded.size(); ++s) {
        const auto& packs = loaded[s]->collision;
        for (size_t k = 0; k < packs.size(); ++k)
            for (size_t i = 0; i < packs[k].polygons.size(); ++i) {
                const nfs::CollisionPolygon& p = packs[k].polygons[i];
                if (ours(p)) continue;
                char key[160];
                std::snprintf(key, sizeof(key), "%.2f %.2f %.2f %.2f %.2f %.2f %d %d", p.corners[0].x, p.corners[0].y,
                              p.corners[1].x, p.corners[1].y, p.corners[2].x, p.corners[2].y, (int)p.surface,
                              (int)p.flags);
                if (p.barrier && !mouths.empty()) {
                    const Vec3 a = p.corners[0], b = p.corners[2];
                    bool opened = false;
                    for (const Mouth& m : mouths) {
                        float t0 = 0.0f, t1 = 0.0f;
                        // A link made by an earlier bake, on the opening's edge: kept.
                        const float la = Dot(a - m.node, m.r), lb = Dot(b - m.node, m.r);
                        if ((std::fabs(la - m.lo) < 0.06f && std::fabs(lb - m.lo) < 0.06f) ||
                            (std::fabs(la - m.hi) < 0.06f && std::fabs(lb - m.hi) < 0.06f))
                            continue;
                        if (!across(m, a, b, t0, t1)) continue;
                        refs.push_back({ (u32)s, (u32)k, (u32)i });
                        opened = true;
                        if (!seen.insert(key).second) break;
                        ++barriersOpened;
                        const Vec3 cut0 = a + (b - a) * t0, cut1 = a + (b - a) * t1;
                        // What is left either side, as it was otherwise.
                        const auto keep = [&](const Vec3& from, const Vec3& to) {
                            if (Length(Vec3{ to.x - from.x, to.y - from.y, 0.0f }) < 0.05f) return;
                            nfs::CollisionPolygon q = p;
                            const nfs::CollisionPolygon shape = collision::MakeBarrier(from, to, p.surface);
                            for (int c = 0; c < 4; ++c) q.corners[c] = shape.corners[c];
                            replacements.push_back(q);
                        };
                        keep(a, cut0);
                        keep(cut1, b);
                        // The road's own barrier on from its end to the opening.
                        for (const Vec3& at : { cut0, cut1 }) {
                            const float lat = Dot(at - m.node, m.r), along = Dot(at - m.node, m.u);
                            const int side = lat >= 0.0f ? 1 : 0;
                            if (!m.wall[side] || along <= 0.05f || std::fabs(lat - (side ? m.hi : m.lo)) > 0.05f) continue;
                            const Vec3 start = m.node + m.r * (side ? m.hi : m.lo);
                            const Vec3 end{ at.x, at.y, start.z };
                            connectors.push_back(collision::MakeBarrier(start, end, p.surface));
                            connectors.push_back(collision::MakeBarrier(end, start, p.surface));
                        }
                        break;
                    }
                    if (opened) continue;
                }
                if (p.barrier || p.Vertical()) {
                    // A wall goes when its middle stands inside a cut.
                    const Vec3 middle = (p.corners[0] + p.corners[2]) * 0.5f;
                    bool inCut = false;
                    for (const roads::GameCut& cut : cuts) inCut |= cut.Inside(middle, 0.05f);
                    if (inCut) {
                        refs.push_back({ (u32)s, (u32)k, (u32)i });
                        if (seen.insert(key).second) ++removedInCuts;
                    }
                    continue;
                }
                nfs::CollisionPolygon q = p;
                bool changed = false;
                for (Vec3& c : q.corners) {
                    const float z = GroundAt(c.x, c.y, c.z);
                    changed |= std::fabs(z - c.z) > 0.01f;
                    c.z = z;
                }
                // Through the cuts.
                std::vector<std::vector<Vec3>> parts{ std::vector<Vec3>(q.corners, q.corners + (q.quad ? 4 : 3)) };
                bool cutTouched = false;
                for (const roads::GameCut& cut : cuts) {
                    std::vector<std::vector<Vec3>> next;
                    for (const auto& part : parts) {
                        bool touched = false;
                        auto kept = roads::ClipOutside(part, cut, [](const Vec3& v) { return v; },
                                                       [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; },
                                                       touched);
                        cutTouched |= touched;
                        for (auto& piece : kept) next.push_back(std::move(piece));
                    }
                    parts = std::move(next);
                }
                if (!changed && !cutTouched) continue;
                refs.push_back({ (u32)s, (u32)k, (u32)i });
                if (!seen.insert(key).second) continue;
                if (!cutTouched) {
                    replacements.push_back(q);
                } else {
                    if (parts.empty()) ++removedInCuts;
                    for (auto& part : parts) pieces.push_back({ std::move(part), q.surface });
                }
            }
    }
    for (auto& [outline, surface] : pieces) {
        // Tiny slivers on the cut's edge are not worth a polygon.
        float area = 0.0f;
        for (size_t i = 0; i < outline.size(); ++i) {
            const Vec3& a = outline[i];
            const Vec3& b = outline[(i + 1) % outline.size()];
            area += a.x * b.y - b.x * a.y;
        }
        if (std::fabs(area) < 0.02f) continue;
        collision::AppendGround(outline, surface, replacements);
    }
    if (refs.empty()) return true;
    editor.Remove(refs);
    replacements.insert(replacements.end(), connectors.begin(), connectors.end());
    if (barriersOpened)
        LOG_INFO("ground: %zu game barriers opened at junction mouths, %zu links to the roads' own", barriersOpened,
                 connectors.size() / 2);
    if (!replacements.empty()) editor.Add(replacements, L"Sculpt the ground");
    tools->RefreshAfterEdit();
    moved = replacements.size() + removedInCuts;
    LOG_INFO("ground: %zu collision polygons replaced, %zu taken out inside road cuts (%zu stored copies)",
             replacements.size(), removedInCuts, refs.size());
    return true;
}

namespace {

// A corner of a game mesh being rewritten: everything the OBJ carries.
struct MeshCorner {
    Vec3 position;     // world
    Vec3 normal;       // world
    Vec2 uv;
    u32  color = 0;
};

MeshCorner MixCorners(const MeshCorner& a, const MeshCorner& b, float t) {
    MeshCorner m;
    m.position = a.position + (b.position - a.position) * t;
    m.normal = a.normal + (b.normal - a.normal) * t;
    m.uv = { a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t };
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = (float)((a.color >> shift) & 0xFF), cb = (float)((b.color >> shift) & 0xFF);
        m.color |= (u32)std::clamp((int)std::lround(ca + (cb - ca) * t), 0, 255) << shift;
    }
    return m;
}

} // namespace

// The ground's meshes: each one the sculpting moves or a cut reaches is
// written out whole -- every vertex where the ground now is, the triangles a
// cut takes out gone and the ones across its edge cut on it -- under the names
// and origin the Blender export gives it, and imported back the same way: the
// path that rebuilds a section around a changed mesh.
bool RoadsExtension::BakeGroundModels(size_t& moved, std::wstring& error) {
    moved = 0;
    nfs::World* world = app_.World();
    if (!world) return false;
    CollectGroundModels(*world);
    BuildGroundField();
    cutsCache_ = network_.Cuts();
    const auto& cuts = cutsCache_;

    std::vector<PickResult> selection;
    std::vector<const GroundModel*> chosen;
    for (const GroundModel& g : groundModels_) {
        bool changed = false;
        for (const roads::GameCut& cut : cuts) {
            const Aabb box = cut.Bounds();
            if (g.bounds.max.x < box.min.x || g.bounds.min.x > box.max.x || g.bounds.max.y < box.min.y ||
                g.bounds.min.y > box.max.y)
                continue;
            for (const Vec3& p : g.world)
                if (cut.Inside(p, 0.01f)) { changed = true; break; }
            // A triangle can cross a cut with no corner inside it.
            const auto& indices = g.model->indices;
            for (size_t k = 0; k + 2 < indices.size() && !changed; k += 3) {
                std::vector<Vec3> tri{ g.world[indices[k]], g.world[indices[k + 1]], g.world[indices[k + 2]] };
                bool touched = false;
                roads::ClipOutside(tri, cut, [](const Vec3& v) { return v; },
                                   [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }, touched);
                changed |= touched;
            }
            if (changed) break;
        }
        for (const Vec3& p : g.world)
            if (!changed && std::fabs(GroundAt(p.x, p.y, p.z) - p.z) > 0.005f) changed = true;
        if (!changed) continue;
        PickResult pick;
        pick.object = g.model;
        pick.transform = g.model->transform;
        selection.push_back(pick);
        chosen.push_back(&g);
    }
    if (selection.empty()) return true;

    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) return false;
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const std::wstring folder = std::wstring(localAppData) + L"\\FRSWorldEditor\\groundbake\\" + world->Region() +
                                Fmt(L"-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                                    now.wMinute, now.wSecond);
    ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const std::wstring obj = folder + L"\\ground.obj";
    ExportSummary summary;
    if (!ExportSelection(*world, selection, obj, world->Region(), summary)) {
        error = L"the terrain meshes could not be exported";
        return false;
    }

    // The export's origin, from its manifest, and its object names in order.
    Vec3 origin{};
    {
        std::ifstream in(summary.manifestPath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t at = text.find("\"origin\"");
        const size_t open = at == std::string::npos ? std::string::npos : text.find('[', at);
        if (open == std::string::npos ||
            std::sscanf(text.c_str() + open + 1, "%f , %f , %f", &origin.x, &origin.y, &origin.z) != 3) {
            error = L"the export's manifest has no origin";
            return false;
        }
    }
    std::vector<std::string> names;
    {
        std::ifstream in(obj, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("o ", 0) == 0) names.push_back(line.substr(2));
        }
    }
    if (names.size() != chosen.size()) {
        error = L"the export does not hold the meshes asked for";
        return false;
    }

    // OBJ is Y up: obj(x, y, z) = game(x, -z, y).
    const auto toObj = [](const Vec3& v) { return Vec3{ v.x, v.z, -v.y }; };
    std::string out = "# FRS World Editor: the game's ground, sculpted and cut for the roads\n";
    size_t base = 1;
    char text[256];
    for (size_t m = 0; m < chosen.size(); ++m) {
        const GroundModel& g = *chosen[m];
        const nfs::SolidObject& model = *g.model;
        std::vector<MeshCorner> corners;
        std::vector<std::pair<u32, std::vector<u32>>> groups;   // texture hash, triangle corners
        std::unordered_map<u32, u32> reused;                    // original vertex -> corner
        const auto original = [&](u32 index) {
            const auto found = reused.find(index);
            if (found != reused.end()) return found->second;
            const nfs::SolidVertex& v = model.vertices[index];
            MeshCorner c;
            const Vec3 p = g.world[index];
            c.position = { p.x, p.y, GroundAt(p.x, p.y, p.z) };
            const Vec3 n = model.transform.TransformDirection(v.normal);
            c.normal = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0, 0, 1 };
            c.uv = v.uv;
            c.color = v.color;
            corners.push_back(c);
            reused[index] = (u32)corners.size() - 1;
            return (u32)corners.size() - 1;
        };
        for (const nfs::SolidMaterial& material : model.materials) {
            const u32 hash = material.textureIndex < model.textureHashes.size()
                                 ? model.textureHashes[material.textureIndex] : 0;
            std::vector<u32> triangles;
            const size_t last = std::min<size_t>((size_t)material.firstIndex + material.indexCount, model.indices.size());
            for (size_t k = material.firstIndex; k + 2 < last; k += 3) {
                const u32 ia = model.indices[k], ib = model.indices[k + 1], ic = model.indices[k + 2];
                // As the game has it, then through each cut.
                std::vector<MeshCorner> tri;
                for (u32 i : { ia, ib, ic }) {
                    const nfs::SolidVertex& v = model.vertices[i];
                    MeshCorner c;
                    const Vec3 p = g.world[i];
                    c.position = { p.x, p.y, GroundAt(p.x, p.y, p.z) };
                    const Vec3 n = model.transform.TransformDirection(v.normal);
                    c.normal = Length(n) > 1e-6f ? Normalize(n) : Vec3{ 0, 0, 1 };
                    c.uv = v.uv;
                    c.color = v.color;
                    tri.push_back(c);
                }
                std::vector<std::vector<MeshCorner>> parts{ tri };
                bool cutTouched = false;
                for (const roads::GameCut& cut : cuts) {
                    std::vector<std::vector<MeshCorner>> next;
                    for (const auto& part : parts) {
                        bool touched = false;
                        auto kept = roads::ClipOutside(part, cut, [](const MeshCorner& c) { return c.position; },
                                                       MixCorners, touched);
                        cutTouched |= touched;
                        for (auto& piece : kept) next.push_back(std::move(piece));
                    }
                    parts = std::move(next);
                }
                if (!cutTouched) {
                    triangles.push_back(original(ia));
                    triangles.push_back(original(ib));
                    triangles.push_back(original(ic));
                    continue;
                }
                for (const auto& part : parts) {
                    for (size_t f = 1; f + 1 < part.size(); ++f) {
                        const Vec3 e1 = part[f].position - part[0].position;
                        const Vec3 e2 = part[f + 1].position - part[0].position;
                        if (Length(Cross(e1, e2)) < 1e-4f) continue;   // a sliver on the cut's edge
                        for (const MeshCorner* c : { &part[0], &part[f], &part[f + 1] }) {
                            corners.push_back(*c);
                            triangles.push_back((u32)corners.size() - 1);
                        }
                    }
                }
            }
            if (!triangles.empty()) groups.push_back({ hash, std::move(triangles) });
        }
        out += "o " + names[m] + "\n";
        for (const MeshCorner& c : corners) {
            const Vec3 p = toObj(c.position - origin);
            std::snprintf(text, sizeof(text), "v %.4f %.4f %.4f %.4f %.4f %.4f\n", p.x, p.y, p.z,
                          ((c.color >> 16) & 0xFF) / 255.0f, ((c.color >> 8) & 0xFF) / 255.0f, (c.color & 0xFF) / 255.0f);
            out += text;
        }
        for (const MeshCorner& c : corners) {
            const Vec3 n = toObj(c.normal);
            std::snprintf(text, sizeof(text), "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
            out += text;
        }
        for (const MeshCorner& c : corners) {
            std::snprintf(text, sizeof(text), "vt %.5f %.5f\n", c.uv.x, 1.0f - c.uv.y);
            out += text;
        }
        for (const auto& [hash, triangles] : groups) {
            if (hash) std::snprintf(text, sizeof(text), "usemtl tex_%08X\n", hash);
            else std::snprintf(text, sizeof(text), "usemtl untextured\n");
            out += text;
            for (size_t k = 0; k + 2 < triangles.size(); k += 3) {
                const size_t a = base + triangles[k], b = base + triangles[k + 1], c = base + triangles[k + 2];
                std::snprintf(text, sizeof(text), "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
                out += text;
            }
        }
        base += corners.size();
        out += "\n";
    }
    {
        std::ofstream file(obj, std::ios::binary | std::ios::trunc);
        file << out;
    }
    moved = selection.size();
    LOG_INFO("ground: %zu terrain meshes written to %S", moved, obj.c_str());
    app_.ImportEdited(obj);
    return true;
}

bool RoadsExtension::BakeCollision(const std::vector<roads::MeshPiece>& pieces, size_t& added, size_t& removed) {
    added = removed = 0;
    nfs::World* world = app_.World();
    CollisionTools* tools = app_.Collision();
    if (!world || !tools) return false;
    collision::Editor& editor = tools->Editor();
    if (editor.World() != world) editor.SetWorld(world);
    // The bake's polygons are the roads' own edit (the "Roads baked" row),
    // kept out of the collision change log so they are not listed twice.
    const UnloggedCollision unlogged(editor);

    // What the last bake put in comes out first.
    std::vector<RecordedPolygon> old;
    {
        std::ifstream in(CollisionRecordPath(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            RecordedPolygon r;
            int quad = 0, barrier = 0, surface = 0;
            fields >> quad >> barrier >> surface;
            for (Vec3& c : r.corners) fields >> c.x >> c.y >> c.z;
            if (!fields) continue;
            r.quad = quad != 0;
            r.barrier = barrier != 0;
            r.surface = (u8)surface;
            old.push_back(r);
        }
    }
    if (!old.empty()) {
        std::unordered_map<long long, std::vector<size_t>> grid;
        for (size_t i = 0; i < old.size(); ++i) {
            const Vec3 c = Centre(old[i].corners);
            grid[CellKey(c.x, c.y)].push_back(i);
        }
        std::vector<collision::PolygonRef> refs;
        const auto& loaded = world->Loaded();
        for (size_t s = 0; s < loaded.size(); ++s) {
            const auto& packs = loaded[s]->collision;
            for (size_t k = 0; k < packs.size(); ++k) {
                const auto& polygons = packs[k].polygons;
                for (size_t i = 0; i < polygons.size(); ++i) {
                    const Vec3 c = Centre(polygons[i].corners);
                    bool match = false;
                    for (int dx = -1; dx <= 1 && !match; ++dx)
                        for (int dy = -1; dy <= 1 && !match; ++dy) {
                            const auto cell = grid.find(CellKey(c.x + dx * kRecordCell, c.y + dy * kRecordCell));
                            if (cell == grid.end()) continue;
                            for (size_t r : cell->second)
                                if (SamePolygon(old[r], polygons[i])) { match = true; break; }
                        }
                    if (match) refs.push_back({ (u32)s, (u32)k, (u32)i });
                }
            }
        }
        if (!refs.empty()) removed = editor.Remove(refs);
        LOG_INFO("roads: %zu stored polygons match the %zu recorded from the last bake; %zu removed", refs.size(),
                 old.size(), removed);
    }

    // The new ones.
    std::vector<nfs::CollisionPolygon> polygons;
    for (const roads::MeshPiece& piece : pieces) {
        for (const roads::CollisionStrip& strip : piece.strips)
            if (strip.left.size() >= 2 && strip.left.size() == strip.right.size())
                collision::AppendStrip(strip.left, strip.right, strip.surface, polygons);
        for (const roads::CollisionArea& area : piece.areas)
            if (area.outline.size() >= 3) collision::AppendGround(area.outline, area.surface, polygons);
        for (const roads::CollisionWall& wall : piece.walls) {
            if (wall.points.size() < 2) continue;
            collision::WallOptions options;
            options.kind = wall.barrier ? collision::WallKind::Barrier : collision::WallKind::Vertical;
            options.surface = wall.surface;
            options.height = wall.height;
            options.sink = wall.barrier ? 0.5f : 0.1f;
            collision::AppendWall(wall.points, options, polygons);
        }
    }
    roadCollision_ = polygons;
    std::vector<collision::PolygonRef> refs;
    if (!polygons.empty()) refs = editor.Add(polygons, L"Bake roads");
    added = polygons.size();
    tools->RefreshAfterEdit();

    // Recorded as they went in, so the next bake can find them again.
    std::ofstream out(CollisionRecordPath(), std::ios::binary | std::ios::trunc);
    out << "# road collision made by the last bake: quad barrier surface, then 4 corners x y z\n";
    char text[512];
    for (const nfs::CollisionPolygon& p : polygons) {
        std::snprintf(text, sizeof(text), "%d %d %d", p.quad ? 1 : 0, p.barrier ? 1 : 0, (int)p.surface);
        out << text;
        for (const Vec3& c : p.corners) {
            std::snprintf(text, sizeof(text), " %.4f %.4f %.4f", c.x, c.y, c.z);
            out << text;
        }
        out << "\n";
    }
    LOG_INFO("roads: %zu collision polygons added (%zu stored, copies included), %zu old removed",
             polygons.size(), refs.size(), removed);
    return true;
}

void RoadsExtension::Unbake() {
    if (!app_.World()) return;
    if (!app_.ConfirmUnsaved()) return;
    DiskStep step(this, L"remove the baked roads");
    size_t collisionAdded = 0, collisionRemoved = 0, polesAdded = 0, polesRemoved = 0;
    if (!BakePoles({}, polesAdded, polesRemoved) ||
        (app_.Collision() && !BakeCollision({}, collisionAdded, collisionRemoved)) || !app_.SaveEdits()) {
        app_.Message(L"The road collision could not be taken out. The log has why.", MB_ICONWARNING);
        return;
    }
    size_t removed = 0;
    if (!RemoveBaked(removed)) {
        app_.Message(L"The baked roads could not be taken out. The log has why.", MB_ICONWARNING);
        return;
    }
    WriteBakeStamp();   // unbaked on purpose: the game start leaves it so until the source changes
    app_.Status(Fmt(L"%zu baked road pieces and %zu collision polygons removed from the game; the road network "
                    L"is kept in the editor (Ctrl+Z puts them back)", removed, collisionRemoved));
}

void RoadsExtension::Bake() {
    if (!app_.World()) {
        app_.Message(L"Open a region first.");
        return;
    }
    // A "yes" here saves the edits, which reopens the region: nothing of the
    // world from before it may be used after.
    if (!app_.ConfirmUnsaved()) return;
    nfs::World* world = app_.World();
    if (!world) return;
    SaveNetwork();
    const std::wstring region = world->Region();
    // Ctrl+Z takes the whole bake back: the world's files as they are now.
    DiskStep step(this, L"bake the roads");

    SaveTerrain();
    std::wstring routes;
    if (!WriteRoutes(routes)) {
        app_.Message(L"The roads' traffic and GPS could not be written, so nothing was baked: " + routes,
                     MB_ICONWARNING);
        return;
    }
    LOG_INFO("roads: %S", routes.c_str());
    std::vector<roads::Pole> poles;
    std::vector<roads::MeshPiece> pieces = network_.BuildMesh(&poles);
    // The terrain bakes with the roads: the same prefix, the same removal.
    {
        roads::RoadField field;
        field.Build(network_, [](const roads::Segment& segment) { return !segment.profile.bridge; });
        for (const terrain::Patch& patch : terrain_.Patches())
            for (roads::MeshPiece& piece : terrain_.BuildMesh(patch, &field, poles, network_.Light())) pieces.push_back(std::move(piece));
    }

    // Street lights and collision first, written by the edits' save, which
    // reloads the region. The mesh import after it keeps them: every write
    // rebuilds a section from the game's bytes with every edit in force.
    size_t polesAdded = 0, polesRemoved = 0;
    if (!BakePoles(poles, polesAdded, polesRemoved)) {
        app_.Message(L"The street lights could not be placed, so nothing was baked. The log has why.", MB_ICONWARNING);
        return;
    }
    size_t collisionAdded = 0, collisionRemoved = 0;
    if (app_.Collision() && !BakeCollision(pieces, collisionAdded, collisionRemoved)) {
        app_.Message(L"The road collision could not be made, so nothing was baked. The log has why.", MB_ICONWARNING);
        return;
    }
    size_t groundPolygons = 0;
    if (app_.Collision() && !BakeGroundCollision(groundPolygons)) {
        app_.Message(L"The ground's collision could not be moved, so nothing was baked. The log has why.",
                     MB_ICONWARNING);
        return;
    }
    if (!app_.SaveEdits()) {
        app_.Message(L"The street lights and road collision could not be written, so nothing was baked. "
                     L"The log has why.", MB_ICONWARNING);
        return;
    }
    // The game's own ground: written as an edit of its meshes, then the
    // offsets are spent -- the moved ground is the ground from now on.
    size_t groundMeshes = 0;
    {
        std::wstring error;
        if (!BakeGroundModels(groundMeshes, error)) {
            app_.Message(L"The sculpted ground could not be written: " + error, MB_ICONWARNING);
            return;
        }
        if (groundMeshes || groundPolygons) {
            for (const roads::Segment& segment : network_.Segments())
                if (fitGround_ && !Bridged(segment) && !segment.profile.keepGround)
                    fitted_.insert(Fingerprint(segment));
            SaveFitted();
            ground_.Clear();
            SaveGround();
        }
    }

    // What the last bake made goes first, so the network is the only source.
    size_t removed = 0;
    if (!RemoveBaked(removed)) {
        app_.Message(L"The roads baked before could not be taken out, so nothing was baked. The log has why.",
                     MB_ICONWARNING);
        return;
    }
    if (pieces.empty()) {
        WriteBakeStamp();
        app_.Status(Fmt(L"No roads to bake; %zu old pieces and %zu collision polygons removed", removed,
                        collisionRemoved));
        return;
    }

    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) return;
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const std::wstring folder = std::wstring(localAppData) + L"\\FRSWorldEditor\\roadbake\\" + region +
                                Fmt(L"-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                                    now.wMinute, now.wSecond);
    const int created = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS) {
        app_.Message(L"Could not make " + folder, MB_ICONWARNING);
        return;
    }
    // A piece must sit in a section the game keeps loaded all along it:
    // one that crosses visibility zones sharing none is cut in two, and
    // again, until each part fits in one.
    {
        const auto samples = [](const roads::MeshPiece& piece) {
            std::vector<Vec3> points;
            size_t total = 0;
            for (const roads::MeshGroup& g : piece.groups) total += g.vertices.size();
            const size_t step = std::max<size_t>(1, total / 40);
            size_t k = 0;
            for (const roads::MeshGroup& g : piece.groups)
                for (const roads::MeshVertex& v : g.vertices)
                    if (k++ % step == 0) points.push_back(v.position);
            return points;
        };
        std::vector<roads::MeshPiece> fitted;
        std::vector<std::pair<roads::MeshPiece, int>> todo;
        for (roads::MeshPiece& piece : pieces) todo.push_back({ std::move(piece), 0 });
        size_t cuts = 0;
        while (!todo.empty()) {
            auto [piece, depth] = std::move(todo.back());
            todo.pop_back();
            if (piece.VertexCount() == 0 || depth >= 6 ||
                app_.World()->SectionForPlacementAlong(samples(piece)) != SIZE_MAX) {
                fitted.push_back(std::move(piece));
                continue;
            }
            // In two across its longest side, at the middle of its triangles.
            const Aabb box = piece.Bounds();
            const int axis = box.max.x - box.min.x >= box.max.y - box.min.y ? 0 : 1;
            const float cut = axis ? (box.min.y + box.max.y) * 0.5f : (box.min.x + box.max.x) * 0.5f;
            roads::MeshPiece halves[2];
            halves[0].name = piece.name + "A";
            halves[1].name = piece.name + "B";
            for (roads::MeshPiece& h : halves) { h.segment = piece.segment; h.node = piece.node; }
            // The collision goes with the first half: it is placed by zone on its own.
            halves[0].strips = std::move(piece.strips);
            halves[0].areas = std::move(piece.areas);
            halves[0].walls = std::move(piece.walls);
            for (const roads::MeshGroup& g : piece.groups)
                for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                    const Vec3 c = (g.vertices[g.indices[k]].position + g.vertices[g.indices[k + 1]].position +
                                    g.vertices[g.indices[k + 2]].position) * (1.0f / 3.0f);
                    roads::MeshGroup& to = halves[(axis ? c.y : c.x) < cut ? 0 : 1].Group(g.texture);
                    for (int i = 0; i < 3; ++i) {
                        to.indices.push_back((u32)to.vertices.size());
                        to.vertices.push_back(g.vertices[g.indices[k + i]]);
                    }
                }
            ++cuts;
            for (roads::MeshPiece& h : halves) todo.push_back({ std::move(h), depth + 1 });
        }
        pieces = std::move(fitted);
        if (cuts) LOG_INFO("roads: %zu pieces cut where they cross visibility zones with no section in common", cuts);
    }

    const std::wstring obj = folder + L"\\roads.obj";
    Vec3 origin{};
    std::wstring error;
    if (!roads::WriteRoadObj(pieces, obj, origin, error)) {
        app_.Message(L"Could not write the roads: " + error, MB_ICONWARNING);
        return;
    }

    // The import makes each piece a model in the section under it and
    // reloads the region.
    app_.ImportObj(obj, origin);

    size_t baked = 0;
    if (nfs::World* reloaded = app_.World())
        for (const nfs::SolidObject* model : reloaded->AllModels())
            baked += model && roads::IsBakedRoadModel(model->name);
    // Pieces with nothing to draw (collision only: posts, pillars) are not
    // written as models, so they are not counted.
    size_t meshes = 0;
    for (const roads::MeshPiece& piece : pieces) meshes += piece.VertexCount() > 0;
    app_.Status(Fmt(L"Roads baked: %zu of %zu pieces are in the game now (%zu old ones replaced), "
                    L"%zu collision polygons (%zu old removed), %zu street lights, ground: %zu meshes and %zu "
                    L"polygons moved; %s",
                    baked, meshes, removed, collisionAdded, collisionRemoved, polesAdded, groundMeshes,
                    groundPolygons, routes.c_str()));
    LOG_INFO("roads: baked %zu of %zu pieces from %S", baked, meshes, obj.c_str());
    WriteBakeStamp();
    if (baked < meshes)
        app_.Message(Fmt(L"%zu of %zu road pieces could not be made into models (the log has why: usually a "
                         L"section with no object of its own to build from).",
                         meshes - baked, meshes),
                     MB_ICONWARNING);
}

} // namespace

std::unique_ptr<EditorExtension> CreateRoadsExtension(MainWindow& window) {
    return std::make_unique<RoadsExtension>(window);
}

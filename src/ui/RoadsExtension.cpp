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
#include "roads/Prefabs.h"
#include "roads/RoadRoutes.h"
#include "terrain/GroundDeform.h"
#include "terrain/Terrain.h"
#include "io/SelectionExport.h"
#include "io/ObjMesh.h"
#include "ui/BlenderLink.h"
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
#include <unordered_set>

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
    void UpdateProperties() const { w.UpdateProperties(); }
    bool ViewCentre(Vec3& point) const { return w.ViewCentrePoint(point); }
    bool ConfirmUnsaved() const { return w.ConfirmUnsavedPlacements(); }
    void ImportObj(const std::wstring& path, const Vec3& origin) const { w.ImportObj(path, true, &origin); }
    void ImportEdited(const std::wstring& path) const { w.ImportObj(path, true, nullptr); }
    CollisionTools* Collision() const { return w.collisionTools_; }
    bool SaveEdits() const { return w.SavePlacements(); }
    ModelThumbnails& Thumbnails() const { return w.thumbnails_; }
    void ShowLibraryShelf(const std::wstring& group) const {
        w.catalog_.ShowShelf(group);
        w.focusPanel_ = 1;
    }
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
constexpr int ID_ROAD_FJ_UNDO       = ID_ROADS_FIRST + 44;   // take a face junction out
constexpr int ID_ROAD_FJ_BLENDER    = ID_ROADS_FIRST + 45;   // refine it in Blender
constexpr int ID_ROAD_FJ_GENERATED  = ID_ROADS_FIRST + 46;   // back to the generated surface
constexpr int ID_ROAD_FJ_EDIT       = ID_ROADS_FIRST + 47;   // the wizard on it again
constexpr int ID_ROAD_GROUND_BAKE   = ID_ROADS_FIRST + 48;   // fit the ground to the selected roads now
constexpr int ID_ROAD_ZONE_RESEED   = ID_ROADS_FIRST + 49;   // other buildings along the selected roads
constexpr int ID_ROAD_GRID          = ID_ROADS_FIRST + 50;   // the street grid tool
constexpr int ID_ROAD_EXTRUDE_LEFT  = ID_ROADS_FIRST + 51;   // another band of terrain beside, left
constexpr int ID_ROAD_EXTRUDE_RIGHT = ID_ROADS_FIRST + 52;
constexpr int ID_ROAD_TRIM_LEFT     = ID_ROADS_FIRST + 53;   // the outermost band taken off
constexpr int ID_ROAD_TRIM_RIGHT    = ID_ROADS_FIRST + 54;
constexpr int ID_ROAD_FILL          = ID_ROADS_FIRST + 55;   // ground between roads that close an area
constexpr int ID_ROAD_GROUND_PATCH  = ID_ROADS_FIRST + 59;   // the ground beside roads made sculptable terrain
constexpr int ID_TERRAIN_TOOL       = ID_ROADS_FIRST + 60;   // the terrain tool: brushes, extrude, cut
constexpr int ID_ROAD_LIGHTS        = ID_ROADS_FIRST + 56;   // the light placing tool
constexpr int ID_ROAD_TREES         = ID_ROADS_FIRST + 57;   // the tree brush
constexpr int ID_ROAD_PROPS         = ID_ROADS_FIRST + 58;   // the prop tool
constexpr int kMostBands = 8;

// Scene tree codes of the face junctions: the node with this bit.
constexpr intptr_t kFaceJunctionTreeBit = 0x10000000;

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
    kTaperLength = 180, kMedianTaper, kWallThickness, kShoulderType, kShoulderWidth, kShoulderTexture, kShoulderLine,
    kGroundAuto, kShoulderSides, kWallStartLeft, kWallStartRight, kWallEndLeft, kWallEndRight,
    kZoneLeft = 200, kZoneRight, kZoneDepth, kZoneSetback, kZoneGap, kZoneSeed, kZoneTurn, kCurveShape = 210,
    // Terrain beside: kSideTerrain + side * 40 (0 left, 1 right) + one of these;
    // band k's width and rise at + kBandWidth + 2k and + kBandRise + 2k.
    kSideTerrain = 230, kSideOn = 0, kSideRelief = 1, kSideSeed = 2, kSideTexture = 3, kSideRock = 4, kSideRockAngle = 5,
    kSideRockHeight = 6, kSideMeet = 7, kBandWidth = 10, kBandRise = 11,
    kLightColour = 320, kLightRadius, kLightStrength, kLightHeight, kLightModel, kLightHeading,
    kPropModel = 340, kPropHeading, kPropScale,
    kCatsEyes = 360, kCatsEyeSpacing, kCatsEyeModel,
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

float FlatDistance(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

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

    // ---- the Library: special objects
    //
    // The game's smackables (props with the game's physics) and the models
    // that carry a light -- the street lights and lamps, and a light with no
    // post -- each on a shelf of their own. Placed from there like any model, by a
    // drag or a double click; a light's settings are on its card.

    static constexpr const wchar_t* kLightsShelf = L"Lights";
    static constexpr const wchar_t* kPropsShelf = L"Props with physics";
    static constexpr const wchar_t* kPrefabsShelf = L"Road prefabs";

    void CatalogRows(std::vector<ModelCatalog::Row>& rows) override {
        const auto add = [&](const std::string& key, const std::wstring& name, const std::wstring& shelf,
                             const nfs::SolidObject* model) {
            ModelCatalog::Row row;
            row.model = model;
            row.name = name;
            row.group = shelf;
            row.owner = static_cast<EditorExtension*>(this);
            row.key = key;
            rows.push_back(std::move(row));
        };
        for (const std::string& name : smackables_) {
            const auto found = modelsByName_.find(Widen(name));
            add("prop:" + name, Widen(name), kPropsShelf,
                found != modelsByName_.end() ? found->second : nullptr);
        }
        const nfs::SolidObject* any = nullptr;
        for (const std::wstring& name : poleModels_) {
            if (name == L"-") continue;
            // Street lights and lamps that stand: not the glows, flares and
            // masks the game lays over them.
            const std::string n = Narrow(name);
            if (n.rfind("XO_", 0) != 0 && n.rfind("XS_", 0) != 0) continue;
            bool overlay = false;
            for (const char* word : { "FLARE", "BULB", "MASK", "GLOW", "BEAM", "HALO", "SHADOW", "SPOT", "CONE" })
                overlay = overlay || n.find(word) != std::string::npos;
            if (overlay || n.find("_1A") == std::string::npos) continue;
            const auto found = modelsByName_.find(name);
            if (found == modelsByName_.end()) continue;
            if (!any) any = found->second;
            add("light:" + Narrow(name), name, kLightsShelf, found->second);
        }
        add("light:", L"Light (no post)", kLightsShelf, any);
        // Road prefabs, each with a picture of itself from above.
        for (const roads::PrefabInfo& prefab : roads::Prefabs()) {
            ModelCatalog::Row row;
            row.name = prefab.name;
            row.group = kPrefabsShelf;
            row.owner = static_cast<EditorExtension*>(this);
            row.key = "prefab:" + prefab.key;
            const std::string key = prefab.key;
            row.picture = [this, key](std::vector<u32>& bgra) {
                return roads::DrawPrefabPicture(key, lastProfile_, (int)ModelThumbnails::kSize, bgra);
            };
            rows.push_back(std::move(row));
        }
    }

    // ---- road prefabs
    //
    // Laid down whole from the Library: turned with the camera (in steps of
    // 15 degrees, so it lines up with a street seen square on), its open ends
    // joining roads that end close by. While dragged it is laid again at
    // every step, without joining; let go, it is laid for good.

    float PrefabHeading() const {
        const float yaw = Degrees(app_.View().GetCamera().Yaw());
        return std::round(yaw / 15.0f) * 15.0f;
    }

    roads::PrefabGround PrefabGroundQuery() const {
        return [this](float x, float y, float fallback) { return GroundOr(x, y, fallback); };
    }

    // Lays a prefab; its first node's id (0 when nothing was laid).
    u32 LayPrefab(const std::string& key, const Vec3& at, bool join) {
        roads::PrefabResult result;
        if (!roads::PlacePrefab(network_, key, at, PrefabHeading(), lastProfile_, PrefabGroundQuery(), result,
                                join ? 6.0f : 0.0f))
            return 0;
        prefabNodes_ = result.nodes;
        prefabJoined_ = result.joined;
        for (u32 id : result.nodes)
            if (network_.FindNode(id)) return id;
        return 0;
    }

    void TakeUpPrefab() {
        for (u32 id : prefabNodes_)
            if (network_.FindNode(id)) network_.RemoveNode(id);
        prefabNodes_.clear();
        network_.SetDirty();
    }

    void PrefabPlaced(const std::string& key, u32 first) {
        Changed(true);
        if (first) Select((int)first);
        const roads::PrefabInfo* info = roads::FindPrefab(key);
        app_.Status(Fmt(L"%s laid: %zu nodes%s -- every road and node of it can be edited now",
                        info ? info->name.c_str() : L"Prefab", prefabNodes_.size(),
                        prefabJoined_ ? Fmt(L", %zu ends joined to roads already there", prefabJoined_).c_str() : L""));
        prefabNodes_.clear();
    }

    // The object a row stands for, made at `at`; its id.
    u32 MakeFromRow(const std::string& key, const Vec3& at) {
        if (key.rfind("prop:", 0) == 0) {
            roads::Prop prop;
            prop.model = key.substr(5);
            prop.position = at;
            prop.heading = Degrees(app_.View().GetCamera().Yaw()) - 90.0f;
            return network_.AddProp(prop);
        }
        if (key.rfind("light:", 0) == 0) {
            roads::Light light = lightDraft_;
            light.model = key.substr(6);
            light.position = at;
            return network_.AddLight(light);
        }
        return 0;
    }

    void ActivateRow(const ModelCatalog::Row& row) override {
        Vec3 at;
        if (!app_.World() || !app_.ViewCentre(at)) return;
        visible_ = true;
        if (row.key.rfind("prefab:", 0) == 0) {
            const std::string key = row.key.substr(7);
            PrefabPlaced(key, LayPrefab(key, at, true));
            return;
        }
        if (const u32 id = MakeFromRow(row.key, at)) {
            Changed(true);
            Select((int)id);
        }
    }

    bool DescribeRow(const ModelCatalog::Row& row, std::wstring& text) override {
        if (row.key.rfind("prefab:", 0) == 0) {
            const roads::PrefabInfo* info = roads::FindPrefab(row.key.substr(7));
            text = (info ? info->name + L": " + info->description : row.name) +
                   L" Drag it onto the map or double-click for the view centre; it faces the way the camera "
                   L"looks, and its ends join roads ending close by.";
            return true;
        }
        if (row.key.rfind("prop:", 0) == 0)
            text = L"A prop with the game's physics (" + row.name + L"): knocked about when hit. Drag it onto the "
                   L"map or double-click for the view centre.";
        else
            text = L"A light (" + row.name + L"): its colour, reach and strength are on its card; it lights the "
                   L"roads, terrain and the game's objects round it. Drag it onto the map or double-click.";
        return true;
    }

    bool BeginDrag(const ModelCatalog::Row& row) override {
        if (row.key.rfind("prop:", 0) != 0 && row.key.rfind("light:", 0) != 0 &&
            row.key.rfind("prefab:", 0) != 0)
            return false;
        dragKey_ = row.key;
        dragObject_ = 0;
        prefabNodes_.clear();
        return true;
    }

    void UpdateDrag(const Vec3& ground) override {
        visible_ = true;
        if (dragKey_.rfind("prefab:", 0) == 0) {
            // Laid again only once it has moved a little: a prefab is many roads.
            if (!prefabNodes_.empty() && FlatDistance(ground, prefabDragAt_) < 1.0f) return;
            TakeUpPrefab();
            prefabDragAt_ = ground;
            dragObject_ = LayPrefab(dragKey_.substr(7), ground, false);
            return;
        }
        if (!dragObject_) dragObject_ = MakeFromRow(dragKey_, ground);
        if (roads::Prop* prop = network_.FindProp(dragObject_)) prop->position = ground;
        if (roads::Light* light = network_.FindLight(dragObject_)) light->position = ground;
        network_.SetDirty();
        if (dragObject_) Select((int)dragObject_);
    }

    void EndDrag(const Vec3* ground) override {
        const u32 id = dragObject_;
        dragObject_ = 0;
        const std::string key = dragKey_;
        dragKey_.clear();
        if (key.rfind("prefab:", 0) == 0) {
            TakeUpPrefab();
            if (!ground) {
                app_.Renderer().ClearSelection();
                app_.Status(L"Placement cancelled");
                return;
            }
            PrefabPlaced(key.substr(7), LayPrefab(key.substr(7), *ground, true));
            return;
        }
        if (!id) return;
        if (!ground) {
            network_.RemoveProps([id](const roads::Prop& p) { return p.id == id; });
            network_.RemoveLight(id);
            app_.Renderer().ClearSelection();
            network_.SetDirty();
            app_.Status(L"Placement cancelled");
            return;
        }
        UpdateDrag(*ground);
        Changed(true);
        Select((int)id);
        app_.Status(L"Placed -- G moves it, R turns it, its card has the rest");
    }

    bool ToolActive() const override {
        return drawTool_ || gridTool_ || lightTool_ || treeTool_ || propTool_ || drawing_ ||
               brush_ != terrain::Brush::None || wizard_ != kWizardOff;
    }
    void CloseTools() override {
        CloseOtherTools();
        drawing_ = false;
        outline_.clear();
    }
    bool ToolDigit(int digit) override {
        // The road tool's modes are 1..5; 1..3 would otherwise go to the gizmo.
        if (!drawTool_) return false;
        return DrawToolKey((WPARAM)('0' + digit));
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
            case ID_ROAD_FJ_UNDO:
                if (const roads::Node* node = SelectedNode()) UndoFaceJunction(node->id);
                return true;
            case ID_ROAD_FJ_BLENDER:
                if (const roads::Node* node = SelectedNode()) RefineFaceJunction(node->id);
                return true;
            case ID_ROAD_FJ_GENERATED:
                if (const roads::Node* node = SelectedNode())
                    if (roads::FaceJunction* fj = network_.FindFaceJunction(node->id)) {
                        fj->refined.clear();
                        network_.SetDirty();
                        Changed(true);
                        app_.Status(L"Junction surface generated again (the Blender refinement is dropped; Ctrl+Z "
                                    L"brings it back)");
                    }
                return true;
            case ID_ROAD_GROUND_BAKE:  BakeTerrainForRoads(); return true;
            case ID_ROAD_ZONE_RESEED:  ReseedZones(); return true;
            case ID_ROAD_GRID:         if (gridTool_) CloseGridTool(); else StartGridTool(); return true;
            case ID_ROAD_EXTRUDE_LEFT:  ExtrudeTerrain(0, true); return true;
            case ID_ROAD_FILL:          ToggleFill(); return true;
            case ID_ROAD_GROUND_PATCH:  TerrainFromRoadGround(); return true;
            case ID_TERRAIN_TOOL:
                SetBrush(brush_ == terrain::Brush::None ? terrain::Brush::Raise : terrain::Brush::None);
                return true;
            case ID_ROAD_LIGHTS:        if (lightTool_) CloseLightTool(); else StartLightTool(); return true;
            case ID_ROAD_TREES:         if (treeTool_) CloseTreeTool(); else StartTreeTool(); return true;
            case ID_ROAD_PROPS:         if (propTool_) ClosePropTool(); else StartPropTool(); return true;
            case ID_ROAD_EXTRUDE_RIGHT: ExtrudeTerrain(1, true); return true;
            case ID_ROAD_TRIM_LEFT:     ExtrudeTerrain(0, false); return true;
            case ID_ROAD_TRIM_RIGHT:    ExtrudeTerrain(1, false); return true;
            case ID_ROAD_FJ_EDIT:
                if (const roads::Node* node = SelectedNode()) EditFaceJunction(node->id);
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
        ::CheckMenuItem(menu, ID_ROAD_GRID, MF_BYCOMMAND | (gridTool_ ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_ROAD_FILL, MF_BYCOMMAND | (network_.FillBetween() ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_ROAD_LIGHTS, MF_BYCOMMAND | (lightTool_ ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_ROAD_TREES, MF_BYCOMMAND | (treeTool_ ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(menu, ID_ROAD_PROPS, MF_BYCOMMAND | (propTool_ ? MF_CHECKED : MF_UNCHECKED));
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
        LoadNativeOriginals();
        if (!demoDone_ && std::getenv("FRS_CITY_DEMO")) {
            demoDone_ = true;
            demoActive_ = true;
            CityDemo(*world);
        }
        if (world->Region() != historyRegion_) {
            historyRegion_ = world->Region();
            ResetHistory();
        } else {
            settled_ = Capture();
        }
    }

    void OnRegionClosed() override {
        nativeOriginal_.clear();
        nativeLit_.clear();
        nativeWritten_.clear();
        nativeRevision_ = 0;
        lightTool_ = false;
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
        if (lightTool_) return LightToolMouseDown(x, y);
        if (propTool_) return PropToolMouseDown(x, y);
        if (treeTool_) {
            treePainting_ = true;
            treeClock_ = 1.0f;
            treeBrushHit_ = AnythingUnder(x, y, treeBrushPoint_);
            return true;
        }
        if (gridTool_) return GridToolMouseDown(x, y);
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
        if (gridTool_) {
            gridHoverValid_ = AnythingUnder(x, y, gridHover_);
            return;
        }
        if (treeTool_) {
            treeBrushHit_ = AnythingUnder(x, y, treeBrushPoint_);
            return;
        }
        if (drawTool_) {
            drawHover_ = DrawToolHit(x, y);
            drawHoverRoad_ = drawMode_ >= kDrawUpgrade ? RoadUnder(x, y) : 0;
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
        if (gridTool_ && GridToolKey(key)) return true;
        if (lightTool_ && key == VK_ESCAPE) { CloseLightTool(); return true; }
        if (propTool_ && key == VK_ESCAPE) { ClosePropTool(); return true; }
        if (treeTool_) {
            if (key == VK_ESCAPE) { CloseTreeTool(); return true; }
            if (key == VK_OEM_4) { treeRadius_ = std::max(1.0f, treeRadius_ / 1.2f); return true; }   // [
            if (key == VK_OEM_6) { treeRadius_ = std::min(200.0f, treeRadius_ * 1.2f); return true; } // ]
        }
        if (drawTool_ && DrawToolKey(key)) return true;
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
        PollRefine();
        if (treeTool_ && treePainting_) {
            if (!(::GetKeyState(VK_LBUTTON) & 0x8000)) EndTreeStroke();
            else PaintTrees(deltaSeconds);
        }
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
            // New ground carries on from the patch's own edge -- an extrusion --
            // rather than dropping to whatever is under it.
            terrain_.Apply(*patch, brush_, brushPoint_, brushRadius_, 0.0f, 0.0f, paintLayer_,
                           [patch](float x, float y) {
                               const int i = std::clamp((int)std::lround((x - patch->origin.x) / patch->cell), 0,
                                                        patch->columns);
                               const int j = std::clamp((int)std::lround((y - patch->origin.y) / patch->cell), 0,
                                                        patch->rows);
                               return patch->H(i, j);
                           });
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

    size_t SceneSignature() const override {
        return network_.Segments().size() * 7919 + network_.Nodes().size() + network_.Lights().size() * 104729 +
               network_.Props().size() * 1299709;
    }

    void PopulateTree(SceneTreeBuilder& tree) override {
        if (!network_.FaceJunctions().empty()) {
            HTREEITEM branch =
                tree.insert(TVI_ROOT, Fmt(L"Junctions into game roads (%zu)", network_.FaceJunctions().size()), -1);
            size_t shown = 0;
            for (const roads::FaceJunction& j : network_.FaceJunctions()) {
                const std::wstring text = Fmt(L"Junction at node %u  -  %zu faces of %s%s", j.node, j.faces.size(),
                                              Widen(j.model).c_str(), j.refined.empty() ? L"" : L"  (refined)");
                if (!tree.matches(text)) continue;
                tree.insert(branch, text, kFaceJunctionTreeBit | (intptr_t)j.node);
                ++shown;
            }
            tree.finish(branch, shown, false);
        }
        // Lights, trees (by model) and props placed in the editor.
        if (!network_.Lights().empty()) {
            HTREEITEM branch = tree.insert(TVI_ROOT, Fmt(L"Lights (%zu)", network_.Lights().size()), -1);
            size_t shown = 0;
            for (const roads::Light& l : network_.Lights()) {
                const std::wstring text = Fmt(L"Light %u  -  #%06X, %.0f m%s%s", l.id, l.color & 0xFFFFFF, l.radius,
                                              l.model.empty() ? L"" : L", ", Widen(l.model).c_str());
                if (!tree.matches(text)) continue;
                tree.insert(branch, text, (intptr_t)l.id);
                ++shown;
            }
            tree.finish(branch, shown, false);
        }
        {
            std::map<std::string, std::vector<const roads::Prop*>> trees;
            std::vector<const roads::Prop*> props;
            for (const roads::Prop& p : network_.Props()) {
                if (p.tree) trees[p.model].push_back(&p);
                else props.push_back(&p);
            }
            if (!trees.empty()) {
                size_t count = 0;
                for (const auto& [model, list] : trees) count += list.size();
                HTREEITEM branch = tree.insert(TVI_ROOT, Fmt(L"Trees (%zu)", count), -1);
                size_t shown = 0;
                for (const auto& [model, list] : trees) {
                    HTREEITEM kind = tree.insert(branch, Fmt(L"%s (%zu)", Widen(model).c_str(), list.size()), -1);
                    size_t inKind = 0;
                    for (const roads::Prop* p : list) {
                        const std::wstring text = Fmt(L"Tree %u  -  %s, x%.2f", p->id, Widen(p->model).c_str(), p->scale);
                        if (!tree.matches(text)) continue;
                        tree.insert(kind, text, (intptr_t)p->id);
                        ++inKind;
                    }
                    tree.finish(kind, inKind, false);
                    shown += inKind;
                }
                tree.finish(branch, shown, false);
            }
            if (!props.empty()) {
                HTREEITEM branch = tree.insert(TVI_ROOT, Fmt(L"Props (%zu)", props.size()), -1);
                size_t shown = 0;
                for (const roads::Prop* p : props) {
                    const std::wstring text = Fmt(L"Prop %u  -  %s%s", p->id, Widen(p->model).c_str(),
                                                  smackables_.count(p->model) ? L"  (physics)" : L"");
                    if (!tree.matches(text)) continue;
                    tree.insert(branch, text, (intptr_t)p->id);
                    ++shown;
                }
                tree.finish(branch, shown, false);
            }
        }
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
        if (code & kFaceJunctionTreeBit) {
            const u32 nodeId = (u32)(code & ~kFaceJunctionTreeBit);
            if (const roads::Node* node = network_.FindNode(nodeId)) {
                Aabb box;
                box.Add(node->position);
                app_.View().FrameBounds(Pad(box, 25.0f));
                moreRoads_.clear();
                Select((int)nodeId);
                app_.Status(L"Junction into a game road: Properties can take it out, refine it in Blender or "
                            L"choose its faces again");
            }
            return;
        }
        // A light, tree or prop: framed and selected.
        {
            Vec3 at;
            bool found = false;
            if (const roads::Light* l = network_.FindLight((u32)code)) { at = l->position; found = true; }
            else if (const roads::Prop* p = network_.FindProp((u32)code)) { at = p->position; found = true; }
            if (found) {
                visible_ = true;
                Aabb box;
                box.Add(at);
                app_.View().FrameBounds(Pad(box, 12.0f));
                moreRoads_.clear();
                Select((int)code);
                return;
            }
        }
        const roads::Segment* s = network_.FindSegment((u32)code);
        if (!s) return;
        // Ctrl adds or takes out one road, Shift every road from the last one
        // clicked; their properties are then edited together.
        const bool ctrl = ::GetKeyState(VK_CONTROL) < 0, shift = ::GetKeyState(VK_SHIFT) < 0;
        std::vector<u32> roads = SelectedRoadIds();
        if ((ctrl || shift) && !roads.empty()) {
            if (shift) {
                const u32 anchor = network_.FindSegment(treeAnchor_) ? treeAnchor_ : roads.front();
                bool inside = false;
                if (!ctrl) roads.clear();
                for (const roads::Segment& o : network_.Segments()) {
                    const bool end = o.id == anchor || o.id == s->id;
                    if (end || inside)
                        if (std::find(roads.begin(), roads.end(), o.id) == roads.end()) roads.push_back(o.id);
                    if (end && anchor != s->id) inside = !inside;
                }
                // The anchor stays the one the properties show.
                if (const auto at = std::find(roads.begin(), roads.end(), anchor); at != roads.end())
                    std::rotate(roads.begin(), at, at + 1);
            } else {
                const auto at = std::find(roads.begin(), roads.end(), s->id);
                if (at == roads.end()) roads.push_back(s->id);
                else if (roads.size() > 1) roads.erase(at);
                treeAnchor_ = s->id;
            }
            SelectRoads(roads);
            app_.Status(Fmt(L"%zu roads selected -- a value changed in Properties goes to all of them", roads.size()));
            return;
        }
        treeAnchor_ = s->id;
        moreRoads_.clear();
        const roads::Node* a = network_.FindNode(s->a);
        const roads::Node* b = network_.FindNode(s->b);
        Aabb box;
        box.Add(a->position);
        box.Add(b->position);
        app_.View().FrameBounds(Pad(box, 20.0f));
        Select((int)s->id);
    }

    int TreeItemMark(intptr_t code) const override {
        if (code & kFaceJunctionTreeBit) return Selected() == (int)(code & ~kFaceJunctionTreeBit) ? 1 : 0;
        const std::vector<u32> roads = SelectedRoadIds();
        return std::find(roads.begin(), roads.end(), (u32)code) != roads.end() ? 1 : 0;
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
        if (const size_t count = SelectedRoadIds().size(); count > 1) {
            text = Fmt(L"%zu roads selected   [a value changed in Properties goes to all of them   Ctrl+click in the "
                       L"Explorer adds or takes out one, Shift+click a run]", count);
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
        if (SelectedProp()) {
            network_.RemoveProps([id](const roads::Prop& p) { return p.id == (u32)id; });
            app_.Renderer().ClearSelection();
            Changed(true);
            return true;
        }
        if (SelectedLight()) {
            network_.RemoveLight((u32)id);
            app_.Renderer().ClearSelection();
            Changed(true);
            return true;
        }
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
        if (const roads::Light* light = SelectedLight()) {
            position = light->position;
            heading = light->heading;
            return true;
        }
        if (const roads::Prop* prop = SelectedProp()) {
            position = prop->position;
            heading = prop->heading;
            return true;
        }
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
        if (roads::Light* light = SelectedLightMutable()) {
            light->position = position;
            light->heading = heading;
            network_.SetDirty();
            if (commit) Changed(true);
            return true;
        }
        if (roads::Prop* prop = SelectedPropMutable()) {
            prop->position = position;
            prop->heading = heading;
            network_.SetDirty();
            if (commit) Changed(true);
            return true;
        }
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
            if (length > 0.25f && s->shape != 0) {
                // A drawn curve: this end's own direction.
                if (s->shape == 1) {
                    // Straight no more: both ends keep where they point now.
                    const roads::Curve curve = network_.CurveOf(*s);
                    const Vec3 d0 = curve.p1 - curve.p0, d1 = curve.p2 - curve.p3;
                    s->dirA = Degrees(std::atan2(d0.y, d0.x));
                    s->dirB = Degrees(std::atan2(d1.y, d1.x));
                    s->handleA = std::max(0.5f, Length(Vec3{ d0.x, d0.y, 0.0f }));
                    s->handleB = std::max(0.5f, Length(Vec3{ d1.x, d1.y, 0.0f }));
                    s->shape = 2;
                }
                (atStart ? s->dirA : s->dirB) = Degrees(std::atan2(offset.y, offset.x));
                (atStart ? s->handleA : s->handleB) = std::max(0.5f, length);
            } else if (length > 0.25f) {
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
        RelightNatives();
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
        {
            float hit = 0.0f;
            if (const u32 light = LightUnder(ray, hit)) {
                index = (int)light;
                distance = 0.0f;
                return true;
            }
            // Trees and props while their tools are on (or one is chosen):
            // elsewhere they would stand in the way of the roads under them.
            {
                if (const u32 prop = PropUnder(ray, hit)) {
                    index = (int)prop;
                    distance = 0.0f;
                    return true;
                }
            }
        }
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

    // Several roads at once (Ctrl/Shift in the Explorer): the selected one
    // first, then the rest -- kept while that one stays selected.
    std::vector<u32> moreRoads_;
    u32 moreFor_ = 0;
    u32 treeAnchor_ = 0;
    std::vector<u32> SelectedRoadIds() const {
        std::vector<u32> ids;
        const roads::Segment* first = SelectedSegment();
        if (!first) return ids;
        ids.push_back(first->id);
        if (moreFor_ == first->id)
            for (u32 id : moreRoads_)
                if (id != first->id && network_.FindSegment(id)) ids.push_back(id);
        return ids;
    }
    void SelectRoads(const std::vector<u32>& ids) {
        if (ids.empty()) return;
        moreRoads_.assign(ids.begin() + 1, ids.end());
        moreFor_ = ids.front();
        Select((int)ids.front());
        app_.UpdateProperties();
    }
    void FillRoad(PropertiesPanel::View& view, const roads::Segment& road);
    void ApplyRoad(roads::RoadProfile& p, int only);

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
        mix("bake 12");
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
        if (demoActive_) return;     // a demo never reaches the game
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
        // A demo (FRS_CITY_DEMO) lives in memory only: never over the world's own files.
        if (demoActive_) return;
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
        const float back = -h + p.ShoulderAt(-1) + p.lanesBackward * p.laneWidth * 0.5f;
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
        for (const roads::MeshPiece& piece : terrain_.BuildMesh(patch, &field_, poles_, network_.Lighting()))
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
        // The buildings the zones stand along roads: whole ones the game
        // places somewhere itself (so they carry its levels of detail).
        {
            std::unordered_set<u32> instanced;
            for (const auto& section : world.Loaded()) {
                if (!section) continue;
                for (const nfs::SceneryInstance& instance : section->scenery.instances)
                    instanced.insert(section->scenery.ModelHash(instance));
            }
            std::vector<roads::BuildingModel> buildings;
            std::unordered_set<std::string> seen;
            for (const nfs::SolidObject* model : world.AllModels()) {
                if (!model || !model->bounds.Valid() || !instanced.count(model->hash)) continue;
                const int kind = roads::ClassifyBuilding(model->name, model->bounds.min, model->bounds.max);
                if (kind == roads::kZoneNone || !seen.insert(model->name).second) continue;
                buildings.push_back({ model->name, model->bounds.min, model->bounds.max, kind });
            }
            std::sort(buildings.begin(), buildings.end(),
                      [](const roads::BuildingModel& x, const roads::BuildingModel& y) { return x.name < y.name; });
            int counts[roads::kZoneKinds] = {};
            for (const roads::BuildingModel& b : buildings) ++counts[b.kind];
            LOG_INFO("roads: zone buildings: %d houses, %d shops, %d apartments, %d towers, %d industry",
                     counts[roads::kZoneHouses], counts[roads::kZoneShops], counts[roads::kZoneApartments],
                     counts[roads::kZoneTowers], counts[roads::kZoneIndustry]);
            network_.SetBuildingModels(std::move(buildings));
            CollectPlantsAndProps(world, instanced);
            // The region's grass, for ground beside roads that names none.
            std::string grass;
            for (const std::wstring& name : groundTextures_) {
                const std::string n = Narrow(name);
                if (n.find("GRASS") != std::string::npos && n.find("SIDEWALK") == std::string::npos) { grass = n; break; }
            }
            if (grass.empty())
                for (const std::wstring& name : groundTextures_)
                    if (Narrow(name).find("DIRT") != std::string::npos) { grass = Narrow(name); break; }
            if (!grass.empty()) network_.SetGroundTexture(grass);
            // Its rock for steep ground, and a grass-into-rock texture if it has one.
            std::string rock, mix;
            const auto has = [&](const char* name) { return texturesByName_.count(Widen(name)) != 0; };
            if (has("TRN_ROCKCLIFFA_KT")) rock = "TRN_ROCKCLIFFA_KT";
            if (has("TRN_GRASSACLIFFA")) mix = "TRN_GRASSACLIFFA";
            for (const auto& [name, texture] : texturesByName_) {
                const std::string n = Narrow(name);
                if (n.rfind("TRN_", 0) != 0) continue;
                const bool rocky = n.find("ROCK") != std::string::npos || n.find("CLIFF") != std::string::npos;
                if (rock.empty() && rocky && n.find("GRASS") == std::string::npos) rock = n;
                if (mix.empty() && rocky && n.find("GRASS") != std::string::npos) mix = n;
            }
            network_.SetRockTextures(rock, mix);
            LOG_INFO("roads: rock on slopes: %s, between: %s", rock.empty() ? "(none)" : rock.c_str(),
                     mix.empty() ? "(none)" : mix.c_str());
            LOG_INFO("roads: ground beside roads: %s", grass.empty() ? "(default)" : grass.c_str());
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
        return nfs::ComposePlacement(pole.position, pole.heading, 0.0f, 0.0f, pole.scale);
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
        if (demoActive_) return;     // a demo is never written
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
        if (brush != terrain::Brush::None) {
            // A brush is a tool too: the others go.
            if (wizard_ != kWizardOff) EndFaceWizard(nullptr);
            if (drawTool_) { drawTool_ = false; EndDrawChain(); }
            gridTool_ = lightTool_ = treeTool_ = propTool_ = false;
            treePainting_ = false;
        }
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
        const bool wasOpen = drawTool_;
        if (gridTool_) CloseGridTool();
        lightTool_ = treeTool_ = propTool_ = false;
        if (brush_ != terrain::Brush::None) SetBrush(terrain::Brush::None);
        drawTool_ = true;
        drawFrom_ = from;
        drawPending_ = false;
        drawHasControl_ = false;
        drawLastValid_ = from && ArrivalAt(from, drawLast_);
        // The type chosen in the tool stays while it is open.
        if (!wasOpen) {
            drawProfile_ = lastProfile_;
            if (from)
                for (u32 id : network_.SegmentsAt(from))
                    if (const roads::Segment* s = network_.FindSegment(id)) { drawProfile_ = s->profile; break; }
            drawZone_ = drawProfile_.zoneLeft;
        }
        ::SetFocus(app_.View().Handle());
        app_.UpdateMenuChecks();
        app_.Status(L"Drawing roads: click open ground for nodes, a node to join it, a road to put a node in it. "
                    L"Enter ends the road, Esc closes the tool.");
    }

    void EndDrawChain() {
        drawFrom_ = 0;
        drawPending_ = false;
        drawHasControl_ = false;
        drawLastValid_ = false;
    }

    void CloseDrawTool() {
        drawTool_ = false;
        EndDrawChain();
        app_.UpdateMenuChecks();
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

    // ---- drawing modes, as a city builder's road tool has them
    //
    // Straight: node to node in straight lines. Curved: a click for where the
    // road starts, one for the point its curve bends towards, one for where it
    // ends. Freeform: each road leaves the last one's end in the direction it
    // arrived, curving round to the click. Upgrade: a click on a road gives it
    // the chosen type, its zoning kept. Bulldoze: a click takes a road (or a
    // node with its roads) out. Snapping turns the line to steps of 15
    // degrees from the road before and its length to steps of 8 m; + and -
    // raise and lower what is drawn, and a road high enough is a bridge.

    enum DrawMode { kDrawStraight = 0, kDrawCurved, kDrawFreeform, kDrawUpgrade, kDrawBulldoze, kDrawModes };

    static const char* DrawModeName(int mode) {
        static const char* const names[kDrawModes] = { "Straight", "Curved", "Freeform", "Upgrade", "Bulldoze" };
        return names[std::clamp(mode, 0, kDrawModes - 1)];
    }

    // The road types offered, from the last profile's textures and lights.
    static constexpr int kPresets = 6;
    static const char* PresetName(int k) {
        static const char* const names[kPresets] = { "Two-lane street", "Four-lane road", "Avenue",
                                                     "Six-lane avenue", "One-way street", "Highway" };
        return names[std::clamp(k, 0, kPresets - 1)];
    }

    roads::RoadProfile PresetProfile(int k) const {
        roads::RoadProfile p = lastProfile_;
        p.bridge = false;
        p.wallLeft = p.wallRight = false;
        p.shoulderType = 0;
        p.medianType = 0;
        p.sidewalkLeft = p.sidewalkRight = true;
        p.laneWidth = 4.0f;
        p.zoneLeft = p.zoneRight = drawZone_;
        switch (k) {
            case 0: p.lanesForward = p.lanesBackward = 1; p.sidewalkWidth = 3.0f; break;
            case 1: p.lanesForward = p.lanesBackward = 2; p.sidewalkWidth = 3.0f; break;
            case 2:
                p.lanesForward = p.lanesBackward = 2; p.sidewalkWidth = 4.0f;
                p.medianType = 3; p.medianWidth = 4.0f; p.medianTexture.clear();
                break;
            case 3:
                p.lanesForward = p.lanesBackward = 3; p.sidewalkWidth = 4.0f;
                p.medianType = 3; p.medianWidth = 4.0f; p.medianTexture.clear();
                break;
            case 4: p.lanesForward = 2; p.lanesBackward = 0; p.sidewalkWidth = 3.0f; break;
            default:
                p.lanesForward = 3; p.lanesBackward = 0; p.sidewalkWidth = 0.0f;
                p.shoulderType = 2; p.shoulderSides = 0; p.shoulderTexture.clear();
                p.wallLeft = p.wallRight = true;
                p.zoneLeft = p.zoneRight = 0;
                p.speedLimit = 100.0f;
                break;
        }
        return p;
    }

    // Where the road being drawn starts, if one is.
    bool DrawStart(Vec3& start) const {
        if (drawPending_) { start = drawStart_; return true; }
        if (const roads::Node* n = network_.FindNode(drawFrom_)) { start = n->position; return true; }
        return false;
    }

    // The direction the only road ending at `node` arrives with: what a
    // freeform road drawn on from it carries on in.
    bool ArrivalAt(u32 node, Vec3& direction) const {
        const std::vector<u32> around = network_.SegmentsAt(node);
        if (around.size() != 1) return false;
        const roads::Segment* s = network_.FindSegment(around[0]);
        if (!s) return false;
        const roads::Curve c = network_.CurveOf(*s);
        direction = s->a == node ? HorizontalDirection(c.Tangent(0.0f) * -1.0f) : HorizontalDirection(c.Tangent(1.0f));
        return true;
    }

    float GroundOr(float x, float y, float fallback) const {
        float h = 0.0f;
        if (GroundBelow(x, y, fallback + 30.0f, h) && h > fallback - 60.0f) return h;
        return fallback;
    }

    // A point on open ground as the tool takes it: snapped from `from`
    // (when given) and raised by the elevation.
    Vec3 TakeGround(Vec3 p, const Vec3* from) const {
        if (from) {
            Vec3 d{ p.x - from->x, p.y - from->y, 0.0f };
            float length = std::sqrt(d.x * d.x + d.y * d.y);
            if (length > 0.5f) {
                float angle = std::atan2(d.y, d.x);
                if (drawSnapAngle_) {
                    const float reference = drawLastValid_ ? std::atan2(drawLast_.y, drawLast_.x) : 0.0f;
                    const float step = Radians(15.0f);
                    float relative = std::remainder(angle - reference, 6.2831853f);
                    const float snapped = std::round(relative / step) * step;
                    if (std::fabs(relative - snapped) < Radians(5.0f)) angle = reference + snapped;
                }
                if (drawSnapLength_) length = std::max(8.0f, std::round(length / 8.0f) * 8.0f);
                p.x = from->x + std::cos(angle) * length;
                p.y = from->y + std::sin(angle) * length;
                p.z = GroundOr(p.x, p.y, p.z);
            }
        }
        p.z += drawElevation_;
        return p;
    }

    // The hit under the cursor as the tool would take it.
    DrawHit DrawToolHit(int x, int y) const {
        DrawHit hit = DrawHitAt(x, y);
        if (drawMode_ >= kDrawUpgrade) return hit;
        if (hit.kind == DrawHit::Ground) {
            Vec3 start;
            const bool started = DrawStart(start);
            if (drawMode_ == kDrawCurved && started && drawHasControl_) hit.point = TakeGround(hit.point, &drawControl_);
            else hit.point = TakeGround(hit.point, started ? &start : nullptr);
        }
        return hit;
    }

    // The shape of the road from `start` to `end` in the current mode.
    struct PlannedRoad {
        int shape = 1;
        float dirA = 0.0f, dirB = 0.0f, handleA = 0.0f, handleB = 0.0f;
        Vec3 arrival;           // the direction it arrives at `end` with
        roads::Curve curve;
    };

    PlannedRoad PlanRoad(const Vec3& start, const Vec3& end) const {
        PlannedRoad plan;
        const Vec3 chord{ end.x - start.x, end.y - start.y, 0.0f };
        const float length = std::sqrt(chord.x * chord.x + chord.y * chord.y);
        const Vec3 c = length > 1e-4f ? chord * (1.0f / length) : Vec3{ 1.0f, 0.0f, 0.0f };
        Vec3 leaveA = c, leaveB = c * -1.0f;
        float hA = length / 3.0f, hB = length / 3.0f;
        plan.shape = 1;
        if (drawMode_ == kDrawCurved && drawHasControl_) {
            // A quadratic curve through the control point, as a cubic.
            const Vec3 toA{ drawControl_.x - start.x, drawControl_.y - start.y, 0.0f };
            const Vec3 toB{ drawControl_.x - end.x, drawControl_.y - end.y, 0.0f };
            const float la = std::sqrt(toA.x * toA.x + toA.y * toA.y), lb = std::sqrt(toB.x * toB.x + toB.y * toB.y);
            if (la > 0.5f && lb > 0.5f) {
                plan.shape = 2;
                leaveA = toA * (1.0f / la);
                leaveB = toB * (1.0f / lb);
                hA = la * 2.0f / 3.0f;
                hB = lb * 2.0f / 3.0f;
            }
        } else if (drawMode_ == kDrawFreeform && drawLastValid_ && length > 1.0f) {
            // A circular arc leaving in the direction the last road arrived.
            const Vec3 t0 = drawLast_;
            const float cosHalf = std::clamp(Dot(t0, c), -1.0f, 1.0f);
            if (cosHalf > 0.05f) {
                const float half = std::acos(cosHalf);
                plan.shape = 2;
                leaveA = t0;
                const Vec3 t1 = c * (2.0f * cosHalf) - t0;       // t0 mirrored about the chord
                leaveB = t1 * -1.0f;
                if (half > 1e-3f) {
                    const float radius = length / (2.0f * std::sin(half));
                    hA = hB = 4.0f / 3.0f * std::tan(half * 0.5f) * radius;
                }
            }
        }
        plan.dirA = Degrees(std::atan2(leaveA.y, leaveA.x));
        plan.dirB = Degrees(std::atan2(leaveB.y, leaveB.x));
        plan.handleA = std::max(0.01f, hA);
        plan.handleB = std::max(0.01f, hB);
        plan.arrival = leaveB * -1.0f;
        const float rise = end.z - start.z;
        plan.curve.p0 = start;
        plan.curve.p3 = end;
        plan.curve.p1 = start + leaveA * plan.handleA + Vec3{ 0.0f, 0.0f, rise / 3.0f };
        plan.curve.p2 = end + leaveB * plan.handleB - Vec3{ 0.0f, 0.0f, rise / 3.0f };
        return plan;
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
        const roads::Node* a = network_.FindNode(drawFrom_);
        const roads::Node* b = network_.FindNode(to);
        if (!a || !b) return false;
        const PlannedRoad plan = PlanRoad(a->position, b->position);
        roads::RoadProfile profile = drawProfile_;
        if (drawElevation_ >= 4.0f && !profile.bridge) {
            profile.bridge = true;            // high enough to be a bridge
            profile.wallLeft = profile.wallRight = true;
        }
        roads::Segment* s = network_.AddSegment(drawFrom_, to, profile);
        if (!s) return false;
        s->shape = plan.shape;
        s->dirA = plan.dirA;
        s->dirB = plan.dirB;
        if (plan.shape == 2) {
            s->handleA = plan.handleA;
            s->handleB = plan.handleB;
        }
        drawLast_ = plan.arrival;
        drawLastValid_ = true;
        drawHasControl_ = false;
        return true;
    }

    // The road under the cursor for upgrade and bulldoze (anywhere along it).
    u32 RoadUnder(int x, int y) const {
        const Ray ray = app_.View().RayAt(x, y);
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
        return network_.FindSegment(owner) ? owner : 0;
    }

    bool DrawToolMouseDown(int x, int y) {
        if (drawMode_ == kDrawUpgrade || drawMode_ == kDrawBulldoze) {
            const DrawHit hit = DrawHitAt(x, y);
            if (drawMode_ == kDrawBulldoze && hit.kind == DrawHit::Node) {
                network_.RemoveNode(hit.node);
                Changed(true);
                app_.Status(Fmt(L"Node %u and its roads bulldozed", hit.node));
                return true;
            }
            const u32 road = RoadUnder(x, y);
            roads::Segment* s = network_.FindSegment(road);
            if (!s) return true;
            if (drawMode_ == kDrawBulldoze) {
                network_.RemoveSegment(road);
                Changed(true);
                app_.Status(Fmt(L"Road %u bulldozed", road));
                return true;
            }
            // Upgrade: the chosen type, keeping the road's zoning and height.
            const roads::RoadProfile old = s->profile;
            roads::RoadProfile p = drawProfile_;
            p.zoneLeft = old.zoneLeft; p.zoneRight = old.zoneRight; p.zoneDepth = old.zoneDepth;
            p.zoneSetback = old.zoneSetback; p.zoneGap = old.zoneGap; p.zoneSeed = old.zoneSeed;
            p.zoneTurn = old.zoneTurn;
            p.keepGround = old.keepGround;
            p.groundAuto = old.groundAuto;
            if (old.bridge) {
                p.bridge = true;
                p.wallLeft = p.wallRight = true;
            }
            s->profile = p;
            Changed(true);
            app_.Status(Fmt(L"Road %u upgraded to %d+%d lanes", road, p.lanesForward, p.lanesBackward));
            return true;
        }

        const DrawHit hit = DrawToolHit(x, y);
        const bool drawingRoad = drawFrom_ || drawPending_;
        // Curved: the second click is where the curve bends towards.
        if (drawMode_ == kDrawCurved && drawingRoad && !drawHasControl_ && hit.kind != DrawHit::None) {
            drawControl_ = hit.point;
            drawHasControl_ = true;
            app_.Status(L"Curve: now click where the road ends");
            return true;
        }
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
                if (drawingRoad) {
                    DrawRoadTo(middle);
                    EndDrawChain();
                    app_.Status(Fmt(L"A node put in road %u and the new road joined to it", hit.segment));
                } else {
                    drawFrom_ = middle;    // and on from it, if the next click is elsewhere
                    drawLastValid_ = false;
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
                    drawLastValid_ = false;
                    app_.Status(drawMode_ == kDrawCurved ? L"Road started: click where its curve bends towards"
                                                         : L"Road started: click where it goes next");
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

    bool DrawToolKey(WPARAM key) {
        if (key >= '1' && key < '1' + kDrawModes) {
            drawMode_ = (int)(key - '1');
            drawHasControl_ = false;
            return true;
        }
        const bool coarse = ::GetKeyState(VK_SHIFT) < 0;
        if (key == VK_OEM_PLUS || key == VK_ADD) { drawElevation_ = std::min(60.0f, drawElevation_ + (coarse ? 5.0f : 1.0f)); return true; }
        if (key == VK_OEM_MINUS || key == VK_SUBTRACT) { drawElevation_ = std::max(-20.0f, drawElevation_ - (coarse ? 5.0f : 1.0f)); return true; }
        if (key == VK_ESCAPE && drawHasControl_) { drawHasControl_ = false; return true; }
        if (key == VK_BACK && drawHasControl_) { drawHasControl_ = false; return true; }
        return false;
    }

    // What the road being drawn would be, for the overlay and the panel.
    bool DrawPreview(PlannedRoad& plan) const {
        Vec3 start;
        if (drawMode_ >= kDrawUpgrade || !DrawStart(start) || drawHover_.kind == DrawHit::None) return false;
        if (drawMode_ == kDrawCurved && !drawHasControl_) return false;
        plan = PlanRoad(start, drawHover_.point);
        return true;
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
            ImGui::TextColored(accent, "Road tool");
            ImGui::SameLine();
            ImGui::TextDisabled("%d+%d lanes, %.1f wide", drawProfile_.lanesForward, drawProfile_.lanesBackward,
                                drawProfile_.HalfWidth() * 2.0f);
            ImGui::Separator();
            for (int m = 0; m < kDrawModes; ++m) {
                if (m) ImGui::SameLine();
                char label[48];
                std::snprintf(label, sizeof(label), "%s (%d)", DrawModeName(m), m + 1);
                const bool on = drawMode_ == m;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(label)) { drawMode_ = m; drawHasControl_ = false; }
                if (on) ImGui::PopStyleColor();
            }
            ImGui::TextUnformatted("Type:");
            for (int k = 0; k < kPresets; ++k) {
                ImGui::SameLine();
                if (ImGui::SmallButton(PresetName(k))) drawProfile_ = PresetProfile(k);
            }
            ImGui::Checkbox("Snap angle (15\xC2\xB0)", &drawSnapAngle_);
            ImGui::SameLine();
            ImGui::Checkbox("Snap length (8 m)", &drawSnapLength_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Elevation (+/-)", &drawElevation_, 0.25f, -20.0f, 60.0f, "%.1f m");
            {
                std::string names;
                for (int k = 0; k < roads::kZoneKinds; ++k) {
                    names += Narrow(roads::ZoneName(k));
                    names.push_back('\0');
                }
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::Combo("Zoning", &drawZone_, names.c_str()))
                    drawProfile_.zoneLeft = drawProfile_.zoneRight = drawZone_;
            }
            for (int k = 0; k < 2; ++k) {
                roads::RoadProfile::SideTerrain& t = drawProfile_.terrain[k];
                ImGui::PushID(k);
                ImGui::Checkbox(k ? "Terrain right" : "Terrain left ", &t.on);
                if (t.on) {
                    ImGui::PushItemWidth(80.0f);
                    for (size_t b = 0; b < t.bands.size(); ++b) {
                        ImGui::PushID((int)b);
                        ImGui::SameLine();
                        ImGui::DragFloat("##w", &t.bands[b].width, 0.5f, 0.5f, 300.0f, "%.0f m wide");
                        ImGui::SameLine();
                        ImGui::DragFloat("##r", &t.bands[b].rise, 0.25f, -200.0f, 200.0f, "%+.1f m");
                        ImGui::PopID();
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(t.bands.size() >= (size_t)kMostBands);
                    if (ImGui::SmallButton("Extrude")) t.bands.push_back({ 15.0f, 0.0f });
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(t.bands.size() <= 1);
                    if (ImGui::SmallButton("-")) t.bands.pop_back();
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::DragFloat("relief", &t.relief, 0.1f, 0.0f, 50.0f, "%.1f m");
                    ImGui::SameLine();
                    ImGui::DragInt("variation", &t.seed, 0.2f);
                    ImGui::SameLine();
                    ImGui::Checkbox("meet ground", &t.meetGround);
                    ImGui::SameLine();
                    ImGui::Checkbox("rock", &t.rock);
                    if (t.rock) {
                        ImGui::SameLine();
                        ImGui::DragFloat("##ra", &t.rockAngle, 0.5f, 1.0f, 89.0f, "from %.0f\xC2\xB0");
                        ImGui::SameLine();
                        ImGui::DragFloat("##rh", &t.rockHeight, 0.25f, 0.0f, 200.0f, "above %.1f m");
                    }
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("Each band: its width, and how far it climbs (+) or falls (-). Extrude adds one beyond.");
            {
                bool fill = network_.FillBetween();
                if (ImGui::Checkbox("Ground between roads that close an area", &fill)) ToggleFill();
            }
            ImGui::Separator();
            PlannedRoad plan;
            const bool drawingRoad = drawFrom_ || drawPending_;
            switch (drawMode_) {
                case kDrawUpgrade:
                    ImGui::TextUnformatted("Click a road to give it the chosen type (its zoning stays).");
                    break;
                case kDrawBulldoze:
                    ImGui::TextUnformatted("Click a road or a node to take it out.");
                    break;
                case kDrawCurved:
                    if (!drawingRoad) ImGui::TextUnformatted("Click where the road starts, or a node to draw from it.");
                    else if (!drawHasControl_) ImGui::TextUnformatted("Click the point the curve bends towards.");
                    else ImGui::TextUnformatted("Click where the road ends (a node joins it, a road gets a node).");
                    break;
                default:
                    if (drawingRoad)
                        ImGui::TextUnformatted(drawMode_ == kDrawFreeform
                                                   ? "Click on: each road curves on from the last one's direction."
                                                   : "Click where the road goes next. On a node it joins it; on a road it makes a node there.");
                    else
                        ImGui::TextUnformatted("Click open ground to start a road, a node to draw from it, a road to put a node in it.");
                    break;
            }
            if (DrawPreview(plan)) {
                float length = 0.0f;
                Vec3 previous = plan.curve.p0;
                for (int i = 1; i <= 32; ++i) {
                    const Vec3 p = plan.curve.At(i / 32.0f);
                    length += Length(p - previous);
                    previous = p;
                }
                const Vec3 c = HorizontalDirection(plan.curve.p3 - plan.curve.p0);
                float turn = 0.0f;
                if (drawLastValid_)
                    turn = Degrees(std::remainder(std::atan2(c.y, c.x) - std::atan2(drawLast_.y, drawLast_.x), 6.2831853f));
                ImGui::TextDisabled("Length %.1f m   Heading %.0f\xC2\xB0   Turn %.0f\xC2\xB0", length,
                                    std::fmod(Degrees(std::atan2(c.y, c.x)) + 360.0f, 360.0f), turn);
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!drawingRoad);
            if (ImGui::Button("End this road  (Enter)")) EndDrawChain();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close the tool  (Esc)")) CloseDrawTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // ---- the street grid tool
    //
    // A city builder's grid: three clicks -- a corner, the end of the first
    // side, and how far across -- lay a rectangle of streets in blocks of the
    // chosen size, zoned so the game's buildings line every block. Corners
    // with two streets are rounded off; nodes of roads already there are
    // joined where a grid node falls on one.

    void StartGridTool() {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        CloseOtherTools();
        visible_ = true;
        drawing_ = false;
        gridTool_ = true;
        app_.UpdateMenuChecks();
        gridStep_ = 0;
        gridLanes_ = std::clamp(std::max(lastProfile_.lanesForward, lastProfile_.lanesBackward), 1, 4);
        ::SetFocus(app_.View().Handle());
        app_.Status(L"Street grid: click a corner, then the end of the first side, then how far across. "
                    L"Esc closes the tool.");
    }

    void CloseGridTool() {
        gridTool_ = false;
        gridStep_ = 0;
        app_.UpdateMenuChecks();
        app_.Status(L"Street grid closed");
    }

    // The rectangle so far: corner, the two directions and lengths, and the
    // number of blocks each way. False until the first side is long enough.
    struct GridShape {
        Vec3 corner, u, v;
        float lengthU = 0.0f, lengthV = 0.0f;
        int blocksU = 1, blocksV = 1;
    };

    bool GridShapeOf(const Vec3& a, const Vec3& b, const Vec3& c, GridShape& shape) const {
        Vec3 u{ b.x - a.x, b.y - a.y, 0.0f };
        const float lu = std::sqrt(u.x * u.x + u.y * u.y);
        if (lu < 5.0f) return false;
        u = u * (1.0f / lu);
        Vec3 v{ -u.y, u.x, 0.0f };
        float lv = (c.x - a.x) * v.x + (c.y - a.y) * v.y;
        if (lv < 0.0f) { v = v * -1.0f; lv = -lv; }
        shape.corner = a;
        shape.u = u;
        shape.v = v;
        shape.lengthU = lu;
        shape.lengthV = lv;
        shape.blocksU = std::max(1, (int)std::lround(lu / gridBlockU_));
        shape.blocksV = std::max(1, (int)std::lround(lv / gridBlockV_));
        return lv >= 5.0f;
    }

    roads::RoadProfile GridProfile() const {
        roads::RoadProfile p = lastProfile_;
        p.lanesForward = p.lanesBackward = gridLanes_;
        p.medianType = gridLanes_ >= 3 ? p.medianType : 0;
        if (gridSidewalks_) {
            if (p.sidewalkWidth < 0.5f) p.sidewalkWidth = 3.0f;
            p.sidewalkLeft = p.sidewalkRight = true;
        }
        p.wallLeft = p.wallRight = false;
        p.bridge = false;
        p.shoulderType = 0;
        p.zoneLeft = p.zoneRight = gridZone_;
        return p;
    }

    float GridHeight(float x, float y, float fallback) const {
        float h = 0.0f;
        if (GroundBelow(x, y, fallback + 60.0f, h) && h > fallback - 80.0f) return h;
        return fallback;
    }

    void BuildGrid(const GridShape& g, bool save = true) {
        const roads::RoadProfile profile = GridProfile();
        const int nu = g.blocksU, nv = g.blocksV;
        const float stepU = g.lengthU / nu, stepV = g.lengthV / nv;
        const float rounding = std::clamp(std::min(stepU, stepV) * 0.3f, profile.HalfWidth() * 1.2f, 14.0f);
        const auto at = [&](float du, float dv) {
            Vec3 p = g.corner + g.u * du + g.v * dv;
            p.z = GridHeight(p.x, p.y, g.corner.z);
            return p;
        };
        // An existing node where a grid node falls is joined, not doubled.
        const auto existing = [&](const Vec3& p) -> u32 {
            for (const roads::Node& node : network_.Nodes())
                if (!node.follow && FlatDistance(node.position, p) < 4.0f) return node.id;
            return 0;
        };
        std::vector<std::vector<u32>> ids(nu + 1, std::vector<u32>(nv + 1, 0));
        std::vector<std::vector<bool>> rounded(nu + 1, std::vector<bool>(nv + 1, false));
        for (int i = 0; i <= nu; ++i)
            for (int j = 0; j <= nv; ++j) {
                const Vec3 p = at(stepU * i, stepV * j);
                if (const u32 old = existing(p)) { ids[i][j] = old; continue; }
                const bool corner = (i == 0 || i == nu) && (j == 0 || j == nv);
                if (corner) { rounded[i][j] = true; continue; }
                ids[i][j] = network_.AddNode(p);
            }
        // A rounded corner: a node on each of its two streets, `rounding` in,
        // heading along the street, and a quarter circle between them.
        std::map<std::tuple<int, int, int, int>, u32> arms;
        const auto arm = [&](int i, int j, int di, int dj) {
            const auto key = std::make_tuple(i, j, di, dj);
            if (const auto found = arms.find(key); found != arms.end()) return found->second;
            const Vec3 dir = g.u * (float)di + g.v * (float)dj;
            const Vec3 p = at(stepU * i + rounding * di, stepV * j + rounding * dj);
            const u32 id = network_.AddNode(p);
            if (roads::Node* n = network_.FindNode(id)) {
                n->hasHeading = true;
                n->heading = std::atan2(dir.y, dir.x) * 180.0f / 3.14159265f;
            }
            arms[key] = id;
            return id;
        };
        const auto end = [&](int i, int j, int di, int dj) {
            return rounded[i][j] ? arm(i, j, di, dj) : ids[i][j];
        };
        size_t made = 0;
        const auto join = [&](u32 a, u32 b, float handle) {
            if (!a || !b || a == b) return;
            if (roads::Segment* s = network_.AddSegment(a, b, profile)) {
                s->handleA = s->handleB = handle;
                ++made;
            }
        };
        for (int j = 0; j <= nv; ++j)
            for (int i = 0; i < nu; ++i) join(end(i, j, 1, 0), end(i + 1, j, -1, 0), 0.0f);
        for (int i = 0; i <= nu; ++i)
            for (int j = 0; j < nv; ++j) join(end(i, j, 0, 1), end(i, j + 1, 0, -1), 0.0f);
        for (int i = 0; i <= nu; i += nu)
            for (int j = 0; j <= nv; j += nv) {
                if (!rounded[i][j]) continue;
                const int di = i == 0 ? 1 : -1, dj = j == 0 ? 1 : -1;
                join(end(i, j, di, 0), end(i, j, 0, dj), rounding * 0.5523f);
            }
        // Ground under the whole grid, for the blocks to stand on -- where the
        // game has none of its own too.
        if (gridGround_) {
            const float pad = profile.HalfWidth() + 3.0f;
            const Vec3 c0 = g.corner - g.u * pad - g.v * pad;
            const Vec3 du = g.u * (g.lengthU + 2.0f * pad), dv = g.v * (g.lengthV + 2.0f * pad);
            const Vec3 quad[4] = { c0, c0 + du, c0 + du + dv, c0 + dv };
            Vec2 lo{ 1e30f, 1e30f }, hi{ -1e30f, -1e30f };
            for (const Vec3& q : quad) {
                lo = { std::min(lo.x, q.x), std::min(lo.y, q.y) };
                hi = { std::max(hi.x, q.x), std::max(hi.y, q.y) };
            }
            const auto inside = [&](float x, float y) {
                bool in = false;
                for (int i = 0, j = 3; i < 4; j = i++)
                    if ((quad[i].y > y) != (quad[j].y > y) &&
                        x < (quad[j].x - quad[i].x) * (y - quad[i].y) / (quad[j].y - quad[i].y) + quad[i].x)
                        in = !in;
                return in;
            };
            const float z = g.corner.z;
            if (const u32 patchId = terrain_.AddWhere(lo, hi, 4.0f, inside,
                                                      [&](float x, float y) { return GridHeight(x, y, z) - 0.05f; })) {
                if (terrain::Patch* patch = terrain_.Find(patchId)) {
                    std::wstring texture = groundTextures_.empty() ? L"" : groundTextures_.front();
                    for (const std::wstring& name : groundTextures_)
                        if (name.find(L"GRASS") != std::wstring::npos) { texture = name; break; }
                    if (!texture.empty()) patch->layers[0].texture = Narrow(texture);
                }
                terrainBuilt_ = 0;
                if (save) SaveTerrain();
            }
        }
        lastProfile_.zoneLeft = lastProfile_.zoneRight = gridZone_;
        Changed(save);
        app_.Status(Fmt(L"Street grid laid: %d x %d blocks, %zu roads -- zoned for %s. Select a road to change "
                        L"its zoning; Roads > Bake hands roads and buildings to the game.",
                        nu, nv, made, roads::ZoneName(gridZone_)));
    }

    bool GridToolMouseDown(int x, int y) {
        Vec3 p;
        if (!AnythingUnder(x, y, p)) return true;
        if (gridStep_ == 0) {
            gridA_ = p;
            gridStep_ = 1;
            return true;
        }
        if (gridStep_ == 1) {
            if (FlatDistance(p, gridA_) < 5.0f) return true;
            gridB_ = p;
            gridStep_ = 2;
            return true;
        }
        GridShape shape;
        if (!GridShapeOf(gridA_, gridB_, p, shape)) return true;
        gridStep_ = 0;
        BuildGrid(shape);
        return true;
    }

    bool GridToolKey(WPARAM key) {
        if (key == VK_ESCAPE) {
            if (gridStep_ > 0) gridStep_ = 0;
            else CloseGridTool();
            return true;
        }
        if (key == VK_BACK) {
            if (gridStep_ > 0) --gridStep_;
            return true;
        }
        return false;
    }

    // The rectangle and its streets as they would be laid.
    template <class Line>
    void DrawGridPreview(const Line& line) {
        if (!gridTool_ || gridStep_ == 0) return;
        const Vec3 lift{ 0, 0, 0.5f };
        if (gridStep_ == 1) {
            if (gridHoverValid_) line(gridA_ + lift, gridHover_ + lift, 0xFF60FF60);
            return;
        }
        GridShape g;
        if (!gridHoverValid_ || !GridShapeOf(gridA_, gridB_, gridHover_, g)) {
            line(gridA_ + lift, gridB_ + lift, 0xFF60FF60);
            return;
        }
        const auto at = [&](float du, float dv) {
            Vec3 p = g.corner + g.u * du + g.v * dv;
            p.z = GridHeight(p.x, p.y, g.corner.z) + 0.5f;
            return p;
        };
        const float half = GridProfile().HalfWidth();
        for (int j = 0; j <= g.blocksV; ++j) {
            const float dv = g.lengthV * j / g.blocksV;
            line(at(0.0f, dv), at(g.lengthU, dv), 0xFF60FF60);
            line(at(0.0f, dv - half), at(g.lengthU, dv - half), 0x8060FF60);
            line(at(0.0f, dv + half), at(g.lengthU, dv + half), 0x8060FF60);
        }
        for (int i = 0; i <= g.blocksU; ++i) {
            const float du = g.lengthU * i / g.blocksU;
            line(at(du, 0.0f), at(du, g.lengthV), 0xFF60FF60);
            line(at(du - half, 0.0f), at(du - half, g.lengthV), 0x8060FF60);
            line(at(du + half, 0.0f), at(du + half, g.lengthV), 0x8060FF60);
        }
    }

    void GridToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##streetgrid", nullptr, flags)) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImGui::TextColored(accent, "Street grid");
            ImGui::SameLine();
            GridShape g;
            if (gridStep_ == 2 && gridHoverValid_ && GridShapeOf(gridA_, gridB_, gridHover_, g))
                ImGui::TextDisabled("%d x %d blocks, %.0f x %.0f m", g.blocksU, g.blocksV, g.lengthU, g.lengthV);
            else
                ImGui::TextDisabled("%d buildings to choose from", (int)network_.BuildingModels().size());
            ImGui::Separator();
            const char* steps[3] = { "Click the first corner.", "Click the end of the first side.",
                                     "Click how far across: the streets are laid." };
            ImGui::TextUnformatted(steps[std::clamp(gridStep_, 0, 2)]);
            ImGui::Spacing();
            ImGui::PushItemWidth(150.0f);
            ImGui::DragFloat("Block length (m)", &gridBlockU_, 0.5f, 20.0f, 400.0f, "%.0f");
            ImGui::SameLine();
            ImGui::DragFloat("Block width (m)", &gridBlockV_, 0.5f, 20.0f, 400.0f, "%.0f");
            ImGui::SliderInt("Lanes each way", &gridLanes_, 1, 4);
            ImGui::SameLine();
            ImGui::Checkbox("Sidewalks", &gridSidewalks_);
            ImGui::SameLine();
            ImGui::Checkbox("Ground under it", &gridGround_);
            ImGui::SameLine();
            {
                bool fill = network_.FillBetween();
                if (ImGui::Checkbox("Ground between roads", &fill)) ToggleFill();
            }
            std::string names;
            for (int k = 0; k < roads::kZoneKinds; ++k) {
                names += Narrow(roads::ZoneName(k));
                names.push_back('\0');
            }
            ImGui::Combo("Zone", &gridZone_, names.c_str());
            ImGui::PopItemWidth();
            if (network_.BuildingModels().empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "This region has no buildings the zones can use.");
            ImGui::TextDisabled("Each road's zoning, lot depth and variation can be changed in its properties.");
            ImGui::Spacing();
            ImGui::BeginDisabled(gridStep_ == 0);
            if (ImGui::Button("Back  (Backspace)")) GridToolKey(VK_BACK);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close the tool  (Esc)")) CloseGridTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // FRS_CITY_DEMO: a street grid laid on the flattest open ground of the
    // region when it opens, shown with the game's look -- for pictures of the
    // tool. Kept in memory only: nothing is saved unless edited afterwards.
    void CityDemo(const nfs::World& world) {
        // Where the game stands its own things, terrain left out.
        std::vector<Vec3> things;
        Aabb area;
        for (const auto& section : world.Loaded()) {
            if (!section) continue;
            for (const nfs::SceneryInstance& instance : section->scenery.instances) {
                const nfs::SolidObject* model = world.FindModel(section->scenery.ModelHash(instance));
                if (!model) continue;
                const Vec3 at{ instance.transform.m[3][0], instance.transform.m[3][1], instance.transform.m[3][2] };
                if (model->IsTerrain()) { area.Add(at); continue; }
                things.push_back(at);
            }
        }
        if (!area.Valid()) return;
        // Out past the city, over nothing: the streets and buildings alone.
        const float sizeU = 270.0f, sizeV = 210.0f;
        const Vec3 corner{ area.min.x - 800.0f, area.min.y - 800.0f, 0.0f };
        LOG_INFO("roads: city demo at %.0f %.0f", corner.x, corner.y);
        gridBlockU_ = 90.0f;
        gridBlockV_ = 70.0f;
        gridZone_ = roads::kZoneMixed;
        gridLanes_ = 1;
        GridShape g;
        if (!GridShapeOf(corner, corner + Vec3{ sizeU, 0, 0 }, corner + Vec3{ 0, sizeV, 0 }, g)) return;
        gridGround_ = false;
        network_.SetFillBetween(true);
        BuildGrid(g, false);
        Vec3 centre = corner + Vec3{ sizeU * 0.5f, sizeV * 0.5f, 0.0f };
        Vec3 eye = corner + Vec3{ -90.0f, -110.0f, 110.0f };
        if (const char* mode = std::getenv("FRS_CITY_DEMO"); mode && std::string(mode) == "terrain") {
            // A road curving away from the grid, hill on its left, falling away on its right.
            roads::RoadProfile p = PresetProfile(1);
            p.zoneLeft = p.zoneRight = 0;
            p.wallRight = true;
            // Left: a hill, then a level shelf, then up again. Right: falling
            // away into a dip, then climbing out of it.
            p.terrain[0].on = true;
            p.terrain[0].bands = { { 22.0f, 16.0f }, { 12.0f, 0.0f }, { 18.0f, 14.0f } };
            p.terrain[0].relief = 2.5f;
            p.terrain[1].on = true;
            p.terrain[1].bands = { { 22.0f, -9.0f }, { 18.0f, 6.0f } };
            p.terrain[1].relief = 1.5f;
            const Vec3 a = corner + Vec3{ sizeU + 60.0f, -40.0f, 0.0f };
            const Vec3 b = a + Vec3{ 150.0f, 150.0f, 0.0f };
            const u32 na = network_.AddNode(a), nb = network_.AddNode(b);
            if (roads::Segment* s = network_.AddSegment(na, nb, p)) {
                s->shape = 2;
                s->dirA = 0.0f;
                s->dirB = -90.0f;
                s->handleA = s->handleB = 150.0f * 0.5523f;
            }
            // On from it with other settings: the ground eases from one to the other.
            roads::RoadProfile on = p;
            on.terrain[0].bands = { { 30.0f, 5.0f } };
            on.terrain[1].bands = { { 20.0f, 7.0f }, { 20.0f, -4.0f } };
            const u32 nc = network_.AddNode(b + Vec3{ 0.0f, 110.0f, 0.0f });
            if (roads::Segment* s = network_.AddSegment(nb, nc, on)) s->shape = 1;
            // A crossroad there, with ground on every corner.
            const u32 nd = network_.AddNode(b + Vec3{ -100.0f, 110.0f, 0.0f });
            const u32 ne = network_.AddNode(b + Vec3{ 100.0f, 110.0f, 0.0f });
            const u32 nf = network_.AddNode(b + Vec3{ 0.0f, 210.0f, 0.0f });
            for (u32 farNode : { nd, ne, nf })
                if (roads::Segment* s = network_.AddSegment(nc, farNode, on)) s->shape = 1;
            Changed(false);
            // Lights of several colours along the curve, trees on the ground
            // beside it, a few of the game's smackables by the road.
            if (const roads::Segment* curve = network_.FindSegment(network_.Segments().back().id)) (void)curve;
            {
                const roads::Segment* bend = nullptr;
                for (const roads::Segment& sg : network_.Segments())
                    if (sg.a == na) bend = &sg;
                if (bend) {
                    const roads::Curve cv = network_.CurveOf(*bend);
                    const u32 colours[] = { 0xFFD9A0, 0x60A0FF, 0xFF5030, 0x70FF90, 0xFF50D0 };
                    for (int k = 0; k < 5; ++k) {
                        const float t = 0.12f + k * 0.18f;
                        Vec3 tangent = cv.Tangent(t);
                        tangent.z = 0.0f;
                        const Vec3 dir = Normalize(tangent);
                        const Vec3 right{ dir.y, -dir.x, 0.0f };
                        roads::Light light;
                        light.position = cv.At(t) + right * (bend->profile.Edge(1) - 1.0f);
                        light.color = colours[k];
                        light.radius = 26.0f;
                        light.strength = 1.6f;
                        light.model = poleModels_.size() > 1 ? Narrow(poleModels_[1]) : std::string();
                        light.heading = Degrees(std::atan2(-right.y, -right.x));
                        network_.AddLight(light);
                    }
                    RebuildRoads();
                    for (int k = 0; k < 400; ++k) {
                        const float t = Random01();
                        Vec3 tangent = cv.Tangent(t);
                        tangent.z = 0.0f;
                        const Vec3 dir = Normalize(tangent);
                        const Vec3 left{ -dir.y, dir.x, 0.0f };
                        const Vec3 at = cv.At(t) + left * (bend->profile.Edge(-1) + 4.0f + Random01() * 45.0f);
                        float z = 0.0f;
                        if (treeModels_.empty() || !SurfaceAt(at.x, at.y, z)) continue;
                        bool crowded = false;
                        for (const roads::Prop& other : network_.Props())
                            if (FlatDistance(other.position, at) < 6.0f) { crowded = true; break; }
                        if (crowded) continue;
                        roads::Prop tree;
                        tree.tree = true;
                        tree.model = treeModels_[(size_t)(Random01() * treeModels_.size()) % treeModels_.size()];
                        tree.position = { at.x, at.y, z - 0.1f };
                        tree.heading = Random01() * 360.0f;
                        tree.scale = 0.85f + 0.35f * Random01();
                        network_.AddProp(tree);
                    }
                    const std::vector<std::string> physics(smackables_.begin(), smackables_.end());
                    for (int k = 0; k < 8 && !physics.empty(); ++k) {
                        const float t = 0.2f + k * 0.08f;
                        Vec3 tangent = cv.Tangent(t);
                        tangent.z = 0.0f;
                        const Vec3 dir = Normalize(tangent);
                        const Vec3 right{ dir.y, -dir.x, 0.0f };
                        roads::Prop prop;
                        prop.model = physics[(size_t)k % physics.size()];
                        prop.position = cv.At(t) + right * (bend->profile.Edge(1) - 2.0f);
                        prop.position.z += bend->profile.sidewalkHeight;
                        prop.heading = Random01() * 360.0f;
                        network_.AddProp(prop);
                    }
                    Changed(false);
                }
            }
            if (std::getenv("FRS_CITY_DEMO_TREES")) StartTreeTool();
            if (std::getenv("FRS_CITY_DEMO_LIBRARY")) {
                app_.ShowLibraryShelf(kLightsShelf);
                if (!network_.Lights().empty()) Select((int)network_.Lights()[1 % network_.Lights().size()].id);
            }
            if (std::getenv("FRS_CITY_DEMO_HUD")) {
                StartDrawTool(0);
                drawProfile_ = p;
                drawMode_ = kDrawCurved;
            }
            centre = a + Vec3{ 60.0f, 55.0f, 4.0f };
            eye = a + Vec3{ 175.0f, -75.0f, 60.0f };
        } else {
            StartGridTool();
        }
        Camera look = app_.View().GetCamera();
        look.LookAt(eye, centre);
        app_.View().FlyTo(look.Position(), look.Yaw(), look.Pitch(), 0.05f);
        if (HWND root = ::GetAncestor(app_.View().Handle(), GA_ROOT))
            if (!app_.View().GetRenderer().GameLook()) ::PostMessageW(root, WM_COMMAND, 44603, 0);
    }
    bool demoDone_ = false;
    bool demoActive_ = false;      // a demo is shown: nothing of the roads or terrain is saved

    void ToggleFill() {
        network_.SetFillBetween(!network_.FillBetween());
        Changed(true);
        app_.UpdateMenuChecks();
        app_.Status(network_.FillBetween() ? L"Ground fills every area the roads close all round"
                                           : L"No ground between the roads");
    }

    // ---- lights placed by hand
    //
    // A click on open ground stands a light there with the tool's settings
    // (a click on a light selects it; G moves it, Delete removes it). Each
    // casts its colour into the vertex colours of everything round it: the
    // procedural roads, terrain and buildings at once, through the street
    // light code; the game's own objects through RelightNatives, previewed
    // live and written into their models at the bake. Their shipped colours
    // are kept (models\lights-<REGION>-original.txt), so lights never add up
    // over bakes and a light taken away gives the colours back.

    void StartLightTool() {
        if (!app_.World()) {
            app_.Message(L"Open a region first.");
            return;
        }
        CloseOtherTools();
        visible_ = true;
        lightTool_ = true;
        ::SetFocus(app_.View().Handle());
        app_.UpdateMenuChecks();
        app_.Status(L"Lights: click open ground to stand a light, a light to select it (G moves, Delete removes). "
                    L"Esc closes the tool.");
    }

    void CloseLightTool() {
        lightTool_ = false;
        app_.UpdateMenuChecks();
        app_.Status(L"Light tool closed");
    }

    // The light whose lamp or post is under a ray.
    u32 LightUnder(const Ray& ray, float& distance) const {
        u32 best = 0;
        for (const roads::Light& light : network_.Lights()) {
            Aabb box;
            box.Add(light.position - Vec3{ 1.0f, 1.0f, 0.0f });
            box.Add(light.Lamp() + Vec3{ 1.0f, 1.0f, 1.0f });
            const float hit = IntersectRayAabb(ray, box);
            if (hit < 0.0f || (best && hit >= distance)) continue;
            best = light.id;
            distance = hit;
        }
        return best;
    }

    const roads::Light* SelectedLight() const {
        const int id = Selected();
        return id > 0 && !(id & (kHandleBits | kTerrainBit | kGraphBit)) ? network_.FindLight((u32)id) : nullptr;
    }
    roads::Light* SelectedLightMutable() {
        const int id = Selected();
        return id > 0 && !(id & (kHandleBits | kTerrainBit | kGraphBit)) ? network_.FindLight((u32)id) : nullptr;
    }

    bool LightToolMouseDown(int x, int y) {
        const Ray ray = app_.View().RayAt(x, y);
        float distance = 0.0f;
        if (const u32 hit = LightUnder(ray, distance)) {
            Select((int)hit);
            return true;
        }
        Vec3 point;
        if (!AnythingUnder(x, y, point)) return true;
        roads::Light light = lightDraft_;
        light.position = point;
        const u32 id = network_.AddLight(light);
        Changed(true);
        Select((int)id);
        app_.Status(Fmt(L"Light %u stood here -- it lights the roads, terrain and the game's objects round it", id));
        return true;
    }

    static ImVec4 ColourOf(u32 rgb) {
        return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
    }
    static u32 RgbOf(const float c[3]) {
        u32 out = 0;
        for (int k = 0; k < 3; ++k) out |= (u32)std::clamp((int)std::lround(c[k] * 255.0f), 0, 255) << (16 - 8 * k);
        return out;
    }

    void LightToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##lights", nullptr, flags)) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            roads::Light* selected = SelectedLightMutable();
            roads::Light& l = selected ? *selected : lightDraft_;
            ImGui::TextColored(accent, "Lights");
            ImGui::SameLine();
            if (selected) ImGui::TextDisabled("editing light %u", selected->id);
            else ImGui::TextDisabled("%zu placed; the next one:", network_.Lights().size());
            ImGui::Separator();
            bool edited = false, released = false;
            const auto track = [&](bool changed) {
                edited |= changed;
                released |= ImGui::IsItemDeactivatedAfterEdit();
            };
            float c[3] = { ColourOf(l.color).x, ColourOf(l.color).y, ColourOf(l.color).z };
            ImGui::SetNextItemWidth(220.0f);
            const bool colourChanged = ImGui::ColorEdit3("Colour", c, ImGuiColorEditFlags_DisplayHex);
            track(colourChanged);
            if (colourChanged) l.color = RgbOf(c);
            ImGui::SameLine();
            static const u32 presets[] = { 0xFFD9A0, 0xFFF2DC, 0xA8C8FF, 0xFF8030, 0x60FF90, 0xFF40C0 };
            for (u32 preset : presets) {
                ImGui::SameLine();
                ImGui::PushID((int)preset);
                if (ImGui::ColorButton("##p", ColourOf(preset), ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16))) {
                    l.color = preset;
                    edited = released = true;
                }
                ImGui::PopID();
            }
            ImGui::PushItemWidth(120.0f);
            track(ImGui::DragFloat("Reach", &l.radius, 0.25f, 2.0f, 150.0f, "%.0f m"));
            ImGui::SameLine();
            track(ImGui::DragFloat("Strength", &l.strength, 0.01f, 0.0f, 5.0f, "%.2f"));
            ImGui::SameLine();
            track(ImGui::DragFloat("Lamp height", &l.height, 0.1f, 0.0f, 60.0f, "%.1f m"));
            ImGui::PopItemWidth();
            {
                const std::string current = l.model.empty() ? "(the light alone)" : l.model;
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::BeginCombo("Post", current.c_str())) {
                    if (ImGui::Selectable("(the light alone)", l.model.empty())) { l.model.clear(); edited = released = true; }
                    for (const std::wstring& name : poleModels_) {
                        if (name == L"-") continue;
                        const std::string n = Narrow(name);
                        if (ImGui::Selectable(n.c_str(), n == l.model)) { l.model = n; edited = released = true; }
                    }
                    ImGui::EndCombo();
                }
            }
            if (selected && edited) network_.SetDirty();
            if (selected && released) Changed(true);
            if (ImGui::Checkbox("Also light the game's models used in several places", &lightShared_))
                nativeRevision_ = 0;
            ImGui::TextDisabled("Such a model is one mesh for all its places: lighting it lights every copy.");
            ImGui::TextDisabled("Click open ground for a light, a light to select it. The game's objects are written "
                                "at the next bake.");
            ImGui::Spacing();
            if (selected && ImGui::Button("New light settings")) app_.Renderer().ClearSelection();
            if (selected) ImGui::SameLine();
            if (ImGui::Button("Close the tool  (Esc)")) CloseLightTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // The game's own models within the lights' reach, lit on top of their
    // shipped colours; models no longer reached get those colours back.
    void RelightNatives() {
        nfs::World* world = app_.World();
        if (!world) return;
        const u64 revision = network_.Revision() * 2 + (lightShared_ ? 1 : 0);
        if (revision == nativeRevision_) return;
        nativeRevision_ = revision;
        const std::vector<roads::Light>& lights = network_.Lights();
        struct Target { const nfs::SolidObject* model; Mat4 transform; };
        std::map<u32, std::vector<Target>> targets;
        std::map<u32, int> uses;
        for (const auto& section : world->Loaded()) {
            if (!section) continue;
            const nfs::Scenery& scenery = section->scenery;
            for (const nfs::SceneryInstance& instance : scenery.instances) {
                const u32 hash = scenery.ModelHash(instance);
                ++uses[hash];
                if (lights.empty()) continue;
                const Aabb& b = instance.worldBounds;
                for (const roads::Light& light : lights) {
                    const Vec3 lamp = light.Lamp();
                    const float r = light.radius;
                    if (lamp.x + r < b.min.x || lamp.x - r > b.max.x || lamp.y + r < b.min.y || lamp.y - r > b.max.y ||
                        lamp.z + r < b.min.z || lamp.z - r > b.max.z)
                        continue;
                    const nfs::SolidObject* model = world->FindModel(hash);
                    if (model && !roads::IsBakedRoadModel(model->name) && !model->vertices.empty())
                        targets[hash].push_back({ model, instance.transform });
                    break;
                }
            }
        }
        std::set<u32> now;
        for (const auto& [hash, list] : targets) {
            if (uses[hash] > 1 && !lightShared_) continue;
            const nfs::SolidObject& model = *list.front().model;
            auto found = nativeOriginal_.find(hash);
            if (found == nativeOriginal_.end()) {
                std::vector<u32> original;
                for (const nfs::SolidVertex& v : model.vertices) original.push_back(v.color);
                found = nativeOriginal_.emplace(hash, std::move(original)).first;
            }
            if (found->second.size() != model.vertices.size()) continue;
            std::vector<u32> colours = found->second;
            for (const Target& t : list)
                for (size_t i = 0; i < model.vertices.size(); ++i) {
                    const Vec3 p = t.transform.TransformPoint(model.vertices[i].position);
                    const Vec3 n = Normalize(t.transform.TransformDirection(model.vertices[i].normal));
                    float c[3] = { (float)((colours[i] >> 16) & 0xFF), (float)((colours[i] >> 8) & 0xFF),
                                   (float)(colours[i] & 0xFF) };
                    for (const roads::Light& light : lights) {
                        const Vec3 d = p - light.Lamp();
                        const float d2 = (d.x * d.x + d.y * d.y + d.z * d.z * 0.36f) / (light.radius * light.radius);
                        if (d2 >= 1.0f) continue;
                        const float fall = 1.0f - d2;
                        const float len = Length(d);
                        const float facing = len > 1e-3f ? std::clamp(-Dot(n, d) / len, 0.0f, 1.0f) : 1.0f;
                        const float f = std::min(1.0f, fall * fall * fall * light.strength * 0.6f * (0.35f + 0.65f * facing));
                        const float lamp[3] = { (float)((light.color >> 16) & 0xFF), (float)((light.color >> 8) & 0xFF),
                                                (float)(light.color & 0xFF) };
                        for (int k = 0; k < 3; ++k)
                            if (lamp[k] > c[k]) c[k] += (lamp[k] - c[k]) * f;
                    }
                    u32 out = colours[i] & 0xFF000000u;
                    for (int k = 0; k < 3; ++k) out |= (u32)std::clamp((int)std::lround(c[k]), 0, 255) << (16 - 8 * k);
                    colours[i] = out;
                }
            WriteModelColours(*world, hash, colours);
            now.insert(hash);
        }
        for (u32 hash : nativeLit_)
            if (!now.count(hash))
                if (const auto found = nativeOriginal_.find(hash); found != nativeOriginal_.end())
                    WriteModelColours(*world, hash, found->second);
        nativeLit_ = std::move(now);
    }

    void WriteModelColours(nfs::World& world, u32 hash, const std::vector<u32>& colours) {
        for (size_t s = 0; s < world.Loaded().size(); ++s)
            if (nfs::LoadedSection* section = world.MutableLoaded(s))
                for (nfs::SolidList& list : section->solidLists)
                    for (nfs::SolidObject& object : list.objects)
                        if (object.hash == hash && object.vertices.size() == colours.size())
                            for (size_t i = 0; i < colours.size(); ++i) object.vertices[i].color = colours[i];
        app_.Renderer().UpdateModelColours(hash, colours);
    }

    std::wstring NativeOriginalPath() const {
        const std::wstring region = app_.World() ? app_.World()->Region() : L"";
        return app_.ModelsFolder() + L"\\lights-" + region + L"-original.txt";
    }

    // The shipped colours of the game's models a bake has lit, as written.
    void LoadNativeOriginals() {
        nativeOriginal_.clear();
        nativeWritten_.clear();
        std::ifstream in(NativeOriginalPath(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string hashText;
            size_t count = 0;
            if (!(fields >> hashText >> count) || count > 1000000) continue;
            std::vector<u32> colours(count);
            std::string word;
            for (size_t i = 0; i < count && fields >> word; ++i) colours[i] = (u32)std::strtoul(word.c_str(), nullptr, 16);
            const u32 hash = (u32)std::strtoul(hashText.c_str(), nullptr, 16);
            nativeOriginal_[hash] = std::move(colours);
            nativeWritten_.insert(hash);
        }
        // Lit in the files: until worked out again, as if lit now.
        nativeLit_ = nativeWritten_;
        nativeRevision_ = 0;
    }

    // Writes the game's models the lights changed (and those they no longer
    // reach, back to how they shipped), as an edit from Blender comes back.
    void WriteNativeLights() {
        nfs::World* world = app_.World();
        if (!world) return;
        nativeRevision_ = 0;
        RelightNatives();
        std::set<u32> write = nativeLit_;
        write.insert(nativeWritten_.begin(), nativeWritten_.end());
        if (write.empty()) return;
        std::vector<PickResult> picks;
        for (u32 hash : write) {
            const nfs::SolidObject* model = world->FindModel(hash);
            if (!model) continue;
            bool found = false;
            for (const auto& section : world->Loaded()) {
                if (!section || found) continue;
                for (const nfs::SceneryInstance& instance : section->scenery.instances)
                    if (section->scenery.ModelHash(instance) == hash) {
                        PickResult picked;
                        picked.object = model;
                        picked.transform = instance.transform;
                        picked.worldBounds = instance.worldBounds;
                        picked.fromInstance = true;
                        if (section->info) picked.sectionName = section->info->name;
                        picks.push_back(picked);
                        found = true;
                        break;
                    }
            }
        }
        // The shipped colours of what stays lit, for the next bake.
        {
            std::ofstream out(NativeOriginalPath(), std::ios::binary | std::ios::trunc);
            out << "# the game's models lit by the editor's lights: model hash, vertex count, shipped colours\n";
            for (u32 hash : nativeLit_) {
                const auto found = nativeOriginal_.find(hash);
                if (found == nativeOriginal_.end()) continue;
                char text[32];
                std::snprintf(text, sizeof(text), "%08X %zu", hash, found->second.size());
                out << text;
                for (u32 c : found->second) {
                    std::snprintf(text, sizeof(text), " %08X", c);
                    out << text;
                }
                out << "\n";
            }
        }
        if (picks.empty()) return;
        wchar_t base[MAX_PATH] = {};
        if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
        ::CreateDirectoryW(folder.c_str(), nullptr);
        folder += L"\\lights";
        ::CreateDirectoryW(folder.c_str(), nullptr);
        const std::wstring objPath = folder + L"\\lights.obj";
        ExportSummary summary;
        if (!ExportSelection(*world, picks, objPath, world->Region(), summary)) {
            LOG_WARN("lights: the game's models could not be exported");
            return;
        }
        LOG_INFO("lights: writing %zu of the game's models (%zu lit)", picks.size(), nativeLit_.size());
        app_.ImportEdited(objPath);
    }

    // ---- trees (painted) and props (placed)
    //
    // The tree brush paints the game's trees where the ground is, up to a
    // density, never on a road and never closer than the spacing; Ctrl (or
    // the eraser) takes them away. The prop tool stands one model per click:
    // the game's smackables -- models with a physics class in the region's
    // layout (ZCS_<name> for XO_<name>_1a) -- come first, and keep the
    // physics the game gives them. Both become placements at the bake.

    void StartTreeTool() {
        if (!app_.World()) { app_.Message(L"Open a region first."); return; }
        CloseOtherTools();
        visible_ = true;
        treeTool_ = true;
        ::SetFocus(app_.View().Handle());
        app_.UpdateMenuChecks();
        app_.Status(L"Tree brush: hold the left button to paint trees, Ctrl to erase; [ and ] size the brush. "
                    L"Esc closes it.");
    }

    void StartPropTool() {
        if (!app_.World()) { app_.Message(L"Open a region first."); return; }
        CloseOtherTools();
        visible_ = true;
        propTool_ = true;
        if (propModel_.empty() && !propModels_.empty()) propModel_ = propModels_.front();
        ::SetFocus(app_.View().Handle());
        app_.UpdateMenuChecks();
        app_.Status(L"Props: click open ground to stand the chosen prop, a prop to select it (G/R move and turn it). "
                    L"Esc closes the tool.");
    }

    // Every placing tool off, before another one starts.
    void CloseOtherTools() {
        if (wizard_ != kWizardOff) EndFaceWizard(nullptr);
        if (drawTool_) CloseDrawTool();
        if (gridTool_) CloseGridTool();
        lightTool_ = treeTool_ = propTool_ = false;
        treePainting_ = false;
        SetBrush(terrain::Brush::None);
    }

    void CloseTreeTool() {
        if (treePainting_) EndTreeStroke();
        treeTool_ = false;
        app_.UpdateMenuChecks();
        app_.Status(L"Tree brush closed");
    }
    void ClosePropTool() {
        propTool_ = false;
        app_.UpdateMenuChecks();
        app_.Status(L"Prop tool closed");
    }

    // The trees and props the region offers, from its own placed models.
    void CollectPlantsAndProps(const nfs::World& world, const std::unordered_set<u32>& instanced) {
        treeModels_.clear();
        propModels_.clear();
        smackables_.clear();
        // The physics classes in the region's layout file (0x34026: 256-byte
        // records, the class name first).
        std::vector<std::string> classes;
        {
            const std::wstring path = app_.GameFolder() + L"\\TRACKS\\" + world.Region() + L".BUN";
            std::ifstream in(path, std::ios::binary);
            std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            size_t off = 0;
            while (off + 8 <= data.size()) {
                u32 id = 0, size = 0;
                std::memcpy(&id, data.data() + off, 4);
                std::memcpy(&size, data.data() + off + 4, 4);
                if (off + 8 + size > data.size()) break;
                if (id == 0x00034026) {
                    size_t p = off + 8;
                    const size_t end = off + 8 + size;
                    while (p < end && (unsigned char)data[p] == 0x11) ++p;
                    // The records start at the first name.
                    while (p + 4 < end && !(data[p] == 'Z' || data[p] == 'X' || data[p] == 'C')) ++p;
                    for (; p + 256 <= end; p += 256) {
                        const char* name = data.data() + p;
                        classes.emplace_back(name, ::strnlen(name, 32));
                    }
                }
                off += 8 + size;
            }
        }
        std::vector<std::string> prefixes;
        for (std::string c : classes) {
            for (char& ch : c) ch = (char)::toupper((unsigned char)ch);
            if (c.rfind("ZCS_", 0) == 0) c = c.substr(4);
            else if (c.rfind("ZCV_", 0) == 0) continue;       // vehicles, doors: not props
            if (c.rfind("XO_", 0) != 0) c = "XO_" + c;
            prefixes.push_back(c + "_");
        }
        std::vector<std::string> others;
        for (const nfs::SolidObject* model : world.AllModels()) {
            if (!model || !model->bounds.Valid() || !instanced.count(model->hash)) continue;
            const std::string& n = model->name;
            if (n.find("_1A") == std::string::npos) continue;
            const Aabb& b = model->bounds;
            if (b.min.z < -1.5f || b.min.z > 1.0f) continue;
            if (n.rfind("XT_", 0) == 0 && b.max.z - b.min.z > 2.0f) {
                treeModels_.push_back(n);
                continue;
            }
            if (n.rfind("XO_", 0) != 0) continue;
            bool smack = false;
            for (const std::string& prefix : prefixes)
                if (n.rfind(prefix, 0) == 0) { smack = true; break; }
            if (smack) {
                smackables_.insert(n);
                propModels_.push_back(n);
            } else if (b.max.x - b.min.x < 8.0f && b.max.y - b.min.y < 8.0f && b.max.z - b.min.z < 8.0f) {
                others.push_back(n);
            }
        }
        std::sort(treeModels_.begin(), treeModels_.end());
        treeModels_.erase(std::unique(treeModels_.begin(), treeModels_.end()), treeModels_.end());
        std::sort(propModels_.begin(), propModels_.end());
        propModels_.erase(std::unique(propModels_.begin(), propModels_.end()), propModels_.end());
        std::sort(others.begin(), others.end());
        others.erase(std::unique(others.begin(), others.end()), others.end());
        propModels_.insert(propModels_.end(), others.begin(), others.end());
        treeChosen_.clear();
        LOG_INFO("roads: %zu trees, %zu props (%zu with the game's physics, from %zu classes)", treeModels_.size(),
                 propModels_.size(), smackables_.size(), classes.size());
    }

    // The top of the ground at a point -- the game's, terrain made here, or
    // the ground beside the roads and between them -- or false.
    bool SurfaceAt(float x, float y, float& height) const {
        bool found = GroundBelow(x, y, 5000.0f, height);
        const Ray down{ { x, y, 5000.0f }, { 0.0f, 0.0f, -1.0f } };
        for (const DrawBatch& batch : batches_)
            for (size_t k = 0; k + 2 < batch.indices.size(); k += 3) {
                const Vec3& a = batch.vertices[batch.indices[k]].position;
                const Vec3& b = batch.vertices[batch.indices[k + 1]].position;
                const Vec3& c = batch.vertices[batch.indices[k + 2]].position;
                if (x < std::min({ a.x, b.x, c.x }) || x > std::max({ a.x, b.x, c.x }) ||
                    y < std::min({ a.y, b.y, c.y }) || y > std::max({ a.y, b.y, c.y }))
                    continue;
                const float d = IntersectRayTriangle(down, a, b, c);
                if (d < 0.0f) continue;
                const float z = 5000.0f - d;
                if (!found || z > height) { height = z; found = true; }
            }
        return found;
    }

    void EndTreeStroke() {
        treePainting_ = false;
        if (treeStrokeChanged_) {
            treeStrokeChanged_ = false;
            Changed(true);
        }
    }

    // One step of the tree brush, while the button is held.
    void PaintTrees(float deltaSeconds) {
        treeClock_ += deltaSeconds;
        if (treeClock_ < 0.08f) return;
        treeClock_ = 0.0f;
        if (!treeBrushHit_) return;
        const Vec3 c = treeBrushPoint_;
        const float r = treeRadius_;
        const bool erase = treeErase_ || ::GetKeyState(VK_CONTROL) < 0;
        if (erase) {
            const size_t before = network_.Props().size();
            network_.RemoveProps([&](const roads::Prop& p) {
                return p.tree && FlatDistance(p.position, c) < r;
            });
            if (network_.Props().size() != before) treeStrokeChanged_ = true;
            return;
        }
        std::vector<std::string> choices;
        for (const std::string& name : treeModels_)
            if (treeChosen_.empty() || treeChosen_.count(name)) choices.push_back(name);
        if (choices.empty()) return;
        // How many the circle may hold, and how many it has.
        const float area = 3.14159265f * r * r;
        const size_t wanted = (size_t)std::max(1.0f, area * treeDensity_ / 1000.0f);
        std::vector<Vec3> close;
        for (const roads::Prop& p : network_.Props())
            if (p.tree && FlatDistance(p.position, c) < r + treeSpacing_) close.push_back(p.position);
        size_t inside = 0;
        for (const Vec3& q : close) inside += FlatDistance(q, c) < r;
        bool added = false;
        for (int attempt = 0; attempt < 8 && inside < wanted; ++attempt) {
            const float a = Random01() * 6.2831853f, d = std::sqrt(Random01()) * r;
            const float x = c.x + std::cos(a) * d, y = c.y + std::sin(a) * d;
            bool crowded = false;
            for (const Vec3& q : close)
                if ((q.x - x) * (q.x - x) + (q.y - y) * (q.y - y) < treeSpacing_ * treeSpacing_) { crowded = true; break; }
            if (crowded) continue;
            float roadZ = 0.0f, outside = 0.0f;
            if (!field_.Empty() && field_.Query(x, y, 3.0f, roadZ, outside) && outside < 2.0f) continue;   // a road
            float z = 0.0f;
            if (!SurfaceAt(x, y, z)) continue;
            roads::Prop tree;
            tree.tree = true;
            tree.model = choices[(size_t)(Random01() * choices.size()) % choices.size()];
            tree.position = { x, y, z - 0.1f };
            tree.heading = Random01() * 360.0f;
            tree.scale = treeScaleMin_ + (treeScaleMax_ - treeScaleMin_) * Random01();
            network_.AddProp(tree);
            close.push_back(tree.position);
            ++inside;
            added = true;
        }
        if (added) treeStrokeChanged_ = true;
    }

    float Random01() {
        randomState_ = randomState_ * 6364136223846793005ull + 1442695040888963407ull;
        return (float)((randomState_ >> 40) & 0xFFFFFF) / 16777216.0f;
    }

    // The prop or tree under a ray.
    u32 PropUnder(const Ray& ray, float& distance) const {
        u32 best = 0;
        for (const roads::Prop& p : network_.Props()) {
            Vec3 lo{ -0.6f, -0.6f, 0.0f }, hi{ 0.6f, 0.6f, 2.0f };
            if (const auto found = modelsByName_.find(Widen(p.model));
                found != modelsByName_.end() && found->second->bounds.Valid()) {
                const Aabb& b = found->second->bounds;
                const float reach = std::max(0.6f, std::min(b.max.x - b.min.x, b.max.y - b.min.y) * 0.5f) * p.scale;
                lo = { -reach, -reach, 0.0f };
                hi = { reach, reach, std::max(1.0f, (b.max.z - b.min.z) * p.scale) };
            }
            Aabb box;
            box.Add(p.position + lo);
            box.Add(p.position + hi);
            const float hit = IntersectRayAabb(ray, box);
            if (hit < 0.0f || (best && hit >= distance)) continue;
            best = p.id;
            distance = hit;
        }
        return best;
    }

    const roads::Prop* SelectedProp() const {
        const int id = Selected();
        return id > 0 && !(id & (kHandleBits | kTerrainBit | kGraphBit)) ? network_.FindProp((u32)id) : nullptr;
    }
    roads::Prop* SelectedPropMutable() {
        const int id = Selected();
        return id > 0 && !(id & (kHandleBits | kTerrainBit | kGraphBit)) ? network_.FindProp((u32)id) : nullptr;
    }

    bool PropToolMouseDown(int x, int y) {
        const Ray ray = app_.View().RayAt(x, y);
        float distance = 0.0f;
        if (const u32 hit = PropUnder(ray, distance)) {
            Select((int)hit);
            return true;
        }
        if (propModel_.empty()) {
            app_.Status(L"Choose a prop in the tool's list first");
            return true;
        }
        Vec3 point;
        if (!AnythingUnder(x, y, point)) return true;
        float z = 0.0f;
        if (SurfaceAt(point.x, point.y, z) && std::fabs(z - point.z) < 3.0f) point.z = z;
        roads::Prop prop;
        prop.model = propModel_;
        prop.position = point;
        prop.heading = propRandomTurn_ ? Random01() * 360.0f : propHeading_;
        const u32 id = network_.AddProp(prop);
        Changed(true);
        Select((int)id);
        app_.Status(Fmt(L"%S stood here%s", propModel_.c_str(),
                        smackables_.count(propModel_) ? L" -- it keeps the game's physics" : L""));
        return true;
    }

    static ImGuiWindowFlags HudFlags() {
        return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
               ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
               ImGuiWindowFlags_NoMove;
    }

    void TreeToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        if (ImGui::Begin("##trees", nullptr, HudFlags())) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            size_t trees = 0;
            for (const roads::Prop& p : network_.Props()) trees += p.tree;
            ImGui::TextColored(accent, "Tree brush");
            ImGui::SameLine();
            ImGui::TextDisabled("%zu trees painted, %zu kinds in the region", trees, treeModels_.size());
            ImGui::Separator();
            ImGui::PushItemWidth(110.0f);
            ImGui::DragFloat("Brush ([ ])", &treeRadius_, 0.25f, 1.0f, 200.0f, "%.0f m");
            ImGui::SameLine();
            ImGui::DragFloat("Density", &treeDensity_, 0.1f, 0.2f, 60.0f, "%.1f / 1000 m2");
            ImGui::SameLine();
            ImGui::DragFloat("Spacing", &treeSpacing_, 0.1f, 0.5f, 50.0f, "%.1f m");
            ImGui::DragFloat("Smallest", &treeScaleMin_, 0.01f, 0.3f, 3.0f, "x %.2f");
            ImGui::SameLine();
            ImGui::DragFloat("Largest", &treeScaleMax_, 0.01f, 0.3f, 3.0f, "x %.2f");
            ImGui::PopItemWidth();
            if (treeScaleMax_ < treeScaleMin_) treeScaleMax_ = treeScaleMin_;
            ImGui::SameLine();
            ImGui::Checkbox("Eraser (or hold Ctrl)", &treeErase_);
            ImGui::TextUnformatted("Kinds (none ticked: all of them):");
            if (ImGui::BeginChild("##kinds", ImVec2(560.0f, 110.0f), true)) {
                int column = 0;
                for (const std::string& name : treeModels_) {
                    bool on = treeChosen_.count(name) != 0;
                    if (column++ % 2) ImGui::SameLine(280.0f);
                    if (ImGui::Checkbox(name.c_str(), &on)) {
                        if (on) treeChosen_.insert(name);
                        else treeChosen_.erase(name);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::TextDisabled("Never on a road; on the game's ground, terrain made here and the ground beside roads.");
            if (ImGui::Button("Close the tool  (Esc)")) CloseTreeTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    void PropToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        if (ImGui::Begin("##props", nullptr, HudFlags())) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            ImGui::TextColored(accent, "Props");
            ImGui::SameLine();
            ImGui::TextDisabled("%zu with the game's physics, %zu other street props", smackables_.size(),
                                propModels_.size() - smackables_.size());
            ImGui::Separator();
            ImGui::SetNextItemWidth(260.0f);
            ImGui::InputTextWithHint("##filter", "filter by name", propFilter_, sizeof(propFilter_));
            ImGui::SameLine();
            ImGui::Checkbox("Random turn", &propRandomTurn_);
            if (!propRandomTurn_) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("Heading", &propHeading_, 1.0f, -360.0f, 360.0f, "%.0f deg");
            }
            std::string filter = propFilter_;
            for (char& ch : filter) ch = (char)::toupper((unsigned char)ch);
            if (ImGui::BeginChild("##propslist", ImVec2(560.0f, 160.0f), true)) {
                for (const std::string& name : propModels_) {
                    if (!filter.empty() && name.find(filter) == std::string::npos) continue;
                    const bool physics = smackables_.count(name) != 0;
                    const std::string label = (physics ? "[physics]  " : "           ") + name;
                    if (ImGui::Selectable(label.c_str(), name == propModel_)) propModel_ = name;
                }
            }
            ImGui::EndChild();
            ImGui::TextDisabled("[physics]: the game's smackables -- they get knocked about when hit (check in game).");
            ImGui::TextDisabled("Click open ground to stand the chosen one; click a prop to select it.");
            if (ImGui::Button("Close the tool  (Esc)")) ClosePropTool();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // The ground beside the selected roads as a terrain patch: the same
    // heights, sampled from the generated ground, with 40 m more round its
    // edge (past the roads' ends too) carrying on from it; steep cells
    // painted with the rock. The roads' own ground beside them goes, the
    // patch takes its place -- and from then on the Terrain brushes sculpt,
    // paint and grow it.
    void TerrainFromRoadGround() {
        const std::vector<u32> ids = SelectedRoadIds();
        std::set<u32> chosen, nodes;
        for (u32 id : ids)
            if (const roads::Segment* s = network_.FindSegment(id); s && (s->profile.terrain[0].on || s->profile.terrain[1].on)) {
                chosen.insert(id);
                nodes.insert(s->a);
                nodes.insert(s->b);
            }
        if (chosen.empty()) {
            app_.Status(L"Select a road with ground beside it (Terrain beside: left or right) first");
            return;
        }
        // The generated ground of those roads, and of the junction corners between them.
        struct Tri { Vec3 a, b, c; };
        std::vector<Tri> tris;
        Aabb box;
        for (const roads::MeshPiece& piece : network_.BuildMesh()) {
            bool take = false;
            if (piece.name.rfind("ROAD_GJ", 0) == 0) take = nodes.count(piece.node) != 0;
            else if (piece.name.rfind("ROAD_G", 0) == 0) take = chosen.count(piece.segment) != 0;
            if (!take) continue;
            for (const roads::MeshGroup& g : piece.groups)
                for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                    const Tri t{ g.vertices[g.indices[k]].position, g.vertices[g.indices[k + 1]].position,
                                 g.vertices[g.indices[k + 2]].position };
                    tris.push_back(t);
                    box.Add(t.a); box.Add(t.b); box.Add(t.c);
                }
        }
        if (tris.empty()) {
            app_.Status(L"Those roads have no ground beside them to turn into terrain");
            return;
        }
        constexpr float kCell = 2.5f, kMargin = 40.0f;
        const Vec2 origin{ std::floor((box.min.x - kMargin) / kCell) * kCell,
                           std::floor((box.min.y - kMargin) / kCell) * kCell };
        const int columns = std::min(1000, (int)std::ceil((box.max.x + kMargin - origin.x) / kCell));
        const int rows = std::min(1000, (int)std::ceil((box.max.y + kMargin - origin.y) / kCell));
        const auto at = [columns](int i, int j) { return (size_t)j * (columns + 1) + i; };
        std::vector<float> height((size_t)(columns + 1) * (rows + 1), 0.0f);
        std::vector<int> rank(height.size(), -1);     // corner steps from the sampled ground; -1 not yet
        // Triangles in 10 m buckets, for the downward rays.
        constexpr float kBucket = 10.0f;
        const int bx = (int)std::ceil(columns * kCell / kBucket) + 1, by = (int)std::ceil(rows * kCell / kBucket) + 1;
        std::vector<std::vector<size_t>> buckets((size_t)bx * by);
        for (size_t t = 0; t < tris.size(); ++t) {
            const Tri& tr = tris[t];
            const float x0 = std::min({ tr.a.x, tr.b.x, tr.c.x }), x1 = std::max({ tr.a.x, tr.b.x, tr.c.x });
            const float y0 = std::min({ tr.a.y, tr.b.y, tr.c.y }), y1 = std::max({ tr.a.y, tr.b.y, tr.c.y });
            for (int v = std::max(0, (int)((y0 - origin.y) / kBucket)); v <= std::min(by - 1, (int)((y1 - origin.y) / kBucket)); ++v)
                for (int u = std::max(0, (int)((x0 - origin.x) / kBucket)); u <= std::min(bx - 1, (int)((x1 - origin.x) / kBucket)); ++u)
                    buckets[(size_t)v * bx + u].push_back(t);
        }
        for (int j = 0; j <= rows; ++j)
            for (int i = 0; i <= columns; ++i) {
                const float x = origin.x + i * kCell, y = origin.y + j * kCell;
                const int u = std::min(bx - 1, (int)((x - origin.x) / kBucket)), v = std::min(by - 1, (int)((y - origin.y) / kBucket));
                const Ray down{ { x, y, box.max.z + 50.0f }, { 0.0f, 0.0f, -1.0f } };
                float best = -1.0f;
                for (size_t t : buckets[(size_t)v * bx + u]) {
                    const float d = IntersectRayTriangle(down, tris[t].a, tris[t].b, tris[t].c);
                    if (d >= 0.0f && (best < 0.0f || d < best)) best = d;
                }
                if (best >= 0.0f) {
                    height[at(i, j)] = box.max.z + 50.0f - best;
                    rank[at(i, j)] = 0;
                }
            }
        // Carried on outwards, ring by ring: each new corner the mean of its
        // neighbours already known -- the ground goes on as it was going.
        for (int step = 1; step <= (int)(kMargin / kCell) + 2; ++step) {
            std::vector<std::pair<size_t, float>> found;
            for (int j = 0; j <= rows; ++j)
                for (int i = 0; i <= columns; ++i) {
                    if (rank[at(i, j)] >= 0) continue;
                    float sum = 0.0f;
                    int count = 0;
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di) {
                            const int ni = i + di, nj = j + dj;
                            if ((di || dj) && ni >= 0 && nj >= 0 && ni <= columns && nj <= rows && rank[at(ni, nj)] >= 0 &&
                                rank[at(ni, nj)] < step) {
                                sum += height[at(ni, nj)];
                                ++count;
                            }
                        }
                    if (count) found.push_back({ at(i, j), sum / count });
                }
            if (found.empty()) break;
            for (const auto& [index, h] : found) {
                height[index] = h;
                rank[index] = step;
            }
        }
        for (size_t k = 0; k < height.size(); ++k)
            if (rank[k] < 0) height[k] = box.min.z;
        // Cells within the margin of the sampled ground, but not under the roads.
        const int reach = (int)(kMargin / kCell);
        const auto keep = [&](float x, float y) {
            const int i = (int)std::floor((x - origin.x) / kCell), j = (int)std::floor((y - origin.y) / kCell);
            if (i < 0 || j < 0 || i >= columns || j >= rows) return false;
            int nearest = 1 << 20;
            for (int dj = 0; dj <= 1; ++dj)
                for (int di = 0; di <= 1; ++di)
                    if (const int r = rank[at(i + di, j + dj)]; r >= 0) nearest = std::min(nearest, r);
            // Under the roads too: the patch sinks under them by itself, and
            // leaving them out left a ragged gap along the edge.
            return nearest <= reach;
        };
        const u32 id = terrain_.AddWhere(origin, { origin.x + columns * kCell, origin.y + rows * kCell }, kCell, keep,
                                         [&](float x, float y) {
                                             const int i = std::clamp((int)std::lround((x - origin.x) / kCell), 0, columns);
                                             const int j = std::clamp((int)std::lround((y - origin.y) / kCell), 0, rows);
                                             return height[at(i, j)];
                                         });
        terrain::Patch* patch = terrain_.Find(id);
        if (!patch) {
            app_.Status(L"No terrain could be made there");
            return;
        }
        // Grass, with the rock coming out on the steep ground the way it did
        // beside the road (the second layer is there to paint by hand).
        patch->layers.resize(2);
        patch->layers[0].texture = network_.GroundTexture();
        patch->layers[0].tile = 24.0f;
        patch->layers[0].surface = 0x0F;
        patch->layers[1].texture = network_.RockTexture().empty() ? network_.GroundTexture() : network_.RockTexture();
        patch->layers[1].tile = 14.0f;
        patch->layers[1].surface = 0x0F;
        patch->autoRock = true;
        patch->rockAngle = 32.0f;
        patch->rockTexture = network_.RockTexture();
        patch->mixTexture = network_.RockMixTexture();
        patch->conform = true;
        patch->shoulder = 3.0f;          // it already meets the road's edge
        patch->sink = 0.15f;
        for (u32 roadId : chosen)
            if (roads::Segment* s = network_.FindSegment(roadId)) s->profile.terrain[0].on = s->profile.terrain[1].on = false;
        terrainBuilt_ = 0;
        terrainBatches_.clear();
        SaveTerrain();
        Changed(true);
        Select(kTerrainBit | (int)id);
        app_.Status(Fmt(L"The ground beside %zu road(s) is terrain patch %u now: Terrain > Raise / Lower / Smooth / "
                        L"Flatten sculpt it, Paint Texture paints it, Add Area grows it on from its edge",
                        chosen.size(), id));
    }

    // The terrain tool's panel: the brushes by what they do, their size and
    // strength, and the selected patch's own settings.
    void TerrainToolHud(float x, float y, float width) {
        ImGui::SetNextWindowPos(ImVec2(x + width * 0.5f, y + 52.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        if (ImGui::Begin("##terraintool", nullptr, HudFlags())) {
            const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            terrain::Patch* patch = SelectedPatchMutable();
            ImGui::TextColored(accent, "Terrain");
            ImGui::SameLine();
            if (patch) ImGui::TextDisabled("patch %u, %d x %d cells of %.1f m", patch->id, patch->columns, patch->rows, patch->cell);
            else ImGui::TextDisabled("click a terrain patch to work on it (the game's ground: raise, lower, smooth, flatten)");
            ImGui::Separator();
            struct Choice { terrain::Brush brush; const char* label; const char* help; };
            static const Choice choices[] = {
                { terrain::Brush::Raise, "Raise", "Lifts the ground under the brush." },
                { terrain::Brush::Lower, "Lower", "Sinks the ground under the brush." },
                { terrain::Brush::Smooth, "Smooth", "Evens out bumps and edges." },
                { terrain::Brush::Flatten, "Flatten", "Levels to the height where the stroke began." },
                { terrain::Brush::Paint, "Paint", "Paints the chosen layer's texture." },
                { terrain::Brush::AddArea, "Extrude", "Grows the patch where the brush goes, carrying on from its edge." },
                { terrain::Brush::EraseArea, "Cut", "Cuts the patch away where the brush goes." },
            };
            for (size_t k = 0; k < std::size(choices); ++k) {
                if (k) ImGui::SameLine();
                const bool on = brush_ == choices[k].brush;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(choices[k].label)) SetBrush(choices[k].brush);
                if (on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", choices[k].help);
            }
            ImGui::PushItemWidth(120.0f);
            ImGui::DragFloat("Size ([ ])", &brushRadius_, 0.25f, 1.0f, 400.0f, "%.0f m");
            ImGui::SameLine();
            ImGui::DragFloat("Strength", &brushStrength_, 0.1f, 0.05f, 200.0f, "%.1f");
            if (patch && brush_ == terrain::Brush::Paint) {
                ImGui::SameLine();
                int layer = paintLayer_;
                std::string names;
                for (size_t l = 0; l < patch->layers.size(); ++l) {
                    names += std::to_string(l + 1) + "  " + patch->layers[l].texture;
                    names.push_back('\0');
                }
                if (ImGui::Combo("Layer", &layer, names.c_str())) paintLayer_ = (u8)layer;
            }
            ImGui::PopItemWidth();
            if (patch) {
                bool rock = patch->autoRock;
                if (ImGui::Checkbox("Rock on steep ground", &rock)) {
                    patch->autoRock = rock;
                    if (rock && patch->rockTexture.empty()) {
                        patch->rockTexture = network_.RockTexture();
                        patch->mixTexture = network_.RockMixTexture();
                    }
                    terrain_.Touch();
                    terrainBuilt_ = 0;
                    SaveTerrain();
                    Commit(L"terrain rock");
                }
                if (patch->autoRock) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::DragFloat("from##rockangle", &patch->rockAngle, 0.5f, 5.0f, 85.0f, "%.0f deg")) {
                        terrain_.Touch();
                        terrainBuilt_ = 0;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        SaveTerrain();
                        Commit(L"terrain rock angle");
                    }
                }
            }
            ImGui::TextDisabled("Hold the left button to work; Esc puts the brush away.");
            if (ImGui::Button("Close the tool  (Esc)")) SetBrush(terrain::Brush::None);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // Another band of ground beside the selected roads, or the outermost one off.
    void ExtrudeTerrain(int side, bool add) {
        std::vector<u32> ids = SelectedRoadIds();
        if (ids.empty()) { app_.Status(L"Select a road first (Ctrl/Shift in the Explorer for several)"); return; }
        for (u32 id : ids)
            if (roads::Segment* s = network_.FindSegment(id)) {
                roads::RoadProfile::SideTerrain& t = s->profile.terrain[side];
                if (add) {
                    if (!t.on) t.on = true;
                    else if (t.bands.size() < (size_t)kMostBands) t.bands.push_back({ 15.0f, 0.0f });
                } else if (t.bands.size() > 1) {
                    t.bands.pop_back();
                }
            }
        Changed(true);
    }

    // Other buildings along the selected roads: the next variation.
    void ReseedZones() {
        std::vector<u32> ids = SelectedRoadIds();
        if (ids.empty()) { app_.Status(L"Select a road first (Ctrl/Shift in the Explorer for several)"); return; }
        for (u32 id : ids)
            if (roads::Segment* s = network_.FindSegment(id)) ++s->profile.zoneSeed;
        Changed(true);
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
        wizardFaces_.clear(); wizardKinds_.clear();
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
        if (lightTool_ && wizard_ == kWizardOff) {
            LightToolHud(x, y, width);
            return;
        }
        if (brush_ != terrain::Brush::None && wizard_ == kWizardOff && !drawTool_ && !gridTool_ && !treeTool_) {
            TerrainToolHud(x, y, width);
            return;
        }
        if (treeTool_ && wizard_ == kWizardOff) {
            TreeToolHud(x, y, width);
            return;
        }
        if (propTool_ && wizard_ == kWizardOff) {
            PropToolHud(x, y, width);
            return;
        }
        if (gridTool_ && wizard_ == kWizardOff) {
            GridToolHud(x, y, width);
            return;
        }
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
                    ImGui::Text("Click or drag over the faces in the new road's way.  %zu chosen", wizardFaces_.size());
                    ImGui::TextDisabled("A chosen face clicked again is let go. Kerbs beside them come along.");
                    break;
                case kWizardNode:
                    ImGui::TextUnformatted("Click the end node of the procedural road that joins here.");
                    break;
                case kWizardPreview:
                    ImGui::Text("Green: the new surface, orange: kerbs. %zu faces; click faces to change them.",
                                wizardFaces_.size());
                    ImGui::TextUnformatted("Drag the road end's gizmo to move it: the preview follows when let go.");
                    if (ImGui::Checkbox("Pedestrian crossings  (C)", &wizardCrosswalks_)) WizardPreview();
                    ImGui::TextDisabled("The game's road is taken kerb to kerb across the crossing; its lane "
                                        "markings go.");
                    ImGui::TextDisabled("The game's mesh is not changed until the roads are baked.");
                    break;
                default: break;
            }
            if (wizard_ == kWizardFaces || wizard_ == kWizardPreview) {
                ImGui::Spacing();
                ImGui::TextUnformatted("A click marks:");
                ImGui::SameLine();
                ImGui::RadioButton("as textured (1)", &wizardMark_, 0);
                ImGui::SameLine();
                ImGui::RadioButton("road (2)", &wizardMark_, 1);
                ImGui::SameLine();
                ImGui::RadioButton("sidewalk (3)", &wizardMark_, 2);
                ImGui::TextDisabled("Red: road, blue: sidewalk (brighter: marked by hand). Clicking a face with "
                                    "another mark re-marks it.");
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
        wizardFaces_.clear(); wizardKinds_.clear();
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
            if (const auto mark = wizardKinds_.find(t); mark != wizardKinds_.end()) j.faces.back().kind = mark->second;
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
        j.crosswalks = wizardCrosswalks_;

        // The crossing takes the game's road from kerb to kerb, its lane
        // markings going: its road faces between the corner kerbs (and the
        // stripe bands beyond) are added, from the chosen ones outwards over
        // the faces they touch.
        if (const roads::Node* node = network_.FindNode(wizardNode_)) {
            Vec3 into;
            const roads::Segment* segment = nullptr;
            if (network_.EndDirection(wizardNode_, into, segment)) {
                Vec3 gf = HorizontalDirection(j.gameForward);
                const Vec3 right{ into.y, -into.x, 0.0f };
                const float sine = std::fabs(Dot(gf, right));
                if (sine > 0.3f) {
                    const float reachHalf = (segment->profile.HalfRoad() + std::max(j.fillet, 1.0f)) / sine +
                                            roads::kCrosswalkDepth + 1.0f;
                    // Where the new road's middle line meets the game's road.
                    float centre = 0.0f, best = 1e30f;
                    for (const roads::GameFace& f : j.faces) {
                        if (!IsRoadTexture(f.texture)) continue;
                        const Vec3 c = (f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f);
                        const Vec3 d{ c.x - node->position.x, c.y - node->position.y, 0.0f };
                        if (std::fabs(Dot(d, right)) > segment->profile.HalfRoad()) continue;
                        if (Dot(d, into) > 0.0f && Dot(d, into) < best) { best = Dot(d, into); centre = Dot(d, gf); }
                    }
                    if (best < 1e29f) {
                        std::set<size_t> taken = chosen;
                        const auto key = [](const Vec3& p) {
                            return std::make_pair((long long)std::llround(p.x * 50.0), (long long)std::llround(p.y * 50.0));
                        };
                        std::set<std::pair<long long, long long>> reached;
                        for (size_t t : chosen)
                            if (IsRoadTexture(FaceOf(g, t).texture))
                                for (int k = 0; k < 3; ++k) reached.insert(key(g.world[indices[t * 3 + k]]));
                        size_t added = 0;
                        for (bool grew = true; grew;) {
                            grew = false;
                            for (size_t t = 0; t < count; ++t) {
                                if (taken.count(t)) continue;
                                const roads::GameFace f = FaceOf(g, t);
                                if (!IsRoadTexture(f.texture) || FacePlanArea(f) < 5e-4f) continue;
                                const Vec3 c = (f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f);
                                if (std::fabs(c.z - node->position.z) > 6.0f) continue;
                                const Vec3 d{ c.x - node->position.x, c.y - node->position.y, 0.0f };
                                if (std::fabs(Dot(d, gf) - centre) > reachHalf) continue;
                                bool touches = false;
                                for (const Vec3& p : f.p) touches |= reached.count(key(p)) != 0;
                                if (!touches) continue;
                                taken.insert(t);
                                for (const Vec3& p : f.p) reached.insert(key(p));
                                j.faces.push_back(f);
                                grew = true;
                                ++added;
                            }
                        }
                        (void)added;
                    }
                }
            }
        }

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
        const auto mark = wizardKinds_.find(triangle);
        const int kind = mark == wizardKinds_.end() ? 0 : mark->second;
        if (has == add && (!add || kind == wizardMark_)) return;
        if (add) {
            wizardFaces_.insert(triangle);
            if (wizardMark_) wizardKinds_[triangle] = wizardMark_;
            else wizardKinds_.erase(triangle);
        } else {
            wizardFaces_.erase(triangle);
            wizardKinds_.erase(triangle);
        }
        WizardPreview();
        WizardStatus();
    }
    // Road or sidewalk, as marked or by its texture.
    bool WizardIsRoad(size_t triangle) const {
        const auto mark = wizardKinds_.find(triangle);
        if (mark != wizardKinds_.end()) return mark->second == 1;
        if (wizardModel_ < 0 || wizardModel_ >= (int)groundModels_.size()) return false;
        return IsRoadTexture(TriangleTexture(groundModels_[(size_t)wizardModel_], triangle));
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
            {
                const auto mark = wizardKinds_.find(triangle);
                const int kind = mark == wizardKinds_.end() ? 0 : mark->second;
                wizardPaintAdd_ = wizardFaces_.count(triangle) == 0 || kind != wizardMark_;
            }
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
            else if (wizard_ == kWizardFaces) { wizard_ = kWizardModel; wizardFaces_.clear(); wizardKinds_.clear(); wizardModel_ = -1; }
            WizardStatus();
            return true;
        }
        if ((key == '1' || key == '2' || key == '3') && (wizard_ == kWizardFaces || wizard_ == kWizardPreview)) {
            wizardMark_ = (int)(key - '1');
            return true;
        }
        if (key == 'C' && wizard_ == kWizardPreview) {
            wizardCrosswalks_ = !wizardCrosswalks_;
            WizardPreview();
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
        // Road faces red, sidewalk faces blue; brighter where marked by hand.
        for (size_t t : wizardFaces_) {
            const bool marked = wizardKinds_.count(t) != 0;
            D3DCOLOR c = WizardIsRoad(t) ? (marked ? 0xFFFF5030 : 0xFFC04040) : (marked ? 0xFF40B0FF : 0xFF4070C0);
            if (wizard_ == kWizardPreview) c = (c & 0x00FFFFFF) | 0x80000000;
            triangle(t, c);
        }
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
            return !Bridged(segment) && !segment.profile.keepGround && segment.profile.groundAuto &&
                   !fitted_.count(Fingerprint(segment));
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
    float BaseGroundHeight(float x, float y) const { return BaseGroundHeight(x, y, brushPoint_.z); }
    float BaseGroundHeight(float x, float y, float nearZ) const {
        float best = 0.0f, bestGap = 1e30f;
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

    // ---- the ground fitted to chosen roads, now

    // The game's ground under and beside the selected roads fitted to them at
    // once, into the sculpted offsets (shown now, written at the next bake);
    // from then on those roads leave the ground alone.
    void BakeTerrainForRoads() {
        std::vector<u32> ids = SelectedRoadIds();
        if (ids.empty()) { app_.Status(L"Select a road first (Ctrl/Shift in the Explorer for several)"); return; }
        if (groundModels_.empty()) { app_.Status(L"No game ground here to fit"); return; }
        std::set<u32> chosen;
        for (u32 id : ids)
            if (const roads::Segment* s = network_.FindSegment(id); s && !Bridged(*s) && !s->profile.keepGround)
                chosen.insert(id);
        if (chosen.empty()) { app_.Status(L"Bridges and roads over a game road do not shape the ground"); return; }
        roads::RoadField field;
        field.Build(network_, [&](const roads::Segment& s) { return chosen.count(s.id) != 0; });
        Aabb area;
        float widest = 0.0f;
        for (u32 id : chosen) {
            const roads::Segment* s = network_.FindSegment(id);
            const roads::Curve curve = network_.CurveOf(*s);
            for (int i = 0; i <= 32; ++i) area.Add(curve.At(i / 32.0f));
            widest = std::max(widest, s->profile.HalfWidth());
        }
        area = Pad(area, widest + kGroundShoulder + 2.0f);
        const auto& cuts = cutsCache_;
        float lastRoad = area.Center().z;
        const Aabb changed = ground_.Fit(
            area,
            [&](float x, float y) {
                float roadZ = 0.0f, outside = 0.0f;
                if (field.Query(x, y, kGroundShoulder, roadZ, outside)) lastRoad = roadZ;
                return BaseGroundHeight(x, y, lastRoad);
            },
            [&](float x, float y, float h, float& wanted) {
                for (const roads::GameCut& cut : cuts)
                    if (cut.Inside({ x, y, h }, -kGroundShoulder)) return false;
                float roadZ = 0.0f, outside = 0.0f;
                if (!field.Query(x, y, kGroundShoulder, roadZ, outside) || std::fabs(h - roadZ) >= 12.0f) return false;
                const float bed = roadZ - kGroundSink;
                if (outside <= 0.0f) wanted = bed;
                else {
                    const float t = std::clamp(outside / kGroundShoulder, 0.0f, 1.0f);
                    wanted = bed + (h - bed) * (t * t * (3.0f - 2.0f * t));
                }
                return true;
            });
        // Fitted now: they no longer pull the ground about by themselves.
        for (u32 id : chosen) fitted_.insert(Fingerprint(*network_.FindSegment(id)));
        SaveFitted();
        SaveGround();
        groundFieldRevision_ = 0;
        BuildGroundField();
        RefreshGround(nullptr);
        Commit(L"terrain baked for roads");
        app_.Status(Fmt(L"Ground fitted to %zu road%s and frozen: moving %s no longer drags the ground (Ctrl+Z takes "
                        L"it back; the next bake writes it into the game)",
                        chosen.size(), chosen.size() == 1 ? L"" : L"s", chosen.size() == 1 ? L"it" : L"them"));
        (void)changed;
    }

    // ---- junctions into game roads: undo, edit, refine in Blender

    void UndoFaceJunction(u32 nodeId) {
        const auto& all = network_.FaceJunctions();
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].node == nodeId) {
                network_.RemoveFaceJunction(i);
                Changed(true);
                app_.Status(L"Junction into the game road taken out: its faces come back as the game has them at "
                            L"the next bake (Ctrl+Z brings it back)");
                return;
            }
    }

    // The wizard again on a junction made before: its model and faces chosen,
    // straight to the preview.
    void EditFaceJunction(u32 nodeId) {
        const roads::FaceJunction* fj = network_.FindFaceJunction(nodeId);
        if (!fj) return;
        StartFaceWizard();
        if (wizard_ == kWizardOff) return;
        for (size_t m = 0; m < groundModels_.size(); ++m) {
            if (!groundModels_[m].model || groundModels_[m].model->name != fj->model) continue;
            wizardModel_ = (int)m;
            const GroundModel& g = groundModels_[m];
            const auto& indices = g.model->indices;
            for (const roads::GameFace& f : fj->faces) {
                const Vec3 c = (f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f);
                for (size_t t = 0; t * 3 + 2 < indices.size(); ++t) {
                    const Vec3 d = (g.world[indices[t * 3]] + g.world[indices[t * 3 + 1]] + g.world[indices[t * 3 + 2]]) *
                                   (1.0f / 3.0f);
                    if (std::fabs(d.x - c.x) < 0.01f && std::fabs(d.y - c.y) < 0.01f && std::fabs(d.z - c.z) < 0.01f) {
                        wizardFaces_.insert(t);
                        if (f.kind) wizardKinds_[t] = f.kind;
                        break;
                    }
                }
            }
            break;
        }
        if (wizardModel_ < 0) {
            app_.Status(L"The junction's game model is not loaded here: choose its faces from the start");
            return;
        }
        wizardCrosswalks_ = fj->crosswalks;
        wizardNode_ = nodeId;
        wizard_ = kWizardPreview;
        Select((int)nodeId);
        WizardPreview();
        WizardStatus();
    }

    BlenderLink refineLink_;
    u32 refineNode_ = 0;
    Vec3 refineOrigin_{};

    static bool WriteTgaFile(const std::wstring& path, const std::vector<u32>& bgra, u32 width, u32 height) {
        if (bgra.size() < (size_t)width * height) return false;
        FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
        u8 header[18] = {};
        header[2] = 2;
        header[12] = (u8)(width & 0xFF);
        header[13] = (u8)(width >> 8);
        header[14] = (u8)(height & 0xFF);
        header[15] = (u8)(height >> 8);
        header[16] = 32;
        header[17] = 8;
        std::fwrite(header, sizeof(header), 1, file);
        for (u32 y = 0; y < height; ++y) std::fwrite(bgra.data() + (size_t)(height - 1 - y) * width, 4, width, file);
        std::fclose(file);
        return true;
    }

    // The junction's surface in Blender, with the game's road around it and
    // the new road's end for reference (only JUNCTION comes back).
    void RefineFaceJunction(u32 nodeId) {
        const roads::FaceJunction* fj = network_.FindFaceJunction(nodeId);
        const roads::Node* node = network_.FindNode(nodeId);
        if (!fj || !node || !app_.World()) return;
        const std::wstring blender = BlenderLink::FindBlender(app_.Window(), L"Software\\FRSWorldEditor");
        if (blender.empty()) return;
        const std::wstring label = Fmt(L"%s-junction-%u", app_.World()->Region().c_str(), nodeId);
        const std::wstring folder = BlenderLink::NewSessionFolder(label);
        if (folder.empty()) { app_.Message(L"No folder could be made for the edit.", MB_ICONWARNING); return; }
        const std::wstring objPath = folder + L"\\" + label + L".obj";
        const std::wstring mtlPath = folder + L"\\" + label + L".mtl";
        ::CreateDirectoryW((folder + L"\\textures").c_str(), nullptr);

        struct Tri { Vec3 p[3]; Vec2 uv[3]; Vec3 n[3]; std::string texture; };
        struct Object { std::string name; std::vector<Tri> tris; };
        std::vector<Object> objects;
        const float reach = 45.0f;
        const auto nearNode = [&](const Vec3& p) {
            return std::fabs(p.x - node->position.x) < reach && std::fabs(p.y - node->position.y) < reach;
        };
        // The junction, as it is now.
        {
            Object o;
            o.name = "JUNCTION_" + std::to_string(nodeId);
            roads::MeshPiece piece;
            network_.BuildFaceJunctionPiece(*fj, piece);
            for (const roads::MeshGroup& g : piece.groups)
                for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                    Tri t;
                    for (int i = 0; i < 3; ++i) {
                        const roads::MeshVertex& v = g.vertices[g.indices[k + i]];
                        t.p[i] = v.position;
                        t.uv[i] = v.uv;
                        t.n[i] = v.normal;
                    }
                    t.texture = g.texture;
                    o.tris.push_back(std::move(t));
                }
            objects.push_back(std::move(o));
        }
        // The game's road around it, without the faces the junction takes.
        for (const GroundModel& g : groundModels_) {
            if (!g.model || g.model->name != fj->model) continue;
            std::set<std::tuple<long, long, long>> taken;
            const auto key = [](const Vec3& c) {
                return std::make_tuple(std::lround(c.x * 100.0f), std::lround(c.y * 100.0f), std::lround(c.z * 100.0f));
            };
            for (const roads::GameFace& f : fj->faces) taken.insert(key((f.p[0] + f.p[1] + f.p[2]) * (1.0f / 3.0f)));
            Object o;
            o.name = "GAME_" + g.model->name + "_reference";
            const auto& indices = g.model->indices;
            for (size_t t = 0; t * 3 + 2 < indices.size(); ++t) {
                Tri tri;
                for (int i = 0; i < 3; ++i) {
                    const u32 v = indices[t * 3 + i];
                    tri.p[i] = g.world[v];
                    tri.uv[i] = g.model->vertices[v].uv;
                    tri.n[i] = Vec3{ 0, 0, 1 };
                }
                const Vec3 c = (tri.p[0] + tri.p[1] + tri.p[2]) * (1.0f / 3.0f);
                if (!nearNode(c) || taken.count(key(c))) continue;
                tri.texture = TriangleTexture(g, t);
                o.tris.push_back(std::move(tri));
            }
            objects.push_back(std::move(o));
            break;
        }
        // The new road's end.
        {
            Vec3 into;
            const roads::Segment* segment = nullptr;
            if (network_.EndDirection(nodeId, into, segment)) {
                Object o;
                o.name = "ROAD_" + std::to_string(segment->id) + "_reference";
                for (const roads::MeshPiece& piece : network_.BuildMesh()) {
                    if (piece.segment != segment->id) continue;
                    for (const roads::MeshGroup& g : piece.groups)
                        for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                            Tri t;
                            for (int i = 0; i < 3; ++i) {
                                const roads::MeshVertex& v = g.vertices[g.indices[k + i]];
                                t.p[i] = v.position;
                                t.uv[i] = v.uv;
                                t.n[i] = v.normal;
                            }
                            if (!nearNode((t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3.0f))) continue;
                            t.texture = g.texture;
                            o.tris.push_back(std::move(t));
                        }
                }
                objects.push_back(std::move(o));
            }
        }
        // Written about the node, Blender's way up.
        refineOrigin_ = { std::floor(node->position.x), std::floor(node->position.y), std::floor(node->position.z) };
        const auto toBlender = [](const Vec3& v) { return Vec3{ v.x, v.z, -v.y }; };
        FILE* obj = nullptr;
        if (::_wfopen_s(&obj, objPath.c_str(), L"wb") != 0 || !obj) {
            app_.Message(L"Could not write " + objPath, MB_ICONWARNING);
            return;
        }
        std::fprintf(obj, "# FRS World Editor: junction %u. Edit JUNCTION; the _reference objects are not read back.\n",
                     nodeId);
        std::fprintf(obj, "mtllib %s\n", Narrow(label + L".mtl").c_str());
        std::set<std::string> textures;
        size_t base = 1;
        for (const Object& o : objects) {
            if (o.tris.empty()) continue;
            std::fprintf(obj, "o %s\n", o.name.c_str());
            for (const Tri& t : o.tris)
                for (int i = 0; i < 3; ++i) {
                    const Vec3 p = toBlender(t.p[i] - refineOrigin_);
                    std::fprintf(obj, "v %.4f %.4f %.4f\n", p.x, p.y, p.z);
                }
            for (const Tri& t : o.tris)
                for (int i = 0; i < 3; ++i) std::fprintf(obj, "vt %.5f %.5f\n", t.uv[i].x, 1.0f - t.uv[i].y);
            for (const Tri& t : o.tris)
                for (int i = 0; i < 3; ++i) {
                    const Vec3 n = toBlender(t.n[i]);
                    std::fprintf(obj, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
                }
            std::string current;
            for (size_t k = 0; k < o.tris.size(); ++k) {
                if (o.tris[k].texture != current) {
                    current = o.tris[k].texture;
                    textures.insert(current);
                    std::fprintf(obj, "usemtl %s\n", current.c_str());
                }
                const size_t a = base + k * 3;
                std::fprintf(obj, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, a + 1, a + 1, a + 1, a + 2, a + 2,
                             a + 2);
            }
            base += o.tris.size() * 3;
        }
        std::fclose(obj);
        // Materials named after the game's textures, with their images.
        FILE* mtl = nullptr;
        if (::_wfopen_s(&mtl, mtlPath.c_str(), L"w") == 0 && mtl) {
            for (const std::string& name : textures) {
                std::fprintf(mtl, "newmtl %s\nKd 1.00 1.00 1.00\n", name.c_str());
                if (const nfs::Texture* texture = app_.World()->FindTexture(TextureNameHash(name))) {
                    std::vector<u32> pixels;
                    u32 width = 0, height = 0;
                    if (nfs::DecodeTexture(*texture, 0, pixels, width, height) &&
                        WriteTgaFile(folder + L"\\textures\\" + Widen(name) + L".tga", pixels, width, height))
                        std::fprintf(mtl, "map_Kd textures/%s.tga\n", name.c_str());
                }
                std::fprintf(mtl, "\n");
            }
            std::fclose(mtl);
        }
        std::wstring error;
        if (!refineLink_.Start(blender, objPath, error)) {
            app_.Message(error, MB_ICONWARNING);
            return;
        }
        refineNode_ = nodeId;
        app_.Status(L"Junction open in Blender: edit JUNCTION (weld it to the game's road and the new road's end), "
                    L"save there (Ctrl+S) and it comes back here");
    }

    // A save in Blender: JUNCTION read back as the junction's surface.
    void PollRefine() {
        if (!refineNode_ || !refineLink_.Active() || !refineLink_.PollSaved()) return;
        roads::FaceJunction* fj = network_.FindFaceJunction(refineNode_);
        if (!fj) { refineNode_ = 0; return; }
        std::vector<ObjMesh> meshes;
        std::wstring error;
        if (!ReadObjMeshes(refineLink_.ObjPath(), meshes, error)) {
            app_.Status(L"The junction could not be read back from Blender: " + error);
            return;
        }
        const ObjMesh* junction = nullptr;
        for (const ObjMesh& m : meshes)
            if (m.name.rfind("JUNCTION", 0) == 0) { junction = &m; break; }
        if (!junction) {
            app_.Status(L"No JUNCTION object in what Blender saved: keep its name");
            return;
        }
        const auto fromBlender = [](const Vec3& v) { return Vec3{ v.x, -v.z, v.y }; };
        std::vector<roads::RefinedTriangle> tris;
        for (const ObjFaceGroup& group : junction->groups) {
            std::string texture = group.material;
            if (const size_t dot = texture.find('.'); dot != std::string::npos) texture.resize(dot);   // Blender's .001
            for (u32 k = group.firstIndex; k + 2 < group.firstIndex + group.indexCount; k += 3) {
                roads::RefinedTriangle t;
                for (int i = 0; i < 3; ++i) {
                    const u16 v = junction->indices[k + i];
                    t.p[i] = fromBlender(junction->positions[v]) + refineOrigin_;
                    t.n[i] = v < junction->normals.size() ? fromBlender(junction->normals[v]) : Vec3{ 0, 0, 1 };
                    t.uv[i] = v < junction->uvs.size() ? Vec2{ junction->uvs[v].x, 1.0f - junction->uvs[v].y } : Vec2{ 0, 0 };
                }
                t.texture = texture;
                tris.push_back(std::move(t));
            }
        }
        if (tris.empty()) {
            app_.Status(L"JUNCTION came back empty from Blender: nothing changed");
            return;
        }
        fj->refined = std::move(tris);
        network_.SetDirty();
        Changed(true);
        app_.Status(Fmt(L"Junction refined in Blender: %zu triangles (Properties > Generate again drops it)",
                        fj->refined.size()));
    }

    // The road drawing tool beside the gizmo's buttons on the toolbar, pressed
    // while it is on.
    // The tools, in the toolbar right after the gizmo's move, rotate and
    // scale: they are tools of the same kind, and only one is on at a time.
    static constexpr struct { int id; const wchar_t* text; } kToolButtons[] = {
        { ID_ROAD_DRAW, L"Roads" }, { ID_ROAD_GRID, L"Grid" }, { ID_ROAD_TREES, L"Trees" },
        { ID_TERRAIN_TOOL, L"Terrain" },
    };

    void AddToolbarButton() {
        HWND toolbar = app_.Toolbar();
        if (!toolbar) return;
        // After Scale, the last of the gizmo's three.
        LRESULT at = ::SendMessageW(toolbar, TB_COMMANDTOINDEX, ID_GIZMO_SCALE, 0);
        at = at < 0 ? (LRESULT)::SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) : at + 1;
        for (const auto& tool : kToolButtons) {
            TBBUTTON button{};
            button.iBitmap = I_IMAGENONE;
            button.idCommand = tool.id;
            button.fsState = TBSTATE_ENABLED;
            button.fsStyle = (BYTE)(BTNS_CHECK | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
            button.iString = (INT_PTR)tool.text;
            ::SendMessageW(toolbar, TB_INSERTBUTTONW, (WPARAM)at++, (LPARAM)&button);
        }
        TBBUTTON separator{};
        separator.fsStyle = BTNS_SEP;
        ::SendMessageW(toolbar, TB_INSERTBUTTONW, (WPARAM)at, (LPARAM)&separator);
        ::SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
    }

    void SyncToolbar() const {
        HWND toolbar = app_.Toolbar();
        if (!toolbar) return;
        const bool on[] = { drawTool_, gridTool_, treeTool_, brush_ != terrain::Brush::None };
        for (size_t k = 0; k < std::size(kToolButtons); ++k)
            ::SendMessageW(toolbar, TB_CHECKBUTTON, kToolButtons[k].id, MAKELPARAM(on[k] ? 1 : 0, 0));
    }

    void AddMenu() {
        AddToolbarButton();
        HMENU menu = app_.Menu();
        if (!menu) return;
        // Tools: every tool that works in the view, one on at a time.
        HMENU toolsMenu = ::CreatePopupMenu();
        ::AppendMenuW(toolsMenu, MF_STRING, ID_GIZMO_MOVE, L"&Move\t1");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_GIZMO_ROTATE, L"&Rotate\t2");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_GIZMO_SCALE, L"&Scale\t3");
        ::AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(toolsMenu, MF_STRING, ID_ROAD_DRAW, L"&Roads (draw, curve, upgrade, bulldoze)\tCtrl+Shift+D");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_ROAD_GRID, L"Street &Grid with Zoned Blocks\tCtrl+Shift+U");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_ROAD_TREES, L"&Tree Brush\tCtrl+Shift+A");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_TERRAIN_TOOL, L"T&errain (sculpt, paint, extrude, cut)");
        ::AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(toolsMenu, MF_STRING, ID_ROAD_FACE_JUNCTION, L"&Junction into a Game Road...\tCtrl+Shift+J");
        ::AppendMenuW(toolsMenu, MF_STRING, ID_TERRAIN_OUTLINE, L"Draw a Terrain &Area...\tCtrl+Shift+T");

        HMENU roadsMenu = ::CreatePopupMenu();
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_NEW, L"&New Road at View Centre\tCtrl+R");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_EXTEND, L"&Pull a Road out of the Selected Node\tCtrl+Shift+R");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_SPLIT, L"&Split the Selected Road Where Clicked");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_DELETE, L"&Delete Selected Node or Road");
        ::AppendMenuW(roadsMenu, MF_SEPARATOR, 0, nullptr);
        HMENU lanesMenu = ::CreatePopupMenu();
        ::AppendMenuW(lanesMenu, MF_STRING, ID_ROAD_LANE_ADD, L"Add a &Lane after the Selected Node");
        ::AppendMenuW(lanesMenu, MF_STRING, ID_ROAD_LANE_DROP, L"Drop a La&ne after the Selected Node");
        ::AppendMenuW(lanesMenu, MF_STRING, ID_ROAD_LANE_SPLIT, L"Split the &Outer Lane off Here (an exit)");
        ::AppendMenuW(lanesMenu, MF_STRING, ID_ROAD_SPLIT_WAYS, L"Split the Two &Directions at This Road End");
        ::AppendMenuW(lanesMenu, MF_STRING, ID_ROAD_AUTO_HEADING, L"Node Direction &Automatic");
        ::AppendMenuW(roadsMenu, MF_POPUP, (UINT_PTR)lanesMenu, L"&Lanes and Nodes");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_FILL, L"&Fill Ground Between Roads That Close an Area");
        ::AppendMenuW(roadsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(roadsMenu, MF_STRING, ID_VIEW_ROADS, L"Show &Roads (procedural)");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_VIEW_ROAD_GRAPH, L"Show the Road &Graph (traffic and GPS)");
        ::AppendMenuW(roadsMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_BAKE, L"&Bake Roads, Buildings, Lights and Trees into the Game\tCtrl+Shift+B");
        ::AppendMenuW(roadsMenu, MF_STRING, ID_ROAD_UNBAKE, L"&Remove Baked Roads from the Game");
        HMENU terrainMenu = ::CreatePopupMenu();
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_OUTLINE, L"&Draw a Terrain Area...\tCtrl+Shift+T");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_TERRAIN_ALONG, L"Terrain &Along the Selected Road");
        ::AppendMenuW(terrainMenu, MF_STRING, ID_ROAD_GROUND_PATCH,
                      L"&Make the Ground Beside the Selected Roads Sculptable");
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
        // Tools after View and Camera; Roads and Terrain before Help.
        ::InsertMenuW(menu, help, MF_BYPOSITION | MF_POPUP, (UINT_PTR)roadsMenu, L"&Roads");
        ::InsertMenuW(menu, help + 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)terrainMenu, L"&Terrain");
        int toolsAt = 3;
        for (int i = 0; i < ::GetMenuItemCount(menu); ++i) {
            wchar_t text[64] = {};
            ::GetMenuStringW(menu, (UINT)i, text, 63, MF_BYPOSITION);
            if (std::wstring(text) == L"C&amera") { toolsAt = i + 1; break; }
        }
        ::InsertMenuW(menu, (UINT)toolsAt, MF_BYPOSITION | MF_POPUP, (UINT_PTR)toolsMenu, L"T&ools");
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
            { FVIRTKEY | FCONTROL | FSHIFT, 'U', (WORD)ID_ROAD_GRID },
            { FVIRTKEY | FCONTROL | FSHIFT, 'A', (WORD)ID_ROAD_TREES },
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
    // The street grid tool: its step (0 corner, 1 first side, 2 across), the
    // points clicked, the point under the cursor, and what it lays.
    bool  gridTool_ = false;
    // The light tool, the settings of the next light, and the game's models lit.
    bool  lightTool_ = false;
    std::string dragKey_;              // a Library row being dragged onto the map
    std::vector<u32> prefabNodes_;     // the nodes of the prefab being laid
    size_t prefabJoined_ = 0;
    Vec3  prefabDragAt_;
    u32   dragObject_ = 0;
    // The tree brush and the prop tool.
    bool  treeTool_ = false, propTool_ = false;
    bool  treePainting_ = false, treeStrokeChanged_ = false, treeBrushHit_ = false, treeErase_ = false;
    Vec3  treeBrushPoint_;
    float treeRadius_ = 15.0f, treeDensity_ = 6.0f, treeSpacing_ = 5.0f, treeClock_ = 0.0f;
    float treeScaleMin_ = 0.85f, treeScaleMax_ = 1.2f;
    std::vector<std::string> treeModels_, propModels_;
    std::set<std::string> treeChosen_, smackables_;
    std::string propModel_;
    float propHeading_ = 0.0f;
    bool  propRandomTurn_ = false;
    char  propFilter_[64] = {};
    u64   randomState_ = 0x9E3779B97F4A7C15ull;
    roads::Light lightDraft_;
    bool  lightShared_ = false;
    std::map<u32, std::vector<u32>> nativeOriginal_;   // shipped colours of the game's models lit
    std::set<u32> nativeLit_, nativeWritten_;
    u64   nativeRevision_ = 0;
    int   gridStep_ = 0;
    Vec3  gridA_, gridB_, gridHover_;
    bool  gridHoverValid_ = false;
    float gridBlockU_ = 90.0f, gridBlockV_ = 70.0f;
    int   gridLanes_ = 1;
    bool  gridSidewalks_ = true;
    bool  gridGround_ = true;
    int   gridZone_ = roads::kZoneMixed;
    u32   drawFrom_ = 0;               // the node the road being drawn goes on from
    bool  drawPending_ = false;        // a first click on open ground, no node yet
    Vec3  drawStart_;
    roads::RoadProfile drawProfile_;
    DrawHit drawHover_;
    int   drawMode_ = 0;                 // a DrawMode
    bool  drawSnapAngle_ = true, drawSnapLength_ = false;
    float drawElevation_ = 0.0f;         // above the ground, for what is drawn next
    int   drawZone_ = 0;
    Vec3  drawControl_;                  // curved: the point the curve bends towards
    bool  drawHasControl_ = false;
    Vec3  drawLast_;                     // the direction the last road arrived with
    bool  drawLastValid_ = false;
    u32   drawHoverRoad_ = 0;            // upgrade / bulldoze: the road under the cursor
    int   wizard_ = 0;                 // WizardStep
    int   wizardModel_ = -1;           // in groundModels_
    std::set<size_t> wizardFaces_;     // its triangles
    std::map<size_t, int> wizardKinds_;   // marked by hand: 1 road, 2 sidewalk
    int   wizardMark_ = 0;             // what a click marks: 0 as its texture says, 1 road, 2 sidewalk
    u32   wizardNode_ = 0;
    long long wizardHover_ = -1;
    bool  wizardPainting_ = false, wizardPaintAdd_ = true;
    bool  wizardRefresh_ = false;      // the node was moved: work the preview out again
    bool  wizardCrosswalks_ = false;   // crossing stripes (C in the preview)
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
    if (const roads::Prop* prop = SelectedProp()) {
        view.title = Fmt(prop->tree ? L"Tree %u" : L"Prop %u", prop->id);
        view.moveLabel = L"";
        view.removeLabel = prop->tree ? L"Delete tree" : L"Delete prop";
        view.actions.push_back({ L"Tree brush", ID_ROAD_TREES, L"Place", L"Paint trees (Ctrl+Shift+A)." });
        std::vector<std::wstring> models;
        for (const std::string& name : prop->tree ? treeModels_ : propModels_) models.push_back(Widen(name));
        AddRow(view, L"Model", L"Model", Widen(prop->model),
               smackables_.count(prop->model) ? L"One of the game's smackables: it keeps the game's physics."
                                              : L"The game's model it is.",
               kPropModel, models, true);
        AddRow(view, L"Placement", L"Heading", Number(prop->heading), L"Degrees.", kPropHeading);
        AddRow(view, L"Placement", L"Scale", Number(prop->scale), L"1 is the model's own size.", kPropScale);
        return true;
    }
    if (const roads::Light* light = SelectedLight()) {
        view.title = light->model.empty() ? Fmt(L"Light %u", light->id)
                                          : Fmt(L"%S (light %u)", light->model.c_str(), light->id);
        view.moveLabel = L"";
        view.removeLabel = L"Delete light";
        AddRow(view, L"Light", L"Colour (RRGGBB)", Fmt(L"%06X", light->color & 0xFFFFFF),
               L"The light's colour, as hexadecimal red, green, blue.", kLightColour);
        AddRow(view, L"Light", L"Reach", Number(light->radius), L"How far it lights, in metres.", kLightRadius);
        AddRow(view, L"Light", L"Strength", Number(light->strength), L"1 is a street light of the game.", kLightStrength);
        AddRow(view, L"Light", L"Lamp height", Number(light->height), L"The lamp above where it stands.", kLightHeight);
        AddRow(view, L"Light", L"Post", light->model.empty() ? L"-" : Widen(light->model),
               L"The game's model it stands as, or - for the light alone.", kLightModel, poleModels_, true);
        AddRow(view, L"Light", L"Heading", Number(light->heading), L"Degrees, for the post.", kLightHeading);
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
        if (const roads::FaceJunction* fj = network_.FindFaceJunction(node->id)) {
            view.actions.push_back({ L"Undo junction", ID_ROAD_FJ_UNDO, L"Junction into a game road",
                                     L"Takes the junction out: the game's faces come back as they were (Ctrl+Z "
                                     L"brings it back)." });
            view.actions.push_back({ L"Choose faces again", ID_ROAD_FJ_EDIT, L"Junction into a game road",
                                     L"The junction wizard on this junction, with its faces chosen." });
            view.actions.push_back({ L"Refine in Blender", ID_ROAD_FJ_BLENDER, L"Junction into a game road",
                                     L"Opens the junction's surface in Blender with the game's road around it and "
                                     L"the new road's end, to weld and tidy by hand; each save there comes back. "
                                     L"Once refined it no longer follows the node." });
            if (!fj->refined.empty())
                view.actions.push_back({ L"Generate again", ID_ROAD_FJ_GENERATED, L"Junction into a game road",
                                         L"Drops the Blender refinement: the surface is generated again." });
            AddRow(view, L"Junction", L"Surface", fj->refined.empty() ? L"generated" : Fmt(L"refined in Blender (%zu triangles)", fj->refined.size()),
                   L"Generated from the faces and the node, or as refined in Blender.", 0);
        }
        if (const roads::FaceJunction* fj = network_.FindFaceJunction(node->id))
            AddRow(view, L"Junction", L"Pedestrian crossings", fj->crosswalks ? L"yes" : L"no",
                   L"Crossing stripes (RDP_INTERSECT_AA_WH) across the game's road either side of the crossing and "
                   L"across the new road's mouth.", kNodeCrosswalks, { L"yes", L"no" });
        else if (const roads::GameJunction* gj = network_.FindGameJunction(node->id))
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
    FillRoad(view, *s);
    const std::vector<u32> roads = SelectedRoadIds();
    if (roads.size() > 1) {
        // Several: the first one's rows; where the others differ, "(mixed)"
        // until a value is given, which then goes to all of them.
        view.title = Fmt(L"%zu roads", roads.size());
        view.moveLabel = L"";
        view.removeLabel = L"";
        view.actions.clear();
        view.actions.push_back({ L"Bake terrain", ID_ROAD_GROUND_BAKE, L"Ground",
                                 L"Fits the game's ground to these roads now and freezes it." });
        for (size_t k = 1; k < roads.size(); ++k) {
            const roads::Segment* other = network_.FindSegment(roads[k]);
            if (!other) continue;
            PropertiesPanel::View theirs;
            FillRoad(theirs, *other);
            for (auto& row : view.rows) {
                if (!row.field) continue;
                for (const auto& their : theirs.rows)
                    if (their.field == row.field && their.value != row.value) row.value = L"(mixed)";
            }
        }
        for (auto& row : view.rows)
            if (row.name == L"Length") {
                float total = 0.0f;
                for (u32 id : roads)
                    if (const roads::Segment* r = network_.FindSegment(id)) total += SegmentLength(*r);
                row.value = Number(total) + L" in all";
            }
    }
    return true;
}

void RoadsExtension::FillRoad(PropertiesPanel::View& view, const roads::Segment& road) {
    const roads::Segment* s = &road;
    const roads::RoadProfile& p = s->profile;
    view.title = Fmt(L"Road %u", s->id);
    view.moveLabel = L"Pull a road out";
    view.removeLabel = L"Delete road";
    view.actions.push_back({ L"Node in the middle", ID_ROAD_INSERT_MIDDLE, L"Roads",
                             L"Splits this road in two at its middle, with a node there." });
    view.actions.push_back({ L"Draw roads", ID_ROAD_DRAW, L"Roads", L"The road drawing tool (Ctrl+Shift+D)." });
    const std::vector<std::wstring> yesNo = { L"yes", L"no" };
    AddRow(view, L"Road", L"Length", Number(SegmentLength(*s)), L"Along the curve.", 0);
    {
        const wchar_t* const shapes[3] = { L"auto", L"straight", L"drawn" };
        AddRow(view, L"Road", L"Curve", shapes[std::clamp(s->shape, 0, 2)],
               L"auto: smooth through its nodes and the roads beside it. straight: node to node. drawn: the curve "
               L"the road tool drew, leaving each node in a fixed direction.", kCurveShape,
               { shapes[0], shapes[1], shapes[2] });
    }
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
    AddRow(view, L"Lanes", L"Cat's eyes", p.catsEyes ? L"yes" : L"no",
           L"Road studs on the yellow line, wherever it is painted: down the middle, along each edge of a median, "
           L"or the left edge of a one-way road.", kCatsEyes, yesNo);
    if (p.catsEyes) {
        AddRow(view, L"Lanes", L"Cat's eye spacing", Number(p.catsEyeSpacing), L"From one stud to the next.",
               kCatsEyeSpacing);
        std::vector<std::wstring> studs;
        for (const auto& [name, model] : modelsByName_)
            if (name.find(L"CATSEYE") != std::wstring::npos || name.find(L"ROADSTUD") != std::wstring::npos ||
                name.find(L"REFLECTOR") != std::wstring::npos)
                studs.push_back(name);
        std::sort(studs.begin(), studs.end());
        AddRow(view, L"Lanes", L"Cat's eye model", Widen(p.catsEyeModel), L"The game's model each stud is.",
               kCatsEyeModel, studs, true);
    }
    view.actions.push_back({ L"Bake terrain", ID_ROAD_GROUND_BAKE, L"Ground",
                             L"Fits the game's ground to this road now and freezes it: after that the road no longer "
                             L"drags the ground about when it is moved. Written into the game at the next bake." });
    if (p.zoneLeft || p.zoneRight)
        view.actions.push_back({ L"New buildings", ID_ROAD_ZONE_RESEED, L"Zoning",
                                 L"Other buildings along this road (or every selected one): the next variation." });
    if (p.terrain[0].on || p.terrain[1].on)
        view.actions.push_back({ L"Make sculptable", ID_ROAD_GROUND_PATCH, L"Terrain beside",
                                 L"Turns the ground beside this road (or every selected one) into terrain you can "
                                 L"sculpt, paint and grow with the Terrain brushes -- 40 m more of it round the edge, "
                                 L"past the road's ends too." });
    view.actions.push_back({ L"Extrude left", ID_ROAD_EXTRUDE_LEFT, L"Terrain beside",
                             L"Another band of ground beyond the last one on the left (turns it on)." });
    view.actions.push_back({ L"Extrude right", ID_ROAD_EXTRUDE_RIGHT, L"Terrain beside",
                             L"Another band of ground beyond the last one on the right (turns it on)." });
    if (p.terrain[0].on && p.terrain[0].bands.size() > 1)
        view.actions.push_back({ L"Trim left", ID_ROAD_TRIM_LEFT, L"Terrain beside", L"Takes the outermost band off." });
    if (p.terrain[1].on && p.terrain[1].bands.size() > 1)
        view.actions.push_back({ L"Trim right", ID_ROAD_TRIM_RIGHT, L"Terrain beside", L"Takes the outermost band off." });
    view.actions.push_back({ L"Street grid", ID_ROAD_GRID, L"Zoning",
                             L"Lays a grid of zoned streets in three clicks (Ctrl+Shift+U)." });
    AddRow(view, L"Ground", L"Ground follows the road", p.groundAuto ? L"yes" : L"no",
           L"yes: the game's ground is fitted to this road by itself, shown at once and written at the bake. no: "
           L"the ground is left alone until Bake terrain is pressed.", kGroundAuto, yesNo);
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
        AddRow(view, L"Median", L"Smoothing (%)", Number(p.medianTaper),
               L"Towards a node where the road going on has no median, it narrows away (the lanes closing in) over "
               L"this share of the road's length. 0 ends it abruptly.", kMedianTaper);
    }
    {
        const wchar_t* const kinds[3] = { L"none", L"plain", L"striped" };
        AddRow(view, L"Shoulder", L"Shoulder", kinds[std::clamp(p.shoulderType, 0, 2)],
               L"A strip at road level beyond the outer lane on each side, inside the kerb: plain asphalt "
               L"(RDP_ROADSTRIP_DRIFT_MA) or striped (RDP_ROADSTRIP_MC, its hatching starting from nothing where it "
               L"narrows away). Where the road going on at a node has none, it narrows away over the taper length.",
               kShoulderType, { kinds[0], kinds[1], kinds[2] });
        if (p.shoulderType) {
            const wchar_t* const sides[3] = { L"both", L"left", L"right" };
            AddRow(view, L"Shoulder", L"Sides", sides[std::clamp(p.shoulderSides, 0, 2)],
                   L"Which sides have one, left and right in the way the road was drawn: an exit or a one-way road "
                   L"often has it on its outer side only. A side with a sidewalk never has one.",
                   kShoulderSides, { sides[0], sides[1], sides[2] });
            AddRow(view, L"Shoulder", L"Width", Number(p.shoulderWidth), L"Each side.", kShoulderWidth);
            AddRow(view, L"Shoulder", L"Edge line", p.shoulderLine ? L"yes" : L"no",
                   L"A solid white line between the shoulder and the lane.", kShoulderLine, yesNo);
            AddRow(view, L"Shoulder", L"Texture", Widen(p.ShoulderTexture()),
                   L"A road strip texture, laid across the shoulder's width.", kShoulderTexture,
                   lanes_, true);
        }
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
    AddRow(view, L"Walls", L"Start left (+/-)", Number(p.wallStartLeft),
           L"Moves where the left wall starts at the road's first end (where it was drawn from): where the road "
           L"splits off another there, the wall starts once they have parted -- more is later, negative earlier. "
           L"Elsewhere it leaves the wall out that far.", kWallStartLeft);
    AddRow(view, L"Walls", L"Start right (+/-)", Number(p.wallStartRight), L"The same for the right wall.",
           kWallStartRight);
    AddRow(view, L"Walls", L"End left (+/-)", Number(p.wallEndLeft),
           L"The same at the road's last end: how much earlier the left wall stops (negative: later).", kWallEndLeft);
    AddRow(view, L"Walls", L"End right (+/-)", Number(p.wallEndRight), L"The same for the right wall.",
           kWallEndRight);
    if (p.wallStyle == 0)
        AddRow(view, L"Walls", L"Thickness", Number(p.wallThickness),
               L"How deep the wall is, outwards from the road's edge: two faces and a top. 0 keeps it a single "
               L"sheet.", kWallThickness);
    AddRow(view, L"Walls", L"Texture", Widen(p.wallTexture), L"The wall.", kWallTexture, walls_, true);
    AddRow(view, L"Walls", L"Texture length", Number(p.wallTextureLength),
           L"How much wall one repeat of its texture covers.", kWallTextureLength);
    {
        for (int k = 0; k < 2; ++k) {
            const roads::RoadProfile::SideTerrain& t = p.terrain[k];
            const wchar_t* group = k ? L"Terrain right" : L"Terrain left";
            const int f = kSideTerrain + k * 40;
            AddRow(view, group, L"Terrain", t.on ? L"yes" : L"no",
                   k ? L"Ground beside the road on the right (in the way it was drawn), beyond its edge and wall."
                     : L"Ground beside the road on the left (in the way it was drawn), beyond its edge and wall.",
                   f + kSideOn, yesNo);
            if (!t.on) continue;
            for (size_t b = 0; b < t.bands.size() && b < (size_t)kMostBands; ++b) {
                const std::wstring n = std::to_wstring(b + 1);
                AddRow(view, group, (L"Band " + n + L" width").c_str(), Number(t.bands[b].width),
                       L"How wide this band is. Bands follow one another outwards: Extrude adds one beyond the last.",
                       f + kBandWidth + 2 * (int)b);
                AddRow(view, group, (L"Band " + n + L" rise (+up/-down)").c_str(), Number(t.bands[b].rise),
                       L"How far the ground climbs across this band: positive a hill, negative falling away, 0 flat.",
                       f + kBandRise + 2 * (int)b);
            }
            AddRow(view, group, L"Relief", Number(t.relief),
                   L"Random bumps, up to this high, growing away from the road (0: smooth).", f + kSideRelief);
            AddRow(view, group, L"Relief variation", std::to_wstring(t.seed),
                   L"Another number gives other bumps. Roads with the same one meet seamlessly.", f + kSideSeed);
            AddRow(view, group, L"Meet the ground", t.meetGround ? L"yes" : L"no",
                   L"The outer edge comes down or up to the ground there -- the game's, or terrain made here -- so "
                   L"they join without a gap.", f + kSideMeet, yesNo);
            AddRow(view, group, L"Rock on slopes", t.rock ? L"yes" : L"no",
                   L"Steep, high ground turns to the region's rock, through its grass-and-rock texture, the edge "
                   L"broken up at random.", f + kSideRock, yesNo);
            if (t.rock) {
                AddRow(view, group, L"Rock from (degrees)", Number(t.rockAngle),
                       L"About how steep the ground is where rock begins: less shows more rock.", f + kSideRockAngle);
                AddRow(view, group, L"Rock above (m)", Number(t.rockHeight),
                       L"How high above the road the rock takes over fully; lower down, steep ground stays half "
                       L"grass.", f + kSideRockHeight);
            }
            AddRow(view, group, L"Texture", t.texture.empty() ? L"(region grass)" : Widen(t.texture),
                   L"The ground's texture; (region grass) is the grass the region's own ground uses most.",
                   f + kSideTexture, groundTextures_, true);
        }
    }
    {
        std::vector<std::wstring> kinds;
        for (int k = 0; k < roads::kZoneKinds; ++k) kinds.push_back(roads::ZoneName(k));
        AddRow(view, L"Zoning", L"Left side", roads::ZoneName(p.zoneLeft),
               L"The game's buildings stood along the left side (in the way the road was drawn), facing the road, "
               L"as a city builder zones a street: houses, shops, apartments, towers, industry or a mix.",
               kZoneLeft, kinds);
        AddRow(view, L"Zoning", L"Right side", roads::ZoneName(p.zoneRight), L"The same on the right side.",
               kZoneRight, kinds);
        if (p.zoneLeft || p.zoneRight) {
            AddRow(view, L"Zoning", L"Lot depth", Number(p.zoneDepth),
                   L"How deep a building may be, back from the sidewalk: deeper lets bigger ones in.", kZoneDepth);
            AddRow(view, L"Zoning", L"Setback", Number(p.zoneSetback),
                   L"Space between the road's outer edge (the sidewalk's, when it has one) and the buildings.",
                   kZoneSetback);
            AddRow(view, L"Zoning", L"Gap", Number(p.zoneGap), L"Space between one building and the next.", kZoneGap);
            AddRow(view, L"Zoning", L"Variation", std::to_wstring(p.zoneSeed),
                   L"Which buildings are drawn: another number gives the street other ones (the New buildings "
                   L"button counts it up).", kZoneSeed);
            const wchar_t* const turns[4] = { L"0", L"90", L"180", L"270" };
            AddRow(view, L"Zoning", L"Facade turn", turns[(p.zoneTurn / 90) & 3],
                   L"Degrees every building is turned, for when the game's models face another way than the road.",
                   kZoneTurn, { turns[0], turns[1], turns[2], turns[3] });
        }
    }
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
}

bool RoadsExtension::ApplyProperties() {
    if (Selected() < 0) return false;
    PropertiesPanel& properties = app_.Properties();
    if (roads::Prop* prop = SelectedPropMutable()) {
        std::wstring text;
        float value = 0.0f;
        if (properties.ReadText(kPropModel, text) && !text.empty()) prop->model = Narrow(text);
        if (properties.ReadNumber(kPropHeading, value)) prop->heading = value;
        if (properties.ReadNumber(kPropScale, value)) prop->scale = std::clamp(value, 0.1f, 10.0f);
        Changed(true);
        return true;
    }
    if (roads::Light* light = SelectedLightMutable()) {
        std::wstring text;
        float value = 0.0f;
        if (properties.ReadText(kLightColour, text) && !text.empty()) {
            const std::wstring hex = text[0] == L'#' ? text.substr(1) : text;
            light->color = (u32)std::wcstoul(hex.c_str(), nullptr, 16) & 0xFFFFFF;
        }
        if (properties.ReadNumber(kLightRadius, value)) light->radius = std::clamp(value, 2.0f, 150.0f);
        if (properties.ReadNumber(kLightStrength, value)) light->strength = std::clamp(value, 0.0f, 5.0f);
        if (properties.ReadNumber(kLightHeight, value)) light->height = std::clamp(value, 0.0f, 60.0f);
        if (properties.ReadText(kLightModel, text)) light->model = text == L"-" ? std::string() : Narrow(text);
        if (properties.ReadNumber(kLightHeading, value)) light->heading = value;
        Changed(true);
        return true;
    }
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
            if (roads::FaceJunction* fj = network_.FindFaceJunction(node->id)) fj->crosswalks = on;
            else if (roads::GameJunction* gj = network_.FindGameJunction(node->id)) gj->crosswalks = on;
            else node->crosswalks = on;
        }
        Changed(true);
        return true;
    }

    // A road, or several: with several, only the value just changed goes to
    // them all, each keeping the rest of its own.
    const std::vector<u32> roads = SelectedRoadIds();
    if (roads.empty()) return true;
    const int only = roads.size() > 1 ? properties.LastField() : -1;
    for (u32 id : roads)
        if (roads::Segment* s = network_.FindSegment(id)) {
            std::wstring shape;
            if ((only < 0 || only == kCurveShape) && properties.ReadText(kCurveShape, shape)) {
                if (shape == L"auto" && s->shape != 0) { s->shape = 0; s->handleA = s->handleB = 0.0f; }
                else if (shape == L"straight" && s->shape != 1) { s->shape = 1; s->handleA = s->handleB = 0.0f; }
            }
            ApplyRoad(s->profile, only);
            if (roads.size() == 1) lastProfile_ = s->profile;
        }
    Changed(true);
    return true;
}

void RoadsExtension::ApplyRoad(roads::RoadProfile& p, int only) {
    PropertiesPanel& properties = app_.Properties();
    std::wstring text;
    const auto readText = [&](int field, std::wstring& into) {
        return (only < 0 || field == only) && properties.ReadText(field, into);
    };
    const auto readNumber = [&](int field, float& into) {
        return (only < 0 || field == only) && properties.ReadNumber(field, into);
    };
    float value = 0.0f;
    if (readNumber(kLanesForward, value)) p.lanesForward = std::clamp((int)std::lround(value), 0, 8);
    if (readNumber(kLanesBackward, value)) p.lanesBackward = std::clamp((int)std::lround(value), 0, 8);
    if (p.Lanes() == 0) p.lanesForward = 1;
    if (readNumber(kLaneWidth, value)) p.laneWidth = std::clamp(value, 1.0f, 20.0f);
    if (readNumber(kTextureLength, value)) p.textureLength = std::clamp(value, 0.5f, 500.0f);
    if (readText(kCatsEyes, text)) p.catsEyes = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readNumber(kCatsEyeSpacing, value)) p.catsEyeSpacing = std::clamp(value, 1.0f, 200.0f);
    if (readText(kCatsEyeModel, text) && !text.empty()) p.catsEyeModel = Narrow(text);
    if (readNumber(kSidewalkWidth, value)) p.sidewalkWidth = std::clamp(value, 0.0f, 20.0f);
    if (readNumber(kSidewalkHeight, value)) p.sidewalkHeight = std::clamp(value, 0.0f, 2.0f);
    if (readNumber(kWallHeight, value)) p.wallHeight = std::clamp(value, 0.0f, 30.0f);
    if (readNumber(kWallStartLeft, value)) p.wallStartLeft = std::clamp(value, -500.0f, 500.0f);
    if (readNumber(kWallStartRight, value)) p.wallStartRight = std::clamp(value, -500.0f, 500.0f);
    if (readNumber(kWallEndLeft, value)) p.wallEndLeft = std::clamp(value, -500.0f, 500.0f);
    if (readNumber(kWallEndRight, value)) p.wallEndRight = std::clamp(value, -500.0f, 500.0f);
    if (readNumber(kWallThickness, value)) p.wallThickness = std::clamp(value, 0.0f, 5.0f);
    for (int side = 0; side < 2; ++side)
        if (readText(side ? kZoneRight : kZoneLeft, text))
            for (int k = 0; k < roads::kZoneKinds; ++k)
                if (text == roads::ZoneName(k)) (side ? p.zoneRight : p.zoneLeft) = k;
    if (readNumber(kZoneDepth, value)) p.zoneDepth = std::clamp(value, 5.0f, 300.0f);
    for (int k = 0; k < 2; ++k) {
        roads::RoadProfile::SideTerrain& t = p.terrain[k];
        const int f = kSideTerrain + k * 40;
        if (readText(f + kSideOn, text)) t.on = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
        for (size_t b = 0; b < t.bands.size() && b < (size_t)kMostBands; ++b) {
            if (readNumber(f + kBandWidth + 2 * (int)b, value)) t.bands[b].width = std::clamp(value, 0.5f, 300.0f);
            if (readNumber(f + kBandRise + 2 * (int)b, value)) t.bands[b].rise = std::clamp(value, -200.0f, 200.0f);
        }
        if (readNumber(f + kSideRelief, value)) t.relief = std::clamp(value, 0.0f, 50.0f);
        if (readNumber(f + kSideSeed, value)) t.seed = (int)std::lround(value);
        if (readText(f + kSideMeet, text)) t.meetGround = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
        if (readText(f + kSideRock, text)) t.rock = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
        if (readNumber(f + kSideRockAngle, value)) t.rockAngle = std::clamp(value, 1.0f, 89.0f);
        if (readNumber(f + kSideRockHeight, value)) t.rockHeight = std::clamp(value, 0.0f, 200.0f);
        if (readText(f + kSideTexture, text) && !text.empty())
            t.texture = text[0] == L'(' ? std::string() : Narrow(text);
    }
    if (readNumber(kZoneSetback, value)) p.zoneSetback = std::clamp(value, -10.0f, 100.0f);
    if (readNumber(kZoneGap, value)) p.zoneGap = std::clamp(value, 0.0f, 100.0f);
    if (readNumber(kZoneSeed, value)) p.zoneSeed = (int)std::lround(value);
    if (readNumber(kZoneTurn, value)) p.zoneTurn = ((((int)std::lround(value) + 45) / 90) * 90) % 360;
    if (readText(kShoulderType, text)) {
        const int kind = text == L"plain" ? 1 : text == L"striped" ? 2 : text == L"none" ? 0 : p.shoulderType;
        if (kind != p.shoulderType) p.shoulderTexture.clear();   // the kind's own
        p.shoulderType = kind;
    }
    if (readNumber(kShoulderWidth, value)) p.shoulderWidth = std::clamp(value, 0.2f, 10.0f);
    if (readText(kShoulderSides, text))
        p.shoulderSides = text == L"left" ? 1 : text == L"right" ? 2 : text == L"both" ? 0 : p.shoulderSides;
    if (readText(kShoulderLine, text)) p.shoulderLine = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kShoulderTexture, text)) {
        const std::string name = Narrow(text);
        if (!name.empty() && name != p.ShoulderTexture()) p.shoulderTexture = name;
    }
    if (readNumber(kWallTextureLength, value)) p.wallTextureLength = std::clamp(value, 0.5f, 500.0f);
    if (readText(kBridge, text)) {
        const bool bridge = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
        if (bridge && !p.bridge) p.wallLeft = p.wallRight = true;    // railings
        p.bridge = bridge;
    }
    if (readText(kBankMode, text))
        p.bankMode = text == L"auto" ? 1 : text == L"fixed" ? 2 : 0;
    if (readNumber(kDesignSpeed, value)) p.designSpeed = std::clamp(value, 10.0f, 400.0f);
    if (readNumber(kBankMax, value)) p.bankMax = std::clamp(value, 0.0f, 45.0f);
    if (readNumber(kBankAngle, value)) p.bankAngle = std::clamp(value, -45.0f, 45.0f);
    if (readNumber(kDeckThickness, value)) p.deckThickness = std::clamp(value, 0.0f, 20.0f);
    if (readNumber(kPillarSpacing, value)) p.pillarSpacing = std::clamp(value, 0.0f, 500.0f);
    if (readNumber(kPillarWidth, value)) p.pillarWidth = std::clamp(value, 0.3f, 20.0f);
    if (readNumber(kTaperLength, value)) p.taperLength = std::clamp(value, 5.0f, 500.0f);
    if (readText(kPillarModel, text)) {
        const std::string name = Narrow(text);
        p.pillarModel = name == "-" ? std::string() : name;
    }
    if (readNumber(kPillarModelTurn, value)) p.pillarModelTurn = value;
    if (readNumber(kPillarCount, value)) p.pillarCount = std::clamp((int)std::lround(value), 0, 16);
    if (readNumber(kPoleSpacing, value)) p.poleSpacing = std::clamp(value, 2.0f, 1000.0f);
    if (readNumber(kPoleInset, value)) p.poleInset = std::clamp(value, -20.0f, 20.0f);
    if (readNumber(kPoleTurn, value)) p.poleTurn = value;
    if (readNumber(kLampRadius, value)) p.lampRadius = std::clamp(value, 1.0f, 200.0f);
    if (readNumber(kLampStrength, value)) p.lampStrength = std::clamp(value, 0.0f, 4.0f);
    if (readText(kAmbient, text)) p.ambient = ParseColour(text, p.ambient);
    if (readText(kLampColor, text)) p.lampColor = ParseColour(text, p.lampColor);
    if (readText(kPoleSides, text))
        for (int k = 0; k < 5; ++k)
            if (text == kSideNames[k]) p.poleSides = k;
    if (readText(kPoleModel, text)) {
        const std::string name = Narrow(text);
        p.poleModel = name == "-" ? std::string() : name;
    }
    const auto readTexture = [&](int field, std::string& into) {
        if (!readText(field, text)) return;
        const std::string name = Narrow(text);
        if (!name.empty()) into = name;
    };
    readTexture(kLaneCentre, p.laneCentre);
    readTexture(kLaneInner, p.laneInner);
    readTexture(kSidewalkTexture, p.sidewalkTexture);
    readTexture(kWallTexture, p.wallTexture);
    readTexture(kDeckTexture, p.deckTexture);
    readTexture(kPillarTexture, p.pillarTexture);
    if (readText(kMedianType, text))
        for (int k = 0; k < 5; ++k)
            if (text == kMedianNames[k]) {
                if (k != p.medianType) p.medianTexture.clear();   // the kind's own texture
                p.medianType = k;
            }
    if (readNumber(kMedianWidth, value)) p.medianWidth = std::clamp(value, 0.3f, 60.0f);
    if (readNumber(kMedianHeight, value)) p.medianHeight = std::clamp(value, 0.0f, 3.0f);
    if (readNumber(kMedianTaper, value)) p.medianTaper = std::clamp(value, 0.0f, 100.0f);
    if (readText(kMedianTexture, text)) {
        const std::string name = Narrow(text);
        if (!name.empty() && name != p.MedianTexture()) p.medianTexture = name;
    }
    if (readText(kTraffic, text)) p.traffic = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kGps, text)) p.gps = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kGroundAuto, text)) p.groundAuto = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readNumber(kSpeedLimit, value)) p.speedLimit = std::clamp(value, 10.0f, 300.0f);
    if (readText(kWalkLeft, text)) p.sidewalkLeft = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kWalkRight, text)) p.sidewalkRight = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kWallStyle, text))
        for (int k = 0; k < 3; ++k)
            if (text == kWallStyleNames[k]) p.wallStyle = k;
    if (readText(kWallModel, text)) {
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
    if (readNumber(kWallModelSpacing, value)) p.wallModelSpacing = std::clamp(value, 0.5f, 100.0f);
    if (readNumber(kWallModelTurn, value)) p.wallModelTurn = value;
    if (readText(kWallLeft, text)) p.wallLeft = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
    if (readText(kWallRight, text)) p.wallRight = !text.empty() && (text[0] == L'y' || text[0] == L'Y');
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
    if (!visible_ || (network_.Nodes().empty() && network_.Lights().empty() && brush_ == terrain::Brush::None &&
                      network_.Props().empty() && !drawing_ && !drawTool_ && !gridTool_ && !lightTool_ &&
                      !treeTool_ && !propTool_ && !SelectedPatch()))
        return;
    struct Vertex { float x, y, z; D3DCOLOR colour; };
    std::vector<Vertex> solid, lines;
    const int selected = Selected();
    const std::vector<u32> selectedRoads = SelectedRoadIds();

    const auto line = [&](const Vec3& a, const Vec3& b, D3DCOLOR c) {
        lines.push_back({ a.x, a.y, a.z, c });
        lines.push_back({ b.x, b.y, b.z, c });
    };

    // Each road's centre line, a little above the surface.
    for (const roads::Segment& s : network_.Segments()) {
        const bool chosen = std::find(selectedRoads.begin(), selectedRoads.end(), s.id) != selectedRoads.end();
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
        const bool started = DrawStart(from);
        PlannedRoad plan;
        if (DrawPreview(plan)) {
            const float half = drawProfile_.HalfWidth();
            Vec3 previous = plan.curve.At(0.0f);
            for (int i = 1; i <= 40; ++i) {
                const float t = i / 40.0f;
                const Vec3 p = plan.curve.At(t);
                const Vec3 d0 = HorizontalDirection(plan.curve.Tangent((i - 1) / 40.0f));
                const Vec3 d1 = HorizontalDirection(plan.curve.Tangent(t));
                const Vec3 s0{ d0.y, -d0.x, 0.0f }, s1{ d1.y, -d1.x, 0.0f };
                const Vec3 lift{ 0, 0, 0.4f };
                line(previous + lift, p + lift, 0xFFFFD040);
                line(previous + s0 * half + lift, p + s1 * half + lift, 0xA0FFD040);
                line(previous - s0 * half + lift, p - s1 * half + lift, 0xA0FFD040);
                previous = p;
            }
        } else if (started && drawMode_ == kDrawCurved && !drawHasControl_) {
            line(from + Vec3{ 0, 0, 0.4f }, at, 0x80FFD040);      // towards the bend point
        }
        if (started && drawMode_ == kDrawCurved && drawHasControl_) {
            const Vec3 c = drawControl_ + Vec3{ 0, 0, 0.4f };
            line(from + Vec3{ 0, 0, 0.4f }, c, 0x80FFFFFF);
            line(c, at, 0x80FFFFFF);
            line(c, c + Vec3{ 0, 0, 3.0f }, 0xFFFFFFFF);
        }
        if (drawElevation_ != 0.0f)
            line(at, at - Vec3{ 0, 0, drawElevation_ }, 0xFF80C0FF);   // down to the ground
    }
    // Upgrade / bulldoze: the road that a click would change.
    if (drawTool_ && drawMode_ >= kDrawUpgrade)
        if (const roads::Segment* s = network_.FindSegment(drawHoverRoad_)) {
            const roads::Curve curve = network_.CurveOf(*s);
            const D3DCOLOR c = drawMode_ == kDrawBulldoze ? 0xFFFF4040 : 0xFF40E0FF;
            const float half = s->profile.HalfWidth();
            for (int i = 0; i < 40; ++i) {
                const Vec3 p0 = curve.At(i / 40.0f), p1 = curve.At((i + 1) / 40.0f);
                const Vec3 d = HorizontalDirection(curve.Tangent(i / 40.0f));
                const Vec3 side{ d.y * half, -d.x * half, 0.6f };
                const Vec3 other{ -d.y * half, d.x * half, 0.6f };
                line(p0 + side, p1 + side, c);
                line(p0 + other, p1 + other, c);
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

    DrawGridPreview(line);

    // The selected terrain patch: its outline, along the edges of its cells.
    if (const terrain::Patch* patch = SelectedPatch()) {
        const auto filled = [&](int i, int j) {
            return i >= 0 && j >= 0 && i < patch->columns && j < patch->rows &&
                   patch->paint[(size_t)j * patch->columns + i] != terrain::kEmpty;
        };
        const Vec3 lift{ 0.0f, 0.0f, 0.35f };
        const D3DCOLOR colour = 0xFF40E0FF;
        for (int j = 0; j <= patch->rows; ++j)
            for (int i = 0; i <= patch->columns; ++i) {
                // The edge below corner (i, j) along x, and the one along y.
                if (i < patch->columns && filled(i, j) != filled(i, j - 1))
                    line(patch->Corner(i, j) + lift, patch->Corner(i + 1, j) + lift, colour);
                if (j < patch->rows && filled(i, j) != filled(i - 1, j))
                    line(patch->Corner(i, j) + lift, patch->Corner(i, j + 1) + lift, colour);
            }
    }

    // The tree brush where it would paint.
    if (treeTool_ && treeBrushHit_) {
        const Vec3 c = treeBrushPoint_ + Vec3{ 0, 0, 0.4f };
        const D3DCOLOR colour = (treeErase_ || ::GetKeyState(VK_CONTROL) < 0) ? 0xFFFF6060 : 0xFF60FF60;
        for (int k = 0; k < 48; ++k) {
            const float a0 = k * 6.2831853f / 48, a1 = (k + 1) * 6.2831853f / 48;
            line(c + Vec3{ treeRadius_ * std::cos(a0), treeRadius_ * std::sin(a0), 0 },
                 c + Vec3{ treeRadius_ * std::cos(a1), treeRadius_ * std::sin(a1), 0 }, colour);
        }
    }
    if (const roads::Prop* prop = SelectedProp()) {
        const Vec3 p = prop->position;
        const float r = 1.0f * prop->scale;
        for (int k = 0; k < 4; ++k) {
            const float a0 = k * 1.5707963f, a1 = (k + 1) * 1.5707963f;
            line(p + Vec3{ r * std::cos(a0), r * std::sin(a0), 0.2f }, p + Vec3{ r * std::cos(a1), r * std::sin(a1), 0.2f },
                 0xFFFFFFFF);
        }
        line(p, p + Vec3{ 0, 0, 3.0f * prop->scale }, 0xFFFFFFFF);
    }

    // Lights: the post, the lamp in its colour, and its reach on the ground.
    for (const roads::Light& light : network_.Lights()) {
        const bool chosen = (int)light.id == selected;
        const D3DCOLOR c = 0xFF000000u | (light.color & 0xFFFFFF);
        const Vec3 lamp = light.Lamp();
        line(light.position, lamp, chosen ? 0xFFFFFFFF : 0xFF808080);
        const float s = chosen ? 1.2f : 0.8f;
        const Vec3 tips[6] = { lamp + Vec3{ s, 0, 0 }, lamp + Vec3{ 0, s, 0 }, lamp + Vec3{ -s, 0, 0 },
                               lamp + Vec3{ 0, -s, 0 }, lamp + Vec3{ 0, 0, s }, lamp + Vec3{ 0, 0, -s } };
        for (int k = 0; k < 4; ++k)
            for (int tip = 4; tip < 6; ++tip) {
                const Vec3& a = tips[k];
                const Vec3& b = tips[(k + 1) % 4];
                const Vec3& t = tips[tip];
                solid.push_back({ a.x, a.y, a.z, c });
                solid.push_back({ b.x, b.y, b.z, c });
                solid.push_back({ t.x, t.y, t.z, c });
            }
        if (chosen || lightTool_) {
            const Vec3 g = light.position + Vec3{ 0, 0, 0.3f };
            for (int k = 0; k < 32; ++k) {
                const float a0 = k * 6.2831853f / 32, a1 = (k + 1) * 6.2831853f / 32;
                line(g + Vec3{ light.radius * std::cos(a0), light.radius * std::sin(a0), 0 },
                     g + Vec3{ light.radius * std::cos(a1), light.radius * std::sin(a1), 0 }, (c & 0x00FFFFFF) | 0xA0000000);
            }
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
    if (demoActive_) {
        app_.Message(L"A demo is shown (FRS_CITY_DEMO): it is never baked into the game. Restart the editor "
                     L"without it to bake.");
        return;
    }
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
            for (roads::MeshPiece& piece : terrain_.BuildMesh(patch, &field, poles, network_.Lighting())) pieces.push_back(std::move(piece));
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
                if (fitGround_ && !Bridged(segment) && !segment.profile.keepGround && segment.profile.groundAuto)
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
    // The game's own objects the lights placed by hand reach.
    WriteNativeLights();
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

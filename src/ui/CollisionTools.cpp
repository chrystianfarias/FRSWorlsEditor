#include "ui/CollisionTools.h"

#include "core/Log.h"
#include "io/CollisionGenerate.h"
#include "io/PlacementWrite.h"
#include "io/SectionSplice.h"
#include "nfs/ChunkReader.h"
#include "nfs/FreeRoam.h"
#include "nfs/World.h"
#include "render/CollisionMesh.h"
#include "ui/Resource.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

// Commands of the Collision menu. The range is the tools' own: below the
// Travel To menu (46000) and the extensions (47000).
enum : int {
    kIdBarrier       = 44001,
    kIdWall          = 44002,
    kIdGround        = 44003,
    kIdUndo          = 44004,
    kIdDelete        = 44005,
    kIdAlwaysPresent = 44006,
    kIdCopies        = 44007,
    kIdRaceGroups    = 44008,
    kIdMove          = 44009,
    kIdGenerate      = 44010,
    kIdHeightFirst   = 44020,   // + preset index
    kIdSurfaceSelect = 44100,   // + surface index
    kIdSurfaceGround = 44200,
    kIdSurfaceWall   = 44300,
};

constexpr float kHeights[] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f, 12.0f, 20.0f };
constexpr int kSurfaceField = 101;
constexpr int kRaceField    = 102;

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::wstring SurfaceLabel(u8 surface) { return Widen(nfs::SurfaceName(surface)); }

D3DCOLOR Argb(u32 rgb, u32 alpha) { return (alpha << 24) | (rgb & 0xFFFFFF); }

void Line(std::vector<WorldRenderer::OverlayVertex>& out, const Vec3& a, const Vec3& b, D3DCOLOR colour) {
    out.push_back({ a.x, a.y, a.z, colour });
    out.push_back({ b.x, b.y, b.z, colour });
}

void Quad(std::vector<WorldRenderer::OverlayVertex>& out, const Vec3& a, const Vec3& b,
          const Vec3& c, const Vec3& d, D3DCOLOR colour) {
    for (const Vec3* p : { &a, &b, &c, &a, &c, &d }) out.push_back({ p->x, p->y, p->z, colour });
}

// --- self-test (FRS_COLLISION_TOOLS_SELFTEST=1) ---------------------------------

int g_failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    LOG_INFO("collision selftest: %s  %s", ok ? "ok  " : "FAIL", what);
}

// Encodes a pack, parses it back and compares: what a save will hand the game.
bool RoundTrips(const nfs::CollisionPack& pack) {
    std::vector<u8> chunk;
    std::wstring error;
    if (!nfs::EncodeCollision(pack, chunk, error)) {
        LOG_INFO("collision selftest: encode failed: %S", error.c_str());
        return false;
    }
    nfs::ChunkReader reader(ByteSpan{ chunk.data(), chunk.size() }, 0);
    nfs::Chunk top;
    nfs::CollisionPack back;
    if (!reader.Next(top) || !nfs::ParseCollision(top, back)) return false;
    if (back.polygons.size() != pack.polygons.size()) return false;
    for (size_t i = 0; i < pack.polygons.size(); ++i) {
        const auto &a = pack.polygons[i], &b = back.polygons[i];
        if (a.attributes[0] != b.attributes[0] || a.attributes[2] != b.attributes[2] ||
            a.attributes[3] != b.attributes[3] || a.barrier != b.barrier || a.quad != b.quad)
            return false;
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(a.corners[c].x - b.corners[c].x) > 0.07f ||
                std::fabs(a.corners[c].y - b.corners[c].y) > 0.07f)
                return false;
            if (!a.barrier && std::fabs(a.corners[c].z - b.corners[c].z) > 0.01f) return false;
        }
    }
    return true;
}

void RunCollisionSelfTest(nfs::World& world) {
    g_failures = 0;
    const auto& loaded = world.Loaded();
    std::vector<std::vector<u32>> before(loaded.size());
    for (size_t s = 0; s < loaded.size(); ++s)
        for (const auto& pack : loaded[s]->collision) before[s].push_back(nfs::CollisionFingerprint(pack));

    collision::Editor editor;
    editor.SetWorld(&world);

    // By the Q38 bus stop, where the earlier collision tests were made.
    const float x = 1707.7f, y = -933.8f;
    float z = 0.0f;
    Check(nfs::GroundHeight(world, x, y, z), "ground found at the test spot");

    std::vector<nfs::CollisionPolygon> barrier, wall, ground;
    collision::WallOptions options;
    collision::AppendWall({ { x, y, z }, { x + 20.0f, y, z } }, options, barrier);
    Check(barrier.size() == 6, "a 20 m barrier makes 3 pieces, both ways");
    options.kind = collision::WallKind::Vertical;
    options.height = 5.0f;
    collision::AppendWall({ { x, y + 4, z }, { x + 6, y + 4, z }, { x + 6, y + 10, z } }, options, wall);
    Check(wall.size() == 4 && wall[0].Vertical() && wall[0].corners[1].z - wall[0].corners[0].z > 5.4f,
          "a vertical wall: one piece a segment, both ways, 5 m above the foot");
    const std::vector<Vec3> outline = { { x, y - 10, z + 0.3f }, { x + 6, y - 10, z + 0.3f },
                                        { x + 6, y - 4, z + 0.3f }, { x + 3, y - 7, z + 0.3f },
                                        { x, y - 4, z + 0.3f } };
    Check(collision::AppendGround(outline, nfs::kSurfaceSidewalk, ground) && ground.size() == 3,
          "a concave five-point outline makes three triangles");

    const auto addedBarrier = editor.Add(barrier, L"test barrier");
    const auto addedWall = editor.Add(wall, L"test wall");
    const auto addedGround = editor.Add(ground, L"test ground");
    Check(addedBarrier.size() >= barrier.size() && addedWall.size() >= wall.size() &&
          addedGround.size() >= ground.size(), "everything went into at least one pack");
    for (const auto& ref : addedGround) {
        const auto& polygon = loaded[ref.section]->collision[ref.pack].polygons[ref.polygon];
        if (polygon.surface != nfs::kSurfaceSidewalk || polygon.raceGroup != 0) {
            Check(false, "new ground keeps its surface and race group 0");
            break;
        }
    }

    // A polygon the game stores in two packs: new ground on it goes to both.
    collision::PolygonRef shared{};
    bool foundShared = false;
    for (size_t s = 0; s < loaded.size() && !foundShared; ++s)
        for (size_t p = 0; p < loaded[s]->collision.size() && !foundShared; ++p)
            for (size_t i = 0; i < loaded[s]->collision[p].polygons.size() && i < 400; ++i) {
                const auto& polygon = loaded[s]->collision[p].polygons[i];
                if (polygon.barrier || polygon.Vertical() || polygon.raceGroup) continue;
                if (editor.CopiesOf({ (u32)s, (u32)p, (u32)i }).size() >= 2) {
                    shared = { (u32)s, (u32)p, (u32)i };
                    foundShared = true;
                    break;
                }
            }
    Check(foundShared, "found a polygon kept by two sections");
    if (foundShared) {
        const auto& edge = loaded[shared.section]->collision[shared.pack].polygons[shared.polygon];
        const Vec3 m = (edge.corners[0] + edge.corners[1] + edge.corners[2]) * (1.0f / 3.0f);
        const Vec3 tri[3] = { m + Vec3{ -0.5f, -0.5f, 0.1f }, m + Vec3{ 0.5f, -0.5f, 0.1f }, m + Vec3{ 0, 0.5f, 0.1f } };
        const auto packs = editor.PacksFor(collision::MakeGround(tri, 3, 1));
        Check(packs.size() >= 2, "ground over a section edge goes into both sections");
        LOG_INFO("collision selftest: edge polygon in %s goes into %zu packs",
                 loaded[shared.section]->info ? loaded[shared.section]->info->name.c_str() : "?", packs.size());
    }

    // Surface, move, encode.
    Check(editor.SetSurface(addedGround, nfs::kSurfaceGrass) >= addedGround.size(), "surface change reaches every copy");
    const Vec3 start = loaded[addedGround[0].section]->collision[addedGround[0].pack].polygons[addedGround[0].polygon].corners[0];
    Check(editor.BeginMove(addedGround), "move begins");
    editor.UpdateMove({ 1.0f, 2.0f, 0.5f });
    editor.EndMove(true);
    const Vec3 moved = loaded[addedGround[0].section]->collision[addedGround[0].pack].polygons[addedGround[0].polygon].corners[0];
    Check(std::fabs(moved.x - start.x - 1.0f) < 1e-3f && std::fabs(moved.z - start.z - 0.5f) < 1e-3f,
          "move shifts the corners");

    size_t changedPacks = 0;
    bool allRoundTrip = true;
    for (size_t s = 0; s < loaded.size(); ++s)
        for (u32 p : loaded[s]->changedCollisionPacks) {
            ++changedPacks;
            allRoundTrip = allRoundTrip && RoundTrips(loaded[s]->collision[p]);
        }
    Check(changedPacks > 0 && allRoundTrip, "every changed pack encodes and reads back the same");

    // The save plan, nothing written.
    {
        PlacementSavePlan plan;
        const bool planned = PlanPlacementSave(world, world.OverlayFolder(), plan);
        Check(planned && plan.ok && !plan.spans.empty(), "the save plan rebuilds the changed sections");
        size_t found = 0;
        for (const SectionSpan& span : plan.spans) {
            if (span.layoutFile) continue;
            nfs::ChunkReader reader(ByteSpan{ span.bytes.data(), span.bytes.size() }, span.fileOffset);
            nfs::Chunk chunk;
            while (reader.Next(chunk)) {
                if (chunk.id != nfs::kChunkCollisionPack) continue;
                nfs::CollisionPack pack;
                if (!nfs::ParseCollision(chunk, pack)) continue;
                for (const auto& polygon : pack.polygons) found += polygon.surface == nfs::kSurfaceGrass && polygon.Vertical() == false && polygon.quad == false && std::fabs(polygon.corners[0].x - moved.x) < 0.2f;
            }
        }
        Check(found >= 1, "the rebuilt section bytes carry the new ground");
        LOG_INFO("collision selftest: save plan of %zu spans:\n%S", plan.spans.size(), plan.report.c_str());
    }

    // Deleting a shared polygon removes it from both sections.
    if (foundShared) {
        const size_t copies = editor.CopiesOf(shared).size();
        Check(editor.Remove({ shared }) == copies, "delete reaches every copy");
    }

    // Collision made from a street light's mesh: solid, walls round the pole.
    {
        const nfs::SolidObject* model = nullptr;
        Mat4 transform;
        for (size_t s = 0; s < loaded.size() && !model; ++s) {
            const nfs::Scenery& scenery = loaded[s]->scenery;
            for (const nfs::SceneryInstance& instance : scenery.instances) {
                const nfs::SolidObject* candidate = world.FindModel(scenery.ModelHash(instance));
                if (candidate && candidate->name.find("STREETLIGHT") != std::string::npos) {
                    model = candidate;
                    transform = instance.transform;
                    break;
                }
            }
        }
        Check(model != nullptr, "found a street light placed in the city");
        if (model) {
            std::vector<nfs::CollisionPolygon> generated;
            const auto result = collision::GenerateObjectCollision(*model, transform, &world, generated);
            LOG_INFO("collision selftest: %s -> %s, %zu walls, %zu ground", model->name.c_str(),
                     result.shape == collision::GeneratedShape::Solid ? "solid" : "drivable",
                     result.walls, result.ground);
            Check(result.shape == collision::GeneratedShape::Solid && result.walls >= 6 &&
                  generated.front().Vertical() && generated.front().surface == 23,
                  "a street light gets POST walls round its pole");
            const auto added = editor.Add(generated, L"street light");
            const auto again = editor.Add(generated, L"street light again");
            Check(!added.empty() && again.empty(), "making it twice adds nothing the second time");
        }
    }

    // Undo everything: the packs are back as loaded.
    while (editor.Undo()) {}
    bool same = true, clean = true;
    for (size_t s = 0; s < loaded.size(); ++s) {
        for (size_t p = 0; p < loaded[s]->collision.size(); ++p)
            same = same && nfs::CollisionFingerprint(loaded[s]->collision[p]) == before[s][p];
        clean = clean && !loaded[s]->collisionChanged;
    }
    Check(same, "undo restores every pack");
    Check(clean, "undo clears the unsaved marks");
    LOG_INFO("collision selftest: %s (%d failures)", g_failures ? "FAILED" : "PASSED", g_failures);
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void CollisionTools::OnCreate() {
    host_.viewport->AddTool(this);
    BuildMenu();
    ExtendAccelerators();
}

void CollisionTools::BuildMenu() {
    if (!host_.menu) return;
    const auto surfaces = [](int first) {
        HMENU menu = ::CreatePopupMenu();
        for (u8 s = 0; s < nfs::kSurfaceCount; ++s) {
            std::wstring text = Format(L"%02X  %s", s, SurfaceLabel(s).c_str());
            ::AppendMenuW(menu, MF_STRING, (UINT_PTR)(first + s), text.c_str());
        }
        return menu;
    };

    HMENU heights = ::CreatePopupMenu();
    for (size_t i = 0; i < std::size(kHeights); ++i)
        ::AppendMenuW(heights, MF_STRING, (UINT_PTR)(kIdHeightFirst + (int)i),
                      Format(L"%.1f m", kHeights[i]).c_str());

    popup_ = ::CreatePopupMenu();
    ::AppendMenuW(popup_, MF_STRING, ID_VIEW_COLLISION, L"&Show Collision\tCtrl+K");
    ::AppendMenuW(popup_, MF_STRING, kIdRaceGroups, L"Show &Race Barriers");
    ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(popup_, MF_STRING, kIdBarrier, L"Draw &Barrier (no height limit)\tCtrl+Shift+W");
    ::AppendMenuW(popup_, MF_STRING, kIdWall, L"Draw &Wall with Height\tCtrl+Shift+H");
    ::AppendMenuW(popup_, MF_STRING, kIdGround, L"Draw &Ground\tCtrl+Shift+G");
    ::AppendMenuW(popup_, MF_POPUP, (UINT_PTR)heights, L"Wall &Height");
    ::AppendMenuW(popup_, MF_POPUP, (UINT_PTR)surfaces(kIdSurfaceWall), L"Surface for New &Walls");
    ::AppendMenuW(popup_, MF_POPUP, (UINT_PTR)surfaces(kIdSurfaceGround), L"Surface for New G&round");
    ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(popup_, MF_POPUP, (UINT_PTR)surfaces(kIdSurfaceSelect), L"Set Surface of &Selection");
    ::AppendMenuW(popup_, MF_STRING, kIdAlwaysPresent, L"Make Selection &Always Present");
    ::AppendMenuW(popup_, MF_STRING, kIdCopies, L"Select &Copies in Other Sections");
    ::AppendMenuW(popup_, MF_STRING, kIdMove, L"&Move Selection\tG");
    ::AppendMenuW(popup_, MF_STRING, kIdDelete, L"&Delete Selection\tDel");
    ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(popup_, MF_STRING, kIdGenerate, L"Make Collision for Selected &Objects\tCtrl+Shift+K");
    ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(popup_, MF_STRING, kIdUndo, L"&Undo (collision and objects)\tCtrl+Z");
    ::AppendMenuW(popup_, MF_STRING, ID_OBJECT_SAVE, L"Write Edits to the &Game\tCtrl+Shift+S");

    // Before Help, the last menu on the bar.
    const int count = ::GetMenuItemCount(host_.menu);
    ::InsertMenuW(host_.menu, count > 0 ? (UINT)(count - 1) : 0, MF_BYPOSITION | MF_POPUP,
                  (UINT_PTR)popup_, L"&Collision");
    if (host_.window) ::DrawMenuBar(host_.window);
}

void CollisionTools::ExtendAccelerators() {
    if (!host_.accelerators || !*host_.accelerators) return;
    const int count = ::CopyAcceleratorTableW(*host_.accelerators, nullptr, 0);
    std::vector<ACCEL> table((size_t)count);
    ::CopyAcceleratorTableW(*host_.accelerators, table.data(), count);
    const BYTE ctrl = FVIRTKEY | FCONTROL, ctrlShift = FVIRTKEY | FCONTROL | FSHIFT;
    table.push_back({ ctrl, 'Z', (WORD)kIdUndo });
    table.push_back({ ctrlShift, 'W', (WORD)kIdBarrier });
    table.push_back({ ctrlShift, 'H', (WORD)kIdWall });
    table.push_back({ ctrlShift, 'G', (WORD)kIdGround });
    table.push_back({ ctrlShift, 'K', (WORD)kIdGenerate });
    if (HACCEL extended = ::CreateAcceleratorTableW(table.data(), (int)table.size()))
        *host_.accelerators = extended;
}

void CollisionTools::UpdateMenuChecks(HMENU menu) {
    const auto check = [menu](int id, bool on) {
        ::CheckMenuItem(menu, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    };
    check(kIdRaceGroups, Renderer().RaceGroupsVisible());
    check(kIdBarrier, mode_ == Mode::Barrier);
    check(kIdWall, mode_ == Mode::Wall);
    check(kIdGround, mode_ == Mode::Ground);
    for (size_t i = 0; i < std::size(kHeights); ++i)
        check(kIdHeightFirst + (int)i, std::fabs(kHeights[i] - wallHeight_) < 0.01f);
    for (u8 s = 0; s < nfs::kSurfaceCount; ++s) {
        check(kIdSurfaceGround + s, s == groundSurface_);
        check(kIdSurfaceWall + s, s == wallSurface_);
    }
    ::EnableMenuItem(menu, kIdUndo, MF_BYCOMMAND | (!history_.empty() || editor_.CanUndo() ? MF_ENABLED : MF_GRAYED));
}

void CollisionTools::OnRegionLoaded() {
    StopTool();
    moving_ = false;
    editor_.SetWorld(World());
    // A check of the whole edit path, run on the loaded region without
    // writing anything: set FRS_COLLISION_TOOLS_SELFTEST=1 before starting.
    static const bool selfTest = std::getenv("FRS_COLLISION_TOOLS_SELFTEST") != nullptr;
    if (selfTest && World()) {
        RunCollisionSelfTest(*World());
        editor_.SetWorld(World());
        for (size_t i = 0; i < World()->Loaded().size(); ++i) Renderer().RefreshCollision(i);
    }
    ResetHistory();
}

void CollisionTools::OnRegionClosed() {
    StopTool();
    moving_ = false;
    editor_.SetWorld(nullptr);
    history_.clear();
    placementUndo_.clear();
    settled_ = {};
}

void CollisionTools::RefreshAfterEdit() {
    for (u32 section : editor_.TakeTouchedSections()) Renderer().RefreshCollision(section);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool CollisionTools::OnCommand(int id) {
    if (id >= kIdSurfaceSelect && id < kIdSurfaceSelect + nfs::kSurfaceCount) {
        SetSelectionSurface((u8)(id - kIdSurfaceSelect));
        return true;
    }
    if (id >= kIdSurfaceGround && id < kIdSurfaceGround + nfs::kSurfaceCount) {
        groundSurface_ = (u8)(id - kIdSurfaceGround);
        Status(L"New ground will be " + SurfaceLabel(groundSurface_) + L".");
        if (host_.menuChecks) host_.menuChecks();
        return true;
    }
    if (id >= kIdSurfaceWall && id < kIdSurfaceWall + nfs::kSurfaceCount) {
        wallSurface_ = (u8)(id - kIdSurfaceWall);
        Status(L"New walls will be " + SurfaceLabel(wallSurface_) + L".");
        if (host_.menuChecks) host_.menuChecks();
        return true;
    }
    if (id >= kIdHeightFirst && id < kIdHeightFirst + (int)std::size(kHeights)) {
        wallHeight_ = kHeights[id - kIdHeightFirst];
        UpdateOverlay();
        Status(Format(L"Walls will stand %.1f m tall.", wallHeight_));
        if (host_.menuChecks) host_.menuChecks();
        return true;
    }
    switch (id) {
        case kIdBarrier: StartTool(mode_ == Mode::Barrier ? Mode::None : Mode::Barrier); return true;
        case kIdWall:    StartTool(mode_ == Mode::Wall ? Mode::None : Mode::Wall);       return true;
        case kIdGround:  StartTool(mode_ == Mode::Ground ? Mode::None : Mode::Ground);   return true;
        case kIdUndo:    Undo(); return true;
        case kIdDelete:  DeleteSelection(); return true;
        case kIdAlwaysPresent: ClearSelectionRaceGroup(); return true;
        case kIdCopies:  SelectCopies(); return true;
        case kIdMove:    BeginMove(); return true;
        case kIdGenerate:
        case IDC_PROP_COLLISION:
            GenerateForSelection();
            return true;
        case kIdRaceGroups:
            Renderer().SetRaceGroupsVisible(!Renderer().RaceGroupsVisible());
            if (host_.menuChecks) host_.menuChecks();
            return true;
        default:
            return false;
    }
}

std::vector<collision::PolygonRef> CollisionTools::SelectionRefs() const {
    std::vector<collision::PolygonRef> refs;
    for (const CollisionPick& pick : Renderer().CollisionSelection())
        refs.push_back({ pick.section, pick.pack, pick.polygon });
    return refs;
}

void CollisionTools::SelectRefs(const std::vector<collision::PolygonRef>& refs) {
    std::vector<CollisionPick> picks;
    for (const collision::PolygonRef& ref : refs) picks.push_back({ ref.section, ref.pack, ref.polygon, 0.0f });
    Renderer().SetCollisionSelectionList(std::move(picks));
}

void CollisionTools::SetSelectionSurface(u8 surface) {
    const auto refs = SelectionRefs();
    if (refs.empty()) { Status(L"Select collision polygons first (Ctrl+K shows them)."); return; }
    const size_t changed = editor_.SetSurface(refs, surface);
    RefreshAfterEdit();
    Status(Format(L"%zu polygons are now %s (copies included; not written yet).", changed,
                  SurfaceLabel(surface).c_str()));
}

void CollisionTools::ClearSelectionRaceGroup() {
    const auto refs = SelectionRefs();
    if (refs.empty()) { Status(L"Select collision polygons first (Ctrl+K shows them)."); return; }
    const size_t changed = editor_.SetRaceGroup(refs, 0);
    RefreshAfterEdit();
    Status(Format(L"%zu polygons now exist in free roam as well as in races (not written yet).", changed));
}

void CollisionTools::SelectCopies() {
    const auto refs = SelectionRefs();
    if (refs.empty()) { Status(L"Select collision polygons first."); return; }
    const auto all = editor_.WithCopies(refs);
    SelectRefs(all);
    Status(Format(L"%zu polygons selected: %zu more are copies kept by neighbouring sections.",
                  all.size(), all.size() > refs.size() ? all.size() - refs.size() : 0));
}

void CollisionTools::Undo() {
    if (moving_) { EndMove(false); return; }
    if (!points_.empty()) {
        points_.pop_back();
        UpdateOverlay();
        return;
    }
    WatchHistory();
    if (history_.empty()) { Status(L"Nothing to undo."); return; }

    const StepKind kind = history_.back();
    history_.pop_back();
    if (kind == StepKind::Collision) {
        const std::wstring label = editor_.UndoLabel();
        if (!editor_.Undo()) { Status(L"That collision edit is too old to undo."); return; }
        ++seenUndone_;
        Renderer().SetCollisionSelectionList({});
        RefreshAfterEdit();
        Status(L"Undone: " + label);
    } else if (!placementUndo_.empty()) {
        PlacementState state = std::move(placementUndo_.back());
        placementUndo_.pop_back();
        RestorePlacements(state);
        settled_ = std::move(state);
        settledSignature_ = PlacementSignature();
        Status(L"Undone: the last change to the objects");
    }
    if (host_.menuChecks) host_.menuChecks();
}

// ---------------------------------------------------------------------------
// Undo history
// ---------------------------------------------------------------------------

CollisionTools::PlacementState CollisionTools::CapturePlacements() const {
    PlacementState state;
    if (nfs::World* world = World())
        for (const auto& section : world->Loaded()) {
            state.placements.push_back(section->placements);
            state.removed.push_back(section->removedNatives);
            state.imported.push_back(section->importedModels);
            state.changed.push_back(section->placementsChanged ? 1 : 0);
        }
    return state;
}

void CollisionTools::RestorePlacements(const PlacementState& state) {
    nfs::World* world = World();
    if (!world) return;
    for (size_t i = 0; i < world->Loaded().size() && i < state.placements.size(); ++i) {
        nfs::LoadedSection* section = world->MutableLoaded(i);
        section->placements = state.placements[i];
        section->removedNatives = state.removed[i];
        section->importedModels = state.imported[i];
        section->placementsChanged = state.changed[i] != 0;
    }
    Renderer().ClearSelection();
    Renderer().RebuildInstances(*world);
}

size_t CollisionTools::PlacementSignature() const {
    nfs::World* world = World();
    if (!world) return 0;
    u64 hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, size_t size) {
        const u8* bytes = (const u8*)data;
        for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    };
    for (const auto& section : world->Loaded()) {
        const size_t count = section->placements.size();
        mix(&count, sizeof(count));
        for (const auto& placed : section->placements) {
            mix(&placed.sceneryNumber, sizeof(placed.sceneryNumber));
            mix(&placed.instance.modelHash, sizeof(placed.instance.modelHash));
            mix(&placed.instance.transform, sizeof(placed.instance.transform));
        }
        const size_t removed = section->removedNatives.size();
        mix(&removed, sizeof(removed));
        if (removed) mix(section->removedNatives.data(), removed * sizeof(section->removedNatives[0]));
        const size_t imported = section->importedModels.size();
        mix(&imported, sizeof(imported));
        if (imported) mix(section->importedModels.data(), imported * sizeof(u32));
    }
    return (size_t)hash;
}

void CollisionTools::ResetHistory() {
    history_.clear();
    placementUndo_.clear();
    settled_ = CapturePlacements();
    settledSignature_ = PlacementSignature();
    seenRecorded_ = editor_.StepsRecorded();
    seenUndone_ = editor_.StepsUndone();
}

void CollisionTools::WatchHistory() {
    if (!World()) return;

    // Collision steps, in the order the editor recorded them; one it took
    // back on its own (a cancelled move) leaves the history again.
    while (seenRecorded_ < editor_.StepsRecorded()) {
        history_.push_back(StepKind::Collision);
        ++seenRecorded_;
    }
    while (seenUndone_ < editor_.StepsUndone()) {
        ++seenUndone_;
        for (size_t i = history_.size(); i-- > 0;)
            if (history_[i] == StepKind::Collision) { history_.erase(history_.begin() + (ptrdiff_t)i); break; }
    }

    // Placements: one step each time they come to rest in a new state. While
    // something is being dragged they are not at rest.
    if (host_.viewport->Transforming() || host_.viewport->GizmoDragging() || ::GetKeyState(VK_LBUTTON) < 0)
        return;
    const size_t signature = PlacementSignature();
    if (signature == settledSignature_) return;
    placementUndo_.push_back(std::move(settled_));
    history_.push_back(StepKind::Placements);
    settled_ = CapturePlacements();
    settledSignature_ = signature;

    // A hundred steps back is plenty; the oldest go first.
    while (history_.size() > 100) {
        if (history_.front() == StepKind::Placements && !placementUndo_.empty())
            placementUndo_.erase(placementUndo_.begin());
        history_.erase(history_.begin());
    }
}

void CollisionTools::Tick(float) {
    WatchHistory();
}

// ---------------------------------------------------------------------------
// Collision made from objects
// ---------------------------------------------------------------------------

void CollisionTools::GenerateForSelection() {
    nfs::World* world = World();
    const auto& selection = Renderer().Selection();
    if (!world || selection.empty()) {
        Status(L"Select one or more objects first (with the collision hidden, Ctrl+K).");
        return;
    }
    std::vector<nfs::CollisionPolygon> polygons;
    size_t solid = 0, drivable = 0, skipped = 0, walls = 0, ground = 0;
    bool capped = false;
    std::wstring why;
    for (const PickResult& picked : selection) {
        if (!picked.object) { ++skipped; continue; }
        const collision::GenerateResult result =
            collision::GenerateObjectCollision(*picked.object, picked.transform, world, polygons);
        if (!result.walls && !result.ground) {
            ++skipped;
            if (why.empty()) why = Widen(picked.object->name) + L": " + result.note;
            continue;
        }
        (result.shape == collision::GeneratedShape::Drivable ? drivable : solid)++;
        walls += result.walls;
        ground += result.ground;
        capped = capped || result.groundCapped;
        LOG_INFO("collision for %s: %s, %zu walls, %zu ground", picked.object->name.c_str(),
                 result.shape == collision::GeneratedShape::Drivable ? "drivable" : "solid",
                 result.walls, result.ground);
    }
    if (polygons.empty()) {
        Status(L"No collision made" + (why.empty() ? std::wstring(L".") : L" -- " + why));
        return;
    }
    const auto added = editor_.Add(polygons, Format(L"Make collision for %zu objects", solid + drivable));
    RefreshAfterEdit();
    if (!Renderer().CollisionVisible() && host_.window)
        ::SendMessageW(host_.window, WM_COMMAND, ID_VIEW_COLLISION, 0);
    Status(Format(L"Collision made for %zu objects (%zu solid, %zu drivable%s): %zu walls, %zu ground, "
                  L"%zu stored%s. Ctrl+Z undoes; Ctrl+Shift+S writes.",
                  solid + drivable, solid, drivable,
                  skipped ? Format(L", %zu skipped", skipped).c_str() : L"", walls, ground, added.size(),
                  capped ? L" -- very detailed tops were cut short" : L""));
    if (host_.menuChecks) host_.menuChecks();
}

bool CollisionTools::DeleteSelection() {
    const auto refs = SelectionRefs();
    if (refs.empty()) return false;
    const size_t removed = editor_.Remove(refs);
    Renderer().SetCollisionSelectionList({});
    RefreshAfterEdit();
    Status(Format(L"Deleted %zu collision polygons (%zu selected, the rest copies in neighbouring "
                  L"sections). Ctrl+Z undoes; Ctrl+Shift+S writes.", removed, refs.size()));
    if (host_.menuChecks) host_.menuChecks();
    return true;
}

// ---------------------------------------------------------------------------
// Drawing tools
// ---------------------------------------------------------------------------

void CollisionTools::StartTool(Mode mode) {
    if (moving_) EndMove(false);
    points_.clear();
    mode_ = mode;
    if (mode != Mode::None && !World()) {
        mode_ = Mode::None;
        Status(L"Open a region first.");
    }
    if (mode_ != Mode::None) {
        if (!Renderer().CollisionVisible() && host_.window)
            ::SendMessageW(host_.window, WM_COMMAND, ID_VIEW_COLLISION, 0);
        Renderer().SetCollisionSelectionList({});
        ::SetFocus(host_.viewport->Handle());
    }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
}

void CollisionTools::StopTool() {
    mode_ = Mode::None;
    points_.clear();
    haveCursor_ = false;
    Renderer().SetToolOverlay({}, {});
}

bool CollisionTools::PointUnder(int x, int y, Vec3& point) const {
    nfs::World* world = World();
    if (!world) return false;
    const Ray ray = host_.viewport->RayAt(x, y);

    float best = 1e30f;
    PickResult mesh;
    if (Renderer().Pick(ray, mesh) && mesh.distance < 5000.0f) best = mesh.distance;
    CollisionPick hit;
    if (Renderer().PickCollision(ray, hit) && hit.distance < best) best = hit.distance;

    if (best < 1e29f) {
        point = ray.origin + ray.direction * best;
    } else if (!points_.empty() && std::fabs(ray.direction.z) > 1e-4f) {
        // Over nothing: on the level of the last point.
        const float t = (points_.back().z - ray.origin.z) / ray.direction.z;
        if (t <= 0.0f || t > 5000.0f) return false;
        point = ray.origin + ray.direction * t;
        best = t;
    } else {
        return false;
    }

    // Onto the collision ground when it is close: the car drives on that,
    // and the render mesh can sit a little above or below it.
    float ground = 0.0f;
    if (nfs::GroundBelow(*world, point.x, point.y, point.z + 1.0f, ground) &&
        std::fabs(ground - point.z) < 1.5f)
        point.z = ground;

    // Snap to a point of this drawing, else a collision corner nearby.
    const_cast<CollisionTools*>(this)->cursorSnapped_ = false;
    if (::GetKeyState(VK_MENU) < 0) return true;
    const float radius = std::clamp(best * 0.015f, 0.2f, 3.0f);
    float nearest = radius * radius;
    Vec3 snapped = point;
    bool found = false;
    const auto consider = [&](const Vec3& candidate, bool keepHeight) {
        const float dx = candidate.x - point.x, dy = candidate.y - point.y;
        const float d = dx * dx + dy * dy;
        if (d < nearest && std::fabs(candidate.z - point.z) < 4.0f + (keepHeight ? 1e9f : 0.0f)) {
            nearest = d;
            snapped = { candidate.x, candidate.y, keepHeight ? point.z : candidate.z };
            found = true;
        }
    };
    for (const Vec3& p : points_) consider(p, false);
    if (!found) {
        for (const auto& section : world->Loaded())
            for (const nfs::CollisionPack& pack : section->collision) {
                const Aabb& b = pack.bounds;
                if (b.Valid() && (point.x < b.min.x - radius || point.x > b.max.x + radius ||
                                  point.y < b.min.y - radius || point.y > b.max.y + radius))
                    continue;
                for (const nfs::CollisionPolygon& polygon : pack.polygons) {
                    if (polygon.raceGroup && !Renderer().RaceGroupsVisible()) continue;
                    if (polygon.barrier) {
                        consider(polygon.corners[0], true);
                        consider(polygon.corners[2], true);
                        continue;
                    }
                    const int corners = polygon.quad ? 4 : 3;
                    for (int c = 0; c < corners; ++c) {
                        // A vertical wall's foot, not its top.
                        if (polygon.Vertical() && (c == 1 || c == 2)) continue;
                        consider(polygon.corners[c], false);
                    }
                }
            }
    }
    if (found) {
        point = snapped;
        const_cast<CollisionTools*>(this)->cursorSnapped_ = true;
    }
    return true;
}

void CollisionTools::UpdateOverlay() {
    if (mode_ == Mode::None && !moving_) {
        Renderer().SetToolOverlay({}, {});
        return;
    }
    if (moving_) return;

    std::vector<WorldRenderer::OverlayVertex> triangles, lines;
    std::vector<Vec3> chain = points_;
    if (haveCursor_) chain.push_back(cursor_);

    const D3DCOLOR pointColour = 0xFFFFFFFF;
    for (const Vec3& p : points_) {
        Line(lines, p + Vec3{ -0.4f, 0, 0 }, p + Vec3{ 0.4f, 0, 0 }, pointColour);
        Line(lines, p + Vec3{ 0, -0.4f, 0 }, p + Vec3{ 0, 0.4f, 0 }, pointColour);
        Line(lines, p, p + Vec3{ 0, 0, 1.0f }, pointColour);
    }
    if (haveCursor_) {
        const D3DCOLOR c = cursorSnapped_ ? 0xFFFFE040 : 0xFF40FFFF;
        const float s = cursorSnapped_ ? 0.6f : 0.35f;
        Line(lines, cursor_ + Vec3{ -s, -s, 0 }, cursor_ + Vec3{ s, s, 0 }, c);
        Line(lines, cursor_ + Vec3{ -s, s, 0 }, cursor_ + Vec3{ s, -s, 0 }, c);
        Line(lines, cursor_, cursor_ + Vec3{ 0, 0, 1.5f }, c);
    }

    if (mode_ == Mode::Ground) {
        const u32 rgb = CollisionMesh::SurfaceRgb(groundSurface_);
        for (size_t i = 0; i + 1 < chain.size(); ++i) Line(lines, chain[i], chain[i + 1], Argb(rgb, 0xFF));
        if (chain.size() >= 3) {
            Line(lines, chain.back(), chain.front(), Argb(0x808080, 0xFF));
            std::vector<nfs::CollisionPolygon> preview;
            collision::AppendGround(chain, groundSurface_, preview);
            for (const nfs::CollisionPolygon& polygon : preview) {
                const Vec3* c = polygon.corners;
                const Vec3 lift{ 0, 0, 0.08f };
                Quad(triangles, c[0] + lift, c[1] + lift, c[2] + lift,
                     (polygon.quad ? c[3] : c[2]) + lift, Argb(rgb, 0x70));
            }
        }
    } else {
        const bool barrier = mode_ == Mode::Barrier;
        const float below = barrier ? nfs::CollisionPack::kBarrierBelow : 0.5f;
        const float above = barrier ? nfs::CollisionPack::kBarrierAbove : wallHeight_;
        const D3DCOLOR fill = barrier ? 0x70FF3030 : 0x70FF8040;
        const D3DCOLOR edge = barrier ? 0xFFFFD040 : 0xFFFF8040;
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            const Vec3 a = chain[i], b = chain[i + 1];
            const Vec3 a0{ a.x, a.y, a.z - below }, a1{ a.x, a.y, a.z + above };
            const Vec3 b0{ b.x, b.y, b.z - below }, b1{ b.x, b.y, b.z + above };
            Quad(triangles, a0, a1, b1, b0, fill);
            Line(lines, a, b, edge);
            Line(lines, a1, b1, edge);
        }
    }
    Renderer().SetToolOverlay(std::move(triangles), std::move(lines));
}

void CollisionTools::Finish() {
    if (mode_ == Mode::None) return;
    std::vector<nfs::CollisionPolygon> polygons;
    std::wstring label;
    if (mode_ == Mode::Ground) {
        if (points_.size() < 3) { Status(L"Ground needs at least three points."); return; }
        if (!collision::AppendGround(points_, groundSurface_, polygons)) {
            Status(L"Those points enclose no area.");
            return;
        }
        label = L"Draw ground";
    } else {
        if (points_.size() < 2) { Status(L"A wall needs at least two points."); return; }
        collision::WallOptions options;
        options.kind = mode_ == Mode::Barrier ? collision::WallKind::Barrier : collision::WallKind::Vertical;
        options.surface = wallSurface_;
        options.height = wallHeight_;
        collision::AppendWall(points_, options, polygons);
        label = mode_ == Mode::Barrier ? L"Draw barrier" : L"Draw wall";
    }

    const auto added = editor_.Add(polygons, label);
    RefreshAfterEdit();
    points_.clear();
    UpdateOverlay();
    if (added.empty()) {
        Status(L"There is no section collision there to add to.");
        return;
    }
    std::set<u32> sections;
    for (const auto& ref : added) sections.insert(ref.section);
    Status(Format(L"%s: %zu polygons added to %zu sections (not written yet: Ctrl+Shift+S). "
                  L"Ctrl+Z undoes.", label.c_str(), polygons.size(), sections.size()));
    if (host_.menuChecks) host_.menuChecks();
}

// ---------------------------------------------------------------------------
// Moving a selection
// ---------------------------------------------------------------------------

bool CollisionTools::BeginMove() {
    const auto refs = SelectionRefs();
    nfs::World* world = World();
    if (refs.empty() || !world) {
        Status(L"Select collision polygons first (Ctrl+K shows them).");
        return false;
    }
    if (!editor_.BeginMove(refs)) return false;

    float sum = 0.0f;
    size_t n = 0;
    for (const auto& ref : refs) {
        const nfs::CollisionPolygon& polygon = world->Loaded()[ref.section]->collision[ref.pack].polygons[ref.polygon];
        if (polygon.barrier) continue;
        for (int c = 0; c < (polygon.quad ? 4 : 3); ++c) { sum += polygon.corners[c].z; ++n; }
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::ScreenToClient(host_.viewport->Handle(), &cursor);
    const Ray ray = host_.viewport->RayAt(cursor.x, cursor.y);
    if (n) {
        movePlane_ = sum / (float)n;
    } else {
        Vec3 point;
        movePlane_ = PointUnder(cursor.x, cursor.y, point) ? point.z : host_.viewport->GetCamera().Position().z - 10.0f;
    }
    const float t = std::fabs(ray.direction.z) > 1e-4f ? (movePlane_ - ray.origin.z) / ray.direction.z : -1.0f;
    moveAnchor_ = t > 0.0f ? ray.origin + ray.direction * t : ray.origin;
    moveRise_ = 0.0f;
    moveCount_ = editor_.Moving() ? refs.size() : 0;
    moving_ = true;
    Status(L"Moving collision: the mouse slides it, the wheel raises it (Shift for fine steps); "
           L"click to keep, Esc to put it back.");
    return true;
}

void CollisionTools::UpdateMove(int x, int y) {
    if (!moving_) return;
    const Ray ray = host_.viewport->RayAt(x, y);
    if (std::fabs(ray.direction.z) < 1e-4f) return;
    const float t = (movePlane_ - ray.origin.z) / ray.direction.z;
    if (t <= 0.0f) return;
    const Vec3 at = ray.origin + ray.direction * t;
    editor_.UpdateMove({ at.x - moveAnchor_.x, at.y - moveAnchor_.y, moveRise_ });
    RefreshAfterEdit();
}

void CollisionTools::EndMove(bool keep) {
    if (!moving_) return;
    moving_ = false;
    editor_.EndMove(keep);
    RefreshAfterEdit();
    Status(keep ? L"Collision moved (not written yet: Ctrl+Shift+S). Ctrl+Z undoes."
                : L"Move cancelled.");
    if (host_.menuChecks) host_.menuChecks();
}

// ---------------------------------------------------------------------------
// Viewport input
// ---------------------------------------------------------------------------

bool CollisionTools::MouseDoubleClick(int x, int y, WPARAM keys) {
    if (mode_ == Mode::None || moving_) return MouseDown(x, y, keys);
    // The first click of the pair already placed the point: this finishes.
    Vec3 point;
    if (PointUnder(x, y, point) &&
        (points_.empty() || std::hypot(point.x - points_.back().x, point.y - points_.back().y) >= 0.1f))
        points_.push_back(point);
    Finish();
    return true;
}

bool CollisionTools::MouseDown(int x, int y, WPARAM keys) {
    (void)keys;
    if (moving_) { EndMove(true); return true; }
    if (mode_ == Mode::None) return false;

    Vec3 point;
    if (!PointUnder(x, y, point)) { Status(L"Nothing under the pointer to stand a point on."); return true; }

    // Clicking the first point again closes a ground outline.
    if (mode_ == Mode::Ground && points_.size() >= 3 &&
        std::hypot(point.x - points_.front().x, point.y - points_.front().y) < 0.05f) {
        Finish();
        return true;
    }
    if (!points_.empty() && std::hypot(point.x - points_.back().x, point.y - points_.back().y) < 0.1f)
        return true;   // the same point twice
    points_.push_back(point);
    cursor_ = point;
    haveCursor_ = true;
    UpdateOverlay();
    return true;
}

void CollisionTools::MouseMove(int x, int y) {
    if (moving_) { UpdateMove(x, y); return; }
    if (mode_ == Mode::None) return;
    haveCursor_ = PointUnder(x, y, cursor_);
    UpdateOverlay();
}

bool CollisionTools::MouseWheel(int notches, WPARAM keys) {
    if (moving_) {
        moveRise_ += (float)notches * ((GET_KEYSTATE_WPARAM(keys) & MK_SHIFT) ? 0.05f : 0.25f);
        POINT cursor{};
        ::GetCursorPos(&cursor);
        ::ScreenToClient(host_.viewport->Handle(), &cursor);
        UpdateMove(cursor.x, cursor.y);
        return true;
    }
    if (mode_ == Mode::Wall && (GET_KEYSTATE_WPARAM(keys) & MK_CONTROL)) {
        wallHeight_ = std::clamp(wallHeight_ + 0.5f * (float)notches, 0.5f, 100.0f);
        UpdateOverlay();
        Status(Format(L"Wall height %.1f m", wallHeight_));
        return true;
    }
    return false;
}

bool CollisionTools::KeyDown(WPARAM key, bool repeat) {
    if (moving_) {
        if (key == VK_ESCAPE) { EndMove(false); return true; }
        if (key == VK_RETURN) { EndMove(true); return true; }
        return false;
    }
    if (mode_ != Mode::None) {
        switch (key) {
            case VK_RETURN: Finish(); return true;
            case VK_BACK:
                if (!points_.empty()) points_.pop_back();
                UpdateOverlay();
                return true;
            case VK_ESCAPE:
                if (!points_.empty()) { points_.clear(); UpdateOverlay(); }
                else { StopTool(); if (host_.menuChecks) host_.menuChecks(); Status(L"Collision tool off."); }
                return true;
            case VK_ADD: case VK_OEM_PLUS:
            case VK_SUBTRACT: case VK_OEM_MINUS:
                if (mode_ != Mode::Wall) return false;
                wallHeight_ = std::clamp(wallHeight_ + ((key == VK_ADD || key == VK_OEM_PLUS) ? 0.5f : -0.5f),
                                         0.5f, 100.0f);
                UpdateOverlay();
                Status(Format(L"Wall height %.1f m", wallHeight_));
                return true;
            default:
                return false;
        }
    }
    // Ctrl+Z reaches here only when the accelerator did not take it.
    if (key == 'Z' && ::GetKeyState(VK_CONTROL) < 0) { Undo(); return true; }
    if (!repeat && key == 'G' && ::GetKeyState(VK_CONTROL) >= 0 && Renderer().CollisionVisible() &&
        !Renderer().CollisionSelection().empty())
        return BeginMove();
    return false;
}

// ---------------------------------------------------------------------------
// Status and properties
// ---------------------------------------------------------------------------

bool CollisionTools::StatusText(std::wstring& text) const {
    if (moving_) {
        text = L"Moving collision -- click to keep, Esc to put back, wheel to raise";
        return true;
    }
    switch (mode_) {
        case Mode::Barrier:
            text = Format(L"Barrier (%s): click points, Enter finishes, Backspace undoes a point, Esc stops. "
                          L"Alt places without snapping. %zu points", SurfaceLabel(wallSurface_).c_str(),
                          points_.size());
            return true;
        case Mode::Wall:
            text = Format(L"Wall %.1f m (%s): click points, Enter finishes, +/- or Ctrl+wheel height, "
                          L"Esc stops. %zu points", wallHeight_, SurfaceLabel(wallSurface_).c_str(),
                          points_.size());
            return true;
        case Mode::Ground:
            text = Format(L"Ground (%s): click the outline, Enter or a click on the first point fills it, "
                          L"Esc stops. %zu points", SurfaceLabel(groundSurface_).c_str(), points_.size());
            return true;
        default:
            return false;
    }
}

bool CollisionTools::FillProperties(PropertiesPanel::View& view) {
    nfs::World* world = World();
    const auto& selection = Renderer().CollisionSelection();
    if (!world || selection.empty()) return false;
    const auto& loaded = world->Loaded();

    std::set<u8> surfaces, groups;
    std::set<std::string> sectionNames;
    size_t ground = 0, vertical = 0, barriers = 0;
    float lowest = 1e30f, highest = -1e30f;
    const nfs::CollisionPolygon* first = nullptr;
    for (const CollisionPick& pick : selection) {
        if (pick.section >= loaded.size() || pick.pack >= loaded[pick.section]->collision.size()) continue;
        const nfs::CollisionPack& pack = loaded[pick.section]->collision[pick.pack];
        if (pick.polygon >= pack.polygons.size()) continue;
        const nfs::CollisionPolygon& polygon = pack.polygons[pick.polygon];
        if (!first) first = &polygon;
        surfaces.insert(polygon.surface);
        groups.insert(polygon.raceGroup);
        if (loaded[pick.section]->info) sectionNames.insert(loaded[pick.section]->info->name);
        if (polygon.barrier) { ++barriers; continue; }
        polygon.Vertical() ? ++vertical : ++ground;
        for (int c = 0; c < (polygon.quad ? 4 : 3); ++c) {
            lowest = std::min(lowest, polygon.corners[c].z);
            highest = std::max(highest, polygon.corners[c].z);
        }
    }
    if (!first) return false;

    using Row = PropertiesPanel::Row;
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value,
                             const wchar_t* description) {
        Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        view.rows.push_back(std::move(row));
    };

    view.mode = PropertiesPanel::Mode::Extension;
    view.title = selection.size() == 1
                     ? std::wstring(first->barrier ? L"Collision barrier"
                                    : first->Vertical() ? L"Collision wall" : L"Collision ground")
                     : Format(L"%zu collision polygons", selection.size());
    view.moveLabel.clear();
    view.removeLabel = L"Delete";

    add(L"Collision", L"Polygons", std::to_wstring(selection.size()), L"How many are selected.");
    add(L"Collision", L"Kind",
        Format(L"%zu ground, %zu walls with height, %zu barriers", ground, vertical, barriers),
        L"Ground is driven on. A wall with height stops a car up to its top; a barrier has no top.");

    // Surface: a drop-down of the game's table.
    Row surface;
    surface.group = L"Surface";
    surface.name = L"Surface";
    surface.value = surfaces.size() == 1 ? SurfaceLabel(*surfaces.begin()) : L"(several)";
    surface.description = L"What the car feels and hears on it: grip, tyre sound, skid marks, sparks. "
                          L"Changing it changes every selected polygon and its copies.";
    surface.editable = true;
    surface.field = kSurfaceField;
    for (u8 s = 0; s < nfs::kSurfaceCount; ++s) surface.choices.push_back(SurfaceLabel(s));
    if (surfaces.size() != 1) surface.choices.push_back(surface.value);
    view.rows.push_back(std::move(surface));

    Row race;
    race.group = L"Surface";
    race.name = L"Race barrier group";
    race.value = groups.size() == 1 ? Widen(world->RaceBarrierGroupName(*groups.begin())) : L"(several)";
    race.description = L"NONE: always there. Any other group exists only during the races that use it -- "
                       L"in free roam the car goes straight through. Collision > Make Selection Always "
                       L"Present sets NONE.";
    race.editable = true;
    race.field = kRaceField;
    const auto& names = world->RaceBarrierGroups();
    for (size_t g = 0; g < std::max<size_t>(names.size(), 1); ++g)
        race.choices.push_back(Widen(world->RaceBarrierGroupName((u8)g)));
    if (groups.size() != 1) race.choices.push_back(race.value);
    view.rows.push_back(std::move(race));

    std::wstring sectionList;
    for (const std::string& name : sectionNames) sectionList += (sectionList.empty() ? L"" : L", ") + Widen(name);
    add(L"Where", L"Sections", sectionList,
        L"The streaming sections whose collision holds the selection.");
    if (selection.size() == 1) {
        const CollisionPick& pick = selection.front();
        const auto copies = editor_.CopiesOf({ pick.section, pick.pack, pick.polygon });
        std::wstring others;
        for (const auto& copy : copies)
            if (!(copy.section == pick.section && copy.pack == pick.pack) && loaded[copy.section]->info)
                others += (others.empty() ? L"" : L", ") + Widen(loaded[copy.section]->info->name);
        add(L"Where", L"Copies in other sections", others.empty() ? L"none" : others,
            L"The game stores polygons along a section edge in each neighbour. Deleting, moving or "
            L"changing this one does the same to its copies.");
        const int corners = first->barrier ? 2 : (first->quad ? 4 : 3);
        for (int c = 0; c < corners; ++c) {
            const Vec3& p = first->corners[first->barrier ? c * 2 : c];
            add(L"Corners", Format(L"Corner %d", c + 1).c_str(),
                first->barrier ? Format(L"%.2f, %.2f", p.x, p.y) : Format(L"%.2f, %.2f, %.2f", p.x, p.y, p.z),
                L"World position. A barrier has no height of its own.");
        }
    }
    if (lowest <= highest)
        add(L"Where", L"Height", Format(L"%.2f .. %.2f", lowest, highest), L"Lowest and highest corner.");
    add(L"Keys", L"Edit", L"G move, Del delete, Ctrl+Z undo",
        L"Moving and deleting reach the copies in neighbouring sections too. Ctrl+Shift+S writes.");
    return true;
}

bool CollisionTools::ApplyProperties() {
    nfs::World* world = World();
    if (!world || Renderer().CollisionSelection().empty() || !host_.properties) return false;

    std::wstring text;
    if (host_.properties->ReadText(kSurfaceField, text))
        for (u8 s = 0; s < nfs::kSurfaceCount; ++s)
            if (text == SurfaceLabel(s)) {
                bool differs = false;
                for (const CollisionPick& pick : Renderer().CollisionSelection())
                    differs = differs ||
                              world->Loaded()[pick.section]->collision[pick.pack].polygons[pick.polygon].surface != s;
                if (differs) SetSelectionSurface(s);
                break;
            }
    if (host_.properties->ReadText(kRaceField, text)) {
        const size_t count = std::max<size_t>(world->RaceBarrierGroups().size(), 1);
        for (size_t g = 0; g < count; ++g)
            if (text == Widen(world->RaceBarrierGroupName((u8)g))) {
                bool differs = false;
                for (const CollisionPick& pick : Renderer().CollisionSelection())
                    differs = differs ||
                              world->Loaded()[pick.section]->collision[pick.pack].polygons[pick.polygon].raceGroup != g;
                if (differs) {
                    const size_t changed = editor_.SetRaceGroup(SelectionRefs(), (u8)g);
                    RefreshAfterEdit();
                    Status(Format(L"%zu polygons now in race group %s (not written yet).", changed,
                                  text.c_str()));
                }
                break;
            }
    }
    return true;
}

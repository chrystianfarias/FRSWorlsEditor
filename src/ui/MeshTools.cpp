#include "ui/MeshTools.h"

#include "core/Log.h"
#include "io/SelectionExport.h"
#include "io/GlbToObj.h"
#include "io/LightBake.h"
#include "io/PlacementWrite.h"
#include "online/Sketchfab.h"
#include "io/SelectionImport.h"
#include "nfs/World.h"
#include "render/Gizmo.h"
#include "ui/CollisionTools.h"
#include "ui/CrashReport.h"
#include "io/WorldFile.h"
#include "io/SectionEdits.h"
#include "ui/Resource.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace {

enum : int {
    kIdEdit      = 44501,
    kIdVertices  = 44502,
    kIdEdges     = 44503,
    kIdFaces     = 44504,
    kIdSelectAll = 44505,
    kIdFinish    = 44506,
    kIdDiscard   = 44507,
    kIdExtrude   = 44509,
    kIdFill      = 44510,
};

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

void Line(std::vector<WorldRenderer::OverlayVertex>& out, const Vec3& a, const Vec3& b, D3DCOLOR colour) {
    out.push_back({ a.x, a.y, a.z, colour });
    out.push_back({ b.x, b.y, b.z, colour });
}

// Pixels from (x, y) to the segment a-b on screen.
float SegmentDistance(const Vec2& a, const Vec2& b, float x, float y) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = dx * dx + dy * dy;
    float t = length > 1e-6f ? ((x - a.x) * dx + (y - a.y) * dy) / length : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float px = a.x + dx * t - x, py = a.y + dy * t - y;
    return std::sqrt(px * px + py * py);
}

bool SameShape(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3)) == 0);
}

std::wstring EditFolder() {
    wchar_t base[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
    std::wstring folder = std::wstring(base) + L"\\FRSWorldEditor";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\meshedit";
    ::CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup and commands
// ---------------------------------------------------------------------------

void MeshTools::OnCreate() {
    host_.viewport->AddTool(this);
    undo::Register(this);
    if (host_.menu) {
        popup_ = ::CreatePopupMenu();
        ::AppendMenuW(popup_, MF_STRING, kIdEdit, L"&Edit Mesh or Collision of the Selection\tTab");
        ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(popup_, MF_STRING, kIdVertices, L"Pick &Vertices\t1");
        ::AppendMenuW(popup_, MF_STRING, kIdEdges, L"Pick E&dges\t2");
        ::AppendMenuW(popup_, MF_STRING, kIdFaces, L"Pick &Faces\t3");
        ::AppendMenuW(popup_, MF_STRING, kIdSelectAll, L"Select &All\tCtrl+A");
        ::AppendMenuW(popup_, MF_STRING, kIdExtrude, L"E&xtrude Picked Collision Edges\tE");
        ::AppendMenuW(popup_, MF_STRING, kIdFill, L"&Fill Between Picked Collision Edges or Corners\tF");
        ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(popup_, MF_STRING, ID_GIZMO_MOVE, L"Gizmo: &Move");
        ::AppendMenuW(popup_, MF_STRING, ID_GIZMO_ROTATE, L"Gizmo: &Rotate");
        ::AppendMenuW(popup_, MF_STRING, ID_GIZMO_SCALE, L"Gizmo: &Scale");
        ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(popup_, MF_STRING, kIdFinish, L"Finish and &Write\tTab");
        ::AppendMenuW(popup_, MF_STRING, kIdDiscard, L"&Discard the Edit");
        ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(popup_, MF_STRING, kBakeCommand, L"Bake Night &Lighting...\tCtrl+Shift+L");
        const int count = ::GetMenuItemCount(host_.menu);
        ::InsertMenuW(host_.menu, count > 0 ? (UINT)(count - 1) : 0, MF_BYPOSITION | MF_POPUP,
                      (UINT_PTR)popup_, L"&Mesh");
        if (host_.window) ::DrawMenuBar(host_.window);
    }
    if (host_.accelerators && *host_.accelerators) {
        const int count = ::CopyAcceleratorTableW(*host_.accelerators, nullptr, 0);
        std::vector<ACCEL> table((size_t)count);
        ::CopyAcceleratorTableW(*host_.accelerators, table.data(), count);
        table.push_back({ FVIRTKEY | FCONTROL, 'A', (WORD)kIdSelectAll });
        table.push_back({ FVIRTKEY | FCONTROL | FSHIFT, 'L', (WORD)kBakeCommand });
        if (HACCEL extended = ::CreateAcceleratorTableW(table.data(), (int)table.size()))
            *host_.accelerators = extended;
    }
}

bool MeshTools::OnCommand(int id) {
    // While editing, the keys 1 2 3 pick what a click takes; the toolbar's
    // buttons with the same commands keep choosing the gizmo.
    if (active_ && (id == ID_GIZMO_MOVE || id == ID_GIZMO_ROTATE || id == ID_GIZMO_SCALE)) {
        const int key = id == ID_GIZMO_MOVE ? '1' : id == ID_GIZMO_ROTATE ? '2' : '3';
        if (::GetKeyState(key) < 0) {
            SetPickMode(id == ID_GIZMO_MOVE ? Pick::Vertex : id == ID_GIZMO_ROTATE ? Pick::Edge : Pick::Face);
            return true;
        }
        return false;
    }

    switch (id) {
        case kIdEdit:      if (!active_) Begin(); return true;
        case kIdVertices:  SetPickMode(Pick::Vertex); return true;
        case kIdEdges:     SetPickMode(Pick::Edge); return true;
        case kIdFaces:     SetPickMode(Pick::Face); return true;
        case kIdSelectAll: if (!active_) return false; SelectAll(); return true;
        case kIdExtrude:   ExtrudeCollision(); return true;
        case kIdFill:      FillCollision(); return true;
        case kIdFinish:
            if (active_) Finish();
            else if (host_.otherActive && host_.otherActive()) host_.otherEnd();
            return true;
        case kIdDiscard:
            if (active_) { Stop(true); Status(L"Mesh edit discarded."); }
            return true;
        default:
            return false;
    }
}

void MeshTools::UpdateMenuChecks(HMENU menu) {
    const auto check = [menu](int id, bool on) {
        ::CheckMenuItem(menu, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    };
    check(kIdEdit, active_);
    check(kIdVertices, active_ && pick_ == Pick::Vertex);
    check(kIdEdges, active_ && pick_ == Pick::Edge);
    check(kIdFaces, active_ && pick_ == Pick::Face);
    ::EnableMenuItem(menu, (UINT)kIdExtrude, MF_BYCOMMAND | (active_ && collision_ ? MF_ENABLED : MF_GRAYED));
    ::EnableMenuItem(menu, (UINT)kIdFill, MF_BYCOMMAND | (active_ && collision_ ? MF_ENABLED : MF_GRAYED));
    for (int id : { kIdVertices, kIdEdges, kIdFaces, kIdSelectAll, kIdFinish, kIdDiscard })
        ::EnableMenuItem(menu, (UINT)id, MF_BYCOMMAND | (active_ ? MF_ENABLED : MF_GRAYED));
    const GizmoMode mode = host_.viewport->GetGizmoMode();
    check(ID_GIZMO_MOVE, mode == GizmoMode::Move);
    check(ID_GIZMO_ROTATE, mode == GizmoMode::Rotate);
    check(ID_GIZMO_SCALE, mode == GizmoMode::Scale);
}

// ---------------------------------------------------------------------------
// Starting and finishing
// ---------------------------------------------------------------------------

bool MeshTools::Begin() {
    // A visibility zone picked while they are shown: its corners are what is edited.
    if (host_.otherBegin && host_.otherBegin()) {
        if (host_.menuChecks) host_.menuChecks();
        return true;
    }
    nfs::World* world = World();
    const auto& selection = Renderer().Selection();
    // The collision shown: it is what the edit mode edits -- all of it
    // around the camera, nothing needs picking first. Picking first is only
    // for an object's mesh.
    if (Renderer().CollisionVisible() || (!Renderer().CollisionSelection().empty() && selection.empty())) {
        LOG_INFO("edit mode: collision around the camera (%zu polygons were picked)",
                 Renderer().CollisionSelection().size());
        return BeginCollisionArea();
    }
    if (selection.size() != 1 || !selection.front().object)
        LOG_INFO("edit mode refused: %zu objects selected, %zu collision polygons, collision %s", selection.size(),
                 Renderer().CollisionSelection().size(), Renderer().CollisionVisible() ? "shown" : "hidden");
    if (!world || selection.size() != 1 || !selection.front().object) {
        Status(L"Select one object first (hide the collision with Ctrl+K to pick meshes), then Tab.");
        return false;
    }
    const PickResult& picked = selection.front();
    const nfs::SolidObject& model = *picked.object;
    if (model.vertices.empty() || model.indices.size() < 3) {
        Status(L"That object has no mesh to edit.");
        return false;
    }

    modelHash_ = model.hash;
    modelName_ = model.name;
    transform_ = picked.transform;
    inverse_ = InverseAffine(transform_);

    world_.resize(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) world_[i] = transform_.TransformPoint(model.vertices[i].position);
    original_ = world_;
    originalLocal_.resize(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) originalLocal_[i] = model.vertices[i].position;

    // Vertices standing at one spot -- split by texture or normal -- move as one.
    std::map<std::tuple<long, long, long>, u32> spot;
    weldOf_.assign(world_.size(), 0);
    weldVertices_.clear();
    for (size_t i = 0; i < world_.size(); ++i) {
        const auto key = std::make_tuple(std::lround(world_[i].x * 1000.0f), std::lround(world_[i].y * 1000.0f),
                                         std::lround(world_[i].z * 1000.0f));
        const auto found = spot.find(key);
        if (found != spot.end()) {
            weldOf_[i] = found->second;
            weldVertices_[found->second].push_back((u32)i);
        } else {
            const u32 weld = (u32)weldVertices_.size();
            spot.emplace(key, weld);
            weldOf_[i] = weld;
            weldVertices_.push_back({ (u32)i });
        }
    }
    std::set<std::pair<u32, u32>> edgeSet;
    faces_.clear();
    for (size_t t = 0; t + 2 < model.indices.size(); t += 3) {
        const u16 a = model.indices[t], b = model.indices[t + 1], c = model.indices[t + 2];
        if (a >= world_.size() || b >= world_.size() || c >= world_.size()) continue;
        const u32 wa = weldOf_[a], wb = weldOf_[b], wc = weldOf_[c];
        if (wa == wb || wb == wc || wa == wc) continue;
        faces_.push_back({ wa, wb, wc });
        for (auto [p, q] : { std::make_pair(wa, wb), std::make_pair(wb, wc), std::make_pair(wc, wa) })
            edgeSet.insert({ std::min(p, q), std::max(p, q) });
    }
    edges_.assign(edgeSet.begin(), edgeSet.end());

    // Every place that draws this model changes with it.
    placements_ = 0;
    for (const auto& section : world->Loaded()) {
        for (const auto& instance : section->scenery.instances)
            placements_ += section->scenery.ModelHash(instance) == modelHash_;
        for (const auto& placed : section->placements) placements_ += placed.instance.modelHash == modelHash_;
    }

    selected_.clear();
    undo_.clear();
    undoStamps_.clear();
    dragging_ = false;
    active_ = true;
    Renderer().ClearSelection();
    ::SetFocus(host_.viewport->Handle());
    UpdateOverlay();
    Status(Format(L"Editing %s: %zu vertices, %zu faces%s. 1/2/3 vertices/edges/faces, Tab finishes.",
                  Widen(modelName_).c_str(), weldVertices_.size(), faces_.size(),
                  placements_ > 1 ? Format(L" -- placed %zu times, all of them change", placements_).c_str() : L""));
    if (host_.menuChecks) host_.menuChecks();
    LOG_INFO("mesh edit: %s, %zu vertices (%zu welded), %zu faces, %zu placements", modelName_.c_str(),
             world_.size(), weldVertices_.size(), faces_.size(), placements_);
    return true;
}

void MeshTools::Stop(bool restore) {
    if (!active_) return;
    boxPending_ = boxActive_ = false;
    if (collision_) { StopCollision(restore); return; }
    if (restore && !SameShape(world_, original_)) {
        world_ = original_;
        Push();
    }
    active_ = false;
    dragging_ = false;
    selected_.clear();
    undo_.clear();
    undoStamps_.clear();
    Renderer().SetToolOverlay({}, {}, this);
    if (host_.menuChecks) host_.menuChecks();
}

void MeshTools::Finish() {
    nfs::World* world = World();
    if (!world || !active_) return;
    if (collision_) {
        const bool changed = collisionSteps_;
        StopCollision(false);
        Status(changed ? L"Collision reshaped -- Ctrl+Z takes a step back, the next save (Ctrl+Shift+S) writes it."
                       : L"Collision unchanged.");
        return;
    }
    if (SameShape(world_, original_)) {
        Stop(false);
        Status(L"Mesh unchanged.");
        return;
    }

    // The model as edited, written out as the Blender round trip writes it.
    const nfs::SolidObject* model = world->FindModel(modelHash_);
    const std::wstring folder = EditFolder();
    if (!model || folder.empty() || !host_.importQuietly) {
        Status(L"The edit could not be written: no folder to export it to.");
        return;
    }
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const std::wstring objPath = folder + Format(L"\\%s-%04u%02u%02u-%02u%02u%02u.obj", Widen(modelName_).c_str(),
                                                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                                                 now.wSecond);
    PickResult picked;
    picked.object = model;
    picked.transform = transform_;
    ExportSummary summary;
    if (!ExportSelection(*world, { picked }, objPath, world->Region(), summary)) {
        Status(L"The edit could not be exported; the log has why. Still editing.");
        return;
    }
    // Leave edit mode first: the import reopens the region.
    active_ = false;
    selected_.clear();
    undo_.clear();
    undoStamps_.clear();
    Renderer().SetToolOverlay({}, {}, this);
    LOG_INFO("mesh edit: %s written through %S", modelName_.c_str(), objPath.c_str());
    host_.importQuietly(objPath);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void MeshTools::Push() {
    if (collision_) { PushCollision(); return; }
    nfs::World* world = World();
    if (!world) return;
    std::vector<Vec3> local(world_.size());
    // A vertex that has not moved keeps its stored position to the bit: the
    // way to world space and back would leave float noise on all of them.
    for (size_t i = 0; i < world_.size(); ++i)
        local[i] = i < original_.size() && std::memcmp(&world_[i], &original_[i], sizeof(Vec3)) == 0
                       ? originalLocal_[i] : inverse_.TransformPoint(world_[i]);
    for (size_t s = 0; s < world->Loaded().size(); ++s)
        for (nfs::SolidList& list : world->MutableLoaded(s)->solidLists)
            for (nfs::SolidObject& object : list.objects) {
                if (object.hash != modelHash_ || object.vertices.size() != local.size()) continue;
                for (size_t i = 0; i < local.size(); ++i) object.vertices[i].position = local[i];
            }
    Renderer().UpdateModelPositions(modelHash_, local);
}

std::vector<u32> MeshTools::SelectedWelds() const {
    std::set<u32> welds;
    for (u32 item : selected_) {
        if (pick_ == Pick::Vertex) welds.insert(item);
        else if (pick_ == Pick::Edge && item < edges_.size()) { welds.insert(edges_[item].first); welds.insert(edges_[item].second); }
        else if (pick_ == Pick::Face && item < faces_.size()) for (u32 w : faces_[item]) welds.insert(w);
    }
    return { welds.begin(), welds.end() };
}

void MeshTools::SetPickMode(Pick mode) {
    if (!active_) return;
    // The selection carries over as the welds it covers, where it can.
    const std::vector<u32> welds = SelectedWelds();
    const std::set<u32> weldSet(welds.begin(), welds.end());
    pick_ = mode;
    selected_.clear();
    if (mode == Pick::Vertex) selected_.insert(welds.begin(), welds.end());
    else if (mode == Pick::Edge) {
        for (size_t e = 0; e < edges_.size(); ++e)
            if (weldSet.count(edges_[e].first) && weldSet.count(edges_[e].second)) selected_.insert((u32)e);
    } else {
        for (size_t f = 0; f < faces_.size(); ++f)
            if (weldSet.count(faces_[f][0]) && weldSet.count(faces_[f][1]) && weldSet.count(faces_[f][2]))
                selected_.insert((u32)f);
    }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(mode == Pick::Vertex ? L"Picking vertices" : mode == Pick::Edge ? L"Picking edges" : L"Picking faces");
}

void MeshTools::SelectAll() {
    selected_.clear();
    const size_t count = pick_ == Pick::Vertex ? weldVertices_.size() : pick_ == Pick::Edge ? edges_.size() : faces_.size();
    for (size_t i = 0; i < count; ++i) selected_.insert((u32)i);
    UpdateOverlay();
}

bool MeshTools::UndoLatest() {
    if (!active_ || undo_.empty()) return false;
    Undo();
    return true;
}

void MeshTools::Undo() {
    if (dragging_) return;
    if (undo_.empty()) { Status(L"Nothing to undo in this mesh edit."); return; }
    world_ = std::move(undo_.back());
    undo_.pop_back();
    if (!undoStamps_.empty()) undoStamps_.pop_back();
    Push();
    UpdateOverlay();
    Status(L"Mesh edit: one step back.");
}

bool MeshTools::GizmoPivot(Vec3& pivot) {
    if (!active_ || selected_.empty()) return false;
    if (collision_ && !dragging_ && Collision() && Collision()->Editor().StepsUndone() != collisionUndone_) CatchUp();
    if (dragging_) { pivot = pivotStart_; return true; }
    const std::vector<u32> welds = SelectedWelds();
    if (welds.empty()) return false;
    Vec3 sum{};
    for (u32 w : welds) sum = sum + WeldPosition(w);
    pivot = sum * (1.0f / (float)welds.size());
    return true;
}

void MeshTools::GizmoChanged(const Mat4& transform, bool done, bool keep) {
    if (!active_) return;
    if (!dragging_) {
        GizmoPivot(pivotStart_);
        if (collision_ && (!Collision() || !Collision()->Editor().BeginReshape(collisionRefs_))) return;
        dragStart_ = world_;
        dragging_ = true;
    }
    if (done && !keep) {
        world_ = dragStart_;
    } else {
        world_ = dragStart_;
        for (u32 weld : SelectedWelds())
            for (u32 v : weldVertices_[weld]) world_[v] = transform.TransformPoint(dragStart_[v] - pivotStart_);
    }
    Push();
    if (done && collision_) {
        dragging_ = false;
        const bool changed = keep && !SameShape(world_, dragStart_);
        CollisionTools* tools = Collision();
        if (tools) {
            tools->Editor().EndMove(changed);
            if (changed) collisionSteps_ = true;
            else ReadCollision();
            tools->RefreshAfterEdit();
            collisionUndone_ = tools->Editor().StepsUndone();
        }
        dragStart_.clear();
    } else if (done) {
        dragging_ = false;
        if (keep && !SameShape(world_, dragStart_)) {
            undo_.push_back(std::move(dragStart_));
            undoStamps_.push_back(undo::NextStamp());
            if (undo_.size() > 100) { undo_.erase(undo_.begin()); undoStamps_.erase(undoStamps_.begin()); }
        }
        dragStart_.clear();
    }
    UpdateOverlay();
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

bool MeshTools::MouseDown(int x, int y, WPARAM keys) {
    if (!active_) return false;
    if (collision_ && Collision() && Collision()->Editor().StepsUndone() != collisionUndone_) CatchUp();
    const bool add = (keys & MK_CONTROL) != 0;
    const GizmoView view = host_.viewport->ToolView();
    const Vec3 eye = host_.viewport->GetCamera().Position();
    const float fx = (float)x, fy = (float)y;

    long best = -1;
    float bestScore = 1e30f;
    if (pick_ == Pick::Vertex) {
        for (size_t w = 0; w < weldVertices_.size(); ++w) {
            Vec2 pixel;
            if (!GizmoProject(view, WeldPosition((u32)w), pixel)) continue;
            const float d = std::hypot(pixel.x - fx, pixel.y - fy);
            if (d > 10.0f) continue;
            // Nearest on screen, the nearer of two at the same spot.
            const float score = d + 0.001f * Length(WeldPosition((u32)w) - eye);
            if (score < bestScore) { bestScore = score; best = (long)w; }
        }
    } else if (pick_ == Pick::Edge) {
        for (size_t e = 0; e < edges_.size(); ++e) {
            Vec2 a, b;
            if (!GizmoProject(view, WeldPosition(edges_[e].first), a) ||
                !GizmoProject(view, WeldPosition(edges_[e].second), b))
                continue;
            const float d = SegmentDistance(a, b, fx, fy);
            if (d > 7.0f) continue;
            const Vec3 middle = (WeldPosition(edges_[e].first) + WeldPosition(edges_[e].second)) * 0.5f;
            const float score = d + 0.001f * Length(middle - eye);
            if (score < bestScore) { bestScore = score; best = (long)e; }
        }
    } else {
        const Ray ray = host_.viewport->RayAt(x, y);
        for (size_t f = 0; f < faces_.size(); ++f) {
            const Vec3 a = WeldPosition(faces_[f][0]), b = WeldPosition(faces_[f][1]), c = WeldPosition(faces_[f][2]);
            float hit = IntersectRayTriangle(ray, a, b, c);
            if (hit < 0.0f) hit = IntersectRayTriangle(ray, a, c, b);   // either side
            if (hit >= 0.0f && hit < bestScore) { bestScore = hit; best = (long)f; }
        }
    }

    if (best < 0) {
        // Nothing under the press: the start of a box, or a plain click that
        // drops the selection when it is let go without moving.
        boxPending_ = true;
        boxActive_ = false;
        boxAdd_ = add;
        boxSubtract_ = (keys & MK_SHIFT) != 0 && !add;
        boxX0_ = boxX1_ = x;
        boxY0_ = boxY1_ = y;
        ::SetCapture(host_.viewport->Handle());
        return true;
    } else if (add) {
        if (!selected_.erase((u32)best)) selected_.insert((u32)best);
    } else {
        selected_ = { (u32)best };
    }
    // A collision quad is one face to the person editing it: both halves.
    if (pick_ == Pick::Face && best >= 0 && (size_t)best < facePolygon_.size()) {
        const bool on = selected_.count((u32)best) != 0;
        for (size_t f = 0; f < facePolygon_.size(); ++f)
            if (facePolygon_[f] == facePolygon_[(size_t)best]) {
                if (on) selected_.insert((u32)f);
                else selected_.erase((u32)f);
            }
    }
    UpdateOverlay();
    return true;
}

void MeshTools::MouseMove(int x, int y) {
    if (!active_ || !boxPending_) return;
    boxX1_ = x;
    boxY1_ = y;
    if (!boxActive_ && (std::abs(x - boxX0_) > 4 || std::abs(y - boxY0_) > 4)) boxActive_ = true;
}

bool MeshTools::MouseUp(int x, int y, WPARAM keys) {
    (void)keys;
    if (!active_ || !boxPending_) return false;
    boxPending_ = false;
    if (::GetCapture() == host_.viewport->Handle()) ::ReleaseCapture();
    if (boxActive_) {
        boxActive_ = false;
        SelectBox(boxX0_, boxY0_, x, y, boxAdd_, boxSubtract_);
    } else if (!boxAdd_) {
        selected_.clear();
    }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    return true;
}

bool MeshTools::BoxRect(int& left, int& top, int& right, int& bottom) const {
    if (!active_ || !boxActive_) return false;
    left = std::min(boxX0_, boxX1_);
    right = std::max(boxX0_, boxX1_);
    top = std::min(boxY0_, boxY1_);
    bottom = std::max(boxY0_, boxY1_);
    return true;
}

void MeshTools::SelectBox(int x0, int y0, int x1, int y1, bool add, bool subtract) {
    const float left = (float)std::min(x0, x1), right = (float)std::max(x0, x1);
    const float top = (float)std::min(y0, y1), bottom = (float)std::max(y0, y1);
    const GizmoView view = host_.viewport->ToolView();
    // Inside the box and in front of the camera; hidden ones count too, as
    // Blender's X-ray does -- what the box covers is what it takes.
    std::vector<char> inside(weldVertices_.size(), 0);
    for (size_t w = 0; w < weldVertices_.size(); ++w) {
        Vec2 pixel;
        if (!GizmoProject(view, WeldPosition((u32)w), pixel)) continue;
        inside[w] = pixel.x >= left && pixel.x <= right && pixel.y >= top && pixel.y <= bottom;
    }
    std::vector<u32> hits;
    if (pick_ == Pick::Vertex) {
        for (size_t w = 0; w < inside.size(); ++w) if (inside[w]) hits.push_back((u32)w);
    } else if (pick_ == Pick::Edge) {
        for (size_t e = 0; e < edges_.size(); ++e)
            if (inside[edges_[e].first] && inside[edges_[e].second]) hits.push_back((u32)e);
    } else {
        std::vector<char> faceIn(faces_.size(), 0);
        for (size_t f = 0; f < faces_.size(); ++f)
            faceIn[f] = inside[faces_[f][0]] && inside[faces_[f][1]] && inside[faces_[f][2]];
        // A collision quad comes whole or not at all.
        if (!facePolygon_.empty()) {
            std::map<u32, bool> whole;
            for (size_t f = 0; f < faces_.size(); ++f) {
                auto found = whole.find(facePolygon_[f]);
                whole[facePolygon_[f]] = (found == whole.end() ? true : found->second) && faceIn[f];
            }
            for (size_t f = 0; f < faces_.size(); ++f) faceIn[f] = whole[facePolygon_[f]];
        }
        for (size_t f = 0; f < faces_.size(); ++f) if (faceIn[f]) hits.push_back((u32)f);
    }
    if (!add && !subtract) selected_.clear();
    for (u32 hit : hits) {
        if (subtract) selected_.erase(hit);
        else selected_.insert(hit);
    }
    Status(Format(L"%zu %s in the box, %zu picked. Ctrl+drag adds, Shift+drag takes away.", hits.size(),
                  pick_ == Pick::Vertex ? L"vertices" : pick_ == Pick::Edge ? L"edges" : L"faces", selected_.size()));
}

bool MeshTools::KeyDown(WPARAM key, bool repeat) {
    if (!active_) {
        if (key == VK_TAB && !repeat && ::GetKeyState(VK_CONTROL) >= 0) {
            if (host_.otherActive && host_.otherActive()) { host_.otherEnd(); return true; }
            return Begin() || true;
        }
        return false;
    }
    switch (key) {
        case VK_TAB:
            if (!repeat && !dragging_) Finish();
            return true;
        case VK_ESCAPE:
            selected_.clear();
            UpdateOverlay();
            return true;
        case 'E':
            if (collision_ && !repeat && ::GetKeyState(VK_CONTROL) >= 0) { ExtrudeCollision(); return true; }
            return false;
        case 'F':
            if (collision_ && !repeat && ::GetKeyState(VK_CONTROL) >= 0) { FillCollision(); return true; }
            return false;
        case 'Z':
            if (::GetKeyState(VK_CONTROL) < 0) {
                undo::UndoLatest();
                if (collision_) CatchUp();
                return true;
            }
            return false;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Drawing, status and properties
// ---------------------------------------------------------------------------

void MeshTools::UpdateOverlay() {
    if (!active_) { Renderer().SetToolOverlay({}, {}, this); return; }
    std::vector<WorldRenderer::OverlayVertex> triangles, lines;
    const D3DCOLOR wire = 0x9040C0FF, hot = 0xFFFFA020, fill = 0x80FF9020;

    // The whole wire, unless it is huge: then the selection alone.
    const bool wholeWire = edges_.size() <= 30000;
    if (wholeWire)
        for (const auto& [a, b] : edges_) Line(lines, WeldPosition(a), WeldPosition(b), wire);

    const Vec3 eye = host_.viewport->GetCamera().Position();
    const auto dot = [&](const Vec3& p, D3DCOLOR colour) {
        const float s = std::clamp(Length(p - eye) * 0.004f, 0.02f, 0.5f);
        Line(lines, p + Vec3{ -s, 0, 0 }, p + Vec3{ s, 0, 0 }, colour);
        Line(lines, p + Vec3{ 0, -s, 0 }, p + Vec3{ 0, s, 0 }, colour);
        Line(lines, p + Vec3{ 0, 0, -s }, p + Vec3{ 0, 0, s }, colour);
    };
    if (pick_ == Pick::Vertex && weldVertices_.size() <= 20000)
        for (size_t w = 0; w < weldVertices_.size(); ++w)
            if (!selected_.count((u32)w)) dot(WeldPosition((u32)w), 0xC0FFFFFF);

    for (u32 item : selected_) {
        if (pick_ == Pick::Vertex && item < weldVertices_.size()) {
            dot(WeldPosition(item), hot);
        } else if (pick_ == Pick::Edge && item < edges_.size()) {
            Line(lines, WeldPosition(edges_[item].first), WeldPosition(edges_[item].second), hot);
        } else if (pick_ == Pick::Face && item < faces_.size()) {
            const Vec3 a = WeldPosition(faces_[item][0]), b = WeldPosition(faces_[item][1]), c = WeldPosition(faces_[item][2]);
            for (const Vec3* p : { &a, &b, &c }) triangles.push_back({ p->x, p->y, p->z, fill });
            Line(lines, a, b, hot); Line(lines, b, c, hot); Line(lines, c, a, hot);
        }
    }
    Renderer().SetToolOverlay(std::move(triangles), std::move(lines), this);
}

bool MeshTools::StatusText(std::wstring& text) const {
    if (!active_) return false;
    text = Format(collision_ ? L"Editing %s -- %s, %zu picked. Click picks (Ctrl adds), the gizmo moves/turns/scales, "
                               L"Ctrl+Z steps back, Tab finishes, Mesh > Discard puts it back."
                             : L"Mesh edit %s -- %s, %zu picked. Click picks (Ctrl adds), the gizmo moves/turns/scales, "
                               L"Ctrl+Z steps back, Tab writes, Mesh > Discard drops it.",
                  Widen(modelName_).c_str(),
                  pick_ == Pick::Vertex ? L"vertices" : pick_ == Pick::Edge ? L"edges" : L"faces",
                  selected_.size());
    return true;
}

bool MeshTools::FillProperties(PropertiesPanel::View& view) {
    if (!active_) return false;
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value,
                             const wchar_t* description) {
        PropertiesPanel::Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        view.rows.push_back(std::move(row));
    };
    view.mode = PropertiesPanel::Mode::Extension;
    view.title = L"Editing " + Widen(modelName_);
    view.moveLabel = L"Finish and write (Tab)";
    view.removeLabel.clear();
    add(L"Mesh", L"Model", Widen(modelName_), L"The model whose shape is being edited.");
    add(L"Mesh", L"Vertices", std::to_wstring(weldVertices_.size()),
        L"Points of the mesh; copies split for texture or shading move together.");
    add(L"Mesh", L"Faces", std::to_wstring(faces_.size()), L"Triangles.");
    if (collision_)
        add(L"Mesh", L"Polygons", std::to_wstring(collisionRefs_.size()),
            L"The picked polygons, the ones sharing a corner with them, and their copies in neighbouring sections.");
    else
        add(L"Mesh", L"Placed", std::to_wstring(placements_),
            L"Every placement of this model draws the same mesh: all of them change.");
    add(L"Selection", L"Picking", pick_ == Pick::Vertex ? L"Vertices (1)" : pick_ == Pick::Edge ? L"Edges (2)" : L"Faces (3)",
        L"1, 2 and 3 switch; the selection carries over.");
    add(L"Selection", L"Picked", std::to_wstring(selected_.size()), L"Ctrl+click adds or removes, Ctrl+A takes all.");
    Vec3 pivot;
    if (const_cast<MeshTools*>(this)->GizmoPivot(pivot))
        add(L"Selection", L"Centre", Format(L"%.2f, %.2f, %.2f", pivot.x, pivot.y, pivot.z),
            L"Where the gizmo stands: the middle of the picked vertices.");
    if (!collision_) add(L"Edit", L"Steps", std::to_wstring(undo_.size()), L"Ctrl+Z takes one back.");
    add(L"Edit", L"Changed",
        !SameShape(world_, original_) ? (collision_ ? L"Yes -- the next save writes it" : L"Yes -- Tab writes it") : L"No",
        L"");
    return true;
}

void MeshTools::Tick(float deltaSeconds) {
    FollowCamera();
    // An undo from the menu or another tool: the session catches up.
    if (active_ && collision_ && !dragging_ && Collision() &&
        Collision()->Editor().StepsUndone() != collisionUndone_)
        CatchUp();
    static float clock = 0.0f;
    static bool reported = false;
    clock += deltaSeconds;
    nfs::World* world = World();

    // FRS_VIS_REPORT="x y;x y;...": the sections the game loads standing at
    // each point (its visibility lists), by name.
    static bool visReported = false;
    if (!visReported && clock > 5.0f && world) {
        if (const char* list = std::getenv("FRS_VIS_REPORT")) {
            visReported = true;
            std::string text = list;
            size_t at = 0;
            while (at < text.size()) {
                const size_t end = text.find(';', at);
                const std::string item = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? text.size() : end + 1;
                float x = 0, y = 0;
                if (std::sscanf(item.c_str(), "%f %f", &x, &y) != 2) continue;
                std::vector<u32> numbers;
                const bool claimed = world->Visibility().VisibleAt({ x, y, 0.0f }, numbers);
                std::string names;
                for (u32 n : numbers)
                    for (const auto& s : world->Table().Sections())
                        if (s.number == n) { names += s.name + " "; break; }
                LOG_INFO("vis report: (%.0f %.0f) %s %zu sections: %s", x, y, claimed ? "in a zone," : "NO ZONE,",
                         numbers.size(), names.c_str());
            }
        }
    }

    // FRS_PACK_REPORT="x0 y0 x1 y1": every collision pack that covers that
    // area, as shipped and as now -- where new collision went, and whether it
    // lies inside what the section covered in the game.
    static bool packsReported = false;
    if (!packsReported && clock > 25.0f && world) {
        if (const char* area = std::getenv("FRS_PACK_REPORT")) {
            packsReported = true;
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            std::sscanf(area, "%f %f %f %f", &x0, &y0, &x1, &y1);
            for (u32 s = 0; s < (u32)world->Loaded().size(); ++s) {
                const auto& section = *world->Loaded()[s];
                if (!section.info) continue;
                std::vector<nfs::CollisionPack> shipped;
                ShippedCollisionPacks(*world, *section.info, shipped);
                for (u32 p = 0; p < (u32)section.collision.size(); ++p) {
                    const nfs::CollisionPack& pack = section.collision[p];
                    Aabb now, before;
                    size_t inArea = 0;
                    for (const auto& polygon : pack.polygons) {
                        bool in = false;
                        for (const Vec3& c : polygon.corners) {
                            if (!polygon.barrier) now.Add(c);
                            in |= c.x >= x0 && c.x <= x1 && c.y >= y0 && c.y <= y1;
                        }
                        inArea += in;
                    }
                    if (!inArea) continue;
                    // Every barrier in the area: where it runs and which side it holds.
                    for (size_t b = 0; b < pack.polygons.size(); ++b) {
                        const auto& polygon = pack.polygons[b];
                        if (!polygon.barrier) continue;
                        const Vec3 &a = polygon.corners[0], &e = polygon.corners[2];
                        const bool in = (a.x >= x0 && a.x <= x1 && a.y >= y0 && a.y <= y1) ||
                                        (e.x >= x0 && e.x <= x1 && e.y >= y0 && e.y <= y1);
                        if (!in) continue;
                        // Left of the direction from a to e is the side it blocks.
                        const float nx = -(e.y - a.y), ny = e.x - a.x;
                        LOG_INFO("pack report:   %s #%zu barrier (%.2f %.2f)->(%.2f %.2f) blocks toward (%.2f %.2f) "
                                 "group %u surface %u", section.info->name.c_str(), b, a.x, a.y, e.x, e.y, nx, ny,
                                 polygon.raceGroup, polygon.surface);
                    }
                    if (p < shipped.size())
                        for (const auto& polygon : shipped[p].polygons)
                            if (!polygon.barrier) for (const Vec3& c : polygon.corners) before.Add(c);
                    LOG_INFO("pack report: %s pack %u: %zu polygons (%zu shipped), %zu in the area; ground now "
                             "(%.0f %.0f %.0f)-(%.0f %.0f %.0f), shipped (%.0f %.0f %.0f)-(%.0f %.0f %.0f); grid %ux%u "
                             "at (%.0f %.0f); section centre (%.0f %.0f %.0f)",
                             section.info->name.c_str(), p, pack.polygons.size(),
                             p < shipped.size() ? shipped[p].polygons.size() : 0, inArea, now.min.x, now.min.y,
                             now.min.z, now.max.x, now.max.y, now.max.z, before.min.x, before.min.y, before.min.z,
                             before.max.x, before.max.y, before.max.z, pack.gridWidth, pack.gridHeight,
                             pack.gridOrigin.x, pack.gridOrigin.y, section.info->position.x,
                             section.info->position.y, section.info->position.z);
                }
            }
            // Every section: its shipped ground's extent, to see the map's squares.
            for (const auto& entry : world->Table().Sections()) {
                std::vector<nfs::CollisionPack> shipped;
                ShippedCollisionPacks(*world, entry, shipped);
                Aabb ground;
                size_t polygons = 0;
                for (const auto& pack : shipped)
                    for (const auto& polygon : pack.polygons) {
                        ++polygons;
                        if (!polygon.barrier) for (const Vec3& c : polygon.corners) ground.Add(c);
                    }
                LOG_INFO("section grid: %s order %u centre (%.0f %.0f) packs %zu polygons %zu ground (%.0f %.0f)-(%.0f %.0f)",
                         entry.name.c_str(), entry.order, entry.position.x, entry.position.y, shipped.size(), polygons,
                         ground.Valid() ? ground.min.x : 0.0f, ground.Valid() ? ground.min.y : 0.0f,
                         ground.Valid() ? ground.max.x : 0.0f, ground.Valid() ? ground.max.y : 0.0f);
            }
            LOG_INFO("pack report: done");
        }
    }

    // FRS_BARRIER_REPORT=1: how level the barriers stand, by the nearest
    // ground corner (the old way) and by the ground under each end.
    static bool barriersReported = false;
    if (!barriersReported && clock > 25.0f && world && std::getenv("FRS_BARRIER_REPORT")) {
        barriersReported = true;
        size_t barriers = 0, oldSteep = 0, newSteep = 0, ambiguous = 0, noGround = 0, byChain = 0;
        Vec3 example{};
        for (const auto& section : world->Loaded())
            for (const nfs::CollisionPack& pack : section->collision) {
                pack.ResolveBarriers();
                for (const nfs::CollisionPolygon& polygon : pack.polygons) {
                    if (!polygon.barrier) continue;
                    ++barriers;
                    const float za = pack.GroundHeightNear(polygon.corners[0], 0.0f);
                    const float zb = pack.GroundHeightNear(polygon.corners[2], za);
                    oldSteep += std::fabs(za - zb) > 2.0f;
                    Vec3 quad[4];
                    pack.BarrierQuad(polygon, quad);
                    newSteep += std::fabs(quad[0].z - quad[3].z) > 2.0f;
                    std::vector<float> ha, hb;
                    pack.GroundHeightsAt(polygon.corners[0], 0.75f, ha);
                    pack.GroundHeightsAt(polygon.corners[2], 0.75f, hb);
                    ambiguous += ha.size() > 1 && hb.size() > 1;
                    noGround += ha.empty() || hb.empty();
                    float aloneA = 0.0f, aloneB = 0.0f;
                    pack.BarrierEndsAlone(polygon, aloneA, aloneB);
                    if (std::fabs(aloneA + nfs::CollisionPack::kBarrierBelow - quad[0].z) > 2.0f) {
                        if (!byChain) example = { polygon.corners[0].x, polygon.corners[0].y, quad[0].z };
                        ++byChain;
                    }
                }
            }
        LOG_INFO("barrier report: %zu barriers took another level from their neighbours; one at (%.1f %.1f %.1f)",
                 byChain, example.x, example.y, example.z);
        // Which way round the game stores its ground, seen from above.
        size_t ccw = 0, cw = 0, flat = 0, quadsCcw = 0, quadsCw = 0;
        for (const auto& section : world->Loaded())
            for (const nfs::CollisionPack& pack : section->collision)
                for (const nfs::CollisionPolygon& polygon : pack.polygons) {
                    if (polygon.barrier) continue;
                    const int n = polygon.quad ? 4 : 3;
                    double area = 0.0;
                    for (int i = 0; i < n; ++i) {
                        const Vec3& a = polygon.corners[i];
                        const Vec3& b = polygon.corners[(i + 1) % n];
                        area += (double)a.x * b.y - (double)b.x * a.y;
                    }
                    if (std::fabs(area) < 1e-3) ++flat;
                    else if (area > 0) { ++ccw; quadsCcw += polygon.quad; }
                    else { ++cw; quadsCw += polygon.quad; }
                }
        LOG_INFO("winding report: ground seen from above: %zu counter-clockwise (%zu quads), %zu clockwise "
                 "(%zu quads), %zu walls/flat", ccw, quadsCcw, cw, quadsCw, flat);
        if (std::getenv("FRS_BARRIER_LOOK") && byChain) {
            if (!host_.viewport->GetRenderer().CollisionVisible() && host_.window)
                ::PostMessageW(host_.window, WM_COMMAND, MAKEWPARAM(ID_VIEW_COLLISION, 0), 0);
            host_.viewport->GetCamera().SetPosition(example + Vec3{ -30.0f, -30.0f, 25.0f });
            host_.viewport->GetCamera().SetYawPitch(Radians(45.0f), Radians(-30.0f));
        }
        LOG_INFO("barrier report: %zu barriers; ends more than 2 m apart: %zu the old way, %zu now; "
                 "%zu over two grounds at both ends, %zu with an end over no ground",
                 barriers, oldSteep, newSteep, ambiguous, noGround);
    }
    if (reported || clock < 25.0f || !world || !std::getenv("FRS_ANIM_REPORT")) return;
    reported = true;
    // FRS_ANIM_REPORT=1: each texture animation, its frames, and the models
    // whose materials name one of them; also every texture called *WATER*.
    {
        for (const auto& [key, animation] : world->TextureAnimations()) {
            std::string frames;
            for (u32 frame : animation.frames) {
                const nfs::Texture* t = world->FindDisplayTexture(frame);
                char item[96];
                std::snprintf(item, sizeof(item), "%08X=%s ", frame, t ? t->name.c_str() : "?");
                frames += item;
            }
            LOG_INFO("anim report: %s key %08X fps %.0f frames %s", animation.name.c_str(), key, animation.fps,
                     frames.c_str());
            std::map<std::string, std::string> users;
            for (const auto& section : world->Loaded())
                for (const auto& list : section->solidLists)
                    for (const auto& object : list.objects)
                        for (u32 hash : object.textureHashes) {
                            if (hash == key) users[object.name] += "key ";
                            else if (std::find(animation.frames.begin(), animation.frames.end(), hash) !=
                                     animation.frames.end())
                                users[object.name] += "frame ";
                        }
            for (const auto& [name, how] : users)
                LOG_INFO("anim report:   used by %s (%s)", name.c_str(), how.c_str());
        }
        std::set<std::string> waters;
        for (const auto& section : world->Loaded())
            for (const auto& pack : section->texturePacks)
                for (const nfs::Texture& t : pack->Textures())
                    if (t.name.find("WATER") != std::string::npos) {
                        char item[96];
                        std::snprintf(item, sizeof(item), "%s %08X", t.name.c_str(), t.hash);
                        waters.insert(item);
                    }
        for (const std::string& w : waters) LOG_INFO("anim report: water texture %s", w.c_str());
        std::map<std::string, std::string> waterModels;
        for (const auto& section : world->Loaded())
            for (const auto& list : section->solidLists)
                for (const auto& object : list.objects)
                    for (u32 hash : object.textureHashes) {
                        const nfs::Texture* t = world->FindDisplayTexture(hash);
                        if (t && t->name.find("WATER") != std::string::npos) waterModels[object.name] += t->name + " ";
                    }
        for (const auto& [name, textures] : waterModels)
            LOG_INFO("anim report: model %s uses %s", name.c_str(), textures.c_str());
    }

}

// FRS_MESH_TOOLS_SELFTEST=1: the edit path on a street light, nothing written
// into the game -- the import is only planned.
void MeshTools::OnRegionLoaded() {
    Stop(false);
    static const bool selfTest = std::getenv("FRS_MESH_TOOLS_SELFTEST") != nullptr;
    nfs::World* world = World();

    // FRS_WORLDFILE_SELFTEST=<scratch folder>: the open world's files packed
    // into <scratch>\\test.frsworld (read only), read back and unpacked into
    // <scratch>\\unpacked; every file compared.
    if (const char* scratch = std::getenv("FRS_WORLDFILE_SELFTEST"); scratch && world) {
        const std::wstring folder = Widen(scratch);
        const std::wstring models = world->OverlayFolder();
        io::WorldFile file;
        std::wstring error;
        size_t packed = 0, unpacked = 0;
        bool ok = io::PackWorld(models, file, packed, error) && file.Save(folder + L"\\test.frsworld", error);
        io::WorldFile back;
        ok = ok && back.Load(folder + L"\\test.frsworld", error);
        ::CreateDirectoryW((folder + L"\\unpacked").c_str(), nullptr);
        ok = ok && io::UnpackWorld(back, folder + L"\\unpacked", folder + L"\\backup", unpacked, error);
        size_t same = 0;
        for (const std::string& name : back.Names()) {
            if (name.rfind("models/", 0) != 0) continue;
            std::wstring relative = Widen(name.substr(7));
            for (wchar_t& c : relative) if (c == L'/') c = L'\\';
            FILE* a = _wfopen((models + L"\\" + relative).c_str(), L"rb");
            FILE* b = _wfopen((folder + L"\\unpacked\\" + relative).c_str(), L"rb");
            bool equal = a && b;
            while (equal) {
                const int ca = std::fgetc(a), cb = std::fgetc(b);
                if (ca != cb) equal = false;
                if (ca == EOF || cb == EOF) break;
            }
            if (a) std::fclose(a);
            if (b) std::fclose(b);
            same += equal;
            if (!equal) LOG_ERROR("world file selftest: %S differs", relative.c_str());
        }
        LOG_INFO("world file selftest: %zu packed, %zu unpacked, %zu identical, names %zu, signature %s -- %s %S",
                 packed, unpacked, same, back.Names().size(), io::WorldSignature(models).c_str(),
                 ok && same == packed && unpacked == packed ? "PASSED" : "FAILED", error.c_str());
    }

    // FRS_CRASH_DEMO=<game folder>: the crash window, as if the game had just
    // died of an access violation.
    if (const char* folder = std::getenv("FRS_CRASH_DEMO")) {
        const std::wstring gameFolder = Widen(folder);
        FILETIME launched{};
        ::GetSystemTimeAsFileTime(&launched);
        ULARGE_INTEGER t{};
        t.LowPart = launched.dwLowDateTime; t.HighPart = launched.dwHighDateTime;
        t.QuadPart -= 3ull * 3600 * 10000000ull;   // three hours: some logs count as written by the run
        launched.dwLowDateTime = t.LowPart; launched.dwHighDateTime = t.HighPart;
        const std::wstring report = crash::BuildReport(gameFolder, launched, 0xC0000005);
        LOG_INFO("crash demo: report of %zu characters", report.size());
        crash::Show(host_.window, report, gameFolder);
    }

    // FRS_COLLISION_EDIT_SELFTEST=1: the collision edit mode on a quad of
    // ground -- a corner moved, undone, an edge extruded, undone.
    if (std::getenv("FRS_COLLISION_EDIT_SELFTEST") && world && Collision()) {
        bool ok = true;
        const auto check = [&](bool condition, const char* what) {
            if (!condition) { ok = false; LOG_ERROR("collision edit selftest: %s FAILED", what); }
            else LOG_INFO("collision edit selftest: %s ok", what);
        };
        collision::Editor& editor = Collision()->Editor();
        // The collision tools hear of the region after this one does.
        if (editor.World() != world) editor.SetWorld(world);
        collision::PolygonRef target{};
        bool found = false;
        size_t sections = 0, polygonsSeen = 0, quads = 0;
        for (u32 s = 0; s < (u32)world->Loaded().size() && !found; ++s) {
            const auto& packs = world->Loaded()[s]->collision;
            sections += !packs.empty();
            for (u32 p = 0; p < (u32)packs.size() && !found; ++p)
                for (u32 i = 0; i < (u32)packs[p].polygons.size(); ++i) {
                    const auto& polygon = packs[p].polygons[i];
                    ++polygonsSeen;
                    if (!polygon.quad || polygon.barrier || polygon.Vertical()) continue;
                    ++quads;
                    if (quads > 2000 || editor.CopiesOf({ s, p, i }).size() > 1) {
                        target = { s, p, i };
                        found = true;
                        break;
                    }
                }
        }
        LOG_INFO("collision edit selftest: %zu sections with collision, %zu polygons looked at, %zu ground quads, "
                 "target has %zu copies", sections, polygonsSeen, quads, found ? editor.CopiesOf(target).size() : 0);
        check(found, "a ground quad");
        if (found) {
            Renderer().ClearSelection();
            Renderer().SetCollisionSelectionList({ { target.section, target.pack, target.polygon, 0.0f } });
            check(Begin() && collision_, "Tab starts the collision edit");
            {
                // An area pick over the whole view, the camera on the target.
                const nfs::CollisionPolygon& p = *editor.Polygon(target);
                const Vec3 centre = (p.corners[0] + p.corners[2]) * 0.5f;
                host_.viewport->GetCamera().SetPosition(centre + Vec3{ -20.0f, -20.0f, 25.0f });
                host_.viewport->GetCamera().SetYawPitch(Radians(45.0f), Radians(-40.0f));
                pick_ = Pick::Vertex;
                SelectBox(-100000, -100000, 100000, 100000, false, false);
                const size_t all = selected_.size();
                check(all > 0, "a box over the view takes vertices");
                SelectBox(-100000, -100000, 100000, 100000, false, true);
                check(selected_.empty(), "Shift+box takes them away");
                pick_ = Pick::Face;
                SelectBox(-100000, -100000, 100000, 100000, false, false);
                bool whole = true;
                for (u32 f : selected_)
                    for (size_t g = 0; g < faces_.size(); ++g)
                        if (facePolygon_[g] == facePolygon_[f] && !selected_.count((u32)g)) whole = false;
                check(!selected_.empty() && whole, "faces come by whole polygons");
                LOG_INFO("collision edit selftest: box took %zu vertices, %zu faces", all, selected_.size());
                selected_.clear();
            }
            const nfs::CollisionPolygon before = *editor.Polygon(target);
            // The weld at corner 0 of the target.
            long weld = -1;
            for (size_t w = 0; w < weldVertices_.size() && weld < 0; ++w)
                if (Length(WeldPosition((u32)w) - before.corners[0]) < 1e-4f) weld = (long)w;
            check(weld >= 0, "its corner is a vertex");
            if (weld >= 0) {
                pick_ = Pick::Vertex;
                selected_ = { (u32)weld };
                Vec3 pivot;
                GizmoPivot(pivot);
                GizmoChanged(Mat4::Translation(pivot + Vec3{ 0.0f, 0.0f, 1.5f }), true, true);
                const nfs::CollisionPolygon after = *editor.Polygon(target);
                check(std::fabs(after.corners[0].z - before.corners[0].z - 1.5f) < 1e-3f, "the corner rose 1.5 m");
                bool copiesMoved = true;
                for (const auto& copy : editor.CopiesOf(target)) {
                    const nfs::CollisionPolygon* polygon = editor.Polygon(copy);
                    if (!polygon) continue;
                    bool has = false;
                    for (const Vec3& c : polygon->corners) has |= Length(c - after.corners[0]) < 1e-3f;
                    copiesMoved &= has;
                }
                check(copiesMoved, "every copy moved with it");
                const size_t logged = editor.Log().size();
                check(logged >= 1, "the move is in the collision log");
                editor.Undo();
                Collision()->RefreshAfterEdit();
                ReadCollision();
                check(std::fabs(editor.Polygon(target)->corners[0].z - before.corners[0].z) < 1e-4f, "Ctrl+Z puts it back");
                check(editor.Log().size() == logged - 1, "and takes its log entry out");

                // Extrude the edge from corner 0 to corner 1.
                long other = -1;
                for (size_t w = 0; w < weldVertices_.size() && other < 0; ++w)
                    if (Length(WeldPosition((u32)w) - before.corners[1]) < 1e-4f) other = (long)w;
                const std::pair<u32, u32> wanted{ (u32)std::min(weld, other), (u32)std::max(weld, other) };
                const auto e = std::find(edges_.begin(), edges_.end(), wanted);
                check(other >= 0 && e != edges_.end(), "the edge between corners 0 and 1");
                if (e != edges_.end()) {
                    pick_ = Pick::Edge;
                    selected_ = { (u32)(e - edges_.begin()) };
                    const size_t polygons = collisionRefs_.size();
                    const u64 steps = editor.StepsRecorded();
                    ExtrudeCollision();
                    check(collisionRefs_.size() > polygons, "extrude adds polygons");
                    check(editor.StepsRecorded() == steps + 1, "as one undo step");
                    check(selected_.size() == 1, "the new edge is picked");
                    if (selected_.size() == 1) {
                        const auto [a, b] = edges_[*selected_.begin()];
                        const float d = Length((WeldPosition(a) + WeldPosition(b)) * 0.5f -
                                               (before.corners[0] + before.corners[1]) * 0.5f);
                        check(std::fabs(d - 2.0f) < 0.05f, "2 m out");
                    }
                    editor.Undo();
                    Collision()->RefreshAfterEdit();
                }

                // Two changes kept, the first taken back from the Edits tab:
                // the second stays.
                StopCollision(false);
                Renderer().SetCollisionSelectionList({ { target.section, target.pack, target.polygon, 0.0f } });
                Begin();
                for (size_t w = 0; w < weldVertices_.size(); ++w)
                    if (Length(WeldPosition((u32)w) - before.corners[0]) < 1e-4f) weld = (long)w;
                pick_ = Pick::Vertex;
                selected_ = { (u32)weld };
                Vec3 pivot2;
                GizmoPivot(pivot2);
                GizmoChanged(Mat4::Translation(pivot2 + Vec3{ 0.0f, 0.0f, 1.5f }), true, true);
                const size_t first = editor.Log().size() - 1;
                const size_t packsBefore = world->Loaded()[target.section]->collision[target.pack].polygons.size();
                std::vector<nfs::CollisionPolygon> wall;
                const Vec3 a = before.corners[2] + Vec3{ 0, 0, 0.2f }, b = before.corners[3] + Vec3{ 0, 0, 0.2f };
                wall.push_back(collision::MakeBarrier(a, b, nfs::kSurfaceSolidWall));
                editor.Add(wall, L"Selftest barrier");
                Collision()->RefreshAfterEdit();
                const size_t entries = editor.Log().size();
                std::wstring summary;
                const size_t reverted = editor.RevertLog({ first }, summary);
                Collision()->RefreshAfterEdit();
                const auto& polygons = world->Loaded()[target.section]->collision[target.pack].polygons;
                bool cornerBack = false, barrierKept = false;
                for (const auto& p : polygons) {
                    if (!p.barrier && Length(p.corners[0] - before.corners[0]) < 0.05f) cornerBack = true;
                    if (p.barrier && Length(Vec3{ p.corners[0].x - a.x, p.corners[0].y - a.y, 0 }) < 0.05f) barrierKept = true;
                }
                check(reverted == 1 && editor.Log().size() == entries - 1, "Edits revert takes one entry out");
                check(cornerBack, "the reverted move is undone in the pack");
                check(barrierKept && polygons.size() >= packsBefore, "the later barrier stays");
                // Leave the region as it was.
                std::wstring ignored;
                editor.RevertLog({ editor.Log().size() - 1 }, ignored);
                Collision()->RefreshAfterEdit();
                active_ = true;
                collision_ = true;
            }
            StopCollision(false);
            check(!active_, "Tab ends it");
        }

        // Ground made by the editor faces up, whichever way round it was given.
        {
            const Vec3 cw[4] = { { 0, 0, 0 }, { 0, 4, 0 }, { 4, 4, 0 }, { 4, 0, 0 } };
            const nfs::CollisionPolygon made = collision::MakeGround(cw, 4, nfs::kSurfaceRoad);
            double area = 0.0;
            for (int i = 0; i < 4; ++i)
                area += (double)made.corners[i].x * made.corners[(i + 1) % 4].y -
                        (double)made.corners[(i + 1) % 4].x * made.corners[i].y;
            check(area > 0.0, "ground given clockwise is stored counter-clockwise");
            std::vector<nfs::CollisionPolygon> strip;
            collision::AppendStrip({ { 0, 0, 0 }, { 0, 10, 1 } }, { { 6, 0, 0 }, { 6, 10, 1 } }, nfs::kSurfaceRoad,
                                   strip);
            bool up = !strip.empty();
            for (const auto& p : strip) {
                const int n = p.quad ? 4 : 3;
                double a = 0.0;
                for (int i = 0; i < n; ++i)
                    a += (double)p.corners[i].x * p.corners[(i + 1) % n].y - (double)p.corners[(i + 1) % n].x * p.corners[i].y;
                up &= a > 0.0;
            }
            check(up, "a road strip faces up");
        }

        // A barrier: its side edge moves that end, the other stays.
        collision::PolygonRef barrier{};
        bool haveBarrier = false;
        for (u32 s = 0; s < (u32)world->Loaded().size() && !haveBarrier; ++s) {
            const auto& packs = world->Loaded()[s]->collision;
            for (u32 p = 0; p < (u32)packs.size() && !haveBarrier; ++p)
                for (u32 i = 0; i < (u32)packs[p].polygons.size(); ++i)
                    if (packs[p].polygons[i].barrier) { barrier = { s, p, i }; haveBarrier = true; break; }
        }
        if (haveBarrier) {
            Renderer().ClearSelection();
            Renderer().SetCollisionSelectionList({ { barrier.section, barrier.pack, barrier.polygon, 0.0f } });
            check(Begin() && collision_, "Tab on a barrier");
            const nfs::CollisionPolygon before = *editor.Polygon(barrier);
            Vec3 quad[4];
            editor.BarrierQuad(barrier, quad);
            long foot = -1, top = -1;
            for (size_t w = 0; w < weldVertices_.size(); ++w) {
                if (Length(WeldPosition((u32)w) - quad[0]) < 1e-3f) foot = (long)w;
                if (Length(WeldPosition((u32)w) - quad[1]) < 1e-3f) top = (long)w;
            }
            const std::pair<u32, u32> side{ (u32)std::min(foot, top), (u32)std::max(foot, top) };
            const auto e = std::find(edges_.begin(), edges_.end(), side);
            check(foot >= 0 && top >= 0 && e != edges_.end(), "the barrier has a side edge");
            check(faces_.size() >= 2, "and a face");
            if (e != edges_.end()) {
                pick_ = Pick::Edge;
                selected_ = { (u32)(e - edges_.begin()) };
                Vec3 pivot3;
                GizmoPivot(pivot3);
                GizmoChanged(Mat4::Translation(pivot3 + Vec3{ 2.0f, 0.0f, 0.0f }), true, true);
                const nfs::CollisionPolygon after = *editor.Polygon(barrier);
                check(std::fabs(after.corners[0].x - before.corners[0].x - 2.0f) < 1e-3f &&
                          std::fabs(after.corners[1].x - before.corners[1].x - 2.0f) < 1e-3f,
                      "the side edge moved that end 2 m");
                check(std::fabs(after.corners[2].x - before.corners[2].x) < 1e-4f, "the other end stayed");
                check(after.corners[0].z == 0.0f, "a barrier keeps no height");
                editor.Undo();
                Collision()->RefreshAfterEdit();
            }
            StopCollision(false);
        }

        // F between two edges of a ground quad that face each other: a new face.
        if (found) {
            Renderer().ClearSelection();
            Renderer().SetCollisionSelectionList({ { target.section, target.pack, target.polygon, 0.0f } });
            Begin();
            const nfs::CollisionPolygon q = *editor.Polygon(target);
            const auto weldAt = [&](const Vec3& p) -> long {
                for (size_t w = 0; w < weldVertices_.size(); ++w)
                    if (Length(WeldPosition((u32)w) - p) < 1e-3f) return (long)w;
                return -1;
            };
            const auto edgeOf = [&](long a, long b) -> long {
                const std::pair<u32, u32> e{ (u32)std::min(a, b), (u32)std::max(a, b) };
                const auto it = std::find(edges_.begin(), edges_.end(), e);
                return it == edges_.end() ? -1 : (long)(it - edges_.begin());
            };
            const long e1 = edgeOf(weldAt(q.corners[0]), weldAt(q.corners[1]));
            const long e2 = edgeOf(weldAt(q.corners[2]), weldAt(q.corners[3]));
            check(e1 >= 0 && e2 >= 0, "two opposite edges of the quad");
            if (e1 >= 0 && e2 >= 0) {
                pick_ = Pick::Edge;
                selected_ = { (u32)e1, (u32)e2 };
                const size_t before = collisionRefs_.size();
                const u64 steps = editor.StepsRecorded();
                FillCollision();
                check(collisionRefs_.size() > before && editor.StepsRecorded() == steps + 1, "F fills a face between them");
                check(pick_ == Pick::Face && !selected_.empty(), "and picks it");
                // Del takes the new face out again.
                const size_t withFace = collisionRefs_.size();
                DeleteSelection();
                check(collisionRefs_.size() < withFace && selected_.empty(), "Del deletes the picked face");
                editor.Undo();   // the delete
                Collision()->RefreshAfterEdit();
                CatchUp();
                check(collisionRefs_.size() == withFace, "Ctrl+Z brings it back");
                editor.Undo();   // the fill
                Collision()->RefreshAfterEdit();
                CatchUp();
            }
            StopCollision(false);
        }
        LOG_INFO("collision edit selftest: %s", ok ? "PASSED" : "FAILED");
    }

    // FRS_FIND_MODEL=<part of a model name>: where such models are stored, what
    // places them, and whether the renderer has them -- for a missing object.
    if (const char* wanted = std::getenv("FRS_FIND_MODEL"); wanted && world) {
        std::string key = wanted;
        for (char& c : key) c = (char)std::toupper((unsigned char)c);
        const auto matches = [&](const std::string& name) {
            std::string upper = name;
            for (char& c : upper) c = (char)std::toupper((unsigned char)c);
            return upper.find(key) != std::string::npos;
        };
        const auto& loaded = world->Loaded();
        for (size_t s = 0; s < loaded.size(); ++s) {
            const auto& section = *loaded[s];
            const char* sname = section.info ? section.info->name.c_str() : "?";
            for (const auto& list : section.solidLists)
                for (const auto& object : list.objects)
                    if (matches(object.name)) {
                        const GpuSection* gpu = nullptr;
                        const GpuObject* gpuObject = nullptr;
                        const bool onGpu = Renderer().FindGpuModel(object.hash, gpu, gpuObject);
                        LOG_INFO("find model: %s (%08X) stored in %s%s%s, %zu vertices, %zu indices, %zu materials, "
                                 "duplicate %d, backdrop %d, header at (%.1f %.1f %.1f), on the GPU %d (findModel %d)",
                                 object.name.c_str(), object.hash, sname, world->IsLibrary(section) ? " [library]" : "",
                                 section.info && section.info->IsAlternate() ? " [alternate]" : "",
                                 object.vertices.size(), object.indices.size(), object.materials.size(),
                                 object.duplicatePlacement, object.IsBackdrop(), object.transform.m[3][0],
                                 object.transform.m[3][1], object.transform.m[3][2], onGpu,
                                 world->FindModel(object.hash) == &object);
                        for (size_t m = 0; m < object.materials.size(); ++m) {
                            const nfs::SolidMaterial& material = object.materials[m];
                            const u32 hash = material.textureIndex < object.textureHashes.size()
                                                 ? object.textureHashes[material.textureIndex] : 0;
                            const nfs::Texture* t = world->FindDisplayTexture(hash);
                            LOG_INFO("find model:   material %zu: texture %08X %s (%s, alpha %d/%d), flags %08X, "
                                     "%u triangles, animated %d",
                                     m, hash, t ? t->name.c_str() : "NOT FOUND", t ? nfs::ToString(t->format) : "-",
                                     t ? (int)t->alphaUsage : -1, t ? (int)t->alphaBlend : -1, material.flags,
                                     material.indexCount / 3, world->TextureAnimations().count(hash) ? 1 : 0);
                        }
                    }
            for (size_t i = 0; i < section.scenery.infos.size(); ++i) {
                const auto& info = section.scenery.infos[i];
                if (!matches(info.name)) continue;
                size_t uses = 0;
                for (const auto& instance : section.scenery.instances)
                    if (instance.infoIndex == i) {
                        ++uses;
                        LOG_INFO("find model: instance of info '%s' (hash %08X) in %s, scenery %u record %u, flags %X, "
                                 "at (%.1f %.1f %.1f), model resolves %d",
                                 info.name.c_str(), info.solidHash, sname, instance.sceneryNumber, instance.record,
                                 instance.flags, instance.transform.m[3][0], instance.transform.m[3][1],
                                 instance.transform.m[3][2], world->FindModel(info.solidHash) != nullptr);
                    }
                if (!uses) LOG_INFO("find model: info '%s' in %s has no instance", info.name.c_str(), sname);
            }
        }
        size_t second = 0, unused = 0;
        std::string unusedNames;
        for (const auto& section : loaded) {
            if (!section->info || !section->info->IsAlternate()) continue;
            ++second;
            if (world->IsUnusedAlternate(*section->info)) {
                ++unused;
                if (unusedNames.size() < 300) unusedNames += section->info->name + " ";
            }
        }
        LOG_INFO("find model: second set %zu sections, %zu loaded by the game's lists, %zu never (%s)", second,
                 second - unused, unused, unusedNames.c_str());
        LOG_INFO("find model: done (renderer: %zu unresolved, %zu duplicate instances)",
                 Renderer().UnresolvedInstances(), Renderer().DuplicateInstances());
    }

    // FRS_CAMERA="x y z": the view looks at that point from a little way off,
    // for a capture of one spot.
    if (const char* at = std::getenv("FRS_CAMERA"); at && world) {
        float x = 0, y = 0, z = 0, pitch = -12.0f, distance = 120.0f;   // "x y z [pitch [distance]]"
        if (std::sscanf(at, "%f %f %f %f %f", &x, &y, &z, &pitch, &distance) >= 3) {
            host_.viewport->GetCamera().SetPosition({ x - distance, y - distance, z + distance * 0.5f });
            host_.viewport->GetCamera().SetYawPitch(Radians(45.0f), Radians(pitch));
        }
    }

    // FRS_BAKE_SELFTEST=1: the map's own road by a street light, baked, set
    // beside the colours the game baked into it -- nothing changed.
    if (std::getenv("FRS_BAKE_SELFTEST") && world) {
        const nfs::SolidObject* road = nullptr;
        Mat4 roadTransform;
        for (const auto& section : world->Loaded()) {
            if (road || !section->info || world->IsLibrary(*section)) continue;
            for (const auto& instance : section->scenery.instances) {
                const nfs::SolidObject* model = world->FindModel(section->scenery.ModelHash(instance));
                if (!model || !lightbake::IsLamp(model->name.c_str())) continue;
                // The road under this lamp.
                const Vec3 at{ instance.transform.m[3][0], instance.transform.m[3][1], instance.transform.m[3][2] };
                for (const auto& other : section->scenery.instances) {
                    const nfs::SolidObject* candidate = world->FindModel(section->scenery.ModelHash(other));
                    if (!candidate || candidate->name.rfind("TRN_", 0) != 0 || !other.worldBounds.Valid()) continue;
                    if (at.x >= other.worldBounds.min.x && at.x <= other.worldBounds.max.x &&
                        at.y >= other.worldBounds.min.y && at.y <= other.worldBounds.max.y) {
                        road = candidate;
                        roadTransform = other.transform;
                        break;
                    }
                }
                if (road) break;
            }
        }
        if (road) {
            lightbake::Settings settings;
            const lightbake::Environment environment =
                lightbake::Measure(*world, Transform(road->bounds, roadTransform), settings);
            const std::vector<u32> baked = lightbake::Bake(*road, roadTransform, environment, settings);
            double gameSum = 0, bakedSum = 0, diff = 0;
            for (size_t i = 0; i < baked.size(); ++i) {
                const auto luma = [](u32 c) { return ((c >> 16) & 255) * 0.3 + ((c >> 8) & 255) * 0.59 + (c & 255) * 0.11; };
                gameSum += luma(road->vertices[i].color);
                bakedSum += luma(baked[i]);
                diff += std::fabs(luma(road->vertices[i].color) - luma(baked[i]));
            }
            const double n = (double)std::max<size_t>(1, baked.size());
            LOG_INFO("bake selftest: %s, %zu vertices -- game mean %.1f, baked mean %.1f, mean difference %.1f (of 255)",
                     road->name.c_str(), baked.size(), gameSum / n, bakedSum / n, diff / n);
        } else {
            LOG_INFO("bake selftest: no road found under a street light");
        }
    }

    // FRS_ONLINE_SELFTEST=<a .glb>: a Sketchfab search, the GLB turned into an
    // OBJ beside it, and the import planned as a new object -- nothing written.
    if (const char* glb = std::getenv("FRS_ONLINE_SELFTEST"); glb && world) {
        sketchfab::Query query;
        query.text = "street lamp";
        query.maxVertices = 3000;
        std::vector<sketchfab::Model> found;
        std::string next;
        std::wstring error;
        const bool searched = sketchfab::Search(query, found, next, error);
        size_t withinBudget = 0;
        for (const auto& m : found) withinBudget += (m.glbVertices ? m.glbVertices : m.vertices) <= 3000;
        LOG_INFO("online selftest: search %s, %zu results, %zu within 3000 vertices, next page %s; %S",
                 searched ? "ok" : "FAILED", found.size(), withinBudget, next.empty() ? "no" : "yes", error.c_str());
        const std::wstring source(glb, glb + std::strlen(glb));
        const std::wstring obj = source.substr(0, source.find_last_of(L'.')) + L".obj";
        GlbConversion conversion;
        const bool converted = ConvertGlbToObj(source, obj, "SELFTEST_DUCK", 4.0f, conversion);
        LOG_INFO("online selftest: convert %s -- %zu vertices, %zu triangles, %zu textures, scale %.3f, %.2f tall; %S",
                 converted ? "ok" : "FAILED", conversion.vertices, conversion.triangles, conversion.textures,
                 conversion.scale, conversion.height, conversion.error.c_str());
        if (converted) {
            const Vec3 origin{ 1707.7f, -933.8f, 24.0f };
            ImportPlan plan;
            const bool planned = PlanImport(*world, obj, plan, &origin);
            LOG_INFO("online selftest: import plan %s -- %zu new objects (%s, %zu vertices, %zu triangles, in %s)",
                     planned ? "ok" : "FAILED", plan.created.size(),
                     plan.created.empty() ? "-" : plan.created[0].model.c_str(),
                     plan.created.empty() ? 0 : plan.created[0].vertices,
                     plan.created.empty() ? 0 : plan.created[0].triangles,
                     plan.created.empty() ? "-" : plan.created[0].section.c_str());
        }
    }

    // FRS_UI_DEMO=1: a street light selected and in view, for a look at the
    // panels (with FRS_CAPTURE, a picture of them).
    static const bool demo = std::getenv("FRS_UI_DEMO") != nullptr;
    if (demo && world) {
        for (size_t s = 0; s < world->Loaded().size(); ++s) {
            const auto& section = *world->Loaded()[s];
            if (!section.info || world->IsLibrary(section)) continue;
            for (size_t i = 0; i < section.scenery.instances.size(); ++i) {
                const auto& instance = section.scenery.instances[i];
                const nfs::SolidObject* model = world->FindModel(section.scenery.ModelHash(instance));
                if (!model || model->name.find("STREETLIGHT") == std::string::npos) continue;
                PickResult picked;
                picked.object = model;
                picked.transform = instance.transform;
                picked.loadedSection = (u32)s;
                picked.native = (int)i;
                Renderer().SetSelection(picked);
                const Vec3 at{ instance.transform.m[3][0], instance.transform.m[3][1], instance.transform.m[3][2] };
                host_.viewport->GetCamera().SetPosition({ at.x - 14.0f, at.y - 14.0f, at.z + 9.0f });
                host_.viewport->GetCamera().SetYawPitch(Radians(45.0f), Radians(-22.0f));
                if (std::string(std::getenv("FRS_UI_DEMO")) == "edit") Begin();
                s = world->Loaded().size();
                break;
            }
        }
    }
    if (!selfTest || !world) return;

    int failures = 0;
    const auto check = [&failures](bool ok, const char* what) {
        if (!ok) ++failures;
        LOG_INFO("mesh selftest: %s  %s", ok ? "ok  " : "FAIL", what);
    };
    PickResult picked;
    for (const auto& section : world->Loaded()) {
        for (const auto& instance : section->scenery.instances) {
            const nfs::SolidObject* model = world->FindModel(section->scenery.ModelHash(instance));
            if (model && model->name.find("STREETLIGHT") != std::string::npos) {
                picked.object = model;
                picked.transform = instance.transform;
                break;
            }
        }
        if (picked.object) break;
    }
    check(picked.object != nullptr, "found a street light");
    if (!picked.object) return;
    const std::vector<nfs::SolidVertex> before = picked.object->vertices;

    Renderer().SetSelection(picked);
    check(Begin(), "edit mode starts on the selected object");
    check(!edges_.empty() && !faces_.empty() && weldVertices_.size() <= before.size(), "welds, edges and faces built");

    selected_ = { 0 };
    Vec3 pivot;
    check(GizmoPivot(pivot), "the gizmo stands on the picked vertex");
    Mat4 moved = Mat4::Identity();
    moved.m[3][0] = pivot.x + 1.0f;
    moved.m[3][1] = pivot.y;
    moved.m[3][2] = pivot.z + 0.5f;
    GizmoChanged(moved, false, true);
    GizmoChanged(moved, true, true);
    const u32 vertex = weldVertices_[0].front();
    const Vec3 now = transform_.TransformPoint(picked.object->vertices[vertex].position);
    check(std::fabs(now.x - original_[vertex].x - 1.0f) < 1e-3f && std::fabs(now.z - original_[vertex].z - 0.5f) < 1e-3f,
          "the parsed model follows the gizmo");
    check(undo_.size() == 1, "one undo step recorded");
    Undo();
    check(SameShape(world_, original_), "undo puts it back");
    GizmoChanged(moved, false, true);
    GizmoChanged(moved, true, true);

    // The write path, planned only: export to a scratch folder and read it back.
    const std::wstring folder = EditFolder();
    const std::wstring objPath = folder + L"\\selftest.obj";
    ExportSummary summary;
    PickResult exported = picked;
    check(ExportSelection(*world, { exported }, objPath, world->Region(), summary), "the edited model exports");
    ImportPlan plan;
    const bool planned = PlanImport(*world, objPath, plan);
    check(planned && (!plan.meshes.empty() || !plan.sectionSpans.empty() || !plan.spans.empty()),
          "the import would write the edited mesh");
    LOG_INFO("mesh selftest: plan %zu meshes, %zu section spans, %zu rebuilt; %S", plan.meshes.size(),
             plan.sectionSpans.size(), plan.rebuilt, plan.report.c_str());

    Stop(true);
    bool same = picked.object->vertices.size() == before.size();
    for (size_t i = 0; same && i < before.size(); ++i)
        same = std::memcmp(&picked.object->vertices[i].position, &before[i].position, sizeof(Vec3)) == 0;
    check(same, "discarding restores the model exactly");
    Renderer().ClearSelection();

    // One of the map's own objects moved: it becomes a placement, the save
    // plan takes it, and cancelling puts the map back as it was.
    {
        PickResult native;
        for (size_t s = 0; s < world->Loaded().size() && native.native < 0; ++s) {
            const auto& section = *world->Loaded()[s];
            if (!section.info || world->IsLibrary(section)) continue;
            for (size_t i = 0; i < section.scenery.instances.size(); ++i) {
                const auto& instance = section.scenery.instances[i];
                const nfs::SolidObject* model = world->FindModel(section.scenery.ModelHash(instance));
                if (!model || model->name.find("STREETLIGHT") == std::string::npos) continue;
                native.object = model;
                native.transform = instance.transform;
                native.loadedSection = (u32)s;
                native.native = (int)i;
                break;
            }
        }
        check(native.native >= 0, "found a street light placed by the map");
        if (native.native >= 0) {
            const nfs::LoadedSection& section = *world->Loaded()[native.loadedSection];
            const size_t placementsBefore = section.placements.size(), removedBefore = section.removedNatives.size();
            Renderer().SetSelection(native);
            check(host_.viewport->DetachNatives(true) == 1, "the map's object becomes a placement");
            u32 at = 0;
            int placement = -1;
            check(host_.viewport->SelectedPlacement(at, placement) && at == native.loadedSection &&
                  section.placements.size() == placementsBefore + 1 &&
                  section.removedNatives.size() == removedBefore + 1,
                  "the record is removed and a placement selected in its place");
            if (placement >= 0) {
                Mat4 moved2 = native.transform;
                moved2.m[3][0] += 3.0f;
                host_.viewport->SetPlacementTransform(at, placement, moved2);
                PlacementSavePlan plan2;
                check(PlanPlacementSave(*world, world->OverlayFolder(), plan2) && plan2.ok && !plan2.spans.empty(),
                      "the save plan writes the moved object");
            }
            host_.viewport->CancelDetach();
            check(section.placements.size() == placementsBefore && section.removedNatives.size() == removedBefore,
                  "cancelling gives the object back to the map");
        }
    }
    LOG_INFO("mesh selftest: %s (%d failures)", failures ? "FAILED" : "PASSED", failures);
}

// ---------------------------------------------------------------------------
// Editing collision
// ---------------------------------------------------------------------------

bool MeshTools::BeginCollision() {
    nfs::World* world = World();
    CollisionTools* tools = Collision();
    if (!world || !tools) return false;
    collision::Editor& editor = tools->Editor();
    if (editor.Moving()) {
        LOG_INFO("edit mode refused: a collision move is still in progress");
        return false;
    }

    collisionSelected_.clear();
    for (const CollisionPick& pick : Renderer().CollisionSelection())
        collisionSelected_.push_back({ pick.section, pick.pack, pick.polygon });
    const std::vector<collision::PolygonRef> chosen = editor.WithCopies(collisionSelected_);

    // The ones sharing a corner come along: moving a corner of the ground
    // should not tear it away from the ground around it.
    using Key = std::tuple<long, long, long>;
    const auto key = [](const Vec3& p) {
        return Key{ std::lround(p.x * 100.0f), std::lround(p.y * 100.0f), std::lround(p.z * 100.0f) };
    };
    std::set<Key> corners;
    std::set<std::pair<u32, u32>> packs;
    std::set<std::tuple<u32, u32, u32>> taken;
    std::vector<collision::PolygonRef> all;
    const auto take = [&](const collision::PolygonRef& ref) {
        if (taken.insert({ ref.section, ref.pack, ref.polygon }).second) all.push_back(ref);
    };
    for (const collision::PolygonRef& ref : chosen) {
        const nfs::CollisionPolygon* polygon = editor.Polygon(ref);
        if (!polygon) continue;
        take(ref);
        packs.insert({ ref.section, ref.pack });
        if (!polygon->barrier)
            for (const Vec3& c : polygon->corners) corners.insert(key(c));
    }
    for (const auto& [s, p] : packs) {
        const auto& pack = world->Loaded()[s]->collision[p];
        for (u32 i = 0; i < (u32)pack.polygons.size(); ++i) {
            const nfs::CollisionPolygon& polygon = pack.polygons[i];
            if (polygon.barrier) continue;
            for (const Vec3& c : polygon.corners)
                if (corners.count(key(c))) { take({ s, p, i }); break; }
        }
    }
    if (all.empty()) {
        Status(L"Pick collision polygons first (the collision layer shown), then Tab.");
        return false;
    }
    collisionRefs_.clear();
    collisionStart_.clear();
    return BuildCollision(std::move(all));
}

bool MeshTools::BeginCollisionArea() {
    nfs::World* world = World();
    CollisionTools* tools = Collision();
    if (!world || !tools) return false;
    collision::Editor& editor = tools->Editor();
    if (editor.Moving()) return false;
    collisionSelected_.clear();
    for (const CollisionPick& pick : Renderer().CollisionSelection())
        collisionSelected_.push_back({ pick.section, pick.pack, pick.polygon });

    areaCentre_ = host_.viewport->GetCamera().Position();
    std::vector<collision::PolygonRef> all = collisionSelected_;
    const float r2 = kAreaRadius * kAreaRadius;
    for (u32 s = 0; s < (u32)world->Loaded().size(); ++s) {
        const auto& packs = world->Loaded()[s]->collision;
        for (u32 p = 0; p < (u32)packs.size(); ++p)
            for (u32 i = 0; i < (u32)packs[p].polygons.size(); ++i) {
                const nfs::CollisionPolygon& polygon = packs[p].polygons[i];
                if (!showRaceGroupsInEdit_ && polygon.raceGroup != 0) continue;
                for (const Vec3& c : polygon.corners) {
                    const float dx = c.x - areaCentre_.x, dy = c.y - areaCentre_.y;
                    if (dx * dx + dy * dy <= r2) { all.push_back({ s, p, i }); break; }
                }
            }
    }
    if (all.empty()) {
        Status(L"No collision around the camera to edit.");
        return false;
    }
    collisionRefs_.clear();
    collisionStart_.clear();
    areaMode_ = true;
    if (!BuildCollision(std::move(all))) { areaMode_ = false; return false; }
    // What was picked before starts picked, as faces.
    pick_ = Pick::Face;
    selected_.clear();
    for (const collision::PolygonRef& ref : collisionSelected_)
        for (size_t f = 0; f < facePolygon_.size(); ++f)
            if (collisionRefs_[facePolygon_[f]] == ref) selected_.insert((u32)f);
    if (selected_.empty()) pick_ = Pick::Vertex;
    UpdateOverlay();
    Status(Format(L"Editing the collision around the camera: %zu polygons. 1/2/3 vertices/edges/faces, drag a box, "
                  L"E extrudes, F fills, Tab finishes.", collisionRefs_.size()));
    return true;
}

// The camera went on: the collision around it is gathered again, and what
// was picked stays picked where it still is.
void MeshTools::CatchUp() {
    if (!active_ || !collision_ || dragging_) return;
    if (areaMode_) FollowCamera(true);
    else ReadCollision();
    if (Collision()) collisionUndone_ = Collision()->Editor().StepsUndone();
    UpdateOverlay();
}

void MeshTools::FollowCamera(bool force) {
    if (!active_ || !collision_ || !areaMode_ || dragging_ || boxPending_) return;
    const Vec3 eye = host_.viewport->GetCamera().Position();
    const float dx = eye.x - areaCentre_.x, dy = eye.y - areaCentre_.y;
    if (!force && dx * dx + dy * dy < (kAreaRadius * 0.5f) * (kAreaRadius * 0.5f)) return;
    nfs::World* world = World();
    CollisionTools* tools = Collision();
    if (!world || !tools) return;
    // The picked items, by where their corners are.
    std::vector<std::vector<Vec3>> picked;
    for (u32 item : selected_) {
        std::vector<Vec3> corners;
        if (pick_ == Pick::Vertex && item < weldVertices_.size()) corners = { WeldPosition(item) };
        else if (pick_ == Pick::Edge && item < edges_.size())
            corners = { WeldPosition(edges_[item].first), WeldPosition(edges_[item].second) };
        else if (pick_ == Pick::Face && item < faces_.size())
            corners = { WeldPosition(faces_[item][0]), WeldPosition(faces_[item][1]), WeldPosition(faces_[item][2]) };
        if (!corners.empty()) picked.push_back(std::move(corners));
    }
    const Pick mode = pick_;
    std::vector<collision::PolygonRef> keep;   // polygons with a picked corner stay in
    for (size_t p = 0; p < collisionPoints_.size(); ++p)
        for (const auto& corners : picked)
            for (const Vec3& c : corners)
                if (Length(world_[p] - c) < 1e-3f) keep.push_back(collisionRefs_[collisionPoints_[p].polygon]);

    if (!force) areaCentre_ = eye;
    std::vector<collision::PolygonRef> all = keep;
    const float r2 = kAreaRadius * kAreaRadius;
    for (u32 s = 0; s < (u32)world->Loaded().size(); ++s) {
        const auto& packs = world->Loaded()[s]->collision;
        for (u32 p = 0; p < (u32)packs.size(); ++p)
            for (u32 i = 0; i < (u32)packs[p].polygons.size(); ++i) {
                const nfs::CollisionPolygon& polygon = packs[p].polygons[i];
                if (!showRaceGroupsInEdit_ && polygon.raceGroup != 0) continue;
                for (const Vec3& c : polygon.corners) {
                    const float ex = c.x - areaCentre_.x, ey = c.y - areaCentre_.y;
                    if (ex * ex + ey * ey <= r2) { all.push_back({ s, p, i }); break; }
                }
            }
    }
    if (!BuildCollision(std::move(all))) return;
    pick_ = mode;
    selected_.clear();
    const auto at = [&](const Vec3& c) -> long {
        for (size_t w = 0; w < weldVertices_.size(); ++w)
            if (Length(WeldPosition((u32)w) - c) < 1e-3f) return (long)w;
        return -1;
    };
    for (const auto& corners : picked) {
        std::vector<long> w;
        for (const Vec3& c : corners) w.push_back(at(c));
        if (std::find(w.begin(), w.end(), -1L) != w.end()) continue;
        if (mode == Pick::Vertex) selected_.insert((u32)w[0]);
        else if (mode == Pick::Edge) {
            const std::pair<u32, u32> e{ (u32)std::min(w[0], w[1]), (u32)std::max(w[0], w[1]) };
            const auto found = std::lower_bound(edges_.begin(), edges_.end(), e);
            if (found != edges_.end() && *found == e) selected_.insert((u32)(found - edges_.begin()));
        } else {
            for (size_t f = 0; f < faces_.size(); ++f) {
                std::array<u32, 3> a = faces_[f], b{ (u32)w[0], (u32)w[1], (u32)w[2] };
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                if (a == b) selected_.insert((u32)f);
            }
        }
    }
    UpdateOverlay();
}

bool MeshTools::BuildCollision(std::vector<collision::PolygonRef> seed) {
    CollisionTools* tools = Collision();
    if (!tools) return false;
    collision::Editor& editor = tools->Editor();
    using Key = std::tuple<long, long, long>;
    std::set<std::tuple<u32, u32, u32>> taken;
    std::vector<collision::PolygonRef> all;
    for (const collision::PolygonRef& ref : editor.WithCopies(seed))
        if (editor.Polygon(ref) && taken.insert({ ref.section, ref.pack, ref.polygon }).second) all.push_back(ref);
    if (all.empty()) return false;
    if (all.size() > 20000) {
        Status(L"Too many collision polygons to edit at once: pick fewer.");
        return false;
    }

    std::map<std::tuple<u32, u32, u32>, nfs::CollisionPolygon> before;
    for (size_t i = 0; i < collisionRefs_.size() && i < collisionStart_.size(); ++i)
        before[{ collisionRefs_[i].section, collisionRefs_[i].pack, collisionRefs_[i].polygon }] = collisionStart_[i];
    collisionRefs_ = all;
    collisionStart_.clear();
    collisionPoints_.clear();
    for (u32 i = 0; i < (u32)all.size(); ++i) {
        const nfs::CollisionPolygon& polygon = *editor.Polygon(all[i]);
        const auto known = before.find({ all[i].section, all[i].pack, all[i].polygon });
        collisionStart_.push_back(known != before.end() ? known->second : polygon);
        if (polygon.barrier) {
            // Drawn as a wall: foot and top at each end. Both of an end move
            // where it stands; the height is the view's, not the game's.
            collisionPoints_.push_back({ i, 0x3, true, 0 });
            collisionPoints_.push_back({ i, 0x3, true, 1 });
            collisionPoints_.push_back({ i, 0xC, true, 2 });
            collisionPoints_.push_back({ i, 0xC, true, 3 });
        } else if (polygon.quad) {
            for (int k = 0; k < 4; ++k) collisionPoints_.push_back({ i, (u8)(1 << k), false });
        } else {
            collisionPoints_.push_back({ i, 0x1, false });
            collisionPoints_.push_back({ i, 0x2, false });
            collisionPoints_.push_back({ i, 0xC, false });
        }
    }
    world_.assign(collisionPoints_.size(), Vec3{});
    collision_ = true;
    ReadCollision();

    // Welds, edges and faces, as for a model.
    std::map<Key, u32> spot;
    weldOf_.assign(world_.size(), 0);
    weldVertices_.clear();
    for (size_t i = 0; i < world_.size(); ++i) {
        const Key k{ std::lround(world_[i].x * 1000.0f), std::lround(world_[i].y * 1000.0f),
                     std::lround(world_[i].z * 1000.0f) };
        const auto found = spot.find(k);
        if (found != spot.end()) {
            weldOf_[i] = found->second;
            weldVertices_[found->second].push_back((u32)i);
        } else {
            weldOf_[i] = (u32)weldVertices_.size();
            spot.emplace(k, weldOf_[i]);
            weldVertices_.push_back({ (u32)i });
        }
    }
    std::set<std::pair<u32, u32>> edgeSet;
    faces_.clear();
    facePolygon_.clear();
    const auto edge = [&](u32 a, u32 b) { if (a != b) edgeSet.insert({ std::min(a, b), std::max(a, b) }); };
    const auto face = [&](u32 a, u32 b, u32 c, u32 polygon) {
        if (a == b || b == c || a == c) return;
        faces_.push_back({ a, b, c });
        facePolygon_.push_back(polygon);
    };
    for (size_t p = 0; p < collisionPoints_.size();) {
        const u32 polygon = collisionPoints_[p].polygon;
        size_t end = p;
        while (end < collisionPoints_.size() && collisionPoints_[end].polygon == polygon) ++end;
        std::vector<u32> w;
        for (size_t k = p; k < end; ++k) w.push_back(weldOf_[k]);
        for (size_t k = 0; k < w.size(); ++k) edge(w[k], w[(k + 1) % w.size()]);
        if (w.size() >= 3) face(w[0], w[1], w[2], polygon);
        if (w.size() == 4) face(w[0], w[2], w[3], polygon);
        p = end;
    }
    edges_.assign(edgeSet.begin(), edgeSet.end());

    const bool resumed = active_;
    original_ = world_;
    originalLocal_.clear();
    modelHash_ = 0;
    modelName_ = "collision";
    placements_ = 0;
    selected_.clear();
    undo_.clear();
    undoStamps_.clear();
    dragging_ = false;
    collisionUndone_ = editor.StepsUndone();
    if (!resumed) collisionSteps_ = false;
    active_ = true;
    Renderer().ClearSelection();
    ::SetFocus(host_.viewport->Handle());
    UpdateOverlay();
    Status(Format(L"Editing collision: %zu polygons (%zu picked), %zu vertices. 1/2/3 vertices/edges/faces, Tab finishes.",
                  all.size(), collisionSelected_.size(), weldVertices_.size()));
    if (host_.menuChecks) host_.menuChecks();
    LOG_INFO("collision edit: %zu picked, %zu polygons with neighbours and copies, %zu vertices, %zu faces",
             collisionSelected_.size(), all.size(), weldVertices_.size(), faces_.size());
    return true;
}

void MeshTools::ReadCollision() {
    CollisionTools* tools = Collision();
    if (!tools || !collision_) return;
    const collision::Editor& editor = tools->Editor();
    for (size_t i = 0; i < collisionPoints_.size() && i < world_.size(); ++i) {
        const CollisionPoint& point = collisionPoints_[i];
        const collision::PolygonRef& ref = collisionRefs_[point.polygon];
        const nfs::CollisionPolygon* polygon = editor.Polygon(ref);
        if (!polygon) continue;
        int slot = 0;
        while (slot < 3 && !(point.slots & (1 << slot))) ++slot;
        if (point.barrier) {
            // Only where it stands is stored; the points are the wall drawn.
            Vec3 quad[4];
            editor.BarrierQuad(ref, quad);
            world_[i] = quad[point.quadCorner & 3];
        } else {
            world_[i] = polygon->corners[slot];
        }
    }
    collisionUndone_ = editor.StepsUndone();
}

void MeshTools::PushCollision() {
    CollisionTools* tools = Collision();
    if (!tools || !collision_) return;
    collision::Editor& editor = tools->Editor();
    std::vector<nfs::CollisionPolygon> shapes(collisionRefs_.size());
    std::vector<char> have(collisionRefs_.size(), 0);
    for (size_t i = 0; i < collisionPoints_.size() && i < world_.size(); ++i) {
        const CollisionPoint& point = collisionPoints_[i];
        if (!have[point.polygon]) {
            const nfs::CollisionPolygon* now = editor.Polygon(collisionRefs_[point.polygon]);
            if (!now) continue;
            shapes[point.polygon] = *now;
            have[point.polygon] = 1;
        }
        nfs::CollisionPolygon& shape = shapes[point.polygon];
        if (point.barrier) {
            // An end has a foot and a top: the one that moved further from
            // where the drag began says where the end now stands.
            const std::vector<Vec3>& from = !dragStart_.empty() ? dragStart_ : original_;
            const auto moved = [&](size_t k) {
                if (k >= from.size()) return 0.0f;
                const float dx = world_[k].x - from[k].x, dy = world_[k].y - from[k].y;
                return dx * dx + dy * dy;
            };
            const size_t other = point.quadCorner == 0 || point.quadCorner == 2 ? i + 1 : i - 1;
            if (other < collisionPoints_.size() && collisionPoints_[other].polygon == point.polygon &&
                moved(other) > moved(i))
                continue;   // its partner speaks for this end
            for (int slot = 0; slot < 4; ++slot)
                if (point.slots & (1 << slot)) {
                    shape.corners[slot].x = world_[i].x;
                    shape.corners[slot].y = world_[i].y;
                }
            continue;
        }
        for (int slot = 0; slot < 4; ++slot)
            if (point.slots & (1 << slot)) shape.corners[slot] = world_[i];
    }
    for (size_t p = 0; p < shapes.size(); ++p)
        if (have[p]) editor.SetShape(collisionRefs_[p], shapes[p]);
    tools->RefreshAfterEdit();
}

void MeshTools::StopCollision(bool restore) {
    CollisionTools* tools = Collision();
    if (tools && tools->Editor().Moving()) tools->Editor().EndMove(false);
    if (tools && restore && collisionSteps_) {
        // Put back as one more step, so that too can be undone.
        collision::Editor& editor = tools->Editor();
        if (editor.BeginReshape(collisionRefs_)) {
            for (size_t i = 0; i < collisionRefs_.size(); ++i) editor.SetShape(collisionRefs_[i], collisionStart_[i]);
            editor.EndMove(true);
            tools->RefreshAfterEdit();
        }
    }
    active_ = false;
    collision_ = false;
    areaMode_ = false;
    dragging_ = false;
    selected_.clear();
    collisionPoints_.clear();
    collisionStart_.clear();
    facePolygon_.clear();
    collisionRefs_.clear();
    Renderer().SetToolOverlay({}, {}, this);
    std::vector<CollisionPick> picks;
    for (const collision::PolygonRef& ref : collisionSelected_) picks.push_back({ ref.section, ref.pack, ref.polygon, 0.0f });
    collisionSelected_.clear();
    Renderer().SetCollisionSelectionList(std::move(picks));
    if (host_.menuChecks) host_.menuChecks();
}

void MeshTools::ExtrudeCollision() {
    CollisionTools* tools = Collision();
    if (!active_ || !collision_ || !tools || dragging_) return;
    if (pick_ != Pick::Edge || selected_.empty()) {
        Status(L"Extrude: pick edges first (2, then click; Ctrl+click adds more), then E.");
        return;
    }
    collision::Editor& editor = tools->Editor();
    if (editor.StepsUndone() != collisionUndone_) ReadCollision();

    // Each picked edge, with a polygon it belongs to.
    struct Piece { u32 a, b; nfs::CollisionPolygon source; };
    std::vector<Piece> pieces;
    size_t barriers = 0;
    for (u32 e : selected_) {
        if (e >= edges_.size()) continue;
        const auto [wa, wb] = edges_[e];
        std::set<u32> ofA, ofB;
        for (u32 v : weldVertices_[wa]) ofA.insert(collisionPoints_[v].polygon);
        for (u32 v : weldVertices_[wb]) ofB.insert(collisionPoints_[v].polygon);
        const nfs::CollisionPolygon* source = nullptr;
        bool barrier = false;
        for (u32 p : ofA) {
            if (!ofB.count(p)) continue;
            const nfs::CollisionPolygon* polygon = editor.Polygon(collisionRefs_[p]);
            if (!polygon) continue;
            if (polygon->barrier) { barrier = true; continue; }
            source = polygon;
            break;
        }
        if (source) pieces.push_back({ wa, wb, *source });
        else if (barrier) ++barriers;
    }
    if (pieces.empty()) {
        Status(barriers ? L"Barriers have no height to extrude from: pick edges of ground or walls."
                        : L"Nothing to extrude.");
        return;
    }

    // Outward: away from the middle of the face the edge bounds, across the
    // edge -- along the ground for ground, up for the top of a wall. Edges
    // that share a corner share its new corner.
    std::map<u32, Vec3> push;
    for (const Piece& piece : pieces) {
        const Vec3 a = WeldPosition(piece.a), b = WeldPosition(piece.b);
        const int count = piece.source.quad ? 4 : 3;
        Vec3 centre{};
        for (int c = 0; c < count; ++c) centre = centre + piece.source.corners[c];
        centre = centre * (1.0f / (float)count);
        const float length = Length(b - a);
        const Vec3 along = length > 1e-5f ? (b - a) * (1.0f / length) : Vec3{ 1, 0, 0 };
        Vec3 out = (a + b) * 0.5f - centre;
        out = out - along * Dot(out, along);
        const float size = Length(out);
        out = size > 1e-4f ? out * (1.0f / size) : Vec3{ 0, 0, 1 };
        push[piece.a] = push[piece.a] + out;
        push[piece.b] = push[piece.b] + out;
    }
    constexpr float kDistance = 2.0f;
    std::map<u32, Vec3> moved;
    for (const auto& [weld, direction] : push) {
        const float size = Length(direction);
        moved[weld] = WeldPosition(weld) + (size > 1e-4f ? direction * (kDistance / size) : Vec3{ 0, 0, kDistance });
    }

    std::vector<nfs::CollisionPolygon> polygons;
    for (const Piece& piece : pieces) {
        const Vec3 corners[4] = { WeldPosition(piece.a), WeldPosition(piece.b), moved[piece.b], moved[piece.a] };
        nfs::CollisionPolygon polygon = collision::MakeGround(corners, 4, piece.source.surface);
        polygons.push_back(polygon);
    }
    const std::vector<collision::PolygonRef> added = editor.Add(polygons, L"Extrude collision");
    tools->RefreshAfterEdit();
    if (added.empty()) {
        Status(L"The new faces could not be stored: no section's collision lies there.");
        return;
    }
    collisionSteps_ = true;

    std::vector<collision::PolygonRef> seed = collisionRefs_;
    seed.insert(seed.end(), added.begin(), added.end());
    const Pick keep = pick_;
    if (!BuildCollision(seed)) return;
    pick_ = keep;

    // The far edges, picked.
    const auto weldAt = [&](const Vec3& p) -> long {
        for (size_t w = 0; w < weldVertices_.size(); ++w)
            if (Length(WeldPosition((u32)w) - p) < 1e-3f) return (long)w;
        return -1;
    };
    selected_.clear();
    for (const Piece& piece : pieces) {
        const long a = weldAt(moved[piece.a]), b = weldAt(moved[piece.b]);
        if (a < 0 || b < 0) continue;
        const std::pair<u32, u32> wanted{ (u32)std::min(a, b), (u32)std::max(a, b) };
        const auto found = std::lower_bound(edges_.begin(), edges_.end(), wanted);
        if (found != edges_.end() && *found == wanted) selected_.insert((u32)(found - edges_.begin()));
    }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(Format(L"Extruded %zu edge%s %.0f m outward%s -- move the new edges with the gizmo, Ctrl+Z takes it back.",
                  pieces.size(), pieces.size() == 1 ? L"" : L"s", kDistance,
                  barriers ? Format(L" (%zu barrier edges skipped)", barriers).c_str() : L""));
    LOG_INFO("collision edit: extruded %zu edges into %zu polygons (copies included)", pieces.size(), added.size());
}

bool MeshTools::AddToSession(const std::vector<nfs::CollisionPolygon>& polygons, const std::wstring& label) {
    CollisionTools* tools = Collision();
    if (!tools || polygons.empty()) return false;
    collision::Editor& editor = tools->Editor();
    const std::vector<collision::PolygonRef> added = editor.Add(polygons, label);
    tools->RefreshAfterEdit();
    if (added.empty()) {
        Status(L"The new collision could not be stored: no section's collision lies there.");
        return false;
    }
    collisionSteps_ = true;
    std::vector<collision::PolygonRef> seed = collisionRefs_;
    seed.insert(seed.end(), added.begin(), added.end());
    const Pick keep = pick_;
    if (!BuildCollision(seed)) return false;
    pick_ = keep;
    return true;
}

void MeshTools::FillCollision() {
    CollisionTools* tools = Collision();
    if (!active_ || !collision_ || !tools || dragging_) return;
    collision::Editor& editor = tools->Editor();
    if (editor.StepsUndone() != collisionUndone_) ReadCollision();

    // What a weld stands for: a barrier's corner, or a corner of ground/wall,
    // and the polygon it comes from (for the surface).
    const auto barrierWeld = [&](u32 w) {
        for (u32 v : weldVertices_[w]) if (collisionPoints_[v].barrier) return true;
        return false;
    };
    const auto sourceOf = [&](u32 w) -> const nfs::CollisionPolygon* {
        for (u32 v : weldVertices_[w])
            if (const nfs::CollisionPolygon* p = editor.Polygon(collisionRefs_[collisionPoints_[v].polygon])) return p;
        return nullptr;
    };
    const auto plan = [](const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    const auto joinBarrier = [&](const Vec3& a, const Vec3& b, std::vector<nfs::CollisionPolygon>& out) {
        if (plan(a, b) < 0.05f) return false;
        collision::WallOptions options;
        options.kind = collision::WallKind::Barrier;
        options.bothSides = true;
        collision::AppendWall({ a, b }, options, out);
        return true;
    };

    std::vector<u32> welds;
    std::vector<std::pair<u32, u32>> pickedEdges;
    if (pick_ == Pick::Vertex) welds.assign(selected_.begin(), selected_.end());
    else if (pick_ == Pick::Edge)
        for (u32 e : selected_) if (e < edges_.size()) pickedEdges.push_back(edges_[e]);

    std::vector<nfs::CollisionPolygon> polygons;
    std::wstring what;
    if (pickedEdges.size() == 2) {
        auto [a0, a1] = pickedEdges[0];
        auto [b0, b1] = pickedEdges[1];
        const bool barrierA = barrierWeld(a0) && barrierWeld(a1), barrierB = barrierWeld(b0) && barrierWeld(b1);
        if (barrierA && barrierB) {
            // The two ends that face each other, in plan.
            float best = 1e30f;
            Vec3 from{}, to{};
            for (u32 p : { a0, a1 })
                for (u32 q : { b0, b1 }) {
                    const float d = plan(WeldPosition(p), WeldPosition(q));
                    if (d < best) { best = d; from = WeldPosition(p); to = WeldPosition(q); }
                }
            if (!joinBarrier(from, to, polygons)) { Status(L"Those barriers already meet."); return; }
            what = L"barrier joining the two";
        } else if (!barrierA && !barrierB) {
            Vec3 A0 = WeldPosition(a0), A1 = WeldPosition(a1), B0 = WeldPosition(b0), B1 = WeldPosition(b1);
            // Pair the ends so the face does not twist.
            if (Length(A0 - B0) + Length(A1 - B1) > Length(A0 - B1) + Length(A1 - B0)) std::swap(B0, B1);
            const Vec3 quad[4] = { A0, A1, B1, B0 };
            const nfs::CollisionPolygon* source = sourceOf(a0);
            polygons.push_back(collision::MakeGround(quad, 4, source ? source->surface : nfs::kSurfaceRoad));
            what = polygons.back().Vertical() ? L"wall between the two edges" : L"face between the two edges";
        } else {
            Status(L"Fill: pick two edges of the same kind -- two of ground or walls, or two of barriers.");
            return;
        }
    } else if (welds.size() == 2 && barrierWeld(welds[0]) && barrierWeld(welds[1])) {
        if (!joinBarrier(WeldPosition(welds[0]), WeldPosition(welds[1]), polygons)) {
            Status(L"Those barrier corners already meet.");
            return;
        }
        what = L"barrier between the two corners";
    } else if (welds.size() == 3 || welds.size() == 4) {
        std::vector<Vec3> points;
        for (u32 w : welds) points.push_back(WeldPosition(w));
        // In order round their middle, seen from above -- or, standing up,
        // along the wall and then down.
        Vec3 centre{};
        for (const Vec3& p : points) centre = centre + p;
        centre = centre * (1.0f / (float)points.size());
        float spanX = 0, spanY = 0;
        for (const Vec3& p : points) { spanX = std::max(spanX, std::fabs(p.x - centre.x)); spanY = std::max(spanY, std::fabs(p.y - centre.y)); }
        // Angle in plan; points that share a spot in plan (a wall) are told
        // apart by height.
        const float along = spanX >= spanY ? 1.0f : 0.0f;
        std::sort(points.begin(), points.end(), [&](const Vec3& a, const Vec3& b) {
            const float aa = std::atan2(a.y - centre.y, a.x - centre.x), ab = std::atan2(b.y - centre.y, b.x - centre.x);
            if (std::fabs(aa - ab) > 1e-3f) return aa < ab;
            return a.z < b.z;
        });
        // A wall seen from above is a line: order it along the line, up one
        // end and down the other.
        double area = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const Vec3& a = points[i];
            const Vec3& b = points[(i + 1) % points.size()];
            area += (double)a.x * b.y - (double)b.x * a.y;
        }
        if (std::fabs(area) < 1e-2 && points.size() == 4) {
            std::sort(points.begin(), points.end(), [&](const Vec3& a, const Vec3& b) {
                const float ka = along * a.x + (1 - along) * a.y, kb = along * b.x + (1 - along) * b.y;
                return std::fabs(ka - kb) > 0.05f ? ka < kb : a.z < b.z;
            });
            // low-high at one end, high-low at the other
            if (points[0].z > points[1].z) std::swap(points[0], points[1]);
            if (points[2].z < points[3].z) std::swap(points[2], points[3]);
        }
        const nfs::CollisionPolygon* source = sourceOf(welds[0]);
        polygons.push_back(collision::MakeGround(points.data(), (int)points.size(),
                                                 source && !source->barrier ? source->surface : nfs::kSurfaceRoad));
        what = polygons.back().Vertical() ? L"wall on the picked corners" : L"face on the picked corners";
    } else {
        Status(L"Fill (F): pick two edges, two barrier corners, or three or four vertices.");
        return;
    }

    if (!AddToSession(polygons, L"Fill collision")) return;
    // The new polygon picked, as a face (or, for a barrier, its edges).
    selected_.clear();
    if (polygons.front().barrier) {
        pick_ = Pick::Edge;
        const Vec3 a = polygons.front().corners[0], b = polygons.front().corners[2];
        for (size_t e = 0; e < edges_.size(); ++e) {
            const Vec3 p = WeldPosition(edges_[e].first), q = WeldPosition(edges_[e].second);
            const bool onA = plan(p, a) < 0.05f || plan(q, a) < 0.05f, onB = plan(p, b) < 0.05f || plan(q, b) < 0.05f;
            if (onA && onB) selected_.insert((u32)e);
        }
    } else {
        pick_ = Pick::Face;
        for (size_t f = 0; f < faces_.size(); ++f) {
            bool all = true;
            for (u32 w : faces_[f]) {
                bool corner = false;
                for (int c = 0; c < (polygons.front().quad ? 4 : 3); ++c)
                    corner |= Length(WeldPosition(w) - polygons.front().corners[c]) < 0.05f;
                all &= corner;
            }
            if (all) selected_.insert((u32)f);
        }
    }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(L"Filled: " + what + L". Ctrl+Z takes it back.");
    LOG_INFO("collision edit: filled, %zu polygons added", polygons.size());
}

bool MeshTools::DeleteSelection() {
    if (!active_) return false;
    if (!collision_) {
        Status(L"The mesh editor moves vertices only; it does not delete them.");
        return true;
    }
    CollisionTools* tools = Collision();
    if (!tools || dragging_) return true;
    if (tools->Editor().StepsUndone() != collisionUndone_) CatchUp();
    // The polygons the picked faces are, or that use the picked edges or vertices.
    std::vector<std::set<u32>> polygonsOf(weldVertices_.size());
    for (size_t w = 0; w < weldVertices_.size(); ++w)
        for (u32 v : weldVertices_[w]) polygonsOf[w].insert(collisionPoints_[v].polygon);
    std::set<u32> going;
    for (u32 item : selected_) {
        if (pick_ == Pick::Vertex && item < weldVertices_.size()) {
            going.insert(polygonsOf[item].begin(), polygonsOf[item].end());
        } else if (pick_ == Pick::Edge && item < edges_.size()) {
            for (u32 p : polygonsOf[edges_[item].first])
                if (polygonsOf[edges_[item].second].count(p)) going.insert(p);
        } else if (pick_ == Pick::Face && item < facePolygon_.size()) {
            going.insert(facePolygon_[item]);
        }
    }
    if (going.empty()) {
        Status(L"Pick the faces (3), edges (2) or vertices (1) of the collision to delete, then Del.");
        return true;
    }
    std::vector<collision::PolygonRef> refs;
    for (u32 p : going) refs.push_back(collisionRefs_[p]);
    const size_t removed = tools->Editor().Remove(refs);   // copies in other sections too
    tools->RefreshAfterEdit();
    collisionSteps_ = true;
    selected_.clear();
    areaMode_ = true;
    FollowCamera(true);
    collisionUndone_ = tools->Editor().StepsUndone();
    UpdateOverlay();
    Status(Format(L"%zu collision polygons deleted (copies included). Ctrl+Z puts them back.", removed));
    LOG_INFO("collision edit: %zu polygons deleted", removed);
    return true;
}

bool MeshTools::MoveSelectionToView() {
    if (!active_) return false;
    Finish();
    return true;
}

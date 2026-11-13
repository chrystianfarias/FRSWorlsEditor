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
#include "ui/Resource.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
        ::AppendMenuW(popup_, MF_STRING, kIdEdit, L"&Edit Mesh of the Selected Object\tTab");
        ::AppendMenuW(popup_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(popup_, MF_STRING, kIdVertices, L"Pick &Vertices\t1");
        ::AppendMenuW(popup_, MF_STRING, kIdEdges, L"Pick E&dges\t2");
        ::AppendMenuW(popup_, MF_STRING, kIdFaces, L"Pick &Faces\t3");
        ::AppendMenuW(popup_, MF_STRING, kIdSelectAll, L"Select &All\tCtrl+A");
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
        case kIdFinish:    if (active_) Finish(); return true;
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
    nfs::World* world = World();
    const auto& selection = Renderer().Selection();
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
        dragStart_ = world_;
        GizmoPivot(pivotStart_);
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
    if (done) {
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
        if (!add) selected_.clear();
    } else if (add) {
        if (!selected_.erase((u32)best)) selected_.insert((u32)best);
    } else {
        selected_ = { (u32)best };
    }
    UpdateOverlay();
    return true;
}

bool MeshTools::KeyDown(WPARAM key, bool repeat) {
    if (!active_) {
        if (key == VK_TAB && !repeat && ::GetKeyState(VK_CONTROL) >= 0) return Begin() || true;
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
        case 'Z':
            if (::GetKeyState(VK_CONTROL) < 0) { undo::UndoLatest(); return true; }
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
    text = Format(L"Mesh edit %s -- %s, %zu picked. Click picks (Ctrl adds), the gizmo moves/turns/scales, "
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
    add(L"Mesh", L"Placed", std::to_wstring(placements_),
        L"Every placement of this model draws the same mesh: all of them change.");
    add(L"Selection", L"Picking", pick_ == Pick::Vertex ? L"Vertices (1)" : pick_ == Pick::Edge ? L"Edges (2)" : L"Faces (3)",
        L"1, 2 and 3 switch; the selection carries over.");
    add(L"Selection", L"Picked", std::to_wstring(selected_.size()), L"Ctrl+click adds or removes, Ctrl+A takes all.");
    Vec3 pivot;
    if (const_cast<MeshTools*>(this)->GizmoPivot(pivot))
        add(L"Selection", L"Centre", Format(L"%.2f, %.2f, %.2f", pivot.x, pivot.y, pivot.z),
            L"Where the gizmo stands: the middle of the picked vertices.");
    add(L"Edit", L"Steps", std::to_wstring(undo_.size()), L"Ctrl+Z takes one back.");
    add(L"Edit", L"Changed", !SameShape(world_, original_) ? L"Yes -- Tab writes it" : L"No", L"");
    return true;
}

// FRS_MESH_TOOLS_SELFTEST=1: the edit path on a street light, nothing written
// into the game -- the import is only planned.
void MeshTools::OnRegionLoaded() {
    Stop(false);
    static const bool selfTest = std::getenv("FRS_MESH_TOOLS_SELFTEST") != nullptr;
    nfs::World* world = World();

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
        float x = 0, y = 0, z = 0;
        if (std::sscanf(at, "%f %f %f", &x, &y, &z) == 3) {
            host_.viewport->GetCamera().SetPosition({ x - 120.0f, y - 120.0f, z + 60.0f });
            host_.viewport->GetCamera().SetYawPitch(Radians(45.0f), Radians(-12.0f));
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

bool MeshTools::DeleteSelection() {
    if (!active_) return false;
    Status(L"The mesh editor moves vertices only; it does not delete them.");
    return true;
}

bool MeshTools::MoveSelectionToView() {
    if (!active_) return false;
    Finish();
    return true;
}

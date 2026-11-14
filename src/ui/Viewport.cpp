#include "ui/Viewport.h"

#include "core/Log.h"
#include "render/RenderExtension.h"

#include <windowsx.h>

#include <cstring>

namespace {

const wchar_t* kViewportClass = L"FRSWorldEditor.Viewport";

// Background of an empty viewport: a neutral slate that reads as "no world
// loaded" without being mistaken for geometry.
constexpr u32 kClearColor = 0xFF1E2228;

// The placement dump and the reference check (several megabytes each, in the
// working directory) are for working on the editor: set FRS_DIAGNOSTICS=1.
bool DiagnosticDumps() {
    wchar_t value[8] = {};
    return ::GetEnvironmentVariableW(L"FRS_DIAGNOSTICS", value, 8) && value[0] == L'1';
}

} // namespace

bool Viewport::Create(HWND parent, int controlId) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_OWNDC | CS_DBLCLKS;   // the device keeps a private DC for presenting
        wc.lpfnWndProc   = &Viewport::WndProc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;    // Direct3D paints every pixel
        wc.lpszClassName = kViewportClass;

        if (!::RegisterClassExW(&wc)) {
            LOG_ERROR("failed to register the viewport window class");
            return false;
        }
        registered = true;
    }

    window_ = ::CreateWindowExW(0, kViewportClass, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                0, 0, 16, 16,
                                parent, (HMENU)(INT_PTR)controlId,
                                ::GetModuleHandleW(nullptr), this);
    if (!window_) {
        LOG_ERROR("failed to create the viewport window");
        return false;
    }

    if (!context_.Create(window_)) {
        ::DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    renderer_.SetContext(&context_);

    // The panels draw in this device; its default-pool objects go round a reset.
    ui_.Init(window_, context_.Device());
    context_.beforeReset = [this] { ui_.DeviceLost(); };
    context_.afterReset = [this] { ui_.DeviceReset(); };
    return true;
}

void Viewport::Destroy() {
    ui_.Shutdown();
    renderer_.Clear();
    context_.Destroy();
    if (window_) {
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
}

LRESULT CALLBACK Viewport::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Viewport* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = (CREATESTRUCTW*)lParam;
        self = (Viewport*)create->lpCreateParams;
        // Publish the handle before the first message is handled: WM_NCCREATE
        // already needs it to reach DefWindowProc, and returning zero there
        // would abort the creation.
        self->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (Viewport*)::GetWindowLongPtrW(window, GWLP_USERDATA);
    }

    if (self) return self->Handle(message, wParam, lParam);
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Viewport::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    // The panels see everything first. Over a panel -- or typing into one --
    // the view itself does not react, unless it is in the middle of a drag of
    // its own (looking round, moving something).
    ui_.HandleMessage(window_, message, wParam, lParam);
    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && ui_.WantsMouse()) return TRUE;
    const bool busy = looking_ || Transforming() || GizmoDragging();
    if (!busy && ui_.WantsMouse()) {
        switch (message) {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
                // The keys follow the click into a panel's fields.
                ::SetFocus(window_);
                return 0;
            case WM_LBUTTONUP:
                // A drag that began in the view ends there, wherever it is let go.
                if (::GetCapture() == window_) break;
                return 0;
            case WM_MOUSEWHEEL:
                return 0;
            case WM_MOUSEMOVE:
                gizmoHot_ = kGizmoNone;
                return 0;
            default:
                break;
        }
    }
    if (ui_.WantsKeyboard() && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CHAR))
        return 0;

    switch (message) {
        case WM_SIZE:
            context_.Resize((u32)LOWORD(lParam), (u32)HIWORD(lParam));
            return 0;

        case WM_ERASEBKGND:
            return 1;  // never flash: Direct3D owns the client area

        case WM_PAINT: {
            PAINTSTRUCT paint;
            ::BeginPaint(window_, &paint);
            Tick(0.0f);
            ::EndPaint(window_, &paint);
            return 0;
        }

        case WM_RBUTTONDOWN:
            if (GizmoDragging()) { EndGizmoDrag(false); return 0; }
            if (Transforming()) { EndTransform(false); return 0; }
            BeginLook();
            return 0;

        case WM_RBUTTONUP:
            EndLook();
            return 0;

        case WM_MOUSEMOVE:
            if (GizmoDragging()) {
                UpdateGizmoDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            if (Transforming()) {
                UpdateTransform(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            if (!looking_)
                for (ViewportTool* tool : tools_) tool->MouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (!looking_) {
                // Which handle the pointer is over, to light it up.
                u32 section = 0;
                int placement = -1;
                Mat4 transform;
                gizmoHot_ = GizmoTarget(section, placement, transform)
                                ? GizmoHit(gizmoMode_, { transform.m[3][0], transform.m[3][1], transform.m[3][2] },
                                           CurrentGizmoView(), (float)GET_X_LPARAM(lParam),
                                           (float)GET_Y_LPARAM(lParam))
                                : kGizmoNone;
            }
            if (looking_) {
                POINT cursor;
                ::GetCursorPos(&cursor);
                const int dx = cursor.x - lookAnchor_.x;
                const int dy = cursor.y - lookAnchor_.y;
                if (dx || dy) {
                    flying_ = false;
                    camera_.Rotate((float)dx, (float)dy);
                    // Pin the cursor so the look is never limited by screen edges.
                    ::SetCursorPos(lookAnchor_.x, lookAnchor_.y);
                }
            }
            return 0;

        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            for (ViewportTool* tool : tools_)
                if (tool->MouseWheel(notches, wParam)) return 0;
            // While moving an object the wheel raises and lowers it, by a
            // quarter unit a notch, or a twentieth with Shift held.
            if (transform_ == TransformMode::Move) {
                const float step = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) ? 0.05f : 0.25f;
                transformRise_ += notches * step;
                POINT cursor;
                ::GetCursorPos(&cursor);
                ::ScreenToClient(window_, &cursor);
                UpdateTransform(cursor.x, cursor.y);
                return 0;
            }
            const float factor = std::pow(1.25f, (float)notches);
            camera_.SetSpeed(Clamp(camera_.Speed() * factor, 0.5f, 20000.0f));
            return 0;
        }

        case WM_LBUTTONDBLCLK:
            for (ViewportTool* tool : tools_)
                if (tool->MouseDoubleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam)) return 0;
            [[fallthrough]];
        case WM_LBUTTONDOWN:
            ::SetFocus(window_);
            if (Transforming()) { EndTransform(true); return 0; }
            {
                // A tool's own selection is moved by the gizmo before the tool
                // sees the click as a pick.
                u32 section = 0;
                int placement = -1;
                Mat4 transform;
                if (GizmoTarget(section, placement, transform) && placement == kGizmoTool &&
                    BeginGizmoDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                    return 0;
            }
            for (ViewportTool* tool : tools_)
                if (tool->MouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam)) return 0;
            if (BeginGizmoDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
            PickAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                   (wParam & MK_CONTROL) != 0, (wParam & MK_SHIFT) != 0);
            return 0;

        case WM_LBUTTONUP:
            if (GizmoDragging()) { EndGizmoDrag(true); return 0; }
            for (ViewportTool* tool : tools_)
                if (tool->MouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam)) return 0;
            break;

        case WM_CAPTURECHANGED:
            if (GizmoDragging() && (HWND)lParam != window_) EndGizmoDrag(true);
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE && GizmoDragging()) { EndGizmoDrag(false); return 0; }
            if (wParam == VK_ESCAPE && Transforming()) { EndTransform(false); return 0; }
            if (!looking_ && !Transforming() && !GizmoDragging()) {
                bool used = false;
                for (ViewportTool* tool : tools_)
                    if (!used) used = tool->KeyDown(wParam, (lParam & (1 << 30)) != 0);
                if (used) return 0;
            }
            if (!looking_ && !Transforming() && (lParam & (1 << 30)) == 0) {
                if (wParam == 'G') { BeginTransform(TransformMode::Move);   return 0; }
                if (wParam == 'R') { BeginTransform(TransformMode::Rotate); return 0; }
            }
            if (wParam < 256) keys_[wParam] = true;
            if (wParam == VK_ESCAPE) renderer_.ClearSelection();
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam < 256) keys_[wParam] = false;
            return 0;

        case WM_KILLFOCUS:
            // Releasing focus means the key-up will go somewhere else, so
            // forget everything rather than keep moving.
            std::memset(keys_, 0, sizeof(keys_));
            EndLook();
            return 0;

        case WM_GETDLGCODE:
            // Claim the arrow keys and WASD so the frame does not steal them.
            return DLGC_WANTALLKEYS;

        default:
            break;
    }

    return ::DefWindowProcW(window_, message, wParam, lParam);
}

void Viewport::BeginLook() {
    if (looking_) return;

    looking_ = true;
    ::SetFocus(window_);
    ::SetCapture(window_);
    ::GetCursorPos(&lookAnchor_);
    ::ShowCursor(FALSE);
}

void Viewport::EndLook() {
    if (!looking_) return;

    looking_ = false;
    ::ReleaseCapture();
    ::SetCursorPos(lookAnchor_.x, lookAnchor_.y);
    ::ShowCursor(TRUE);
}

void Viewport::UpdateCamera(float deltaSeconds) {
    if (deltaSeconds <= 0.0f) return;

    // Only drive the camera from the keyboard while the viewport has focus, so
    // typing in a side panel does not fly the camera away. While the viewport
    // mirrors the game, the game is driving.
    if (::GetFocus() != window_ || camera_.HasExplicitView()) return;

    auto down = [this](int key) { return keys_[key]; };

    Vec3 movement;
    if (down('W') || down(VK_UP))    movement.y += 1.0f;
    if (down('S') || down(VK_DOWN))  movement.y -= 1.0f;
    if (down('D') || down(VK_RIGHT)) movement.x += 1.0f;
    if (down('A') || down(VK_LEFT))  movement.x -= 1.0f;
    if (down('E') || down(VK_PRIOR)) movement.z += 1.0f;
    if (down('Q') || down(VK_NEXT))  movement.z -= 1.0f;

    if (movement.x == 0.0f && movement.y == 0.0f && movement.z == 0.0f) return;
    flying_ = false;

    float speed = camera_.Speed();
    if (down(VK_SHIFT))   speed *= 6.0f;
    if (down(VK_CONTROL)) speed *= 0.15f;

    camera_.Move(Normalize(movement) * (speed * deltaSeconds));
}

void Viewport::FlyTo(const Vec3& position, float yaw, float pitch, float seconds) {
    flightFrom_ = camera_.Position();
    flightTo_ = position;
    yawFrom_ = camera_.Yaw();
    pitchFrom_ = camera_.Pitch();
    // The short way round: from 350 degrees to 10 is twenty, not 340.
    float turn = std::fmod(yaw - yawFrom_, 2.0f * kPi);
    if (turn > kPi) turn -= 2.0f * kPi;
    if (turn < -kPi) turn += 2.0f * kPi;
    yawTo_ = yawFrom_ + turn;
    pitchTo_ = pitch;
    flightTime_ = 0.0f;
    flightLength_ = std::max(0.05f, seconds);
    flying_ = true;
}

void Viewport::UpdateFlight(float deltaSeconds) {
    if (!flying_) return;
    flightTime_ += deltaSeconds;
    const float t = std::min(1.0f, flightTime_ / flightLength_);
    const float ease = t * t * (3.0f - 2.0f * t);
    // Up and over rather than straight through: the path rises by a share of
    // the distance and comes down again, so the city passes underneath.
    const Vec3 line = flightFrom_ + (flightTo_ - flightFrom_) * ease;
    const float lift = Length(flightTo_ - flightFrom_) * 0.15f * 4.0f * ease * (1.0f - ease);
    camera_.SetPosition({ line.x, line.y, line.z + lift });
    camera_.SetYawPitch(yawFrom_ + (yawTo_ - yawFrom_) * ease, pitchFrom_ + (pitchTo_ - pitchFrom_) * ease);
    if (t >= 1.0f) flying_ = false;
}

void Viewport::Tick(float deltaSeconds) {
    if (!context_.IsValid()) return;

    if (frameHook_) frameHook_(deltaSeconds);
    UpdateFlight(deltaSeconds);
    UpdateCamera(deltaSeconds);

    {
        u32 section = 0;
        int placement = -1;
        Mat4 transform;
        const bool show = GizmoTarget(section, placement, transform);
        renderer_.SetGizmo(show, gizmoMode_, { transform.m[3][0], transform.m[3][1], transform.m[3][2] },
                           gizmoActive_ != kGizmoNone ? gizmoActive_ : gizmoHot_);
    }

    // A dialog opened from inside a frame would paint the view again while
    // the panels are half laid out.
    if (inFrame_) return;
    if (!context_.BeginFrame(kClearColor)) return;
    inFrame_ = true;
    const bool panels = ui_.Ready();
    if (panels) {
        ui_.BeginFrame(uiBars_);
        if (uiDraw_) uiDraw_();
    }
    renderer_.Render(camera_);
    if (panels) ui_.Render();
    if (world_ && (!captured_ || !capturedSecond_)) {
        captureClock_ += deltaSeconds;
        wchar_t path[MAX_PATH] = {};
        wchar_t wait[16] = L"6";
        ::GetEnvironmentVariableW(L"FRS_CAPTURE_DELAY", wait, 16);
        const float at = (float)_wtof(wait);
        if (!captured_ && captureClock_ > at && ::GetEnvironmentVariableW(L"FRS_CAPTURE", path, MAX_PATH)) {
            captured_ = true;
            context_.Device()->EndScene();
            LOG_INFO("capture %S: %s", path, context_.SaveBackBuffer(path) ? "saved" : "failed");
            context_.Device()->BeginScene();
        }
        // FRS_CAPTURE2: a second picture 0.4 s later, the camera unmoved --
        // what an animation changed in between.
        if (captured_ && !capturedSecond_ && captureClock_ > at + 0.4f) {
            capturedSecond_ = true;
            if (::GetEnvironmentVariableW(L"FRS_CAPTURE2", path, MAX_PATH)) {
                context_.Device()->EndScene();
                LOG_INFO("capture %S: %s", path, context_.SaveBackBuffer(path) ? "saved" : "failed");
                context_.Device()->BeginScene();
            }
        }
    }
    context_.EndFrame();
    inFrame_ = false;
}

void Viewport::PickAt(int x, int y, bool add, bool wholePack) {
    if (!context_.IsValid()) return;

    const float width  = (float)context_.Width();
    const float height = (float)context_.Height();
    if (width <= 0.0f || height <= 0.0f) return;

    const Ray ray = RayAt(x, y);

    // An extension's things win when nothing solid stands in front of them,
    // each in the order the extensions were added.
    for (const RenderExtension* extension : add ? std::vector<RenderExtension*>{} : renderer_.Extensions()) {
        int index = -1;
        float distance = 0.0f;
        if (!extension->Pick(ray, index, distance) || index < 0) continue;
        PickResult blocker;
        if (!renderer_.Pick(ray, blocker) || blocker.distance >= distance - extension->PickTolerance()) {
            renderer_.SelectExtension(extension, index);
            LOG_INFO("pick: extension item %d at distance %.1f", index, distance);
            return;
        }
    }

    // With the collision layer on, a click means the collision: it is what is
    // being looked at, and it lies on the road where the mesh would win.
    if (renderer_.CollisionVisible()) {
        PickCollisionAt(ray, add, wholePack);
        return;
    }

    PickResult hit;
    if (!renderer_.Pick(ray, hit)) {
        // Ctrl-clicking past everything is a miss, not an instruction to drop
        // a selection someone has been building up one object at a time.
        if (!add) renderer_.ClearSelection();
        LOG_INFO("pick: nothing under the cursor");
        return;
    }

    bool added = true;
    if (add) added = renderer_.ToggleSelection(hit);
    else     renderer_.SetSelection(hit);

    if (!added) {
        LOG_INFO("pick: '%s' removed from the selection, %zu left",
                 hit.object->name.c_str(), renderer_.Selection().size());
        return;
    }

    const nfs::SolidObject& object = *hit.object;
    const Vec3 at = hit.transform.TransformPoint(object.bounds.Valid()
                                                     ? object.bounds.Center()
                                                     : Vec3{});
    LOG_INFO("pick: '%s' in section %s, placed by %s, %u triangles, %zu textures",
             object.name.c_str(),
             hit.sectionName.empty() ? "?" : hit.sectionName.c_str(),
             hit.fromInstance ? "a scenery instance" : "its own header matrix",
             object.TriangleCount(), object.textureHashes.size());
    LOG_INFO("      centre (%.1f, %.1f, %.1f)  translation (%.1f, %.1f, %.1f)  "
             "distance %.1f  hash %08X",
             at.x, at.y, at.z,
             hit.transform.m[3][0], hit.transform.m[3][1], hit.transform.m[3][2],
             hit.distance, object.hash);
    LOG_INFO("      local bounds (%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f)  rows "
             "(%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)",
             object.bounds.min.x, object.bounds.min.y, object.bounds.min.z,
             object.bounds.max.x, object.bounds.max.y, object.bounds.max.z,
             hit.transform.m[0][0], hit.transform.m[0][1], hit.transform.m[0][2],
             hit.transform.m[1][0], hit.transform.m[1][1], hit.transform.m[1][2],
             hit.transform.m[2][0], hit.transform.m[2][1], hit.transform.m[2][2]);
}

void Viewport::SetWorld(nfs::World* world) {
    world_ = world;
    renderer_.Clear();
    if (!world_) return;
    renderer_.SyncWithWorld(*world_);
    if (DiagnosticDumps()) {
        renderer_.DumpPlacements(L"frs-placements.txt");
        renderer_.CheckReferences(L"frs-references.txt");
    }
}

void Viewport::RefreshWorld() {
    if (!world_) return;
    renderer_.SyncWithWorld(*world_);

    // Written on every load when asked for, because the moment it is worth
    // having is the moment something looks wrong, and asking for it then
    // means loading the region again.
    if (DiagnosticDumps()) renderer_.DumpPlacements(L"frs-placements.txt");
}

void Viewport::FrameBounds(const Aabb& bounds) {
    camera_.FrameBounds(bounds);
}

void Viewport::FrameWorld() {
    const Aabb bounds = renderer_.Bounds();
    if (bounds.Valid()) camera_.FrameBounds(bounds);
}

void Viewport::PickCollisionAt(const Ray& ray, bool add, bool wholePack) {
    CollisionPick hit;
    if (!renderer_.PickCollision(ray, hit)) {
        if (!add) renderer_.ClearSelection();
        LOG_INFO("pick: no collision under the cursor");
        return;
    }

    const nfs::LoadedSection& section = *world_->Loaded()[hit.section];
    const nfs::CollisionPack& pack = section.collision[hit.pack];
    const nfs::CollisionPolygon& polygon = pack.polygons[hit.polygon];
    const char* sectionName = section.info ? section.info->name.c_str() : "?";

    if (wholePack) {
        const size_t added = renderer_.SelectCollisionPack(hit, add);
        LOG_INFO("pick: collision of section %s, %zu polygons added, %zu selected",
                 sectionName, added, renderer_.CollisionSelection().size());
        return;
    }

    bool added = true;
    if (add) added = renderer_.ToggleCollisionSelection(hit);
    else     renderer_.SetCollisionSelection(hit);

    LOG_INFO("pick: collision %s #%u in section %s %s, bytes %02X %02X %02X %02X, "
             "%zu selected",
             polygon.barrier ? "barrier" : (polygon.quad ? "quad" : "triangle"),
             hit.polygon, sectionName, added ? "selected" : "removed",
             polygon.attributes[0], polygon.attributes[1], polygon.attributes[2],
             polygon.attributes[3], renderer_.CollisionSelection().size());
}

Ray Viewport::RayAt(int x, int y) const {
    const float width  = (float)std::max(1u, context_.Width());
    const float height = (float)std::max(1u, context_.Height());

    // Built straight from the camera basis rather than by inverting the
    // view-projection: the same field of view that made the frame defines it.
    const float tanHalfFov = std::tan(camera_.FieldOfView() * 0.5f);
    const float ndcX = 2.0f * ((float)x + 0.5f) / width - 1.0f;
    const float ndcY = 1.0f - 2.0f * ((float)y + 0.5f) / height;

    Ray ray;
    ray.origin    = camera_.Position();
    ray.direction = Normalize(camera_.Forward()
                              + camera_.Right() * (ndcX * tanHalfFov * (width / height))
                              + camera_.Up() * (ndcY * tanHalfFov));
    return ray;
}

size_t Viewport::DetachNatives(bool remember) {
    if (!world_) return 0;
    std::vector<std::pair<u32, int>> select;
    size_t converted = 0;
    for (const PickResult& picked : renderer_.Selection()) {
        if (picked.IsPlacement()) { select.push_back({ picked.loadedSection, picked.placement }); continue; }
        if (!picked.IsNative()) continue;
        nfs::LoadedSection* section = world_->MutableLoaded(picked.loadedSection);
        if (!section || (size_t)picked.native >= section->scenery.instances.size()) continue;
        const nfs::Scenery& scenery = section->scenery;
        const nfs::SceneryInstance& instance = scenery.instances[(size_t)picked.native];
        if (instance.infoIndex >= scenery.infos.size()) continue;
        const std::pair<u32, u32> key{ instance.sceneryNumber, instance.record };
        if (std::find(section->removedNatives.begin(), section->removedNatives.end(), key) !=
            section->removedNatives.end())
            continue;

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
        const bool wasChanged = section->placementsChanged;
        section->removedNatives.push_back(key);
        section->placements.push_back({ instance.sceneryNumber, placed });
        section->placementsChanged = true;
        const size_t index = section->placements.size() - 1;
        select.push_back({ picked.loadedSection, (int)index });
        if (remember) detached_.push_back({ picked.loadedSection, key, index, wasChanged });
        ++converted;
    }
    if (converted) {
        renderer_.RebuildInstances(*world_);
        renderer_.SelectPlacements(select);
        LOG_INFO("%zu of the map's own objects became placements, to be moved", converted);
    }
    return converted;
}

void Viewport::CancelDetach() {
    if (!world_ || detached_.empty()) return;
    for (auto it = detached_.rbegin(); it != detached_.rend(); ++it) {
        nfs::LoadedSection* section = world_->MutableLoaded(it->section);
        if (!section) continue;
        if (it->placement < section->placements.size())
            section->placements.erase(section->placements.begin() + (ptrdiff_t)it->placement);
        auto& removed = section->removedNatives;
        removed.erase(std::remove(removed.begin(), removed.end(), it->record), removed.end());
        section->placementsChanged = it->wasChanged;
    }
    detached_.clear();
    renderer_.ClearSelection();
    renderer_.RebuildInstances(*world_);
}

bool Viewport::SelectedPlacement(u32& loadedSection, int& placement) const {
    const auto& selection = renderer_.Selection();
    if (selection.size() != 1 || !selection.front().IsPlacement()) return false;
    loadedSection = selection.front().loadedSection;
    placement = selection.front().placement;
    return true;
}

void Viewport::SetPlacementTransform(u32 loadedSection, int placement, const Mat4& transform) {
    if (!world_) return;
    nfs::LoadedSection* section = world_->MutableLoaded(loadedSection);
    if (!section || placement < 0 || (size_t)placement >= section->placements.size()) return;
    section->placements[(size_t)placement].instance.transform = transform;
    section->placementsChanged = true;
    renderer_.UpdatePlacement(loadedSection, placement, transform);
}

bool Viewport::GroundBelow(const Vec3& point, u32 loadedSection, int placement, float& height) {
    Ray ray;
    ray.origin = { point.x, point.y, point.z + 50.0f };
    ray.direction = { 0.0f, 0.0f, -1.0f };

    renderer_.SetPickExclusion(loadedSection, placement);
    PickResult hit;
    const bool found = renderer_.Pick(ray, hit);
    renderer_.SetPickExclusion(0, -1);
    if (!found) return false;
    height = ray.origin.z - hit.distance;
    return true;
}

std::vector<std::pair<u32, int>> Viewport::SelectedPlacements() const {
    std::vector<std::pair<u32, int>> out;
    for (const PickResult& picked : renderer_.Selection())
        if (picked.IsPlacement()) out.push_back({ picked.loadedSection, picked.placement });
    return out;
}

bool Viewport::BeginTransform(TransformMode mode) {
    if (!world_) return false;
    moving_.clear();
    movingPoint_ = false;
    detached_.clear();
    if (!renderer_.HasExtensionSelection()) DetachNatives(true);
    Vec3 sum{};
    Vec3 point;
    float heading = 0.0f;
    if (pointGet_ && pointGet_(point, heading)) {
        movingPoint_ = true;
        pointStart_ = pointNow_ = point;
        pointHeadingStart_ = pointHeadingNow_ = heading;
    }
    for (const auto& [section, placement] : movingPoint_ ? std::vector<std::pair<u32, int>>{}
                                                         : SelectedPlacements()) {
        const nfs::LoadedSection* loaded = world_->MutableLoaded(section);
        if (!loaded || (size_t)placement >= loaded->placements.size()) continue;
        const Mat4& start = loaded->placements[(size_t)placement].instance.transform;
        moving_.push_back({ section, placement, start });
        sum += Vec3{ start.m[3][0], start.m[3][1], start.m[3][2] };
    }
    if (moving_.empty() && !movingPoint_) {
        CancelDetach();
        LOG_INFO("move/rotate: select objects placed with Objects > Place Object or pasted first, "
                 "or a point an extension edits");
        return false;
    }
    if (!movingPoint_ && moving_.size() < renderer_.Selection().size())
        LOG_INFO("move/rotate: objects of the model library (no scenery record) stay where they are");

    transform_ = mode;
    transformPivot_ = movingPoint_ ? pointStart_ : sum * (1.0f / (float)moving_.size());
    transformRise_ = 0.0f;

    POINT cursor;
    ::GetCursorPos(&cursor);
    ::ScreenToClient(window_, &cursor);
    transformMouseX_ = cursor.x;

    // The point under the cursor on the pivot's horizontal plane is where the
    // drag is measured from, so nothing jumps to the cursor.
    const Ray ray = RayAt(cursor.x, cursor.y);
    transformAnchor_ = transformPivot_;
    if (std::fabs(ray.direction.z) > 1e-4f) {
        const float t = (transformPivot_.z - ray.origin.z) / ray.direction.z;
        if (t > 0.0f) transformAnchor_ = ray.origin + ray.direction * t;
    }

    ::SetFocus(window_);
    ::SetCapture(window_);
    LOG_INFO("%s %zu objects: move the mouse, click to keep, Esc or right-click to cancel%s",
             mode == TransformMode::Move ? "move" : "rotate", movingPoint_ ? (size_t)1 : moving_.size(),
             mode == TransformMode::Move ? "; the wheel raises and lowers" : "; Shift turns finely");
    return true;
}

void Viewport::UpdateTransform(int x, int y) {
    Vec3 shift{ 0.0f, 0.0f, transformRise_ };
    float degrees = 0.0f;

    if (transform_ == TransformMode::Move) {
        const Ray ray = RayAt(x, y);
        if (std::fabs(ray.direction.z) > 1e-4f) {
            const float t = (transformPivot_.z - ray.origin.z) / ray.direction.z;
            // Near the horizon the plane runs off to infinity; stop well short.
            if (t > 0.0f && t < 5000.0f) {
                const Vec3 at = ray.origin + ray.direction * t;
                shift.x = at.x - transformAnchor_.x;
                shift.y = at.y - transformAnchor_.y;
            }
        }
    } else if (transform_ == TransformMode::Rotate) {
        const bool fine = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        degrees = (float)(x - transformMouseX_) * (fine ? 0.05f : 0.5f);
    }
    const float c = std::cos(Radians(degrees)), s = std::sin(Radians(degrees));

    if (movingPoint_) {
        // A point object stands on the street, so it follows the ground
        // under it -- looked for from a little above where it was, which keeps
        // it off the bridges and roofs overhead. The wheel lifts it from there.
        Vec3 at = { pointStart_.x + shift.x, pointStart_.y + shift.y, pointStart_.z };
        if (transform_ == TransformMode::Move) {
            Ray down;
            down.origin = { at.x, at.y, pointStart_.z + 3.0f };
            down.direction = { 0.0f, 0.0f, -1.0f };
            PickResult hit;
            if (renderer_.Pick(down, hit) && hit.distance < 30.0f) at.z = down.origin.z - hit.distance;
            at.z += transformRise_;
        }
        pointNow_ = at;
        pointHeadingNow_ = std::fmod(std::fmod(pointHeadingStart_ + degrees, 360.0f) + 360.0f, 360.0f);
        if (pointSet_) pointSet_(pointNow_, pointHeadingNow_, false);
        return;
    }

    for (const Moving& one : moving_) {
        Mat4 transform = one.start;
        // A turn about the world's vertical through the pivot: each object's
        // own axes turn, and its position swings round the pivot.
        for (int row = 0; row < 3; ++row) {
            const float px = one.start.m[row][0], py = one.start.m[row][1];
            transform.m[row][0] = px * c - py * s;
            transform.m[row][1] = px * s + py * c;
        }
        const float dx = one.start.m[3][0] - transformPivot_.x;
        const float dy = one.start.m[3][1] - transformPivot_.y;
        transform.m[3][0] = transformPivot_.x + dx * c - dy * s + shift.x;
        transform.m[3][1] = transformPivot_.y + dx * s + dy * c + shift.y;
        transform.m[3][2] = one.start.m[3][2] + shift.z;
        SetPlacementTransform(one.section, one.placement, transform);
    }
}

void Viewport::EndTransform(bool keep) {
    if (movingPoint_) {
        if (pointSet_) {
            if (keep) pointSet_(pointNow_, pointHeadingNow_, true);
            else pointSet_(pointStart_, pointHeadingStart_, false);
        }
        movingPoint_ = false;
        transform_ = TransformMode::None;
        ::ReleaseCapture();
        return;
    }
    if (!keep)
        for (const Moving& one : moving_) SetPlacementTransform(one.section, one.placement, one.start);
    transform_ = TransformMode::None;
    ::ReleaseCapture();
    if (keep) {
        std::vector<std::pair<u32, int>> moved;
        for (const Moving& one : moving_) moved.push_back({ one.section, one.placement });
        RehomePlacements(moved);
        detached_.clear();
    } else {
        // Cancelled: the map's own objects it took over go back to being theirs.
        CancelDetach();
    }
    moving_.clear();
}

void Viewport::RehomePlacements(std::vector<std::pair<u32, int>> placements) {
    if (!world_) return;

    // Work out where each one belongs before anything moves: taking one out
    // of a section shifts the indices of the ones after it.
    struct Move { u32 from; int index; size_t to; };
    std::vector<Move> moves;
    std::vector<std::pair<u32, int>> staying;
    for (const auto& [section, placement] : placements) {
        const nfs::LoadedSection* from = world_->MutableLoaded(section);
        if (!from || placement < 0 || (size_t)placement >= from->placements.size()) continue;
        const Mat4& t = from->placements[(size_t)placement].instance.transform;
        const size_t target = world_->SectionForPlacement({ t.m[3][0], t.m[3][1], t.m[3][2] });
        if (target == SIZE_MAX || target == section) staying.push_back({ section, placement });
        else moves.push_back({ section, placement, target });
    }
    if (moves.empty()) return;

    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
        return a.from != b.from ? a.from < b.from : a.index > b.index;
    });
    std::vector<std::pair<size_t, nfs::LoadedSection::UserPlacement>> taken;
    for (const Move& move : moves) {
        nfs::LoadedSection* from = world_->MutableLoaded(move.from);
        taken.push_back({ move.to, from->placements[(size_t)move.index] });
        from->placements.erase(from->placements.begin() + move.index);
        from->placementsChanged = true;
        // What stays behind in that section moves up past the gap.
        for (auto& [section, placement] : staying)
            if (section == move.from && placement > move.index) --placement;
    }

    std::vector<std::pair<u32, int>> selected = staying;
    for (auto& [target, placed] : taken) {
        nfs::LoadedSection* to = world_->MutableLoaded(target);
        if (!to || !to->info) continue;
        const u32 hash = placed.instance.modelHash;
        const size_t home = world_->ModelHome(hash);
        const bool library = home < world_->Loaded().size() && world_->IsLibrary(*world_->Loaded()[home]);
        if (!library && !to->HoldsModel(hash)) to->importedModels.push_back(hash);
        placed.sceneryNumber = to->info->number;
        to->placements.push_back(placed);
        to->placementsChanged = true;
        selected.push_back({ (u32)target, (int)to->placements.size() - 1 });
        LOG_INFO("placement of %s moved to section %s%s", placed.instance.modelName.c_str(),
                 to->info->name.c_str(), library ? "" : " (its model is copied there)");
    }

    renderer_.RebuildInstances(*world_);
    renderer_.SelectPlacements(selected);
}

// ---------------------------------------------------------------------------
// The gizmo
// ---------------------------------------------------------------------------

bool Viewport::GizmoTarget(u32& section, int& placement, Mat4& transform) const {
    // One placed object at a time, and not while G or R is already moving it.
    if (!world_ || Transforming()) return false;
    // A tool's own selection, when it has one.
    for (size_t i = 0; i < tools_.size(); ++i) {
        Vec3 pivot;
        if (!tools_[i]->GizmoPivot(pivot)) continue;
        transform = Mat4::Identity();
        transform.m[3][0] = pivot.x;
        transform.m[3][1] = pivot.y;
        transform.m[3][2] = pivot.z;
        section = (u32)i;
        placement = kGizmoTool;
        return true;
    }
    // An extension's point -- a road node -- takes the gizmo the same way:
    // its position, turned by its heading. kGizmoPoint marks it.
    if (renderer_.HasExtensionSelection() && pointGet_) {
        Vec3 position;
        float heading = 0.0f;
        if (!pointGet_(position, heading)) return false;
        transform = AxisRotation({ 0.0f, 0.0f, 1.0f }, Radians(heading));
        transform.m[3][0] = position.x;
        transform.m[3][1] = position.y;
        transform.m[3][2] = position.z;
        section = 0;
        placement = kGizmoPoint;
        return true;
    }
    if (!SelectedPlacement(section, placement)) {
        // One of the map's own objects: the gizmo stands on it, and the
        // first drag makes it a placement.
        const auto& selection = renderer_.Selection();
        if (selection.size() != 1 || !selection.front().IsNative()) return false;
        transform = selection.front().transform;
        section = selection.front().loadedSection;
        placement = kGizmoNative;
        return true;
    }
    const nfs::LoadedSection* loaded = world_->Loaded()[section].get();
    if (placement < 0 || (size_t)placement >= loaded->placements.size()) return false;
    transform = loaded->placements[(size_t)placement].instance.transform;
    return true;
}

GizmoView Viewport::CurrentGizmoView() const {
    GizmoView view;
    const float width = (float)std::max<u32>(1, context_.Width());
    const float height = (float)std::max<u32>(1, context_.Height());
    view.viewProjection = camera_.ViewMatrix() * camera_.ProjectionMatrix(width / height);
    view.width = width;
    view.height = height;
    view.eye = camera_.Position();
    return view;
}

namespace {

// Where along the line origin + axis * t the ray passes closest.
bool ClosestOnAxis(const Vec3& origin, const Vec3& axis, const Ray& ray, float& t) {
    const Vec3 w = origin - ray.origin;
    const float b = Dot(axis, ray.direction);
    const float denominator = 1.0f - b * b;
    if (std::fabs(denominator) < 1e-4f) return false;     // looking straight down the axis
    t = (b * Dot(ray.direction, w) - Dot(axis, w)) / denominator;
    return true;
}

bool OnGround(const Ray& ray, float height, Vec3& point) {
    if (std::fabs(ray.direction.z) < 1e-4f) return false;
    const float s = (height - ray.origin.z) / ray.direction.z;
    if (s <= 0.0f) return false;
    point = ray.origin + ray.direction * s;
    return true;
}

} // namespace

bool Viewport::BeginGizmoDrag(int x, int y) {
    u32 section = 0;
    int placement = -1;
    Mat4 transform;
    if (!GizmoTarget(section, placement, transform)) return false;
    const Vec3 origin{ transform.m[3][0], transform.m[3][1], transform.m[3][2] };
    const int handle = GizmoHit(gizmoMode_, origin, CurrentGizmoView(), (float)x, (float)y);
    if (handle == kGizmoNone) return false;
    // A point only turns about the vertical and has no size.
    if (placement == kGizmoPoint &&
        (gizmoMode_ == GizmoMode::Scale || (gizmoMode_ == GizmoMode::Rotate && handle != 2)))
        return false;

    if (placement == kGizmoNative) {
        // Grabbed: it becomes a placement now, which is what gets dragged.
        detached_.clear();
        if (!DetachNatives(true) || !SelectedPlacement(section, placement)) return false;
        transform = world_->Loaded()[section]->placements[(size_t)placement].instance.transform;
    }

    const Ray ray = RayAt(x, y);
    gizmoAxisStart_ = 0.0f;
    if (gizmoMode_ == GizmoMode::Move) {
        if (handle == kGizmoCentre ? !OnGround(ray, origin.z, gizmoPlaneStart_)
                                   : !ClosestOnAxis(origin, GizmoAxis(handle), ray, gizmoAxisStart_))
            return false;
    }
    gizmoActive_ = handle;
    gizmoSection_ = section;
    gizmoPlacement_ = placement;
    gizmoStart_ = transform;
    gizmoNow_ = transform;
    gizmoMouseStart_ = { (float)x, (float)y };
    ::SetCapture(window_);
    return true;
}

void Viewport::UpdateGizmoDrag(int x, int y) {
    if (gizmoActive_ == kGizmoNone || !world_) return;
    const Vec3 origin{ gizmoStart_.m[3][0], gizmoStart_.m[3][1], gizmoStart_.m[3][2] };
    Mat4 transform = gizmoStart_;

    if (gizmoMode_ == GizmoMode::Move) {
        const Ray ray = RayAt(x, y);
        Vec3 position = origin;
        if (gizmoActive_ == kGizmoCentre) {
            // Across the ground, standing on whatever is under it.
            Vec3 point;
            if (!OnGround(ray, origin.z, point)) return;
            position = origin + (point - gizmoPlaneStart_);
            position.z = origin.z;
            float ground = 0.0f;
            if (gizmoPlacement_ != kGizmoTool && GroundBelow({ position.x, position.y, position.z + 200.0f }, gizmoSection_, gizmoPlacement_, ground))
                position.z = ground;
        } else {
            float t = 0.0f;
            if (!ClosestOnAxis(origin, GizmoAxis(gizmoActive_), ray, t)) return;
            position = origin + GizmoAxis(gizmoActive_) * (t - gizmoAxisStart_);
        }
        transform.m[3][0] = position.x;
        transform.m[3][1] = position.y;
        transform.m[3][2] = position.z;
    } else {
        const GizmoView view = CurrentGizmoView();
        Vec2 centre;
        if (!GizmoProject(view, origin, centre)) return;
        const float x0 = gizmoMouseStart_.x - centre.x, y0 = gizmoMouseStart_.y - centre.y;
        const float x1 = (float)x - centre.x, y1 = (float)y - centre.y;

        Mat4 change = Mat4::Identity();
        if (gizmoMode_ == GizmoMode::Rotate) {
            // The turn of the pointer about the object's centre on screen.
            // Screen y runs down, so an anticlockwise turn seen along an axis
            // that points at the eye is a falling angle here.
            const Vec3 axis = GizmoAxis(gizmoActive_);
            float angle = std::atan2(y1, x1) - std::atan2(y0, x0);
            if (Dot(axis, camera_.Position() - origin) > 0.0f) angle = -angle;
            // Ctrl turns in steps of fifteen degrees.
            if (::GetKeyState(VK_CONTROL) & 0x8000) angle = std::round(angle / Radians(15.0f)) * Radians(15.0f);
            change = AxisRotation(axis, angle);
        } else {
            // Scale by how much nearer to or further from the centre the
            // pointer is than where the grab began; one factor on all three
            // axes, as the game's placements are scaled.
            const float from = std::max(4.0f, std::sqrt(x0 * x0 + y0 * y0));
            const float to = std::max(1.0f, std::sqrt(x1 * x1 + y1 * y1));
            const float startScale = Length(Vec3{ gizmoStart_.m[0][0], gizmoStart_.m[0][1], gizmoStart_.m[0][2] });
            const float factor = std::clamp(startScale * to / from, 0.05f, 3.9f) / std::max(1e-4f, startScale);
            change = Mat4::Identity();
            change.m[0][0] = change.m[1][1] = change.m[2][2] = factor;
        }
        for (int row = 0; row < 3; ++row) {
            const Vec3 r{ gizmoStart_.m[row][0], gizmoStart_.m[row][1], gizmoStart_.m[row][2] };
            for (int column = 0; column < 3; ++column)
                transform.m[row][column] = r.x * change.m[0][column] + r.y * change.m[1][column] +
                                           r.z * change.m[2][column];
        }
    }
    if (gizmoPlacement_ == kGizmoPoint) {
        if (pointSet_) pointSet_({ transform.m[3][0], transform.m[3][1], transform.m[3][2] },
                                 PointHeading(transform), false);
        gizmoNow_ = transform;
        return;
    }
    if (gizmoPlacement_ == kGizmoTool) {
        if (gizmoSection_ < tools_.size()) tools_[gizmoSection_]->GizmoChanged(transform, false, true);
        gizmoNow_ = transform;
        return;
    }
    SetPlacementTransform(gizmoSection_, gizmoPlacement_, transform);
}

float Viewport::PointHeading(const Mat4& transform) {
    const float degrees = Degrees(std::atan2(transform.m[0][1], transform.m[0][0]));
    return std::fmod(degrees + 360.0f, 360.0f);
}

void Viewport::EndGizmoDrag(bool keep) {
    if (gizmoActive_ == kGizmoNone) return;
    const int handle = gizmoActive_;
    gizmoActive_ = kGizmoNone;          // first: releasing the capture comes back here
    if (::GetCapture() == window_) ::ReleaseCapture();
    if (gizmoPlacement_ == kGizmoTool) {
        if (gizmoSection_ < tools_.size())
            tools_[gizmoSection_]->GizmoChanged(keep ? gizmoNow_ : gizmoStart_, true, keep);
        return;
    }
    if (gizmoPlacement_ == kGizmoPoint) {
        const Mat4& at = keep ? gizmoNow_ : gizmoStart_;
        if (pointSet_) pointSet_({ at.m[3][0], at.m[3][1], at.m[3][2] }, PointHeading(at), keep);
        return;
    }
    if (!keep) {
        SetPlacementTransform(gizmoSection_, gizmoPlacement_, gizmoStart_);
        CancelDetach();
        return;
    }
    detached_.clear();
    // Moved onto another section's ground: it changes hands there.
    if (gizmoMode_ == GizmoMode::Move) RehomePlacement(gizmoSection_, gizmoPlacement_);
    (void)handle;
}

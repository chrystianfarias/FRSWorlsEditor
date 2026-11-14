#include "drive/DriveMode.h"

#include "core/Log.h"
#include "drive/CarModel.h"
#include "drive/audio/EngineSound.h"
#include "drive/camera/GameCamera.h"
#include "drive/cars/CarTypeInfoU2.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/physics/VehicleSim.h"
#include "drive/world/EditorTrackWorld.h"
#include "drive/world/TrackWorld.h"
#include "nfs/World.h"
#include "render/Camera.h"
#include "render/WorldRenderer.h"
#include "ui/Resource.h"

#include "imgui.h"

#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

const wchar_t* kSettingsKey = L"Software\\FRSWorldEditor";
// The game's frame limiter: one World::DoTimestep every 1/30 s.
constexpr float kSimFrame = 1.0f / 30.0f;

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string LoadCarName() {
    wchar_t value[64] = {};
    DWORD size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"TestDriveCar", RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS)
        return {};
    std::string out;
    for (const wchar_t* c = value; *c; ++c) out += (char)*c;
    return out;
}

void StoreCarName(const std::string& name) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    const std::wstring wide = Widen(name);
    ::RegSetValueExW(key, L"TestDriveCar", 0, REG_SZ, (const BYTE*)wide.c_str(),
                     (DWORD)((wide.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// A frame whose rows are forward, left, up and the origin -- the game's own
// layout, which is also how the editor's row-vector matrices place a model.
Mat4 ModelFrame(const Vec3 axes[3], const Vec3& origin) {
    Mat4 m = Mat4::Identity();
    for (int r = 0; r < 3; ++r) {
        m.m[r][0] = axes[r].x; m.m[r][1] = axes[r].y; m.m[r][2] = axes[r].z;
    }
    m.m[3][0] = origin.x; m.m[3][1] = origin.y; m.m[3][2] = origin.z;
    return m;
}

void Orthonormalize(Vec3 axes[3]) {
    axes[0] = Normalize(axes[0]);
    axes[2] = Normalize(Cross(axes[0], axes[1]));
    axes[1] = Cross(axes[2], axes[0]);
}

} // namespace

DriveMode::DriveMode(DriveModeHost host) : host_(std::move(host)) {
    const std::string stored = LoadCarName();
    if (!stored.empty()) carName_ = stored;
}

DriveMode::~DriveMode() = default;

// ---------------------------------------------------------------------------
// The editor
// ---------------------------------------------------------------------------

void DriveMode::OnCreate() {
    host_.viewport->AddToolFirst(this);
    host_.viewport->GetRenderer().AddExtension(this);
    host_.viewport->SetFrameHook([this](float dt) { Frame(dt); });

    if (host_.menu) {
        for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
            wchar_t text[64] = {};
            ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
            if (std::wstring(text).find(L"World") == std::wstring::npos) continue;
            if (HMENU world = ::GetSubMenu(host_.menu, i)) {
                ::AppendMenuW(world, MF_SEPARATOR, 0, nullptr);
                ::AppendMenuW(world, MF_STRING, kDriveCommand, L"&Test Drive (game physics)\tCtrl+T");
            }
            break;
        }
    }
    if (host_.toolbar) {
        TBBUTTON button{};
        button.iBitmap = I_IMAGENONE;
        button.idCommand = kDriveCommand;
        button.fsState = TBSTATE_ENABLED;
        button.fsStyle = (BYTE)(BTNS_CHECK | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
        button.iString = (INT_PTR)L"Test drive";
        // Right after the game's own button.
        const int count = (int)::SendMessageW(host_.toolbar, TB_BUTTONCOUNT, 0, 0);
        int at = count;
        for (int i = 0; i < count; ++i) {
            TBBUTTON existing{};
            ::SendMessageW(host_.toolbar, TB_GETBUTTON, (WPARAM)i, (LPARAM)&existing);
            if (existing.idCommand == IDC_LAUNCH_GAME) { at = i + 1; break; }
        }
        ::SendMessageW(host_.toolbar, TB_INSERTBUTTONW, (WPARAM)at, (LPARAM)&button);
        ::SendMessageW(host_.toolbar, TB_AUTOSIZE, 0, 0);
    }
}

bool DriveMode::OnCommand(int id) {
    if (id != kDriveCommand) return false;
    if (active_) Stop();
    else Start();
    return true;
}

void DriveMode::UpdateMenuChecks(HMENU menu) {
    ::CheckMenuItem(menu, kDriveCommand, MF_BYCOMMAND | (active_ ? MF_CHECKED : MF_UNCHECKED));
    if (host_.toolbar) ::SendMessageW(host_.toolbar, TB_CHECKBUTTON, kDriveCommand, MAKELPARAM(active_ ? 1 : 0, 0));
}

void DriveMode::OnRegionLoaded() {
    if (active_) Stop();
}

void DriveMode::OnRegionClosed() {
    if (active_) Stop();
    track_.reset();
}

bool DriveMode::Held(int key) const {
    // Keys count only while the view has the keyboard: typing elsewhere does
    // not drive.
    return ::GetFocus() == host_.viewport->Handle() && ::GetKeyState(key) < 0;
}

bool DriveMode::KeyDown(WPARAM key, bool repeat) {
    if (!active_) {
        if (key == 'T' && !repeat && ::GetKeyState(VK_CONTROL) < 0 && ::GetKeyState(VK_SHIFT) >= 0) {
            Start();
            return true;
        }
        return false;
    }
    // While driving, every key is the car's: none of them may start a
    // gizmo or a tool underneath.
    if (repeat) return true;
    switch (key) {
    case VK_ESCAPE: Stop(); break;
    case 'T':
        if (::GetKeyState(VK_CONTROL) < 0) Stop();
        break;
    case 'E': shiftUp_ = true; break;
    case 'Q': shiftDown_ = true; break;
    case 'M':
        if (sim_) {
            sim_->SetManualGearbox(!sim_->ManualGearbox());
            message_ = sim_->ManualGearbox() ? L"Manual gearbox" : L"Automatic gearbox";
            messageTime_ = 2.0f;
        }
        break;
    case 'C':
        if (camera_ && gameCamera_) camera_->CycleView();
        break;
    case 'V':
        // The game's camera, or the editor's own looser chase camera.
        gameCamera_ = !gameCamera_ || !camera_;
        chaseValid_ = false;
        message_ = gameCamera_ ? L"Game camera" : L"Editor chase camera";
        messageTime_ = 2.0f;
        break;
    case 'N':
        muted_ = !muted_;
        if (sound_) sound_->SetMuted(muted_);
        message_ = muted_ ? L"Sound off" : L"Sound on";
        messageTime_ = 2.0f;
        break;
    case 'R': Respawn(false); break;
    case 'F': Respawn(true); break;
    default: break;
    }
    return true;
}

bool DriveMode::MouseDown(int, int, WPARAM) {
    // No picking while driving; the click only gives the view the keyboard.
    if (!active_) return false;
    ::SetFocus(host_.viewport->Handle());
    return true;
}

bool DriveMode::MouseWheel(int notches, WPARAM) {
    if (!active_) return false;
    chaseDistance_ = std::clamp(chaseDistance_ - notches * 0.75f, 3.0f, 30.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------

bool DriveMode::EnsureGame(std::wstring& error) {
    const std::wstring folder = host_.gameFolder ? host_.gameFolder() : std::wstring();
    if (folder.empty()) { error = L"No game folder is set."; return false; }
    if (exe_ && folder == loadedFolder_) return true;
    try {
        exe_ = std::make_unique<bb::Speed2Image>(folder + L"\\SPEED2.EXE");
        if (!exe_->IsV12()) {
            exe_.reset();
            error = L"SPEED2.EXE is not version 1.2: the physics reads its tables from the 1.2 executable.";
            return false;
        }
        cars_ = bb::CarTypeInfoU2::ReadAll(folder);
    } catch (const std::exception& e) {
        exe_.reset();
        error = L"Could not read the game: " + Widen(e.what());
        return false;
    }
    // Racers first, by name: what a person picks from.
    std::stable_sort(cars_.begin(), cars_.end(), [](const bb::CarTypeInfoU2& a, const bb::CarTypeInfoU2& b) {
        if ((a.UsageType == 0) != (b.UsageType == 0)) return a.UsageType == 0;
        return a.CollectionName < b.CollectionName;
    });
    if (cars_.empty()) { exe_.reset(); error = L"GLOBALB.BUN lists no cars."; return false; }
    car_ = 0;
    for (size_t i = 0; i < cars_.size(); ++i)
        if (cars_[i].CollectionName == carName_) car_ = i;
    loadedFolder_ = folder;
    return true;
}

bool DriveMode::EnsureCar(std::wstring& error) {
    const bb::CarTypeInfoU2& car = cars_[car_];
    if (model_ && modelLoaded_ && modelFor_ == car.CollectionName) return true;
    if (model_) model_->Release();
    model_ = std::make_unique<bb::CarModel>();
    modelLoaded_ = false;
    IDirect3DDevice9* device = host_.viewport->Device();
    if (!device) { error = L"No Direct3D device."; return false; }
    if (!model_->Load(device, loadedFolder_, car, error)) {
        // Driving without a body still tests the road: say so, keep going.
        LOG_WARN("test drive: car model %s: %S", car.CollectionName.c_str(), error.c_str());
        message_ = L"Car model not loaded: " + error;
        messageTime_ = 6.0f;
        error.clear();
        return true;
    }
    modelLoaded_ = true;
    modelFor_ = car.CollectionName;
    LOG_INFO("test drive: %s loaded, %s", car.CollectionName.c_str(), model_->ShaderStatus().c_str());
    return true;
}

bool DriveMode::Start() {
    nfs::World* world = host_.world ? host_.world() : nullptr;
    if (!world) {
        Status(L"Test drive: open a region first.");
        return false;
    }
    std::wstring error;
    if (!EnsureGame(error)) {
        Status(L"Test drive: " + error);
        ::MessageBoxW(host_.window, error.c_str(), L"Test drive", MB_ICONWARNING | MB_OK);
        return false;
    }

    // The collision is read as it is edited right now, every time.
    const auto began = std::chrono::steady_clock::now();
    track_ = bb::BuildTrackWorld(*world, *exe_, error, loadedFolder_);
    if (!track_) {
        Status(L"Test drive: " + error);
        ::MessageBoxW(host_.window, error.c_str(), L"Test drive", MB_ICONWARNING | MB_OK);
        return false;
    }
    const float buildMs =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count();
    LOG_INFO("test drive: track world built in %.1f ms", buildMs);

    // Where the view looks, facing the way it faces.
    Camera& camera = host_.viewport->GetCamera();
    Vec3 point = camera.Position() + camera.Forward() * 10.0f;
    if (host_.viewCentre) {
        Vec3 centre;
        if (host_.viewCentre(centre) && Length(centre - camera.Position()) < 400.0f) point = centre;
    }
    const float heading = camera.Yaw();
    if (!Spawn(point, heading, error)) {
        Status(L"Test drive: " + error);
        ::MessageBoxW(host_.window, error.c_str(), L"Test drive", MB_ICONWARNING | MB_OK);
        track_.reset();
        return false;
    }
    startPoint_ = point;
    startHeading_ = heading;

    active_ = true;
    ::SetFocus(host_.viewport->Handle());
    Status(L"Test drive: arrows/WASD drive, Space handbrake, Shift nitrous, Q/E gears, C camera, "
           L"R restart, F nearest ground, Esc stop");
    if (host_.menuChecks) host_.menuChecks();
    return true;
}

bool DriveMode::Spawn(const Vec3& point, float heading, std::wstring& error) {
    if (!EnsureCar(error)) return false;
    const bb::CarTypeInfoU2& car = cars_[car_];

    // On the highest ground below a little over the point (as the port's
    // city drive snaps the free-roam marker), a little above it.
    float ground = point.z;
    if (!bb::SpawnGroundHeight(*track_, point.x, point.y, point.z + 5.0f, ground)) {
        error = L"There is no collision ground under that point.";
        return false;
    }
    const float position[3] = { point.x, point.y, ground + 0.8f };
    try {
        sim_ = std::make_unique<bb::VehicleSim>(*exe_, car, track_.get(), position, heading);
        camera_ = std::make_unique<bb::GameCamera>();
        camera_->Init(*sim_, *exe_, car);
    } catch (const std::exception& e) {
        sim_.reset();
        camera_.reset();
        error = L"The physics could not start: " + Widen(e.what());
        return false;
    }
    sim_->SetNitrous(5.0f);
    if (!sound_ || soundFor_ != car.CollectionName) {
        if (sound_) sound_->Stop();
        sound_ = std::make_unique<bb::EngineSound>();
        std::wstring soundError;
        if (sound_->Start(loadedFolder_, *exe_, car, soundError)) {
            soundFor_ = car.CollectionName;
            sound_->SetMuted(muted_);
        } else {
            // Driving without the engine's sound still tests the road.
            LOG_WARN("test drive: engine sound %s: %S", car.CollectionName.c_str(), soundError.c_str());
            sound_.reset();
            soundFor_.clear();
        }
    }
    accumulator_ = 0.0f;
    previous_ = current_ = ReadPose();
    chaseValid_ = false;
    shiftUp_ = shiftDown_ = false;
    return true;
}

void DriveMode::Respawn(bool nearestGround) {
    if (!sim_) return;
    std::wstring error;
    Vec3 point = startPoint_;
    float heading = startHeading_;
    if (nearestGround) {
        // The middle of the nearest ground polygon, in plan, keeping the
        // car's heading.
        const auto p = sim_->Position();
        const auto r = sim_->Rotation3x3();
        heading = std::atan2(r[1], r[0]);
        float x = p[0], y = p[1], z = p[2];
        if (!bb::NearestGround(*track_, p[0], p[1], x, y, z)) return;
        point = { x, y, z };
    }
    if (!Spawn(point, heading, error)) {
        message_ = error;
        messageTime_ = 4.0f;
    }
}

void DriveMode::Stop(const wchar_t* why) {
    if (!active_) return;
    active_ = false;
    // Leave the editor camera where the drive left the view.
    Camera& camera = host_.viewport->GetCamera();
    if (camera.HasExplicitView()) {
        const Vec3 eye = camera.Position();
        camera.ClearExplicitView();
        if (sim_) {
            const auto g = sim_->GeometryPosition();
            camera.LookAt(eye, { g[0], g[1], g[2] + 1.0f });
        }
    }
    if (sound_) sound_->Stop();
    sound_.reset();
    soundFor_.clear();
    camera_.reset();
    sim_.reset();
    track_.reset();
    Status(why ? why : L"Test drive stopped.");
    if (host_.menuChecks) host_.menuChecks();
}

void DriveMode::SelectCar(size_t index) {
    if (index >= cars_.size() || index == car_) return;
    car_ = index;
    carName_ = cars_[car_].CollectionName;
    StoreCarName(carName_);
    if (!sim_) return;
    // The new car where the old one is, facing the same way.
    const auto g = sim_->GeometryPosition();
    const auto r = sim_->Rotation3x3();
    std::wstring error;
    if (!Spawn({ g[0], g[1], g[2] + 1.0f }, std::atan2(r[1], r[0]), error)) {
        message_ = error;
        messageTime_ = 4.0f;
    }
}

// ---------------------------------------------------------------------------
// Each frame
// ---------------------------------------------------------------------------

DriveMode::Pose DriveMode::ReadPose() const {
    Pose pose;
    const auto g = sim_->GeometryPosition();
    const auto p = sim_->Position();
    const auto r = sim_->Rotation3x3();   // rows forward, left, up
    pose.position = { g[0], g[1], g[2] };
    for (int k = 0; k < 3; ++k) pose.axes[k] = { r[3 * k], r[3 * k + 1], r[3 * k + 2] };
    // Wheel centre = CG + R * (attach point + compression), as the port's
    // DriveDemo places it, brought into the model's frame.
    const Vec3 cg{ p[0] - g[0], p[1] - g[1], p[2] - g[2] };
    const Vec3 cgLocal{ Dot(cg, pose.axes[0]), Dot(cg, pose.axes[1]), Dot(cg, pose.axes[2]) };
    for (int i = 0; i < 4; ++i) {
        const auto w = sim_->WheelLocalPosition(i);
        pose.wheelOffset[i] = { cgLocal.x + w[0], cgLocal.y + w[1], cgLocal.z + w[2] };
        pose.wheelSpin[i] = sim_->WheelSpinAngle(i);
        pose.wheelSteer[i] = sim_->WheelSteerAngle(i);
    }
    return pose;
}

DriveMode::Pose DriveMode::Blend(float alpha) const {
    Pose pose = current_;
    pose.position = Lerp(previous_.position, current_.position, alpha);
    for (int k = 0; k < 3; ++k) pose.axes[k] = Lerp(previous_.axes[k], current_.axes[k], alpha);
    Orthonormalize(pose.axes);
    for (int i = 0; i < 4; ++i) pose.wheelOffset[i] = Lerp(previous_.wheelOffset[i], current_.wheelOffset[i], alpha);
    return pose;
}

void DriveMode::SimFrame() {
    if (camera_) camera_->BeforeSimFrame();
    const bool left = Held(VK_LEFT) || Held('A');
    const bool right = Held(VK_RIGHT) || Held('D');
    const bool up = Held(VK_UP) || Held('W');
    const bool down = Held(VK_DOWN) || Held('S');
    const bool handbrake = Held(VK_SPACE);
    const bool nitrous = Held(VK_SHIFT);
    // Through the game's own PC keyboard layer: key mask -> joystick events
    // -> RealDriver handlers, with the steering ramps the game has.
    sim_->FrameKeyboard(kSimFrame, left, right, up, down, handbrake, nitrous, shiftUp_, shiftDown_);
    // A shift press is "held" for exactly one game frame.
    shiftUp_ = shiftDown_ = false;
    if (camera_) camera_->AfterSimFrame();
    if (sound_) sound_->SimFrame(*sim_);
}

void DriveMode::Frame(float deltaSeconds) {
    if (!active_ || !sim_) return;
    if (messageTime_ > 0.0f) messageTime_ -= deltaSeconds;
    if (camera_) camera_->SetLookBack(Held('B'));

    const auto began = std::chrono::steady_clock::now();
    accumulator_ += std::min(deltaSeconds, 0.25f);
    int frames = 0;
    while (accumulator_ >= kSimFrame && frames < 4) {
        accumulator_ -= kSimFrame;
        previous_ = current_;
        try {
            SimFrame();
        } catch (const std::exception& e) {
            LOG_ERROR("test drive: physics stopped: %s", e.what());
            Stop((L"Test drive stopped: the physics failed (" + Widen(e.what()) + L")").c_str());
            return;
        }
        current_ = ReadPose();
        ++frames;
    }
    if (frames >= 4) accumulator_ = 0.0f;
    if (frames > 0)
        physicsMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count() /
                     (float)frames;

    // Fell out of the world: back on the nearest ground.
    if (current_.position.z < -500.0f) Respawn(true);

    PlaceCamera(std::clamp(accumulator_ / kSimFrame, 0.0f, 1.0f));

    if (sound_) {
        // Heard from where the view is: the distance filter and the
        // inside-the-car level follow the camera.
        const Vec3 eye = host_.viewport->GetCamera().Position();
        sound_->SetListenerDistance(Length(eye - current_.position));
        sound_->SetInternalView(gameCamera_ && camera_ && camera_->HidesCar());
        sound_->Update(*sim_, deltaSeconds);
    }
}

void DriveMode::PlaceCamera(float alpha) {
    Camera& camera = host_.viewport->GetCamera();
    const float width = (float)std::max(1u, host_.viewport->ViewWidth());
    const float height = (float)std::max(1u, host_.viewport->ViewHeight());
    const Pose pose = Blend(alpha);

    if (gameCamera_ && camera_) {
        float eye[3], right[3], down[3], forward[3];
        camera_->Pose(alpha, eye, right, down, forward);
        float fov = Radians(55.0f), aspect = width / height, nearZ = 0.25f, farZ = 15000.0f;
        camera_->Projection((int)width, (int)height, fov, aspect, nearZ, farZ);
        const Vec3 e{ eye[0], eye[1], eye[2] };
        const Vec3 f{ forward[0], forward[1], forward[2] };
        const Vec3 u{ -down[0], -down[1], -down[2] };
        camera.SetExplicitView(LookAtRH(e, e + f, u), PerspectiveRH(fov, aspect, nearZ, farZ));
        return;
    }
    ChaseCamera(pose, 1.0f / 60.0f);
    camera.SetExplicitView(LookAtRH(chaseEye_, pose.position + pose.axes[2] * 1.0f, { 0.0f, 0.0f, 1.0f }),
                           PerspectiveRH(Radians(60.0f), width / height, 0.25f, 15000.0f));
}

void DriveMode::ChaseCamera(const Pose& pose, float deltaSeconds) {
    // A loose spring behind the car, level with the horizon.
    Vec3 back = pose.axes[0];
    back.z = 0.0f;
    back = Normalize(back);
    if (Length(back) < 0.5f) back = { 1.0f, 0.0f, 0.0f };
    const Vec3 want = pose.position - back * chaseDistance_ + Vec3{ 0.0f, 0.0f, 0.35f * chaseDistance_ };
    if (!chaseValid_) {
        chaseEye_ = want;
        chaseValid_ = true;
        return;
    }
    const float k = 1.0f - std::exp(-deltaSeconds * 6.0f);
    chaseEye_ = Lerp(chaseEye_, want, k);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DriveMode::DrawWithWorld(IDirect3DDevice9* device, WorldRenderer& renderer) {
    if (!active_ || !sim_ || !model_ || !modelLoaded_) return;
    if (gameCamera_ && camera_ && camera_->HidesCar()) return;
    const Pose pose = Blend(std::clamp(accumulator_ / kSimFrame, 0.0f, 1.0f));
    const Mat4 body = ModelFrame(pose.axes, pose.position);

    Mat4 wheels[4];
    for (int i = 0; i < 4; ++i) {
        // Spin about the axle (the car's left axis), then steer about its up.
        const float spin = pose.wheelSpin[i], steer = pose.wheelSteer[i];
        const float cs = std::cos(spin), ss = std::sin(spin);
        const float ct = std::cos(steer), st = std::sin(steer);
        // In the car's frame: forward, left, up after the two rotations.
        const Vec3 spunForward{ cs, 0.0f, -ss };
        const Vec3 spunUp{ ss, 0.0f, cs };
        auto steerZ = [&](const Vec3& v) { return Vec3{ ct * v.x - st * v.y, st * v.x + ct * v.y, v.z }; };
        const Vec3 local[3] = { steerZ(spunForward), steerZ({ 0.0f, 1.0f, 0.0f }), steerZ(spunUp) };
        Vec3 axes[3];
        for (int k = 0; k < 3; ++k)
            axes[k] = pose.axes[0] * local[k].x + pose.axes[1] * local[k].y + pose.axes[2] * local[k].z;
        const Vec3 o = pose.wheelOffset[i];
        const Vec3 centre = pose.position + pose.axes[0] * o.x + pose.axes[1] * o.y + pose.axes[2] * o.z;
        wheels[i] = ModelFrame(axes, centre);
    }
    model_->Draw(device, renderer, body, wheels);
}

void DriveMode::ReleaseGpu() {
    if (model_) model_->Release();
    modelLoaded_ = false;
    modelFor_.clear();
}

void DriveMode::DrawHud(float x, float y, float width, float height) {
    (void)height;
    if (!active_ || !sim_) return;
    ImGui::SetNextWindowPos(ImVec2(x + 12.0f, y + 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##testdrive", nullptr, flags)) {
        // The car, chosen from the game's list.
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::BeginCombo("##car", cars_[car_].CollectionName.c_str())) {
            for (size_t i = 0; i < cars_.size(); ++i) {
                if (i > 0 && cars_[i].UsageType != cars_[i - 1].UsageType) ImGui::Separator();
                const bool selected = i == car_;
                if (ImGui::Selectable(cars_[i].CollectionName.c_str(), selected)) SelectCar(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) Stop();

        const float kmh = std::fabs(sim_->ForwardSpeed()) * 3.6f;
        const int gear = sim_->Gear();
        char gearText[8];
        if (gear == 0) std::snprintf(gearText, sizeof gearText, "R");
        else if (gear == 1) std::snprintf(gearText, sizeof gearText, "N");
        else std::snprintf(gearText, sizeof gearText, "%d", gear - 1);
        ImGui::Text("%5.0f km/h   gear %s%s", kmh, gearText, sim_->ManualGearbox() ? " (manual)" : "");
        const float rpm = sim_->Rpm();
        ImGui::ProgressBar(std::clamp(rpm / 9000.0f, 0.0f, 1.0f), ImVec2(240.0f, 0.0f), nullptr);
        ImGui::SameLine();
        ImGui::Text("%.0f rpm", rpm);
        if (sim_->NitrousActive()) ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "NITROUS");
        if (camera_ && gameCamera_) ImGui::TextDisabled("camera: %s", camera_->ViewName().c_str());
        ImGui::TextDisabled("physics %.2f ms / frame", physicsMs_);
        if (messageTime_ > 0.0f && !message_.empty()) {
            std::string text;
            for (wchar_t c : message_) text += c < 128 ? (char)c : '?';
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", text.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("WASD/arrows drive  Space handbrake  Shift nitrous");
        ImGui::TextDisabled("Q/E gears  M manual  C/V camera  B look back");
        ImGui::TextDisabled("R restart  F nearest ground  N sound  Esc stop");
    }
    ImGui::End();
    (void)width;
}

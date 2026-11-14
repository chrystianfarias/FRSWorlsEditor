#include "drive/fx/DriveFx.h"

#include "core/Log.h"
#include "drive/CarModel.h"
#include "drive/numerics/BWareMath.h"
#include "drive/numerics/Speed2Image.h"
#include "nfs/World.h"
#include "render/WorldRenderer.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>

namespace bb {

namespace {
constexpr uint32_t kRainInTheHeadlightsVa = 0x803A70;   // RainInTheHeadlights, .data, 0.96

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring ExePath(const std::wstring& folder) {
    std::wstring exe = folder;
    if (!exe.empty() && exe.back() != L'\\' && exe.back() != L'/') exe += L'\\';
    return exe + L"SPEED2.EXE";
}
} // namespace

DriveFx::DriveFx() = default;
DriveFx::~DriveFx() { ReleaseGpu(); }

void DriveFx::ReleaseGpu() {
    car_.Release();
    carFor_ = nullptr;
    projector_.Release();
    polys_.Release();
    globals_.Release();
    device_ = nullptr;
}

bool DriveFx::EnsureDevice(IDirect3DDevice9* device, const std::wstring& gameFolder) {
    if (!device || gameFolder.empty()) return false;
    if (device_ == device && folder_ == gameFolder) return polys_.Ready();
    ReleaseGpu();
    device_ = device;
    folder_ = gameFolder;
    if (!globals_.Load(gameFolder)) LOG_WARN("game fx: cannot read GLOBAL\\GLOBALB.BUN");
    polys_.Create(device, ExePath(gameFolder));
    projector_.Create(device, ExePath(gameFolder));
    LOG_INFO("game fx: %s; %s", polys_.Status().c_str(), projector_.Status().c_str());
    if (!exeTried_) {
        exeTried_ = true;
        try {
            exe_ = std::make_unique<Speed2Image>(ExePath(gameFolder));
            math_ = std::make_unique<BWareMath>(*exe_);
        } catch (const std::exception& e) {
            LOG_WARN("game fx: SPEED2.EXE not read (%s): default flare table", e.what());
        }
    }
    return polys_.Ready();
}

void DriveFx::EnsureCar(IDirect3DDevice9* device, const CarModel* car) {
    if (car == carFor_) return;
    car_.Release();
    carFor_ = car;
    if (car && car_.Init(device, *car)) LOG_INFO("game fx: %s", car_.Describe().c_str());
}

void DriveFx::EnsureWorld(const nfs::World* world, const std::wstring& gameFolder) {
    (void)gameFolder;
    if (!world) {
        worldFlares_.Clear();
        return;
    }
    if (worldFlares_.LoadedFor(world, world->Loaded().size())) return;
    worldFlares_.Load(*world);
    if (exe_ && !worldFlares_.HasExeTable()) {
        FlareTableU2 t = CarLightFlaresU2::TableFromExe(*exe_);
        // RenderFlares 0x61AE90 patches types 0 and 1 every frame before the cars (view 1, in game).
        CarLightFlaresU2::PatchForView(t, 1, true);
        worldFlares_.SetTable(std::move(t), exe_->F32(kRainInTheHeadlightsVa));
    }
    LOG_INFO("game fx: %s", worldFlares_.Describe().c_str());
}

bool DriveFx::BlinkOn() {
    // sub_48AC00, once per frame: flt_8358E4 += WorldTimeElapsed; above 0.5 s dword_82DAF0 flips and it restarts.
    const double t = Now();
    const double dt = lastTime_ > 0.0 ? t - lastTime_ : 0.0;
    lastTime_ = t;
    blinkClock_ += dt < 0.25 ? dt : 0.25;
    if (blinkClock_ > 0.5) {
        blink_ = !blink_;
        blinkClock_ = 0.0;
    }
    return blink_;
}

void DriveFx::BeforeCar(IDirect3DDevice9* device, WorldRenderer& renderer, const DriveFxFrame& frame) {
    counts_.projectorDraws = 0;
    counts_.shadowStrips = 0;
    counts_.car = frame.car != nullptr;
    counts_.carVisible = frame.carVisible;
    if (!frame.car || !EnsureDevice(device, frame.gameFolder)) return;
    EnsureCar(device, frame.car);
    counts_.carReady = car_.Ready();
    counts_.projectorReady = projector_.Ready();
    const fx::DeviceCamera cam = fx::ReadDeviceCamera(device);
    fx::StateGuard guard(device);
    if (projectorOn_ && projector_.Ready())
        counts_.projectorDraws = projector_.Draw(guard, renderer, cam, frame.body, car_.HeadlightColour(), globals_);
    if (frame.carVisible && car_.Ready() && shadowLevel_ > 0) {
        car_.SetShadowLevel(shadowLevel_);
        polys_.Begin(guard, cam);
        car_.DrawShadow(guard, polys_, cam, frame.body, frame.ground, frame.math ? frame.math : math_.get(), globals_);
        counts_.shadowStrips = (int)car_.Shadow().Strips.size();
    }
}

void DriveFx::AfterCar(IDirect3DDevice9* device, WorldRenderer& renderer, const DriveFxFrame& frame) {
    (void)renderer;
    counts_.worldQuads = 0;
    counts_.carQuads = 0;
    const bool blink = BlinkOn();
    if (!flaresOn_) return;
    if (!frame.worldFlares && !frame.car) return;
    if (!EnsureDevice(device, frame.gameFolder)) return;
    const fx::DeviceCamera cam = fx::ReadDeviceCamera(device);
    const FlareViewU2 view = fx::MakeFlareView(cam, 1);
    fx::StateGuard guard(device);
    polys_.Begin(guard, cam);
    if (frame.worldFlares && frame.world) {
        EnsureWorld(frame.world, frame.gameFolder);
        // TheVisibleSectionManager's set for the camera position (sub_5E00D0).
        std::vector<u32> visible;
        const bool zone = frame.world->Visibility().VisibleAt(cam.eye, visible);
        WorldFlaresU2::PlayerCar player;
        const WorldFlaresU2::PlayerCar* car = nullptr;
        if (frame.car) {
            for (int k = 0; k < 3; ++k) {
                player.position[k] = frame.body.m[3][k];
                player.forward[k] = frame.body.m[0][k];
            }
            car = &player;
        }
        counts_.worldQuads =
            worldFlares_.Draw(guard, polys_, view, globals_, device, zone ? &visible : nullptr, blink, car);
    }
    if (frame.car && frame.carVisible) {
        EnsureCar(device, frame.car);
        // FRS_FX_FORCEBRAKE=1: the game's own debug ForceBrake (0x8A1BF4, RenderFlaresOnCar flag 2).
        static const bool forceBrake = [] {
            const char* e = std::getenv("FRS_FX_FORCEBRAKE");
            return e && *e == '1';
        }();
        car_.SetState(frame.braking, frame.headlightsOn);
        car_.SetForceFlags(forceBrake ? 2 : 0);
        car_.DrawFlares(guard, polys_, view, frame.body, globals_, &counts_.carQuads);
    }
    // FRS_FX_REPORT=1: the counts once a second in the log, for checks.
    static const bool report = [] {
        const char* e = std::getenv("FRS_FX_REPORT");
        return e && *e == '1';
    }();
    if (report) {
        const double t = Now();
        if (t - lastReport_ >= 1.0) {
            lastReport_ = t;
            LOG_INFO("game fx: camera (%.1f %.1f %.1f) focal %.1f %dx%d: %s", cam.eye.x, cam.eye.y, cam.eye.z, view.Focal,
                     view.Width, view.Height, Status().c_str());
        }
    }
}

std::string DriveFx::Status() const {
    char line[256];
    std::snprintf(line, sizeof line,
                  "flares %d world + %d car, shadow %d strips, headlights %d draws (car %d visible %d ready %d, projector %d)",
                  counts_.worldQuads, counts_.carQuads, counts_.shadowStrips, counts_.projectorDraws, counts_.car ? 1 : 0,
                  counts_.carVisible ? 1 : 0, counts_.carReady ? 1 : 0, counts_.projectorReady ? 1 : 0);
    return line;
}

} // namespace bb

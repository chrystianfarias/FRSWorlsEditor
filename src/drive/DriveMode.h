#pragma once

#include "core/Math.h"
#include "render/RenderExtension.h"
#include "ui/EditorExtension.h"
#include "ui/Viewport.h"

#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nfs { class World; }
namespace bb {
class Speed2Image;
class CarTypeInfoU2;
class CarModel;
class GameCamera;
class EngineSound;
class TrackWorld;
class VehicleSim;
}

struct DriveModeHost {
    HWND      window = nullptr;
    HMENU     menu = nullptr;
    HWND      toolbar = nullptr;
    Viewport* viewport = nullptr;
    std::function<nfs::World*()> world;
    std::function<std::wstring()> gameFolder;
    // The ground under the middle of the view, as for placing an object.
    std::function<bool(Vec3&)> viewCentre;
    std::function<void(const std::wstring&)> status;
    std::function<void()> menuChecks;
};

// Test drive: the player's car, driven by NFSU2's own vehicle physics (the
// bit-exact port in src/drive/physics) on the region's collision as it is
// edited, without starting the game.
//
// Ctrl+T or the toolbar's "Test drive" starts it where the view is looking,
// facing the way the camera faces; Esc goes back to the editor camera. The
// simulation runs at the game's 30 frames a second (its frame limiter) and the
// picture is interpolated between them, as the Unity port does.
//
// Keys, as the game's keyboard defaults: arrows or WASD drive, Space is the
// handbrake, Shift nitrous, Q / E shift down / up (M toggles the manual
// gearbox), C cycles the game's camera views, B looks back, R puts the car
// back at the start, F on the nearest ground, Esc stops.
class DriveMode : public EditorExtension, public ViewportTool, public RenderExtension {
public:
    explicit DriveMode(DriveModeHost host);
    ~DriveMode() override;

    static constexpr int kDriveCommand = 44901;

    bool Active() const { return active_; }

    // ---- EditorExtension
    void OnCreate() override;
    bool OnCommand(int id) override;
    void UpdateMenuChecks(HMENU menu) override;
    void OnRegionLoaded() override;
    void OnRegionClosed() override;
    void DrawHud(float x, float y, float width, float height) override;

    // ---- ViewportTool
    bool KeyDown(WPARAM key, bool repeat) override;
    bool MouseDown(int x, int y, WPARAM keys) override;
    bool MouseWheel(int notches, WPARAM keys) override;

    // ---- RenderExtension
    void DrawWithWorld(IDirect3DDevice9* device, WorldRenderer& renderer) override;
    void ReleaseGpu() override;

private:
    struct Pose {
        Vec3 position;              // the model's origin (CarState geometry position)
        Vec3 axes[3];               // forward, left, up
        Vec3 wheelOffset[4];        // wheel centres in the car's frame, from the model's origin
        float wheelSpin[4] = {};
        float wheelSteer[4] = {};
    };

    bool Start();
    void Stop(const wchar_t* why = nullptr);
    bool EnsureGame(std::wstring& error);
    bool EnsureCar(std::wstring& error);
    bool Spawn(const Vec3& point, float heading, std::wstring& error);
    void Respawn(bool nearestGround);
    void Frame(float deltaSeconds);       // Viewport frame hook
    void SimFrame();
    Pose ReadPose() const;
    Pose Blend(float alpha) const;
    void PlaceCamera(float alpha);
    void ChaseCamera(const Pose& pose, float deltaSeconds);
    void SelectCar(size_t index);
    void Status(const std::wstring& text) const { if (host_.status) host_.status(text); }
    bool Held(int key) const;

    DriveModeHost host_;
    bool active_ = false;

    // Read once from the game folder, kept while it does not change.
    std::wstring loadedFolder_;
    std::unique_ptr<bb::Speed2Image> exe_;
    std::vector<bb::CarTypeInfoU2> cars_;   // racers first
    size_t car_ = 0;                        // index into cars_
    std::string carName_ = "350Z";          // remembered between sessions

    // Per drive.
    std::unique_ptr<bb::TrackWorld> track_;
    std::unique_ptr<bb::VehicleSim> sim_;
    std::unique_ptr<bb::GameCamera> camera_;
    // The engine as the game plays it (src/drive/audio); kept across
    // respawns of the same car so R does not reopen the device.
    std::unique_ptr<bb::EngineSound> sound_;
    std::string soundFor_;
    bool muted_ = false;
    std::unique_ptr<bb::CarModel> model_;
    bool modelLoaded_ = false;
    std::string modelFor_;

    Vec3  startPoint_;
    float startHeading_ = 0.0f;
    float accumulator_ = 0.0f;
    Pose  previous_, current_;
    bool  shiftUp_ = false, shiftDown_ = false;
    bool  gameCamera_ = true;               // false: the editor's own chase camera
    Vec3  chaseEye_;
    bool  chaseValid_ = false;
    float chaseDistance_ = 6.5f;
    std::wstring message_;                  // shown in the HUD for a few seconds
    float messageTime_ = 0.0f;
    float physicsMs_ = 0.0f;
};

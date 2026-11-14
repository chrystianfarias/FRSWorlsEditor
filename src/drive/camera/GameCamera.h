#pragma once

// C++ counterpart of NFSU2Unity Runtime/GameCamera.cs without Unity: the game's in-race drive camera
// (CubicCameraMover, bit-exact port) driven around the ported vehicle simulation.
// The owner calls BeforeSimFrame / AfterSimFrame around every simulation frame (the camera runs once per game
// frame, after World::DoTimestep, like UpdateCameraMovers) and Pose every rendered frame with the same
// interpolation factor used for the car visual.
// C cycles the views like the game (bumper, hood, near chase, far chase [, high chase in drift/drag]),
// B looks back while held.

#include "drive/camera/CameraCar.h"
#include "drive/camera/CameraData.h"
#include "drive/camera/CameraProjection.h"
#include "drive/camera/CubicCameraMover.h"
#include "drive/camera/ViewCamera.h"

#include <memory>
#include <string>

namespace bb {

class VehicleSim;
class Speed2Image;
class CarTypeInfoU2;

class GameCamera {
public:
    // ePlayerSettingsCameras: 0 bumper, 1 hood, 2 near chase, 3 far chase (game default), 4 high chase.
    int ViewSetting = 3;
    // Allow the fifth view (high chase), as in drift races / drag.
    bool AllowHighChase = false;

    // Creates the mover for sim (game: Player::ChangeCameraMover(1, ...)). sim and exe must outlive this object.
    void Init(VehicleSim& sim, const Speed2Image& exe, const CarTypeInfoU2& carType);

    // C key (Player::CycleDriveCameraPovType): scroll the setting, then SetPovType if the car has that POV.
    void CycleView();
    // Selects a drive-camera setting (0 bumper .. 4 high chase) like Player::CycleDriveCameraPovType does.
    void SetView(int setting);
    // B key: looks back while held.
    void SetLookBack(bool held);

    // Right before VehicleSim::Frame*: keeps the velocity at Car::BeginMovement (Car+0x520).
    void BeforeSimFrame();
    // Right after VehicleSim::Frame*: MaybeCameraShake, CubicCameraMover::Update, UpdateCameraShakers.
    void AfterSimFrame();

    // Interpolated pose between the last two game frames (alpha 0..1), game space (Z up): eye position and the
    // camera's axes in world space (right, down, forward as in the game's view matrix columns).
    void Pose(float alpha, float eye[3], float right[3], float down[3], float forward[3]) const;

    // Projection like CameraProjection::Build (vertical FOV in radians, near, far) for a viewport of
    // width x height px. aspect = width / height of the equivalent symmetric frustum.
    void Projection(int width, int height, float& verticalFovRadians, float& aspect, float& nearZ, float& farZ) const;

    // Bumper view (and hood when looking back): the car is not drawn.
    bool HidesCar() const;
    std::string ViewName() const;

    bool Ready() const { return Mover != nullptr; }
    const CubicCameraMover* GetMover() const { return Mover.get(); }

private:
    void Step();

    VehicleSim* sim_ = nullptr;
    std::unique_ptr<CameraData> data_;
    std::unique_ptr<CameraContext> ctx_;
    std::unique_ptr<CameraCar> car_;
    std::unique_ptr<CameraInfo> cameraInfo_;
    std::unique_ptr<ViewCamera> view_;
    std::unique_ptr<CubicCameraMover> Mover;
    float velAtBegin_[3] = {};
    float prevPos_[3] = {}, curPos_[3] = {};
    float prevRot_[4] = { 0, 0, 0, 1 }, curRot_[4] = { 0, 0, 0, 1 };   // quaternions (x, y, z, w)
    bool hasPrev_ = false;
};

} // namespace bb

#pragma once

// Port of NFSU2Unity Core/Camera/CameraEffects.cs.

#include "drive/camera/CameraCar.h"
#include "drive/camera/CameraData.h"
#include "drive/camera/ViewCamera.h"
#include "drive/numerics/X87.h"

#include <array>
#include <cstdint>
#include <vector>

namespace bb {

// CameraMover::TerrainVelocityNoise 0x452ED0 and the noise it applies (sub_441340): speed/terrain/nitrous
// dependent camera wobble (docs/re/camera.md §5).
namespace CameraNoise {

// sub_4411B0: 1D value noise (integer hash, linear interpolation), st0.
F24 Noise1(const CameraData& d, float x);

// sub_441270: six octaves, frequency x2 and amplitude x0.5 per octave.
F24 Octaves(const CameraData& d, float x);

// sub_4412E0: scales a shake angle by the camera FOV, asin(sin(a) * sin(fov/2) / sin(ref/2)).
uint16_t FovScaledAngle(const CameraData& d, const ViewCamera& cam, int a);

// sub_441340(cam, m, time, scale): applies the noise stored in the camera key to the view matrix.
void Apply(const CameraData& d, const ViewCamera& cam, float* m, float time, float scale);

// sub_446D00 + sub_5766E0 + sub_43D100: Table lookup over vec4 entries (count, x0 = 0, inv).
std::array<float, 4> SpeedTable(const CameraData& d, float inv, float x);

// CameraMover::TerrainVelocityNoise(m, car, ampScale, freqScale) 0x452ED0.
void TerrainVelocityNoise(const CameraData& d, ViewCamera& cam, float* m, const CameraCar& car, float ampScale, float freqScale, int worldTimer);

} // namespace CameraNoise

// One camera shaker (0x20 bytes at 0x865360 + i*0x20; i = player number).
class CameraShaker {
public:
    float Dir[3] = {};       // +0x00 (negated, scaled direction)
    float FieldC = 0;        // +0x0C
    bool Active = false;     // +0x10 curve pointer (off_7FB7A0) != 0
    float T = 0;             // +0x14 normalised time
    float Duration = 0;      // +0x18
    float Flicker = 0;       // +0x1C

    // sub_5722A0: piecewise-linear curve value (0 outside [x0, xLast)).
    static F24 Curve(const std::vector<float>& p, float x);

    // sub_572860: current offset (curve value on even flicker half-periods, else 0) times Dir.
    std::array<float, 3> Offset(const CameraData& d) const;

    // UpdateCameraShakers 0x5728C0 (per shaker; runs after UpdateCameraMovers).
    void Update(const CameraData& d, float dt);

    // MaybeCameraShake(player, &car.Accel) 0x572E50 (called from World::DoTimestep per player car).
    void Maybe(const CameraData& d, const float* accel, bool povInCar, int playerCinematic);

    // ApplyCameraShake(viewId, m) 0x572A50 for this shaker (caller picks shaker 0 for views 1/3, 1 for view 2).
    void Apply(const CameraData& d, float* m, float worldTimeScale) const;
};

} // namespace bb

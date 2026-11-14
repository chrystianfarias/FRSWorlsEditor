#pragma once

// Port of NFSU2Unity Core/Camera/CubicCameraMover.cs.

#include "drive/camera/CameraCar.h"
#include "drive/camera/CameraData.h"
#include "drive/camera/CameraEffects.h"
#include "drive/camera/Cubic.h"
#include "drive/camera/ViewCamera.h"
#include "drive/physics/World.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace bb {

// Globals the drive camera reads (game frame context).
class CameraContext {
public:
    int WorldTimer = 0;                       // 0x86518C (4000 Hz ticks)
    float WorldTimeElapsed = 0;               // 0x865154 (dt of this frame's World step; 0 when paused)
    // C# shares the World's RaceModeFlags object by reference: Flags points to OwnFlags by default and can be
    // pointed at the simulation's World::Flags.
    RaceModeFlags OwnFlags;
    const RaceModeFlags* Flags = &OwnFlags;
    bool SplitScreen = false;                 // eGetCurrentViewMode() == 3 -> POV set 1
    float WorldTimeScale = 1.0f;              // pCurrentWorld+0x108 (slow motion)
    CameraShaker Shakers[2];                  // 0x865360
    int PlayerCinematic = 0;                  // Player+0x58
    bool HasPlayer = false;                   // mover+0x20 != 0
    bool UseBirdsEye = false;                 // Player::UseBirdsEye
    int FlushAccumulationCount = 0;           // FlushAccumulationBuffer() calls (motion-blur reset)

    // World::IsInCar(point, 0.5, car) used by CameraWouldBeInCar; empty = never inside another car.
    std::function<bool(const float*, float)> IsInOtherCar;
    // TopologyCoordinate::ConstrainToTopology(type 1, true, from, to, 0.5, 1) on the car topology; empty = false.
    std::function<bool(float*, float*)> ConstrainToTopology;
    // CameraMover::MinGapTopology / MinGapCars (camera vs world / other cars); empty = no-op (not ported).
    std::function<void(float*)> MinGapTopology, MinGapCars;

    CameraContext() = default;
    CameraContext(const CameraContext&) = delete;
    CameraContext& operator=(const CameraContext&) = delete;
};

// Running average of the last N vectors (sub_446E70 ctor, sub_446F10 add); N = 4 for the drive camera.
class VectorAverager {
public:
    int Size;                    // +0x05
    uint8_t Count = 0;           // +0x06
    uint8_t Index = 0;           // +0x07
    std::vector<float> Buffer;   // [+0x08] N x 16 bytes (xyz used)
    float Sum[3] = {};           // +0x10
    float Avg[3] = {};           // +0x20

    explicit VectorAverager(int n) : Size(n), Buffer(static_cast<size_t>(4 * n), 0.0f) {}

    void Reset();
    void Add(const float* v);
};

// CubicCameraMover (type 1, 0x60 bytes, vtable 0x787F28): the in-race drive camera of every player view
// (far/near chase, bumper, hood, high chase). Port of the ctor 0x451660, Update 0x453570 and helpers
// (docs/re/camera.md §3-§4).
class CubicCameraMover {
public:
    const CameraData* Data;
    CameraContext* Ctx;
    ViewCamera* Camera;           // +0x1C
    int ViewId;                   // +0x10
    CameraCar* Car;               // +0x2C
    int Field30 = 0;              // +0x30 (0; lookback is ignored when != 0)
    Cubic1D Fov;                  // +0x34
    Cubic3D Eye;                  // +0x38 car space
    Cubic3D Look;                 // +0x3C car space
    Cubic3D Forward;              // +0x40 smoothed heading (world)
    Cubic3D Up;                   // +0x44 up vector (x, y; z unused)
    int PovType = 0;              // +0x48 requested POV
    int EffectivePov = 0;         // +0x4C POV used (falls back to 0)
    bool LagEnabled = false;      // +0x50 (= !ctor arg_C; SetDisableLag)
    bool LookBack = false;        // +0x51
    bool Snap = false;            // +0x52
    bool NoDof = false;           // +0x53
    uint8_t Field54 = 0;          // +0x54
    int GroundTimer = 0;          // +0x58 WorldTimer when last on the ground
    VectorAverager Averager{4};   // +0x5C

    // CubicCameraMover::CubicCameraMover(view, car, pov, disableLag, lookBack, noDof) 0x451660. The base ctor sets
    // camera far = 10000 and Enable sets near = 0.5. Player::ChangeCameraMover passes (player+0x18, car,
    // GetPovType(), player+0x2A != 0, player+0x249, true). All referenced objects must outlive the mover.
    CubicCameraMover(const CameraData& data, CameraContext& ctx, ViewCamera& camera, int viewId, CameraCar& car, int pov,
                     bool disableLag, bool lookBack, bool noDof);

    // OutsidePOV 0x445A90 (vtbl+0x24): POV of +0x4C is far/near/high chase.
    bool OutsidePOV() const;

    // HighliteMode 0x445AC0: outside POV and (hydraulics, or Mode != 0 with at most two wheels down).
    bool HighliteMode() const;

    // SetPovType 0x445B10 (vtbl+0x20; Player::CycleDriveCameraPovType).
    void SetPovType(int pov) { Snap = true; PovType = pov; }

    // SetLookBack 0x445A30 (vtbl+0x14).
    void SetLookBack(bool on) { if (LookBack != on) Ctx->FlushAccumulationCount++; LookBack = on; }

    // SetDisableLag 0x44DE60 (vtbl+0x1C).
    void SetDisableLag(bool disable) { LagEnabled = !disable; }

    // ResetState 0x445B20 (vtbl+0x38): snaps all five curves and flags the camera velocity reset.
    void ResetState(int realTimeFrames);

    // CubicCameraMover::Update 0x453570 (once per rendered frame, after World::DoTimestep).
    void Update();

private:
    void SeedFov(Cubic1D& c);
    void SeedVec(Cubic3D& c, const float* inv, const float* pos, const float* rate, const float* carVel);
    bool GetPov(int type, Pov& p) const;
    static bool IsOutsideType(int t) { return t == 1 || t == 0 || t == 5; }
    void ApplyCameraShake(float* m);
    bool CameraWouldBeInCar() const;
    std::array<float, 16> CarMatrix() const;
    void GetEyeOffsetAndAngle(const Pov& pov, float* off, uint16_t& angle) const;
    void SetForward(const Pov& pov, bool snap);
    void MakeSpace(float* m) const;
    void SetDesired(float* m, const Pov& pov, const CubicPovData& pd, bool snap);
};

} // namespace bb

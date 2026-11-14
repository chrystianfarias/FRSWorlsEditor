#pragma once

// Port of NFSU2Unity Core/Camera/CameraCar.cs.

#include "drive/camera/CameraData.h"
#include "drive/numerics/X87.h"

#include <functional>

namespace bb {

class VehicleSim;

// The Car fields the drive camera reads (offsets of the game's Car object; CarState = Car+0x40). Filled once
// per frame after World::DoTimestep (see FromSim for the ported vehicle simulation).
class CameraCar {
public:
    float GeomPos[3] = {};   // +0x050 CarState+0x10 (pos - R*cg)
    float Pos[3] = {};       // +0x060 CarState+0x20 (CG)
    float Vel[3] = {};       // +0x070 CarState+0x30
    float Accel[3] = {};     // +0x080 (vel - vel at BeginMovement) / dt, written by sub_604740
    float R[16] = {};        // +0x090 CarState+0x50 rows forward, left, up, (row3)
    float UpR2[3] = {};      // +0x0F0 CarState+0xB0 (row 2 of the copy R2)
    float Speed = 0;                              // +0x430 |v|
    int WheelsOnGround = 0;                       // +0x424
    int Mode = 0;                                 // +0x038 (HighliteMode needs != 0)
    bool OnTopology = false;                      // +0x5CC != 0 (TopologyCoordinate valid; inferido)
    bool HydraulicsBouncing = false, HydraulicsExtended = false;   // Car::AreHydraulicsBouncing / AreHydraulicsExtended
    bool HasDriver = true;                        // +0x030 != 0
    bool DriverFlag2FB = false;                   // RealDriver vtbl+0x54 (sub_410470: driver+0x2FB != 0)
    float Throttle = 0;                           // [+0x030]+0x20C (Car::IsBlowOff)
    // +0x010 != 0. Default false (the code's own fallback, max speed 100): with a RideInfo the game reads
    // RideInfo+0x20 (= RidePhysicsInfo+0x10), which static analysis only finds zeroed, and 0 would turn the noise
    // into NaN (camera.md §5.1, open issue).
    bool HasRideInfo = false;
    float RideInfo20 = 0;                         // [+0x010]+0x20 = RidePhysicsInfo+0x10 (see camera.md §5.1)
    int WheelTerrain[4] = {};                     // +0x394 CarState+0x354 TerrainType per wheel
    const CameraInfo* Cameras = nullptr;          // [+0x00C]+0x720
    // CarState::IsNitrousOn 0x59FA30 (may stop the nitrous as a side effect, like the original).
    std::function<int()> NitrousOn = [] { return 0; };

    static inline const float K_Mph = X87::Bits(0x400F2B02u);        // flt_78426C 2.237
    static inline const float K_ThrottleEps = X87::Bits(0x3A83126Fu); // flt_78435C 0.001
    static inline const float K_BlowOffMph = X87::Bits(0x40A00000u);  // flt_7A5660 5.0

    int IsNitrousOn() const { return NitrousOn(); }

    // Car::IsBlowOff 0x5ECDB0: nitrous on while off the throttle or below 5 mph.
    int IsBlowOff() const;

    // Snapshot of the ported simulation for the camera. velAtBegin is the velocity before the
    // last World step (Car+0x520, copied by Car::BeginMovement); the acceleration is computed like sub_604740:
    // (vel - velAtBegin) * (1/dt), only when dt > 0 (otherwise the previous value is kept).
    void FromSim(VehicleSim& sim, const float* velAtBegin, float dt, const CameraInfo* cameras);
};

} // namespace bb

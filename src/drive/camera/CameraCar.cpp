// Port of NFSU2Unity Core/Camera/CameraCar.cs.

#include "drive/camera/CameraCar.h"

#include "drive/camera/CameraMath.h"
#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/VehicleSim.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/Nitrous.h"
#include "drive/physics/input/RealDriver.h"

#include <cmath>
#include <cstring>

namespace bb {

int CameraCar::IsBlowOff() const {
    F24 mph = F24(Speed) * K_Mph;
    float at = std::fabs(Throttle);
    int c = X87::Fcom(at, K_ThrottleEps) & 0x41;
    if (!X87::ParityEven(c)) return IsNitrousOn();               // |throttle| <= 0.001
    if (!X87::ParityEven(BWareMath2::Fcom(mph, K_BlowOffMph) & 0x05)) return IsNitrousOn();   // mph < 5
    return 0;
}

void CameraCar::FromSim(VehicleSim& sim, const float* velAtBegin, float dt, const CameraInfo* cameras) {
    CarState& s = sim.Car.State;
    std::memcpy(GeomPos, s.GeomPos, 3 * sizeof(float));
    std::memcpy(Pos, s.Pos, 3 * sizeof(float));
    std::memcpy(Vel, s.Vel, 3 * sizeof(float));
    std::memcpy(R, s.R, 16 * sizeof(float));
    UpR2[0] = s.R2[8]; UpR2[1] = s.R2[9]; UpR2[2] = s.R2[10];
    if (!BWareMath2::LeOrNaN(dt, 0.0f)) {
        F24 inv = F24(CameraMath::One) / dt;
        float dx = (F24(Vel[0]) - velAtBegin[0]).Store(), dy = (F24(Vel[1]) - velAtBegin[1]).Store(), dz = (F24(Vel[2]) - velAtBegin[2]).Store();
        Accel[0] = (inv * dx).Store(); Accel[1] = (inv * dy).Store(); Accel[2] = (inv * dz).Store();
    }
    Speed = s.Speed;
    WheelsOnGround = s.WheelsOnGround;
    Mode = sim.Car.Mode;
    OnTopology = true;
    HydraulicsBouncing = sim.Car.AreHydraulicsBouncing();
    // Car::AreHydraulicsExtended 0x5ED300: any CarState+0x3B8 height or driver+0x1FC request > 0
    HydraulicsExtended = false;
    for (int i = 0; i < 4; i++)
        if (s.HydraulicHeight[i] > 0.0f || (sim.Car.Driver != nullptr && sim.Car.Driver->HydraulicHeight[i] > 0.0f)) HydraulicsExtended = true;
    Throttle = sim.Car.Driver != nullptr ? sim.Car.Driver->Throttle : 0.0f;
    for (int i = 0; i < 4; i++) WheelTerrain[i] = s.WheelTerrain[i];
    Cameras = cameras;
    PhysicsEnvironment* env = &sim.Env;
    CarState* sp = &s;
    NitrousOn = [sp, env] { return Nitrous::IsNitrousOn(*sp, env->World.NosNow(), env->Engine); };
}

} // namespace bb

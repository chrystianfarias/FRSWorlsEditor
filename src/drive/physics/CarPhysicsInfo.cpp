#include "drive/physics/CarPhysicsInfo.h"

#include "drive/cars/CarTypeInfoU2.h"
#include "drive/physics/CarPhysicsInfoU2.h"

#include <cstring>
#include <stdexcept>

namespace bb {

namespace {
float F(const uint8_t* b, int o) { float f; std::memcpy(&f, b + o, 4); return f; }
int I(const uint8_t* b, int o) { int v; std::memcpy(&v, b + o, 4); return v; }
} // namespace

std::unique_ptr<CarPhysicsInfo> CarPhysicsInfo::FromBytes(const uint8_t* b, int o) {
    auto c = std::make_unique<CarPhysicsInfo>();
    std::memcpy(c->Raw, b + o, Size);
    const uint8_t* r = c->Raw;
    for (int k = 0; k < 3; k++) c->Cg[k] = F(r, 4 * k);
    for (int i = 0; i < 4; i++) {
        int w = 0x10 + 0x30 * i;
        WheelInfo& wi = c->Wheels[i];
        wi.Radius = F(r, w + 0x10);
        wi.Index = I(r, w + 0x18);
        for (int k = 0; k < 3; k++) wi.Pos[k] = F(r, w + 4 * k);
    }
    for (int a = 0; a < 2; a++) {
        int t = 0xD0 + 0x20 * a;
        TireInfo& ti = c->Tires[a];
        ti.MaxSlipAngle = F(r, t); ti.LoadRef = F(r, t + 4); ti.LateralScale = F(r, t + 8); ti.MuLong = F(r, t + 0xC); ti.MuLat = F(r, t + 0x10);
        int s = 0x170 + 0x20 * a;
        SuspensionInfo& si = c->Suspensions[a];
        si.Progression = F(r, s); si.SpringRate = F(r, s + 4); si.BumpDamping = F(r, s + 8); si.ReboundDamping = F(r, s + 0xC);
        si.AntiRoll = F(r, s + 0x10); si.MaxCompression = F(r, s + 0x14); si.MinCompression = F(r, s + 0x18); si.DisplayMax = F(r, s + 0x1C);
    }
    c->RigidBody = RigidBodyInfo::FromBytes(r, 0x110);
    DriveTrainInfo& d = c->DriveTrain;
    d.NeutralShiftFrac = F(r, 0x1B0); d.Unk04 = F(r, 0x1B4); d.FinalDriveFront = F(r, 0x1B8); d.FinalDriveRear = F(r, 0x1BC);
    d.RearTorqueSplit = F(r, 0x1C0); d.ClutchStiffness = F(r, 0x1C4); d.NumGears = I(r, 0x1C8); d.ClutchRate = F(r, 0x1CC);
    for (int k = 0; k < 8; k++) d.GearRatio[k] = F(r, 0x1D0 + 4 * k);
    EngineInfo& e = c->Engine;
    e.IdleRpm = F(r, 0x1F0); e.RedlineRpm = F(r, 0x1F4); e.MaxRpm = F(r, 0x1F8); e.Inertia = F(r, 0x1FC);
    for (int k = 0; k < 9; k++) { e.Torque[k] = F(r, 0x200 + 4 * k); e.Boost[k] = F(r, 0x224 + 4 * k); }
    for (int k = 0; k < 3; k++) e.EngineBrake[k] = F(r, 0x248 + 4 * k);
    c->Aero.Drag = F(r, 0x260); c->Aero.DownforceFront = F(r, 0x264); c->Aero.DownforceRear = F(r, 0x268); c->Aero.CalibK = F(r, 0x26C);
    c->Control.SteerMaxDeg = F(r, 0x270); c->Control.BrakeTorqueScale = F(r, 0x274); c->Control.EBrakeTorqueScale = F(r, 0x278);
    c->BrakeBiasRear = F(r, 0x27C);
    c->RearGripBase = F(r, 0x280);
    c->RearGripCorner = F(r, 0x284);
    c->YawTorqueClampPilotOff = F(r, 0x288);
    c->YawTorqueClampPilotOn = F(r, 0x28C);
    c->TopSpeed = F(r, 0x290);
    c->NosTorqueMul = F(r, 0x2AC);
    return c;
}

std::unique_ptr<CarPhysicsInfo> CarPhysicsInfo::FromCarType(const CarTypeInfoU2& carType) {
    if (carType.Raw.size() < static_cast<size_t>(CarPhysicsInfoU2::CarTypeInfoOffset + Size))
        throw std::runtime_error("CarTypeInfo record too short for its CarPhysicsInfo");
    return FromBytes(carType.Raw.data(), CarPhysicsInfoU2::CarTypeInfoOffset);
}

} // namespace bb

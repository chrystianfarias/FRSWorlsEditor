// Per-Move tuning of PhysicsMover (before the sub-steps), the rest/roof-reset damping of BottomOut and the
// CarState refresh after the sub-steps. x87 order as in the original; comments give the deciding test.
// Port of NFSU2Unity Core/Physics/PhysicsMoverTuning.cs.

#include "drive/physics/PhysicsMover.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/engine/EngineTables.h"
#include "drive/physics/engine/Nitrous.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cmath>
#include <cstring>

namespace bb {

namespace {
const float InputConstants_DeadZone = X87::Bits(0x3E19999Au);   // flt_7844F0 0.15
const float StallRpm = X87::Bits(0x453B8000u);                  // flt_78759C 3000.0

F24 Len3(float x, float y, float z) { return F24::Sqrt(F24(x) * x + F24(y) * y + F24(z) * z); }
} // namespace

bool PhysicsMover::Gt(F24 a, F24 b) { return (BWareMath2::Fcom(a, b) & 0x41) == 0; }                 // a > b (not NaN)
bool PhysicsMover::Lt(F24 a, F24 b) { return !X87::ParityEven(BWareMath2::Fcom(a, b) & 0x05); }       // a < b (not NaN)

// ------------------------------------------------------------------ Calibrate

void PhysicsMover::Calibrate() {
    FixCarCenterOfGravity();
    Rb->CalculateInertiaTensor();
    F24 c = Rb->CgOffset[0];
    if (K().UseNewSuspensionCalibration == 0) c = WheelK::Zero;
    F24 a = F24(Cpi->Wheels[0].Pos[0]) - c;
    F24 b = F24(Cpi->Wheels[3].Pos[0]) - c;
    b = a - b;                                                 // fsubr st, st(1)
    float rearShare = (a / b * K().Half).Store();              // fdivr st, st(1); fmul 0.5; fstp var_8
    F24 frontShare = F24(K().Half) - rearShare;
    F24 mg = F24(Rb->Mass) * K().Gravity;
    F24 v = State->ForwardSpeed;                               // Car+0x42C
    if ((BWareMath2::Fcom(F24(WheelK::Zero), v) & 0x41) == 0) v = WheelK::Zero;   // 0 > v
    F24 df = F24(Aero->Info->DownforceRear) + Aero->Info->DownforceFront;
    df = df * Aero->Enable * v * Cpi->Aero.CalibK;
    float load = (df + mg).Store();                            // var_C
    float front = (F24(load) * frontShare).Store();
    Susp[0]->Calibrate(front, State->HydraulicHeight[0]);
    Susp[1]->Calibrate(front, State->HydraulicHeight[1]);
    float rear = (F24(load) * rearShare).Store();
    Susp[2]->Calibrate(rear, State->HydraulicHeight[2]);
    Susp[3]->Calibrate(rear, State->HydraulicHeight[3]);
    for (int i = 0; i < 4; i++) Wheels[i]->Inertia = Env->Tweaks.WheelMomentOfInertia;
}

bool PhysicsMover::FixCarCenterOfGravity() {
    float cg[3];
    if (Car->Mode == 1) {
        CarCollisionBody* cb = Car->Collision;
        F24 t = (F24(cb->HydraulicHigh()) - cb->HydraulicLow()) * K().HydraulicCgMix + cb->HydraulicLow();
        F24 q = F24::MulDouble(K().Quarter, static_cast<double>(State->WheelsOnGround));   // fild Car+0x424 ; fmul 0.25
        F24 z = t - q * (t - Car->Cpi->Cg[2]);
        if (!X87::ParityEven(BWareMath2::Fcom(z, F24(Rb->CgOffset[2])) & 0x44)) return false;   // equal: nothing to do
        CgShifted = true;
        cg[0] = 0.0f; cg[1] = 0.0f; cg[2] = z.Store();
    } else {
        if (!CgShifted) return false;
        cg[0] = Car->Cpi->Cg[0]; cg[1] = Car->Cpi->Cg[1]; cg[2] = Car->Cpi->Cg[2];
        CgShifted = false;
    }
    Rb->SetCGOffset(cg);
    return true;
}

// ------------------------------------------------------------------ TuneBoostAndDampers

void PhysicsMover::TuneBoostAndDampers() {
    F24 terr = WheelK::One;
    int n = State->WheelsOnGround;
    if (n > 0) {
        F24 s = WheelK::Zero;
        for (int i = 0; i < 4; i++) {
            F24 t = State->WheelOnGround[i] != 0 ? F24(K().TerrainDrag[State->WheelTerrain[i]]) : F24(WheelK::Zero);
            s = i == 0 ? t : s + t;
        }
        terr = s / F24(static_cast<float>(n));                 // fidiv n
    }
    int ok;
    switch (Car->Mode) {
        case 1: {
            Rb->RestDampingOn = 0;
            Rb->AngDampingOn = 0;
            Rb->Gravity = K().ModeOneGravity;
            Aero->Enable = 0.0f;
            Aero->TerrainDrag = WheelK::One;
            float thr = Driver->Throttle;
            Driver->Throttle = X87::ParityEven(X87::Fcom(K().ModeOneMaxThrottle, thr) & 0x05) ? thr : K().ModeOneMaxThrottle;
            ok = 0;
            break;
        }
        case 0:
        case 2:
            Rb->RestDampingOn = 1;
            Rb->AngDampingOn = 1;
            Rb->Gravity = X87::Bits(0x411CCCCDu);                // immediate 9.8000002
            Aero->Enable = IsOnDyno ? WheelK::Zero : WheelK::One;
            Aero->TerrainDrag = terr.Store();
            ok = 1;
            break;
        default:
            ok = 0;
            break;
    }
    if (State->WheelsOnGround == 0) ok = 0;                     // Car+0x424
    if (Rb->UseYawScaledInertia != ok) {
        if (ok == 0) Rb->L[2] = (F24(Rb->L[2]) / Rb->Info->YawInertiaScale).Store();
        Rb->UseYawScaledInertia = ok;
    }
    if (World().RaceState == 4) {
        float h;
        if (World().Flags.Drag()) {
            h = Engine->Heat;
            if ((X87::Fcom(WheelK::Zero, h) & 0x41) == 0) h = WheelK::Zero;   // 0 > heat
        } else h = WheelK::MinusOne;
        Engine->Heat = h;
        if ((X87::Fcom(Driver->Throttle, K().RestThrottle) & 0x41) == 0) Rb->RestDampingOn = 0;
        else if (State->WheelsOnGround == 4 && (X87::Fcom(std::fabs(State->R[2]), K().RestPitch) & 0x41) == 0) Rb->RestDampingOn = 0;
    }
    float cu = Car->CatchupOverride;
    if (!X87::ParityEven(X87::Fcom(cu, WheelK::Zero) & 0x44) || Car->FlatTyreMask != 0) cu = Car->CatchupScale;   // == 0 -> +0x468
    ApplyCatchupScale(cu);
    if (IsOnDyno) Rb->YawTorqueClamp = K().DynoYawClamp;
    else if (State->PilotFlag == 0) Rb->YawTorqueClamp = Cpi->YawTorqueClampPilotOff;
    else {
        float v = Cpi->YawTorqueClampPilotOn;
        if (!X87::ParityEven(X87::Fcom(v, WheelK::Zero) & 0x44))   // v == 0
        {
            float sl = X87::ParityEven(X87::Fcom(State->ForwardSpeed, K().OneMs) & 0x05) ? State->GetSlipAngle(Env->Math) : WheelK::Zero;
            if (Lt(F24(sl) * Rb->L[2], WheelK::Zero) && (X87::Fcom(std::fabs(sl), K().PointOne) & 0x41) == 0)
                v = K().CounterSteerYawClamp;
        }
        Rb->YawTorqueClamp = v;
    }
    Rb->TopSpeed = IsOnDyno ? 0.0f : Cpi->TopSpeed;
    if ((X87::Fcom(Cpi->RearTorqueSplit(), K().RwdSplit) & 0x41) == 0) {
        F24 mph = F24(State->Speed) * K().MpsToMph;               // Car+0x430
        if (Lt(mph, K().RearSplitMph) && Car->Collision->Field120() > 0) DriveTrain->RearSplit = K().RearSplitLow;
        else DriveTrain->RearSplit = Cpi->RearTorqueSplit();
    }
}

void PhysicsMover::ApplyCatchupScale(float s) {
    F24 sp = K().CatchupEnabled != 0 ? F24(s) : F24(WheelK::One);
    F24 e = sp;
    if (Car->FlatTyreMask != 0 && Car->Collision->Contacts != 0) e = F24(K().ModeOneMaxThrottle) * sp;
    Engine->TorqueScale = e.Store();
    float spf = sp.Store();
    Rb->TopSpeedScale = spf;
    Aero->SpeedScale = spf;
    Aero->DragScale = (F24(WheelK::One) - Car->Slipstream).Store();
    for (int i = 0; i < 4; i++) Wheels[i]->GripScale2 = spf;
}

// ------------------------------------------------------------------ DelegateDriverInput

void PhysicsMover::DelegateDriverInput(const std::function<int(int, float)>& selectGear) {
    CarDriver* d = Driver;
    if (State->Gear != 1 && !World().Flags.Burnout())         // Car+0x434
    {
        if ((X87::Fcom(d->Throttle, d->Brake) & 0x41) == 0 && !X87::ParityEven(X87::Fcom(d->EBrake, d->Throttle) & 0x05)) {
            if (!X87::ParityEven(X87::Fcom(d->Brake, K().Half) & 0x05)) d->Brake = 0.0f;
            if (!X87::ParityEven(X87::Fcom(d->EBrake, K().Half) & 0x05)) d->EBrake = 0.0f;
        } else if (!X87::ParityEven(X87::Fcom(d->Throttle, K().Half) & 0x05)) d->Throttle = 0.0f;
    }
    float t = d->Throttle;
    if (!X87::ParityEven(X87::Fcom(t, InputConstants_DeadZone) & 0x41)) t = 0.0f;   // <= 0.15
    else if ((X87::Fcom(t, WheelK::Zero) & 0x41) == 0 && DriveTrain->Gear > 1
             && Lt(F24(Engine->Omega) * K().RadToRpm, StallRpm)
             && (X87::Fcom(K().Half, t) & 0x41) == 0)
        t = K().Half;
    Steering->SetSteering(d->SteerAngle);
    Engine->Throttle = t;
    int g = d->CurrentGear;
    if (g > 1 && g <= DriveTrain->Info->NumGears + 1) {
        if (DriveTrain->LastRequest == 1) DriveTrain->AutoTimer = 0.0f;
        DriveTrain->LastRequest = g;
    }
    Engine->NosLevel = Nitrous::IsNitrousOn(*State, World().NosNow(), Env->Engine);
    float rpm = TestRpmIsOff() ? State->Rpm : World().TestRPM;
    int req = d->GearCommand;
    d->GearCommand = selectGear ? selectGear(req, rpm)
        : DriveTrain->UpdateGearSelection(req, rpm, World().WorldTimer, World().WorldTimeElapsed, World().Flags.Drag());
    d->CurrentGear = DriveTrain->LastRequest;
    Aero->Throttle = t;
    Aero->GroundFrac = F24::MulDouble(K().Quarter, static_cast<double>(State->WheelsOnGround)).Store();
}

// ------------------------------------------------------------------ TuneWheelParams

void PhysicsMover::TuneWheelParams(float dt) {
    CarDriver* d = Driver;
    float g = GripScale;                                       // var_24
    float mph = (F24(State->ForwardSpeed) * K().MpsToMph).Store();  // var_20
    if (State->PilotFlag == 0 && (World().Flags.Drag() || !World().Flags.DriftPhysics()))
        g = (F24(K().PilotOffGrip) * g).Store();
    F24 hb = EBrakeRearGrip;
    float nhb;
    if ((X87::Fcom(d->EBrake, K().Half) & X87::C0) == 0)       // ebrake >= 0.5
    {
        nhb = (hb - F24(K().EBrakeGripFall) * dt).Store();
        if (!X87::ParityEven(X87::Fcom(nhb, K().EBrakeGripMin) & 0x05)) nhb = K().EBrakeGripMin;
    } else {
        nhb = (F24(K().EBrakeGripRise) * dt + hb).Store();
        if ((X87::Fcom(nhb, WheelK::One) & 0x41) == 0) nhb = WheelK::One;
    }
    EBrakeRearGrip = nhb;
    F24 rain = World().RainIntensity;                          // Car::GetRainIntensity (mocked)
    g = ((F24(WheelK::One) - rain * (F24(WheelK::One) - K().RainGrip)) * g).Store();
    for (int i = 0; i < 4; i++) {
        Wheel* w = Wheels[i];
        if ((X87::Fcom(d->Throttle, K().LaunchThrottle) & 0x41) == 0 && (X87::Fcom(d->Brake, K().Half) & 0x41) == 0
            && !X87::ParityEven(X87::Fcom(mph, K().LaunchMph) & 0x05) && i >= 2)
            w->Brake = (F24(mph) * K().LaunchBrakeMul).Store();
        else {
            F24 b = Cpi->BrakeBiasRear;
            if (i < 2) b = F24(WheelK::One) - b;
            w->Brake = ((b + b) * d->Brake).Store();
        }
        F24 e = F24(K().HandbrakeMask[i]) * d->EBrake;
        float e14 = e.Store();                                 // fst var_14
        bool above = (BWareMath2::Fcom(e, F24(K().EBrakeBoostMin)) & 0x41) == 0;   // e > 0.2
        if (State->PilotFlag == 0) {
            if (above) e = e + K().Half;
        } else if (above) {
            F24 sl;
            if (X87::ParityEven(X87::Fcom(State->ForwardSpeed, K().OneMs) & 0x05)) {
                sl = State->GetSlipAngle(Env->Math);           // fstp var_10 ; fld var_14 ; fld var_10
                e = e14;
            } else sl = WheelK::Zero;
            if ((BWareMath2::Fcom(F24::Abs(sl), F24(K().DriftSlip)) & 0x41) == 0
                && !X87::ParityEven(X87::Fcom(mph, K().DriftMph) & 0x05))
                e = e + K().Half;
        }
        w->EBrake = e.Store();
        F24 gr = g;
        if ((World().Flags.Drag() || !World().Flags.DriftPhysics()) && !World().Flags.Burnout() && i >= 2) {
            float pitch = Rb->R[2];                            // var_C
            float sl = X87::ParityEven(X87::Fcom(State->ForwardSpeed, K().OneMs) & 0x05) ? State->GetSlipAngle(Env->Math) : 0.0f;
            float hit = RecentHitGrip().Store();
            F24 f = RearGripBoost(sl, w->EBrake, State->Speed, Cpi->RearGripBase, Cpi->RearGripCorner, State->PilotFlag, pitch, hit);
            gr = ((f - WheelK::One) * nhb + WheelK::One) * g;
        }
        w->GripScale = gr.Store();
        w->FlatDrag = (static_cast<int8_t>(Car->FlatTyreMask) & (1 << i)) != 0 ? WheelK::One : 0.0f;
        int nos = Nitrous::IsNitrousOn(*State, World().NosNow(), Env->Engine);
        if (nos > 0) w->GripScale = (F24(K().NosFriction[nos]) * w->GripScale).Store();
        if (IsOnDyno && NotDriven(w) && !X87::ParityEven(X87::Fcom(d->Throttle, WheelK::One) & 0x44))
            w->GripScale = (F24(w->GripScale) * WheelK::Zero).Store();
    }
}

bool PhysicsMover::NotDriven(const Wheel* w) {
    int idx = w->Info->Index;
    float split = w->Car->Cpi->RearTorqueSplit();
    if (!X87::ParityEven(X87::Fcom(split, WheelK::FwdSplit) & 0x05)) return idx != 1 && idx != 0;   // split < 0.2
    if ((X87::Fcom(split, WheelK::RwdSplit) & 0x41) != 0) return false;                              // <= 0.8
    return idx != 2 && idx != 3;
}

F24 PhysicsMover::RearGripBoost(float slip, float ebrake, float speed, float baseGrip, float corner, int pilotFlag, float& pitch, float hitGrip) const {
    F24 p = F24::Abs(pitch);
    pitch = p.Store();
    F24 a = p + WheelK::One;
    F24 b = F24::Abs(slip);
    F24 c = pilotFlag == 0 ? F24(baseGrip) + corner : F24(corner) * hitGrip + baseGrip;
    if ((X87::Fcom(ebrake, K().RgbEBrake) & 0x41) == 0) {
        F24 k = F24(K().RgbMaxSlipDeg) * K().DegToRad;
        if (Lt(b, k)) return a;
    }
    F24 dd = (F24(speed) - K().RgbSpeedMin) / (F24(K().RgbSpeedMax) - K().RgbSpeedMin);
    dd = dd * (F24(K().RgbValueMax) - K().RgbValueMin) + K().RgbValueMin;
    F24 x = b * dd * c;
    if (Gt(x, K().RgbMax)) x = K().RgbMax;
    return a + x;
}

F24 PhysicsMover::RecentHitGrip() const {
    int t = Car->Collision->LastHitTime;
    if (t == 0 || t == 0x7FFFFFFF) return WheelK::Zero;
    F24 x = F24::MulDouble(EngineK::TickToSec, static_cast<double>(static_cast<int>(static_cast<uint32_t>(World().WorldTimer) - static_cast<uint32_t>(t))));
    float xs = x.Store();
    if (!Lt(x, K().HitGripMaxSec)) return WheelK::Zero;
    return K().RecentHitGrip.GetValue(xs);
}

// ------------------------------------------------------------------ damping / roof reset (BottomOut)

int PhysicsMover::EvaluateDampingSuitability(int prev) const {
    if (Car->AreHydraulicsBouncing()) return 0;
    for (int i = 0; i < 4; i++) {
        int s = Car->GetHydraulicState(i);
        if (s == 2 || s == 3) return 0;
    }
    int wog = State->WheelsOnGround;                             // Car+0x424
    if ((X87::Fcom(Driver->Throttle, K().RestThrottle) & 0x41) == 0 && State->Gear != 1 && wog > 0 && State->WheelsAttached > 2)
        return 0;
    F24 v = Len3(Rb->Vel[0], Rb->Vel[1], Rb->Vel[2]);
    float vf = v.Store();
    float wf = Len3(Rb->AngVel[0], Rb->AngVel[1], Rb->AngVel[2]).Store();
    if (Gt(v, K().DampSpeed)) return 0;
    if ((X87::Fcom(wf, K().DampSpin) & 0x41) == 0) return 0;
    if (((X87::Fcom(std::fabs(Rb->R[2]), K().DampTilt) & 0x41) == 0 || (X87::Fcom(std::fabs(Rb->R[6]), K().DampTilt) & 0x41) == 0)
        && wog > 0) return 0;
    if (Car->IsBlinking(World().WorldTimer)) return 0;
    if (!Car->IsSupported() && prev != 2) return 0;
    if ((X87::Fcom(vf, K().RestSpeed) & 0x41) == 0) return 1;
    if ((X87::Fcom(wf, K().RestSpin) & 0x41) == 0) return 1;
    return 2;
}

int PhysicsMover::MovementDousingAndRoofReset() {
    int d = EvaluateDampingSuitability(DampState);
    DampState = d;
    switch (d) {
        case 2: {
            Rb->P[0] = Rb->P[1] = Rb->P[2] = 0.0f;
            Rb->L[0] = Rb->L[1] = Rb->L[2] = 0.0f;
            if (State->WheelsOnGround > 0)
                for (int i = 0; i < 4; i++) Wheels[i]->Omega = 0.0f;
            bool timer = Lt(Rb->R[10], K().PointOne) || (Car->SlotType == 1 && Car->NoDriveWheelOnGround());
            if (!timer) RoofResetTimer = 0;
            else {
                int t = RoofResetTimer;
                if (t == 0 || t == 0x7FFFFFFF) RoofResetTimer = World().WorldTimer;
                else {
                    int el = static_cast<int>(static_cast<uint32_t>(World().WorldTimer) - static_cast<uint32_t>(t));
                    bool guard = Car->HasParticipant && Car->ParticipantField10 != 0 && World().Field89E7B0 != 0;   // sub_5EE250
                    if ((BWareMath2::Fcom(F24::MulDouble(EngineK::TickToSec, static_cast<double>(el)), F24(K().RoofResetSec)) & 0x41) == 0 && !guard)
                        Car->ResetRequest = 1;                   // sub_5EBF60
                }
            }
            Car->Collision->Damping = 0;
            Rb->Gravity = 0.0f;
            return 1;
        }
        case 1: {
            F24 k = K().DousingDamping;
            float pz = Rb->P[2];
            Rb->P[0] = (k * Rb->P[0]).Store(); Rb->P[1] = (F24(Rb->P[1]) * k).Store(); Rb->P[2] = (F24(pz) * k).Store();
            float lz = Rb->L[2];
            Rb->L[0] = (k * Rb->L[0]).Store(); Rb->L[1] = (F24(Rb->L[1]) * k).Store(); Rb->L[2] = (F24(lz) * k).Store();
            for (int i = 0; i < 4; i++) Wheels[i]->Omega = (k * Wheels[i]->Omega).Store();
            if (Gt(Len3(Rb->Vel[0], Rb->Vel[1], Rb->Vel[2]), K().RwdSplit)) RoofResetTimer = 0;
            Car->Collision->Damping = 1;
            Rb->Gravity = Car->Mode == 1 ? K().ModeOneGravity : K().Gravity;
            return 1;
        }
        case 0:
            if (Gt(Len3(Rb->Vel[0], Rb->Vel[1], Rb->Vel[2]), K().RwdSplit)) RoofResetTimer = 0;
            Car->Collision->Damping = 1;
            Rb->Gravity = Car->Mode == 1 ? K().ModeOneGravity : K().Gravity;
            return 0;
        default:
            return 0;
    }
}

// ------------------------------------------------------------------ UpdateCarState (0x5A5540)

void PhysicsMover::UpdateCarState(float dt) {
    CarState* cs = State;
    cs->Pos[0] = Rb->Pos[0]; cs->Pos[1] = Rb->Pos[1]; cs->Pos[2] = Rb->Pos[2];
    std::memcpy(cs->R, Rb->R, sizeof cs->R);
    cs->CalculateGeometryPosition(Rb->CgOffset);
    std::memcpy(cs->R2, cs->R, sizeof cs->R2);
    cs->Vel[0] = Rb->Vel[0]; cs->Vel[1] = Rb->Vel[1]; cs->Vel[2] = Rb->Vel[2];
    cs->Field480 = 0;
    F24 vx = Rb->Vel[0], vy = Rb->Vel[1], vz = Rb->Vel[2];
    cs->Speed = F24::Sqrt(vx * vx + vz * vz + vy * vy).Store();   // (x*x + z*z) + y*y
    cs->AngVel[0] = Rb->AngVel[0]; cs->AngVel[1] = Rb->AngVel[1]; cs->AngVel[2] = Rb->AngVel[2];
    cs->Heading = Env->Math.FastATan(cs->R[0], cs->R[1]);
    cs->ForwardSpeed = (F24(cs->Vel[2]) * cs->R[2] + F24(cs->Vel[1]) * cs->R[1] + F24(cs->Vel[0]) * cs->R[0]).Store();

    // visual steering: rate limited to SteerVisRate deg/s
    float a4 = (F24(static_cast<float>(cs->SteerVisual)) * K().SteerToDeg).Store();   // var_4
    F24 s = F24(static_cast<float>(Driver->SteerAngle)) * K().SteerToDeg;
    F24 rate = F24(K().SteerVisRate) * dt;
    F24 a = a4;
    if ((BWareMath2::Fcom(a, F24(K().Deg180)) & X87::C0) == 0) a = a - K().Deg360;
    if ((BWareMath2::Fcom(s, F24(K().Deg180)) & X87::C0) == 0) s = s - K().Deg360;
    float hi = (rate + a).Store();                             // var_8
    float lo = (a - rate).Store();                             // var_4
    if ((BWareMath2::Fcom(s, F24(lo)) & 0x41) != 0) s = lo;    // s <= lo (or NaN)
    if (!X87::ParityEven(BWareMath2::Fcom(F24(hi), s) & 0x05)) s = hi; // hi < s
    int x = X87::FtoL2Lo((s * K().F65536).V);
    cs->SteerVisual = static_cast<uint16_t>(x / 360);

    for (int i = 0; i < 4; i++) {
        Wheel* w = Wheels[i];
        Suspension* sp = Susp[i];
        cs->WheelOmega[i] = w->Omega;
        cs->WheelAngle[i] = w->Angle;
        F24 adj = X87::ParityEven(X87::Fcom(sp->Compression, sp->Info->DisplayMax) & 0x05)
            ? F24(sp->Info->DisplayMax) - sp->Compression : F24(WheelK::Zero);
        cs->SuspDisplay[i] = (F24(sp->Compression) + adj - cs->HydraulicHeight[i]).Store();
        uint8_t old = cs->WheelOnGround[i];
        uint8_t now = w->OnGround;
        cs->WheelOnGround[i] = now;
        if (old == 0) { if (now != 0) cs->WheelsOnGround++; }
        else if (now == 0) cs->WheelsOnGround--;
        cs->WheelTerrain[i] = w->Terrain;
        if (w->Terrain > cs->MaxWheelTerrain[i]) cs->MaxWheelTerrain[i] = w->Terrain;
        if (IsOnDyno) {
            cs->WheelGripUsage[i] = 0.0f;
            cs->WheelSlip[2 * i + 1] = 0.0f;                    // flt_86B394 / dword_86B390 (zero vector)
            cs->WheelSlip[2 * i] = 0.0f;
        } else {
            cs->WheelGripUsage[i] = w->GripUsage;
            cs->WheelSlip[2 * i + 1] = w->LastLatVel;
            cs->WheelSlip[2 * i] = w->LastSlip;
        }
        cs->WheelLoad[i] = w->Load;
        cs->WheelRoadNoise[i] = w->OnGround != 0 ? w->RoadNoise : 0.0f;
    }
    cs->Throttle = Engine->RevLimiter ? 0.0f : (F24(Engine->ThrottleGate) * Engine->Throttle).Store();
    cs->Gear = DriveTrain->Gear;
    cs->ClutchEngaged = DriveTrain->ClutchState == 3 ? 1 : 0;
    cs->ClutchTorque = DriveTrain->ClutchTorque;
    bool blow = DriveTrain->ShiftPending != 0 && DriveTrain->TargetGear > 2;
    F24 rpm = F24(Engine->Omega) * K().RadToRpm;
    int idx = X87::FtoL2Lo((F24(Car->RideRpmDecelIndex) + K().Half).V) + (blow ? 4 : 0);
    float dec = (-F24(K().SmoothRPMDecel[idx]) / Engine->Info->Inertia).Store();   // var_8
    if ((X87::Fcom(dt, WheelK::Zero) & 0x41) == 0) {
        F24 prev = TestRpmIsOff() ? F24(cs->Rpm) : F24(World().TestRPM);
        F24 r = (rpm - prev) / dt;
        if (Lt(r, dec)) {
            F24 prev2 = TestRpmIsOff() ? F24(cs->Rpm) : F24(World().TestRPM);
            F24 cand = F24(dec) * dt + prev2;
            float candF = cand.Store();
            if (!Lt(cand, rpm)) rpm = candF;
        }
    }
    F24 prev3 = TestRpmIsOff() ? F24(cs->Rpm) : F24(World().TestRPM);
    cs->Rpm = (prev3 * K().RpmKeep + rpm * K().RpmNew).Store();

    F24 m03 = F24::Abs(Wheels[0]->SlipVel);
    float a1 = std::fabs(Wheels[1]->SlipVel), a2 = std::fabs(Wheels[2]->SlipVel), a3 = std::fabs(Wheels[3]->SlipVel);
    if ((BWareMath2::Fcom(m03, F24(a3)) & 0x41) != 0) m03 = a3;  // |w0| <= |w3| (or NaN)
    F24 m12 = (X87::Fcom(a1, a2) & 0x41) != 0 ? F24(a2) : F24(a1);
    F24 inv6 = F24(WheelK::One) / K().SpinDiv;
    float s03 = (inv6 * m03).Store();
    F24 s12 = inv6 * m12;
    if ((X87::Fcom(s03, WheelK::One) & 0x41) == 0) s03 = WheelK::One;
    if (Gt(s12, WheelK::One)) s12 = WheelK::One;
    cs->Spin12 = s12.Store();
    cs->Spin03 = s03;
    cs->AvgGripUsage = ((F24(Wheels[3]->GripUsage) + Wheels[0]->GripUsage + Wheels[1]->GripUsage + Wheels[2]->GripUsage) * K().Quarter).Store();
}

} // namespace bb

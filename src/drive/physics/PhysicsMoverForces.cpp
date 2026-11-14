// PhysicsMover::ResolveForces 0x5B0520 and everything it calls (run at the end of every sub-step, inside
// ComputeAuxiliaries): aero, normal loads, the tyre/clutch iteration, tyre forces on the body and the assists.
// Port of NFSU2Unity Core/Physics/PhysicsMoverForces.cs.

#include "drive/physics/PhysicsMover.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/input/RealDriver.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Suspension.h"
#include "drive/physics/wheel/Wheel.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cmath>
#include <stdexcept>

namespace bb {

namespace {
// sub_4011F0: out = v * s (x, y, z stored in order).
void Scale(float* o, const float* v, float s) {
    float vy = v[1], vz = v[2];
    o[0] = (F24(s) * v[0]).Store();
    o[1] = (F24(vy) * s).Store();
    o[2] = (F24(vz) * s).Store();
}
} // namespace

void PhysicsMover::ResolveForces() {
    if (IsOnDyno) {
        if (!Car->HasParticipant) throw std::runtime_error("dyno without [Car+0x1C] (null dereference in the original)");
        if (Car->ParticipantMode1E0 == 2) Engine->ThrottleGate = K().DynoGate;
    }
    // RigidBody::ResolveForces (vtbl+0x14) is an empty function (0x4022C0).
    float downforce[1] = {};
    Aero->ResolveForces(downforce, World().Flags, Env->Rb);
    for (int i = 0; i < 4; i++) {
        Suspension* s = Susp[i];
        s->ExchangeRet = s->Wheel->ExchangeZForces(s->Touching, s->SpringForce);
    }
    IterateForces();
    for (int i = 0; i < 4; i++) {
        Suspension* s = Susp[i];
        if (s->Touching == 0) continue;
        Wheel* w = s->Wheel;
        float r[3];
        r[1] = (F24(w->ContactPt[1]) - Rb->Pos[1]).Store();
        r[2] = (F24(w->ContactPt[2]) - Rb->Pos[2]).Store();
        r[0] = (F24(w->ContactPt[0]) - Rb->Pos[0]).Store();
        if (World().NoWheels == 0) Rb->ApplyForce(w->ForceWorld, r);
    }
    JumpStabilizer();
    if (Car != nullptr) MaybeForceYaw();
    if (World().Flags.DriftAero()) DriftStabilizer();
    ResolveHydraulicForces();
    if (IsOnDyno) ResolveDynoForces();
}

void PhysicsMover::IterateForces() {
    DriveTrain->GuessClutchTorque();
    int contacts = Car->Collision->Contacts;
    for (int i = 0; i < 4; i++) Wheels[i]->GuessGroundForce(contacts);
    float rt[16];
    BWareMath2::TransposeMatrix(rt, Rb->R);
    float fb[3];
    BWareMath2::MulVector3X87(fb, rt, Rb->Force);                // bMulMatrix(bVector3) = eMulVector
    for (int iter = 0; iter < 5; iter++) {
        Wheel* w0 = Wheels[0];
        float fx0 = (F24(w0->Local[4]) * w0->Fy + F24(w0->Local[0]) * w0->Fx).Store();   // var_60
        float fy0 = (F24(w0->Local[5]) * w0->Fy + F24(w0->Local[1]) * w0->Fx).Store();   // var_5C
        float sx = (F24(fb[0]) + fx0).Store();                 // var_70
        float sy = (F24(fb[1]) + fy0).Store();                 // var_6C
        F24 tz = F24(fy0) * Susp[0]->Attach[0] - F24(fx0) * Susp[0]->Attach[1];
        for (int k = 1; k < 4; k++) {
            Wheel* w = Wheels[k];
            F24 fx = F24(w->Local[4]) * w->Fy + F24(w->Local[0]) * w->Fx;
            F24 fy = F24(w->Local[5]) * w->Fy + F24(w->Local[1]) * w->Fx;
            sx = (F24(sx) + fx).Store();
            sy = (F24(sy) + fy).Store();
            tz = tz + fy * Susp[k]->Attach[0];
            tz = tz - fx * Susp[k]->Attach[1];
        }
        F24 im = F24(WheelK::One) / Rb->Mass;
        float ax = (F24(sx) * im).Store();                    // var_78
        float ay = (im * sy).Store();                          // var_74
        const float* ii = Rb->UseYawScaledInertia != 0 ? Rb->InvInertiaYaw : Rb->InvInertia;
        float alpha = (tz / (F24(WheelK::One) / ii[10])).Store();   // var_7C
        for (int k = 0; k < 4; k++) {
            Wheel* w = Wheels[k];
            if (w->Sliding != 0 || w->BrakeLocked != 0) continue;
            Suspension* s = Susp[k];
            float r = w->Info->Radius;
            F24 a = F24(ax) - F24(alpha) * s->Attach[1];
            F24 b = F24(alpha) * s->Attach[0] + ay;
            F24 c = (F24(w->Torque) - F24(r) * w->Fx) / w->Inertia * r;
            F24 x = b * w->Local[4] + a * w->Local[0];
            w->Fx = ((c - x) / r * w->Inertia + w->Fx).Store();
        }
        DriveTrain->ModifyClutchTorque();
    }
    for (int i = 0; i < 4; i++) Wheels[i]->FinalizeGroundForce();
    DriveTrain->FinalizeClutchTorque();
}

void PhysicsMover::JumpStabilizer() {
    if (Car->Mode == 1 || Car->Mode == 2) return;
    F24 wx = Rb->AngVel[0], wy = Rb->AngVel[1], wz = Rb->AngVel[2];
    float spin = F24::Sqrt(wy * wy + wz * wz + wx * wx).Store();   // (y*y + z*z) + x*x
    if (State->WheelsOnGround >= 2) return;                      // Car+0x424
    if (X87::ParityEven(X87::Fcom(spin, K().JumpMinSpin) & 0x05)) return;   // spin >= 0.5 (or NaN)
    float mass = Rb->Mass;
    float k = (-(K().JumpStabilization.GetValue(spin) * mass)).Store();
    float t[3];
    Scale(t, Rb->AngVel, k);
    Rb->AddTorque(t);
}

void PhysicsMover::MaybeForceYaw() {
    bb::Car* car = Car;
    if (car->Mover == nullptr || car->Driver == nullptr || !car->HasParticipant) return;
    float speed = State->Speed;                                 // Car+0x430
    if (X87::ParityEven(X87::Fcom(BWareMath2::Clamp(speed, K().ForceYawSpeedMin, K().ForceYawSpeedMax), speed) & 0x44)) return;
    if (!X87::ParityEven(X87::Fcom(State->R[10], K().PointOne) & 0x05)) return;   // up.z < 0.1
    if (State->WheelsOnGround < 3) return;
    uint16_t yaw = car->Driver->GetForcedYaw();
    if (yaw == 0) return;
    float spin = F24::Sqrt(F24(Rb->AngVel[0]) * Rb->AngVel[0] + F24(Rb->AngVel[1]) * Rb->AngVel[1]
                           + F24(Rb->AngVel[2]) * Rb->AngVel[2]).Store();   // sub_440F70
    float mass = Rb->Mass;
    float k = (-(K().ForceYaw.GetValue(spin) * mass)).Store();
    float t[3];
    Scale(t, Rb->AngVel, k);
    Rb->AddTorque(t);
    float sn, cn;
    BWareMath2::SinCos(yaw, sn, cn);
    const float* R = State->R;
    F24 dx = F24(sn) * R[4] + F24(cn) * R[0] + F24(R[8]) * WheelK::Zero + R[12];
    F24 dy = F24(sn) * R[5] + F24(cn) * R[1] + F24(R[9]) * WheelK::Zero + R[13];
    float dz = (F24(sn) * R[6] + F24(cn) * R[2] + F24(R[10]) * WheelK::Zero + R[14]).Store();
    const float dir[3] = {dx.Store(), dy.Store(), dz};
    float vh[3] = {State->Vel[0], State->Vel[1], 0.0f};
    BWareMath2::NormalizeX87(vh, vh);
    float axis[3];
    BWareMath::CrossX87(axis, vh, dir);
    F24 m = F24(vh[1]) * dir[1] + F24(vh[0]) * dir[0] + F24(vh[2]) * dir[2];
    float len = ((m - WheelK::One) * Rb->Mass * K().ForceYawGain).Store();
    BWareMath2::NormalizeLenX87(axis, axis, len);
    Rb->AddTorque(axis);
}

void PhysicsMover::DriftStabilizer() {
    CarState* cs = State;
    uint16_t a = Env->Math.FastATan(cs->Vel[0], cs->Vel[1]);
    int diff = static_cast<uint16_t>(cs->Heading - a);
    F24 x = F24(static_cast<float>(diff)) * K().AngToRad;
    float sl = x.Store();                                      // fst var_20
    if ((BWareMath2::Fcom(x, K().Pi) & 0x41) == 0) sl = (F24(sl) - (F24(K().Pi) + K().Pi)).Store();
    float wz = cs->AngVel[2];                                  // var_1C
    int type = DriveType(Cpi->RearTorqueSplit());
    const float* misc = Env->Engine.MiscCarTunings.data();
    float gain = misc[type * 81 + 10 + World().Difficulty];    // var_14
    if ((BWareMath2::Fcom(F24(sl) * wz, WheelK::Zero) & 0x41) == 0 && State->WheelsOnGround >= 2) {
        float gv = X87::Bits(static_cast<uint32_t>(diff));     // var_18 still holds the u16 difference
        (Car->SlotType == 2 ? K().DriftYawAi : K().DriftYaw).GetValueTo(gv, std::fabs(sl));
        F24 tq = F24(gv) * gain * wz;
        const float* up = cs->R;                               // Car+0xB0 = CarState+0x70 (row 2)
        F24 tx = tq * up[8];
        float ty = (F24(up[9]) * tq).Store();
        float tzv = (F24(up[10]) * tq).Store();
        Rb->Torque[0] = (tx + Rb->Torque[0]).Store();
        Rb->Torque[1] = (F24(ty) + Rb->Torque[1]).Store();
        Rb->Torque[2] = (F24(tzv) + Rb->Torque[2]).Store();
    }
    float b = misc[type * 81 + 13];
    float v = (F24(std::fabs(sl)) * (F24(WheelK::One) - b) + F24(std::fabs(wz)) * b).Store();
    float mu = (F24(K().DriftRearFriction.GetValue(v)) * misc[type * 81 + 14]).Store();
    Wheels[2]->LongGripScale = mu;
    Wheels[3]->LongGripScale = mu;
}

void PhysicsMover::ResolveHydraulicForces() {
    if (Car->Collision->Contacts > 0) return;
    const float* hop = Driver->Hop;
    auto N = [](float v) { return !(v == 0.0f); };             // test ah,44h: != 0 or NaN
    int mask = 0;
    if (N(hop[1]) && N(hop[0])) mask = 0x0C;
    else if (N(hop[2]) && N(hop[3])) mask = 3;
    else if ((N(hop[2]) && N(hop[1])) || (N(hop[3]) && N(hop[0]))) mask = 0x20;
    bool allow = mask != 0;
    float ugly = World().UglyTimestepHack;
    for (int i = 0; i < 4; i++) {
        F24 tgt = F24(Driver->HydraulicHeight[i]) * K().HydHeightScale;
        HydraulicState[i] = 0;
        bool bit = (mask & (1 << i)) != 0;
        if (bit && (BWareMath2::Fcom(tgt, K().HydMinHeight) & 0x41) != 0) tgt = K().HydMinHeight;
        float h = State->HydraulicHeight[i];
        if ((BWareMath2::Fcom(tgt, h) & 0x41) == 0)               // tgt > h
        {
            float x = (F24(ugly) * K().HydRate + h).Store();
            if (!Lt(tgt, x)) tgt = x;
            State->HydraulicHeight[i] = tgt.Store();
            HydraulicState[i] = 2;
        } else if (Lt(tgt, h)) {
            F24 x = F24(h) - F24(ugly) * K().HydRate;
            if ((BWareMath2::Fcom(WheelK::Zero, x) & 0x41) == 0) x = WheelK::Zero;   // 0 > x
            State->HydraulicHeight[i] = x.Store();
            HydraulicState[i] = 3;
        } else if (Gt(tgt, WheelK::Zero)) HydraulicState[i] = 1;
        if (bit && !X87::ParityEven(X87::Fcom(State->HydraulicHeight[i], K().HydMinHeight) & 0x05)) allow = false;
    }
    if (!allow) return;
    for (int i = 0; i < 4; i++)
        if ((X87::Fcom(hop[i], WheelK::Zero) & 0x41) == 0 && !X87::ParityEven(X87::Fcom(Susp[i]->Compression, K().HydMinCompression) & 0x05))
            return;
    for (int i = 0; i < 4; i++) {
        if ((X87::Fcom(hop[i], WheelK::Zero) & 0x41) != 0) continue;
        float r[3];
        BWareMath2::MulVector3X87(r, Rb->R, Susp[i]->WheelPosLocal);
        float c[3];
        BWareMath::CrossX87(c, Rb->AngVel, r);
        F24 vx = F24(c[0]) + Rb->Vel[0];
        F24 vy = F24(Rb->Vel[1]) + c[1];
        float vz = (F24(Rb->Vel[2]) + c[2]).Store();            // var_18
        float vys = vy.Store();                                // var_10
        const float* n = Wheels[i]->GroundNormal;
        F24 vn = F24(vys) * n[0] + F24(vz) * n[2] + vx * n[1];  // (x and y swapped in the original)
        if ((BWareMath2::Fcom(vn, WheelK::Zero) & X87::C0) != 0) continue;   // vn < 0 (or NaN)
        F24 j = (F24(K().HydBase) - vn * K().HydVelGain) * hop[i];
        if ((BWareMath2::Fcom(j, WheelK::Zero) & 0x41) != 0) continue;
        j = j * Rb->Mass;
        const float imp[3] = {(j * n[0]).Store(), (j * n[1]).Store(), (j * n[2]).Store()};
        Rb->ApplyImpulse(imp, r);
        State->Hopped[i] = 1;
    }
}

void PhysicsMover::ResolveDynoForces() {
    float f[3] = {};                                           // dword_86B390.. (zero vector)
    const float* lf = Rb->LastForce;
    const float* v = Rb->Vel;
    if (Gt(F24(lf[2]) * v[2] + F24(lf[1]) * v[1] + F24(lf[0]) * v[0], WheelK::Zero)) {
        f[0] = lf[0]; f[1] = lf[1]; f[2] = lf[2];
    }
    const float d[3] = {
        (F24(DynoAnchor[0]) - DynoPos[0]).Store(), (F24(DynoAnchor[1]) - DynoPos[1]).Store(), (F24(DynoAnchor[2]) - DynoPos[2]).Store(),
    };
    BWareMath2::ScaleAdd(f, f, d, K().DynoSpring);
    BWareMath2::ScaleAdd(f, f, DynoVel, (-F24(K().DynoDamper)).Store());
    float ugly = World().UglyTimestepHack;
    BWareMath2::ScaleAdd(DynoVel, DynoVel, f, (F24(Rb->InvMass) * ugly).Store());
    BWareMath2::ScaleAdd(DynoPos, DynoPos, DynoVel, ugly);
    DynoPos[2] = Rb->Pos[2];
    Rb->Pos[0] = DynoPos[0]; Rb->Pos[1] = DynoPos[1]; Rb->Pos[2] = DynoPos[2];
    for (int i = 0; i < 4; i++) {
        Wheel* w = Wheels[i];
        if (!NotDriven(w)) continue;
        F24 s = F24(Rb->R[2]) * DynoVel[2] + F24(Rb->R[1]) * DynoVel[1] + F24(Rb->R[0]) * DynoVel[0];
        if (!X87::ParityEven(BWareMath2::Fcom(F24::Abs(s).V, K().DynoMinSpeed) & 0x05)) s = WheelK::Zero;
        w->Omega = (s / w->Info->Radius).Store();
    }
}

} // namespace bb

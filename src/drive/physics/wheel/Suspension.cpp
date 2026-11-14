#include "drive/physics/wheel/Suspension.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/PhysicsEnvironment.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/Wheel.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cstring>

namespace bb {

Suspension::Suspension(int index, RigidBody* rb, bb::Wheel* wheel, TwoWheelAckermanSteering* steering, const SuspensionInfo* si) {
    Index = index; Body = rb; Wheel = wheel; Steering = steering; Info = si; Partner = nullptr;
}

void Suspension::Calibrate(float preload, float rideHeight) {
    Preload = preload;
    const float* p = Wheel->Info->Pos;
    const float* cg = Body->CgOffset;
    float z = (F24(p[2]) - cg[2]).Store();
    Attach[0] = (F24(p[0]) - cg[0]).Store();
    Attach[2] = z;
    Attach[1] = (F24(p[1]) - cg[1]).Store();
    Attach[2] = (F24(Attach[2]) - rideHeight).Store();
}

void Suspension::ComputeAuxiliaries() {
    RigidBody* rb = Body;
    bb::Wheel* w = Wheel;
    const World& world = w->Env->World;
    float r[3];
    BWareMath::MulVector3(r, rb->R, Attach);
    float p[3];
    p[2] = (F24(r[2]) + rb->Pos[2]).Store();      // computed x, y, z on the stack; stored z, x, y
    p[0] = (F24(r[0]) + rb->Pos[0]).Store();
    p[1] = (F24(rb->Pos[1]) + r[1]).Store();
    int terrain = w->Terrain;
    F24 x = w->CheckTerrain(w->GroundNormal, terrain, p, rb->R) + w->Info->Radius;
    w->Terrain = terrain;
    float prev = Compression;                                    // var_D8
    if (x.V > Info->MaxCompression) { Compression = Info->MaxCompression; Touching = 1; }   // test ah,41h; jnz
    else if (x.V < Info->MinCompression) { Compression = Info->MinCompression; Touching = 0; } // test ah,5; jp
    else { Compression = x.Store(); Touching = 1; }
    if (world.UglyTimestepHack > WheelK::Eps1e4)
        CompressionRate = ((F24(Compression) - prev) / world.UglyTimestepHack).Store();
    WheelPosLocal[0] = Attach[0];
    WheelPosLocal[1] = Attach[1];
    WheelPosLocal[2] = Attach[2];
    WheelPosLocal[2] = (F24(Compression) + WheelPosLocal[2]).Store();

    float m[16];
    bb::Wheel::Identity(m);
    float flat = w->FlatDrag;                                    // var_D4
    if (flat != WheelK::Zero)                                    // test ah,44h; jnp skips when == 0
    {
        float rz[16];
        bb::Wheel::Identity(rz);
        float wob = (F24(BWareMath::Sin(w->Angle)) * WheelK::FlatWobble * flat).Store();   // fst var_D4
        float c = BWareMath::Sin((F24(wob) + WheelK::HalfPi).Store());
        float s = BWareMath::Sin(wob);
        rz[4] = -s; rz[1] = s; rz[0] = c; rz[5] = c;
        BWareMath::MulMatrix(m, rz, m);                          // bMulMatrix(&M, &M, &R) = eMulMatrix(&M, &R, &M)
    }
    m[12] = WheelPosLocal[0]; m[13] = WheelPosLocal[1]; m[14] = WheelPosLocal[2]; m[15] = WheelK::One;
    std::memcpy(LocalNoSteer, m, sizeof m);                      // eMulMatrix_0 0x4012B0 = copy
    if (Steering != nullptr)
        BWareMath::MulMatrix(m, Steering->Mat[Index], m);        // bMulMatrix(&M, &M, &mats[index])
    std::memcpy(w->Local, m, sizeof m);                          // sub_591480
    float r2[3];
    BWareMath::MulVector3(r2, rb->R, WheelPosLocal);
    float wr[3];
    BWareMath::CrossX87(wr, rb->AngVel, r2);
    float vx = (F24(wr[0]) + rb->Vel[0]).Store();
    float vy = (F24(rb->Vel[1]) + wr[1]).Store();
    float vz = (F24(rb->Vel[2]) + wr[2]).Store();
    w->ContactVel[0] = vx; w->ContactVel[1] = vy; w->ContactVel[2] = vz;
    BWareMath::MulMatrix(m, m, rb->World);                       // bMulMatrix(&M, &rb->WorldMat, &M)
    std::memcpy(w->World, m, sizeof m);                          // sub_5913D0

    if (Touching == 0) { SpringForce = 0.0f; return; }
    F24 c0 = Compression > WheelK::Zero ? F24(Compression) : F24(WheelK::Zero);
    F24 k = (c0 * Info->Progression + WheelK::One) * Info->SpringRate;
    float damp = CompressionRate > WheelK::Zero ? Info->BumpDamping : Info->ReboundDamping;
    F24 f = F24(damp) * CompressionRate + k * Compression;      // faddp of (k*x) and (damp*rate)
    f = f + Preload;
    SpringForce = f.Store();
    f = (F24(Compression) - Partner->Compression) * Info->AntiRoll + f;
    SpringForce = f.Store();
    if (f.V < WheelK::Zero) SpringForce = 0.0f;                  // fcomp _zero; test ah,5; jp keeps >= / NaN
}

F24 Suspension::ApplyGroundPenaltyImpulse() {
    RigidBody* rb = Body;
    float r[3];
    BWareMath::MulVector3(r, rb->R, WheelPosLocal);
    float c[3];
    BWareMath::CrossX87(c, rb->AngVel, r);
    F24 vx = F24(c[0]) + rb->Vel[0];
    F24 vy = F24(rb->Vel[1]) + c[1];
    F24 vz = F24(rb->Vel[2]) + c[2];
    float fvz = vz.Store(), fvx = vx.Store();                    // var_28, var_10 (vy stays on the stack)
    const float* n = Wheel->GroundNormal;
    F24 vn = -(F24(n[2]) * fvz + F24(n[1]) * vy + F24(n[0]) * fvx);
    if (vn.V < WheelK::Zero) return WheelK::Zero;                // test ah,5; jp continues when >= / NaN
    float imp = (F24(WheelK::PenaltyGain) * vn * rb->Mass).Store();
    const float j[3] = {(F24(n[0]) * imp).Store(), (F24(n[1]) * imp).Store(), (F24(n[2]) * imp).Store()};
    if (Wheel->Env->World.NoWheels == 0) rb->ApplyImpulse(j, r);
    return imp;
}

// ------------------------------------------------------------------ TwoWheelAckermanSteering

void TwoWheelAckermanSteering::SetSteering(uint16_t angle) {
    Angle = angle;
    float L = (F24(Wheels[0].Pos[0]) - Wheels[3].Pos[0]).Store();   // wheelbase
    float T = (F24(Wheels[0].Pos[1]) - Wheels[1].Pos[1]).Store();   // track
    F24 a = F24(static_cast<float>(angle)) * WheelK::AngToRad;
    if (a.V > WheelK::Pi) a = a - (F24(WheelK::Pi) + WheelK::Pi);
    bool pos = true;
    if (a.V < WheelK::Zero) { pos = false; a = -a; }              // test ah,5; jp keeps >= / NaN
    float b = (F24(L) * a / (a * T + L)).Store();
    float a0, a1;
    if (pos) { a1 = b; a0 = a.Store(); }
    else { a1 = (-a).Store(); a0 = -b; }
    Rot(Mat[0], a0);
    Rot(Mat[1], a1);
}

void TwoWheelAckermanSteering::Rot(float* m, float x) {
    Wheel::Identity(m);
    float c = BWareMath::Sin((F24(x) + WheelK::HalfPi).Store());
    float s = BWareMath::Sin(x);
    m[0] = c; m[4] = -s; m[1] = s; m[5] = c;
}

} // namespace bb

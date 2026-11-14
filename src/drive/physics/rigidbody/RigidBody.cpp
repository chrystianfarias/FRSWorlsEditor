#include "drive/physics/rigidbody/RigidBody.h"

#include "drive/numerics/BWareMath.h"

#include <cstring>

namespace bb {

namespace {
inline F24 F(float f) { return F24(f); }
inline bool LeOrNaN(F24 a, F24 b) { return BWareMath2::LeOrNaN(a, b); }
inline bool GeOrNaN(F24 a, F24 b) { return BWareMath2::GeOrNaN(a, b); }
inline bool NeOrNaN(F24 a, F24 b) { return BWareMath2::NeOrNaN(a, b); }

float Fl(const uint8_t* b, int o) { float f; std::memcpy(&f, b + o, 4); return f; }
int I(const uint8_t* b, int o) { int v; std::memcpy(&v, b + o, 4); return v; }
template <size_t N> void Arr(const uint8_t* b, int o, float (&a)[N]) { for (size_t i = 0; i < N; i++) a[i] = Fl(b, o + 4 * static_cast<int>(i)); }
void WI(uint8_t* b, int o, int v) { std::memcpy(b + o, &v, 4); }
void WF(uint8_t* b, int o, float v) { std::memcpy(b + o, &v, 4); }
template <size_t N> void WArr(uint8_t* b, int o, const float (&a)[N]) { for (size_t i = 0; i < N; i++) WF(b, o + 4 * static_cast<int>(i), a[i]); }

// d[o..o+2] += k * v (x87 order: x on the stack, y and z through float temporaries).
void AddScaled(float* d, int o, F24 k, const float* v) {
    F24 ax = k * v[0];
    float ay = (F(v[1]) * k).Store();
    float az = (F(v[2]) * k).Store();
    d[o] = (ax + d[o]).Store();
    d[o + 1] = (F(ay) + d[o + 1]).Store();
    d[o + 2] = (F(az) + d[o + 2]).Store();
}
} // namespace

RigidBodyInfo RigidBodyInfo::FromBytes(const uint8_t* b, int o) {
    RigidBodyInfo r;
    r.Mass = Fl(b, o); r.BoxLength = Fl(b, o + 4); r.BoxWidth = Fl(b, o + 8); r.BoxHeight = Fl(b, o + 12);
    r.YawInertiaScale = Fl(b, o + 0x50); r.Pad54 = Fl(b, o + 0x54); r.Pad58 = Fl(b, o + 0x58); r.Pad5C = Fl(b, o + 0x5C);
    for (int i = 0; i < 16; i++) r.Inertia[i] = Fl(b, o + 0x10 + 4 * i);
    return r;
}

float& RigidBody::State(int j) {
    if (j < 4) return Pos[j];
    if (j < 20) return R[j - 4];
    if (j < 24) return P[j - 20];
    if (j < 28) return L[j - 24];
    if (j == 28) return RestLevel;
    return Pad94[j - 29];
}

// ------------------------------------------------------------------ ctor / SetState

void RigidBody::Construct(const RigidBodyInfo* info, float gravity) {
    Info = info;
    Pos[0] = Pos[1] = Pos[2] = 0.0f;
    std::memset(R, 0, sizeof R);
    R[0] = R[5] = R[10] = R[15] = BWareMath2::K_One;                     // bIdentity
    for (int i = 0; i < 3; i++) { P[i] = 0.0f; L[i] = 0.0f; Vel[i] = 0.0f; AccelG[i] = 0.0f; AngVel[i] = 0.0f; Force[i] = 0.0f; Torque[i] = 0.0f; }
    RestLevel = 0.0f;
    for (int i = 0; i < 3; i++) { LastForce[i] = Force[i]; LastTorque[i] = Torque[i]; }
    YawTorqueClamp = 0.0f;
    TopSpeed = 0.0f;
    RaceEngineFlag = 0;
    TopSpeedScale = BWareMath2::K_One;
    RestDampingOn = 1; AngDampingOn = 1; UseYawScaledInertia = 1;
    Gravity = gravity;
    CalculateInertiaTensor();
}

void RigidBody::SetState(const float* pos, const float* r, const float* vel, const float* angVel) {
    Pos[0] = pos[0]; Pos[1] = pos[1]; Pos[2] = pos[2];
    BWareMath2::CopyMatrix(R, r);                                       // eMulMatrix_0 (copy)
    F24 m = Mass;
    P[0] = (m * vel[0]).Store();
    P[1] = (F(vel[1]) * m).Store();
    P[2] = (F(vel[2]) * m).Store();
    float inertia[16] = {};
    inertia[0] = inertia[5] = inertia[10] = inertia[15] = BWareMath2::K_One;   // bIdentity
    inertia[0] = (F(BWareMath2::K_One) / InvInertia[0]).Store();
    inertia[5] = (F(BWareMath2::K_One) / InvInertia[5]).Store();
    inertia[10] = (F(BWareMath2::K_One) / InvInertia[10]).Store();
    float rt[16];
    BWareMath2::TransposeMatrix(rt, R);
    float a[16];
    BWareMath2::BMulMatrix(a, R, inertia);
    float iw[16];
    BWareMath2::BMulMatrix(iw, a, rt);
    float l[3];
    BWareMath2::MulVector3X87(l, iw, angVel);
    L[0] = l[0]; L[1] = l[1]; L[2] = l[2];
    AngVel[0] = angVel[0]; AngVel[1] = angVel[1]; AngVel[2] = angVel[2];
    Vel[0] = vel[0]; Vel[1] = vel[1]; Vel[2] = vel[2];
}

// ------------------------------------------------------------------ 0x59B940

void RigidBody::CalculateInertiaTensor() {
    Mass = Info->Mass;                                        // fld [info]; fst +0x164
    InvMass = (F(BWareMath2::K_One) / Mass).Store();          // fld 1; fdiv st, st1
    BWareMath2::InvertMatrix(InvInertia, Info->Inertia);
    float m[16];
    BWareMath2::CopyMatrix(m, Info->Inertia);                 // eMulMatrix_0 (copy)
    m[10] = (F(m[10]) * Info->YawInertiaScale).Store();       // var_18 = [2][2]
    BWareMath2::InvertMatrix(InvInertiaYaw, m);
}

// ------------------------------------------------------------------ forces and impulses

void RigidBody::AddTorque(const float* t) {
    float tx = t[0], ty = t[1], tz = t[2];
    Torque[0] = (F(Torque[0]) + tx).Store();
    Torque[1] = (F(ty) + Torque[1]).Store();
    Torque[2] = (F(tz) + Torque[2]).Store();
}

void RigidBody::ApplyForce(const float* f, const float* r) {
    float fx = f[0], fy = f[1], fz = f[2];
    F24 n = F24::Sqrt(F(fz) * fz + F(fy) * fy + F(fx) * fx);
    F24 x = fx, y = fy;
    float z = fz;                                              // var_8
    if (!LeOrNaN(n, RigidBodyData::MaxForce)) {
        F24 k = F(RigidBodyData::MaxForce) / n;
        x = x * k;
        y = y * k;
        z = (F(z) * k).Store();
    }
    Force[0] = (x + Force[0]).Store();
    Force[1] = (y + Force[1]).Store();
    Force[2] = (F(z) + Force[2]).Store();
    float t[3];
    BWareMath::CrossX87(t, r, f);                              // bCross(var_10, r, F)
    AddClampedTorque(t);
}

void RigidBody::AddClampedTorque(const float* t) {
    F24 tx = t[0], tz = t[2], ty = t[1];
    F24 m = F24::Sqrt(ty * ty + tz * tz + tx * tx);
    if (!LeOrNaN(m, RigidBodyData::MaxTorque)) {
        F24 k = F(RigidBodyData::MaxTorque) / m;
        tx = tx * k;
        ty = ty * k;
        tz = tz * k;
    }
    Torque[0] = (tx + Torque[0]).Store();
    Torque[1] = (ty + Torque[1]).Store();
    Torque[2] = (tz + Torque[2]).Store();
}

void RigidBody::ApplyImpulse(const float* jIn, const float* r) {
    if (ApplyImpulseTrace) ApplyImpulseTrace(jIn, r);
    float jy = jIn[1], jz = jIn[2], jx = jIn[0];
    F24 n = F24::Sqrt(F(jy) * jy + F(jz) * jz + F(jx) * jx);
    if (!LeOrNaN(n, RigidBodyData::MaxImpulse)) {
        F24 k = F(RigidBodyData::MaxImpulse) / n;
        jx = (F(jx) * k).Store();
        jy = (F(jy) * k).Store();
        jz = (F(jz) * k).Store();
    }
    P[0] = (F(jx) + P[0]).Store();
    P[1] = (F(jy) + P[1]).Store();
    P[2] = (F(jz) + P[2]).Store();
    float t[3];
    const float jc[3] = {jx, jy, jz};
    BWareMath::CrossX87(t, r, jc);
    F24 tx = t[0], tz = t[2], ty = t[1];
    F24 m = F24::Sqrt(ty * ty + tz * tz + tx * tx);
    if (!LeOrNaN(m, RigidBodyData::MaxAngularImpulse)) {
        F24 k = F(RigidBodyData::MaxAngularImpulse) / m;
        tx = tx * k;
        ty = ty * k;
        tz = tz * k;
    }
    L[0] = (tx + L[0]).Store();
    L[1] = (ty + L[1]).Store();
    L[2] = (tz + L[2]).Store();
}

void RigidBody::ApplyImpulseWithScaledInverseInertiaTensor(const float* jIn, const float* r, float s) {
    float jy = jIn[1], jz = jIn[2], jx = jIn[0];
    F24 n = F24::Sqrt(F(jz) * jz + F(jy) * jy + F(jx) * jx);
    if (!LeOrNaN(n, RigidBodyData::MaxImpulse)) {
        F24 k = F(RigidBodyData::MaxImpulse) / n;
        jx = (F(jx) * k).Store();
        jy = (F(jy) * k).Store();
        jz = (F(jz) * k).Store();
    }
    P[0] = (F(jx) + P[0]).Store();
    P[1] = (F(jy) + P[1]).Store();
    P[2] = (F(jz) + P[2]).Store();
    float t[3];
    const float jc[3] = {jx, jy, jz};
    BWareMath::CrossX87(t, r, jc);
    F24 tx = t[0];
    float ty = t[1], tz = t[2];
    F24 m = F24::Sqrt(F(ty) * ty + F(tz) * tz + tx * tx);
    if (!LeOrNaN(m, RigidBodyData::MaxAngularImpulse)) {
        F24 k = F(RigidBodyData::MaxAngularImpulse) / m;
        tx = tx * k;
        ty = (F(ty) * k).Store();
        tz = (F(tz) * k).Store();
    }
    F24 sx = tx * s, sy = F(ty) * s;
    float sz = (F(tz) * s).Store();
    L[0] = (sx + L[0]).Store();
    L[1] = (sy + L[1]).Store();
    L[2] = (F(sz) + L[2]).Store();
}

void RigidBody::SetCGOffset(const float* newCg) {
    float a[3];
    float b[3];
    BWareMath2::MulVector3X87(a, R, newCg);
    BWareMath2::MulVector3X87(b, R, CgOffset);
    float cx = newCg[0], cy = newCg[1], cz = newCg[2];
    CgOffset[0] = cx; CgOffset[1] = cy; CgOffset[2] = cz;
    F24 dx = F(a[0]) - b[0], dy = F(a[1]) - b[1];
    float dz = (F(a[2]) - b[2]).Store();
    Pos[0] = (dx + Pos[0]).Store();
    Pos[1] = (dy + Pos[1]).Store();
    Pos[2] = (F(dz) + Pos[2]).Store();
}

// ------------------------------------------------------------------ derivatives (0x59C120, 0x59C1E0)

void RigidBody::SkewTimesMatrix(float* d, int o, const float* w, const float* r) {
    d[o + 0] = (F(r[2]) * w[1] - F(w[2]) * r[1]).Store();
    d[o + 4] = (F(r[6]) * w[1] - F(w[2]) * r[5]).Store();
    d[o + 12] = 0.0f;
    d[o + 8] = (F(r[10]) * w[1] - F(r[9]) * w[2]).Store();
    d[o + 1] = (F(w[2]) * r[0] - F(r[2]) * w[0]).Store();
    d[o + 5] = (F(w[2]) * r[4] - F(w[0]) * r[6]).Store();
    d[o + 13] = 0.0f;
    d[o + 9] = (F(w[2]) * r[8] - F(r[10]) * w[0]).Store();
    d[o + 2] = (F(w[0]) * r[1] - F(r[0]) * w[1]).Store();
    d[o + 6] = (F(w[0]) * r[5] - F(r[4]) * w[1]).Store();
    d[o + 14] = 0.0f; d[o + 3] = 0.0f; d[o + 7] = 0.0f; d[o + 11] = 0.0f; d[o + 15] = BWareMath2::K_One;
    d[o + 10] = (F(r[9]) * w[0] - F(r[8]) * w[1]).Store();
}

void RigidBody::GetDerivative(float* d) {
    // 1) top speed limiter
    if (NeOrNaN(TopSpeed, RigidBodyData::Zero)) {
        F24 k = F(TopSpeedScale) * TopSpeed + RigidBodyData::TopSpeedBias;
        if (RaceEngineFlag != 0) k = k * RigidBodyData::RaceEngineTopSpeedMul;
        float slope = R[2];                                   // var_68 = forward.z
        F24 f;
        if (!LeOrNaN(slope, RigidBodyData::Zero)) {
            if (!LeOrNaN(slope, RigidBodyData::UphillMaxSlope)) slope = RigidBodyData::UphillMaxSlope;
            f = F(BWareMath2::K_One) - (F(BWareMath2::K_One) - RigidBodyData::UphillMinFactor) / RigidBodyData::UphillMaxSlope * slope;
        } else {
            F24 neg = -F(slope);
            slope = neg.Store();                                // fst var_68
            if (!LeOrNaN(neg, RigidBodyData::DownhillMaxSlope)) slope = RigidBodyData::DownhillMaxSlope;
            f = (F(RigidBodyData::DownhillMaxFactor) - BWareMath2::K_One) / RigidBodyData::DownhillMaxSlope * slope + BWareMath2::K_One;
        }
        k = k * f;
        F24 vx = Vel[0], vy = Vel[1];
        F24 sxy = F24::Sqrt(vx * vx + vy * vy);
        if (!LeOrNaN(sxy * RigidBodyData::MpsToMph, k)) {
            F24 dot = F(R[2]) * Force[2] + F(R[1]) * Force[1] + F(R[0]) * Force[0];
            if (!LeOrNaN(dot, RigidBodyData::Zero)) {
                float rt[16];
                BWareMath2::TransposeMatrix(rt, R);
                float fl[3];
                BWareMath2::MulVector3X87(fl, rt, Force);
                F24 c = -(F(RigidBodyData::LimiterSlopeGain) * slope);
                if (!LeOrNaN(c, RigidBodyData::LimiterMaxForce)) c = RigidBodyData::LimiterMaxForce;
                fl[0] = c.Store();
                float fw[3];
                BWareMath2::MulVector3X87(fw, R, fl);
                Force[0] = fw[0]; Force[1] = fw[1]; Force[2] = fw[2];
            }
        }
    }
    // 2) basic derivatives
    d[1] = Vel[1]; d[2] = Vel[2]; d[0] = Vel[0];
    SkewTimesMatrix(d, 4, AngVel, R);
    d[20] = Force[0]; d[21] = Force[1]; d[22] = Force[2];
    d[24] = Torque[0]; d[25] = Torque[1]; d[26] = Torque[2];
    // 3) angular damping by table
    F24 wx = AngVel[0], wy = AngVel[1], wz = AngVel[2];
    float wl = F24::Sqrt(wx * wx + wy * wy + wz * wz).Store();  // var_64
    if (AngDampingOn != 0)
        AddScaled(d, 24, -F(Data.AngularDamping.GetValue(wl)), L);
    // 4) rest level
    F24 vx2 = Vel[0], vy2 = Vel[1], vz2 = Vel[2];
    float x = (F24::Sqrt(vx2 * vx2 + vy2 * vy2 + vz2 * vz2) / RigidBodyData::RestSpeedDiv).Store();  // var_68
    F24 w2 = F(wl) / RigidBodyData::RestSpinDiv;
    if (!LeOrNaN(w2, x)) x = w2.Store();
    float g = Data.RestLevel.GetValue(x);
    d[28] = LeOrNaN(g, RestLevel) ? (-F(RigidBodyData::RestLevelFall)).Store() : RigidBodyData::RestLevelRise;
    if (RestDampingOn != 0) {
        AddScaled(d, 24, -(F(RigidBodyData::RestAngularGain) * RestLevel), L);
        AddScaled(d, 20, -(F(RigidBodyData::RestLinearGain) * RestLevel), P);
    }
    // 5) yaw torque clamp (compares the raw torque.z, writes d.L.z)
    if (NeOrNaN(YawTorqueClamp, RigidBodyData::Zero)) {
        if (!LeOrNaN(Torque[2], YawTorqueClamp)) d[26] = YawTorqueClamp;
        else {
            F24 n = -F(YawTorqueClamp);
            if (!LeOrNaN(n, Torque[2])) d[26] = n.Store();
        }
    }
}

// ------------------------------------------------------------------ auxiliaries (0x59C5F0)

void RigidBody::ComputeAuxiliaries() {
    ComputeVelocity();
    F24 vx = Vel[0], vy = Vel[1], vz = Vel[2];
    F24 s = F24::Sqrt(vx * vx + vy * vy + vz * vz);
    float sf = s.Store();                                       // fst var_E4
    if (!LeOrNaN(s, Data.VelocityClamp) &&
        (BWareMath2::Fcom(static_cast<double>(Mass), RigidBodyData::VelocityClampMinMass) & 0x41) == 0) {
        F24 k = F(Data.VelocityClamp) / sf;
        P[0] = (k * P[0]).Store();
        P[1] = (k * P[1]).Store();
        P[2] = (k * P[2]).Store();
        ComputeVelocity();
    }
    F24 m = F(InvMass) * RigidBodyData::InvGravityG;
    float fz = Force[2];
    AccelG[0] = (m * Force[0]).Store();
    AccelG[1] = (F(Force[1]) * m).Store();
    AccelG[2] = (F(fz) * m).Store();

    float rt[16];
    BWareMath2::TransposeMatrix(rt, R);                         // var_C0
    const float* iinv = UseYawScaledInertia != 0 ? InvInertiaYaw : InvInertia;
    float a[16];
    BWareMath2::BMulMatrix(a, R, iinv);                         // var_80 = bMulMatrix(R, Iinv)
    float iw[16];
    BWareMath2::BMulMatrix(iw, a, rt);                          // var_40 = bMulMatrix(var_80, R^T)
    BWareMath2::MulVector3X87(AngVel, iw, L);                   // eMulVector(+0xC0, var_40, L)

    F24 wx = AngVel[0], wy = AngVel[1], wz = AngVel[2];
    F24 wl = F24::Sqrt(wx * wx + wy * wy + wz * wz);
    if (!LeOrNaN(wl, RigidBodyData::MaxAngularSpeed)) {
        F24 k = F(RigidBodyData::MaxAngularSpeed) / wl;
        float az = AngVel[2];
        AngVel[0] = (k * AngVel[0]).Store();
        AngVel[1] = (F(AngVel[1]) * k).Store();
        AngVel[2] = (F(az) * k).Store();
    }
    if (!GeOrNaN(RestLevel, RigidBodyData::Zero)) RestLevel = 0.0f;

    for (int i = 0; i < 12; i++) World[i] = R[i];
    World[12] = Pos[0]; World[13] = Pos[1]; World[14] = Pos[2]; World[15] = BWareMath2::K_One;
    for (int i = 0; i < 3; i++) { LastForce[i] = Force[i]; LastTorque[i] = Torque[i]; }
    for (int i = 0; i < 3; i++) { Force[i] = 0.0f; Torque[i] = 0.0f; }

    if (!LeOrNaN(Gravity, RigidBodyData::Zero)) {
        const float g[3] = {0.0f, 0.0f, (-(F(Mass) * Gravity)).Store()};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        ApplyForce(g, zero);
    }
}

void RigidBody::ComputeVelocity() {
    F24 im = InvMass;
    float pz = P[2];
    Vel[0] = (im * P[0]).Store();
    Vel[1] = (F(P[1]) * im).Store();
    Vel[2] = (F(pz) * im).Store();
}

// ------------------------------------------------------------------ integration (0x5AA480)

void RigidBody::IntegrateEuler(float h, const float* d, int offset) {
    for (int j = 0; j < StateSize; j++) {
        float& s = State(j);
        s = (F(h) * d[offset + j] + s).Store();
    }
}

void RigidBody::Substep(float h, float* scratch) {
    GetDerivative(scratch);
    IntegrateEuler(h, scratch);
    ComputeAuxiliaries();
}

// ------------------------------------------------------------------ memory image (tests, snapshots)

void RigidBody::ReadFrom(const uint8_t* b, int o) {
    Unk18 = I(b, o + 0x18); Unk1C = I(b, o + 0x1C);
    Arr(b, o + 0x20, Pos); Arr(b, o + 0x30, R); Arr(b, o + 0x70, P); Arr(b, o + 0x80, L);
    RestLevel = Fl(b, o + 0x90); Arr(b, o + 0x94, Pad94);
    Arr(b, o + 0xA0, Vel); Arr(b, o + 0xB0, AccelG); Arr(b, o + 0xC0, AngVel); Arr(b, o + 0xD0, World);
    Arr(b, o + 0x110, Force); Arr(b, o + 0x120, Torque); Arr(b, o + 0x130, LastForce); Arr(b, o + 0x140, LastTorque);
    RestDampingOn = I(b, o + 0x150); AngDampingOn = I(b, o + 0x154); UseYawScaledInertia = I(b, o + 0x158);
    RaceEngineFlag = I(b, o + 0x15C); Gravity = Fl(b, o + 0x160); Mass = Fl(b, o + 0x164); InvMass = Fl(b, o + 0x168);
    Arr(b, o + 0x170, InvInertia); Arr(b, o + 0x1B0, InvInertiaYaw); Arr(b, o + 0x1F0, CgOffset);
    YawTorqueClamp = Fl(b, o + 0x200); TopSpeed = Fl(b, o + 0x204); TopSpeedScale = Fl(b, o + 0x208); Pad20C = Fl(b, o + 0x20C);
}

void RigidBody::WriteTo(uint8_t* b, int o) const {
    WI(b, o + 0x18, Unk18); WI(b, o + 0x1C, Unk1C);
    WArr(b, o + 0x20, Pos); WArr(b, o + 0x30, R); WArr(b, o + 0x70, P); WArr(b, o + 0x80, L);
    WF(b, o + 0x90, RestLevel); WArr(b, o + 0x94, Pad94);
    WArr(b, o + 0xA0, Vel); WArr(b, o + 0xB0, AccelG); WArr(b, o + 0xC0, AngVel); WArr(b, o + 0xD0, World);
    WArr(b, o + 0x110, Force); WArr(b, o + 0x120, Torque); WArr(b, o + 0x130, LastForce); WArr(b, o + 0x140, LastTorque);
    WI(b, o + 0x150, RestDampingOn); WI(b, o + 0x154, AngDampingOn); WI(b, o + 0x158, UseYawScaledInertia);
    WI(b, o + 0x15C, RaceEngineFlag); WF(b, o + 0x160, Gravity); WF(b, o + 0x164, Mass); WF(b, o + 0x168, InvMass);
    WArr(b, o + 0x170, InvInertia); WArr(b, o + 0x1B0, InvInertiaYaw); WArr(b, o + 0x1F0, CgOffset);
    WF(b, o + 0x200, YawTorqueClamp); WF(b, o + 0x204, TopSpeed); WF(b, o + 0x208, TopSpeedScale); WF(b, o + 0x20C, Pad20C);
}

} // namespace bb

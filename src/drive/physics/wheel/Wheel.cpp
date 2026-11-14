#include "drive/physics/wheel/Wheel.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsEnvironment.h"

#include <cmath>
#include <cstring>

namespace bb {

Wheel::Wheel(PhysicsEnvironment* env, const WheelInfo* wi, const TireInfo* ti, const ControlInfo* ci, CarState* cs) {
    Env = env; Info = wi; Tire = ti; Control = ci; Car = cs;
    NetTorqueFilt = 0; RoadNoise = 0; SlipCopy = 0; RideOffset = 0;
    RollState = 1;
    GripScale = GripScale2 = LongGripScale = LongForceScale = LatForceScale = 1.0f;
}

const WheelTweaks& Wheel::T() const { return Env->Tweaks; }

// ------------------------------------------------------------------ level 1 leaves

F24 Wheel::GetPilotFactor() const {
    if (BrakeLocked != 0) return WheelK::One;
    if (Omega < WheelK::Zero) return WheelK::One;                 // fcomp _zero; test ah,5; jnp
    if (Info->Index < 2) return WheelK::One;                       // cmp [wi+18h],2; jge
    F24 t = (F24(Car->ForwardSpeed) - WheelK::PilotSpeedMin) * WheelK::PilotSpeedScale;
    if (t.V > WheelK::One) t = WheelK::One;                        // test ah,41h; jnz keeps t <= 1 / NaN
    else if (t.V < WheelK::Zero) t = WheelK::Zero;                 // test ah,5; jp keeps t >= 0 / NaN
    return (F24(WheelK::One) - T().PilotFactor) * t + T().PilotFactor;
}

void Wheel::CheckForBrakeLock(float arg) {
    F24 eb = F24(Control->EBrakeTorqueScale) * EBrake * T().EBrakingTorque;
    F24 b = F24(Control->BrakeTorqueScale) * Brake * T().BrakingTorque;
    F24 bt = (eb + b) * T().StaticToDynamicBrakeForceRatio;
    F24 rhs = F24::Abs(Omega) * T().BrakeLockAngularVelocityFactor + F24(Info->Radius) * arg;
    if (!(bt.V > rhs.V)) { BrakeLocked = 0; return; }            // fcompp; test ah,41h; jnz
    BrakeLocked = bt.V > WheelK::One ? 1 : 0;                     // fcomp float_1; test ah,41h
    Omega = 0.0f;                                                 // mov [ecx+28h], edx (= 0)
}

void Wheel::GetHorizontalGroundForceInCarFrame(float* o) const {
    F24 y = F24(Local[5]) * Fy + F24(Local[1]) * Fx;   // [C4]*Fy + [B4]*Fx
    F24 z = F24(Local[6]) * Fy + F24(Local[2]) * Fx;   // [C8]*Fy + [B8]*Fx
    F24 x = F24(Local[4]) * Fy + F24(Fx) * Local[0];   // [C0]*Fy + Fx*[B0]
    o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

// ------------------------------------------------------------------ level 2

F24 Wheel::GetLateralForce(float load, float slipAngle) const {
    const WheelTables& tb = Env->Tables;
    if (T().UseNewTireForces == 0)
        return F24(tb.CorneringForce.GetValue(slipAngle)) * Load * T().ZForceScale * T().OldCorneringScale;
    F24 xs = F24(slipAngle) * WheelK::RadToDeg / Tire->MaxSlipAngle * WheelK::Twelve * WheelK::Half;
    float x = xs.Store();                                        // fst [arg_4]
    int i = X87::FtoL2Lo(xs.V);                                  // __ftol2 of st0
    float ld = (F24(T().LoadFactor) * load / Tire->LoadRef).Store(); // fstp [arg_0]
    if (i >= 6)
        return F24(tb.LoadSensitivity().GetValue(ld)) * Tire->LateralScale * T().NewCorneringScale;
    // off_7FC7D4[i] / off_7FC7D8[i]: i < 0 would index outside the pointer table (never: callers pass |a|)
    float a0 = tb.Curves[i].GetValue(ld);                        // fstp [arg_0]
    F24 a1 = tb.Curves[i + 1].GetValue(ld);
    F24 frac = F24(x) - F24(static_cast<float>(i));              // fild i; fsubr x
    return (frac * (a1 - a0) + a0) * Tire->LateralScale * T().NewCorneringScale;
}

void Wheel::GetDerivative(float* d) {
    d[0] = Omega;
    F24 net = (F24(Torque) - F24(Info->Radius) * Fx + NetTorqueFilt) * WheelK::Half;
    NetTorqueFilt = net.Store();                                 // fst (net stays on the stack)
    F24 a = net - F24(WheelK::RollingCoef) * Env->Tables.Terrain[Terrain].Rolling * Omega;
    a = a - F24(FlatDrag) * WheelK::FlatDragCoef * Omega;
    d[1] = (a / Inertia).Store();
    if (BrakeLocked != 0) d[1] = 0.0f;
    if (Sliding != 0) return;
    if (!(Env->World.UglyTimestepHack > WheelK::Eps1e3)) return;           // test ah,41h; jnz
    d[1] = (F24(d[1]) - F24(SlipVel) / (F24(Info->Radius) * Env->World.UglyTimestepHack)).Store();
}

void Wheel::ComputeAuxiliaries() {
    Identity(Spin);
    float c = BWareMath::Sin((F24(Angle) + WheelK::HalfPi).Store());
    float s = BWareMath::Sin(Angle);
    Spin[8] = s;                                                 // +0x50
    Spin[2] = -s;                                                // +0x38
    Spin[0] = c;                                                 // +0x30
    Spin[10] = c;                                                // +0x58
    Fx = 0.0f; Fy = 0.0f; Torque = 0.0f; SlipVel = 0.0f;
    if (Angle > WheelK::AngleWrap)                               // test ah,41h; jnz
    {
        Angle = (F24(Angle) - WheelK::AngleWrap).Store();        // fsub; fst
        if (Angle > WheelK::AngleReset) Angle = 0.0f;
    } else if (Angle < WheelK::MinusAngleWrap)                   // test ah,5; jp
    {
        Angle = (F24(Angle) + WheelK::AngleWrap).Store();
        if (Angle < WheelK::MinusAngleReset) Angle = 0.0f;
    }
    if (RollState == 0) { if (Omega < WheelK::Zero) Omega = 0.0f; }
    else if (RollState == 2) { if (Omega > WheelK::Zero) Omega = 0.0f; }
    if (Omega > WheelK::RollEps) RollState = 0;
    else if (Omega < WheelK::MinusRollEps) RollState = 2;         // test ah,5; jp -> 1 when >= / NaN
    else RollState = 1;
}

float Wheel::ExchangeZForces(int touching, float springForce) {
    OnGround = static_cast<uint8_t>(touching);
    if (touching != 0) {
        const float centre[3] = {World[12], World[13], World[14]};   // +0x0A0
        ScaleAdd(ContactPt, centre, GroundNormal, -Info->Radius);
    }
    if (!(springForce > WheelK::Zero)) { Load = 0.0f; return 0.0f; }  // test ah,41h; jnz
    const float side[3] = {World[4], World[5], World[6]};            // +0x080
    float t[3];
    BWareMath::CrossX87(t, side, GroundNormal);
    float t2[3];
    BWareMath::CrossX87(t2, t, side);
    std::memcpy(t, t2, sizeof t);
    F24 k = (F24(t[1]) * GroundNormal[1] + F24(t[2]) * GroundNormal[2] + F24(t[0]) * GroundNormal[0])
            * WheelK::LoadTiltMul - WheelK::LoadTiltSub;
    if (k.V < WheelK::LoadTiltMin) k = WheelK::LoadTiltMin;        // test ah,5; jp keeps k >= 0.3 / NaN
    Load = (k * springForce).Store();
    return 0.0f;
}

// ------------------------------------------------------------------ level 3

// bTransposeMatrix(&T, &World), zero T's column 3 (T.m[0..2][3]), eMulVector(&lv, &T, &ContactVel).
// Row 3 of T is World's column 3.
void Wheel::LocalVelocity(float* lv) const {
    float tr[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) tr[r * 4 + c] = World[c * 4 + r];
    tr[3] = 0.0f; tr[7] = 0.0f; tr[11] = 0.0f;
    BWareMath::MulVector3(lv, tr, ContactVel);
}

float Wheel::SlipAngle(const float* lv) const {
    uint16_t u = Env->Math.FastATan(lv[0], lv[1]);
    F24 st = F24(static_cast<float>(u)) * WheelK::AngToRad;
    float a = st.Store();                                        // fst var_64
    if (st.V > WheelK::Pi) a = (F24(a) - (F24(WheelK::Pi) + WheelK::Pi)).Store();
    if (lv[0] < WheelK::Zero)                                    // test ah,5; jp skips when >= / NaN
    {
        st = F24(WheelK::Pi) + a;
        a = st.Store();
        if (st.V > WheelK::Pi) a = (F24(a) - (F24(WheelK::Pi) + WheelK::Pi)).Store();
    }
    return a;
}

void Wheel::GuessGroundForce(int noAbs) {
    F24 b = F24(Control->BrakeTorqueScale) * Brake * T().BrakingTorque;
    float eb = (F24(Control->EBrakeTorqueScale) * EBrake * T().EBrakingTorque).Store();   // var_5C
    if (Omega > WheelK::Zero) b = -b;                            // test ah,41h; jnz skips the fchs
    int locked = BrakeLocked;
    if (locked == 0) Torque = (b + Torque).Store();
    F24 e = eb;
    if (Omega > WheelK::Zero) e = -e;
    if (locked == 0) Torque = (e + Torque).Store();

    float lv[3];
    LocalVelocity(lv);
    float a = SlipAngle(lv);                                     // var_64

    float slip = (F24(Info->Radius) * Omega - lv[0]).Store();
    SlipVel = slip; SlipCopy = slip;
    if (-T().MaxStaticSlip > SlipVel                             // fld MSS; fchs; fcomp [118h]; test ah,41h
        && Torque < WheelK::Zero                                 // test ah,5; jp
        && eb == WheelK::Zero                                    // test ah,44h; jp
        && noAbs == 0)
        Torque = (F24(T().MaxStaticSlip) / -F24(SlipVel) * Torque).Store();

    float s = SlipVel;                                           // var_5C
    float ly2 = (F24(lv[1]) * lv[1]).Store();                    // var_54
    float mag = F24::Sqrt(F24(s) * s + ly2).Store();             // var_58 (fst; compare uses st0 = same value)
    bool dynamic = false;
    float mu = 0.0f, k = 0.0f;
    if (mag != WheelK::Zero && (lv[1] != WheelK::Zero || lv[0] != WheelK::Zero)) {
        const WheelTables& tb = Env->Tables;
        mu = (F24(tb.Terrain[Terrain].DynamicMu) * GripScale2 * GripScale * Tire->MuLat).Store();   // var_60
        if (Car->PilotFlag == 1) mu = (GetPilotFactor() * mu).Store();
        k = (F24(Load) / mag * T().ZForceScale * mu).Store();      // var_58
        F24 lim = F24(Load) / F24::Sqrt(F24(lv[0]) * lv[0] + ly2) * T().ZForceScale * mu;
        CheckForBrakeLock((lim * std::fabs(lv[0])).Store());
        dynamic = std::fabs(s) > T().MaxStaticSlip               // test ah,41h; jz -> dynamic
                  || std::fabs(a) > Tire->MaxSlipAngle
                  || BrakeLocked != 0;
    }
    if (dynamic) {
        Sliding = 1;
        float fx = (F24(s) * LongForceScale * k).Store();        // var_54
        Fx = fx;
        float fy = (-(F24(k) * LatForceScale * lv[1])).Store();  // var_58
        Fy = fy;
        F24 mph = F24(Car->ForwardSpeed) * WheelK::MpsToMph;
        if (mph.V < WheelK::One && mu > WheelK::PointOne)         // test ah,5; jp / test ah,41h; jnz
        {
            F24 inv = F24(WheelK::One) / mu;
            Fx = (F24(fx) * inv).Store();
            Fy = (inv * fy).Store();
        }
        LastSlip = s;
        LastLatVel = lv[1];
        return;
    }
    Sliding = 0;
    Fx = (F24(LongForceScale) / Info->Radius * Torque).Store();
    float sx = WheelK::One;                                      // var_58
    if (!(lv[0] > WheelK::Zero)) sx = WheelK::MinusOne;
    float sa = WheelK::MinusOne;                                 // var_54
    if (!(a > WheelK::Zero)) sa = WheelK::One;
    F24 lat = GetLateralForce((F24(T().ZForceScale) * Load).Store(), std::fabs(a));
    LastSlip = 0.0f;
    LastLatVel = lv[1];
    Fy = (lat * LatForceScale * sa * sx).Store();
}

void Wheel::FinalizeGroundForce() {
    bool boost = false;
    if (Torque > WheelK::Zero && BrakeLocked == 0)               // test ah,41h; jnz
    {
        boost = true;
        Fx = (F24(T().DriveBoost) * Fx).Store();
    }
    float F = F24::Sqrt(F24(Fy) * Fy + F24(Fx) * Fx).Store();   // var_A4
    float fmax = (F24(Env->Tables.Terrain[Terrain].CircleMu) * Tire->MuLong * LongGripScale * GripScale2
                  * GripScale * Load * T().ZForceScale).Store();   // var_A8
    if (Car->PilotFlag == 1) fmax = (GetPilotFactor() * fmax).Store();
    if (F > fmax && F > WheelK::Eps1e3) {
        F24 sc = F24(fmax) / F;
        F24 fy = Fy;
        Fx = (sc * Fx).Store();
        Fy = (fy * sc).Store();
    } else if (boost)
        Fx = (F24(Fx) / T().DriveBoost).Store();
    if (fmax > WheelK::Eps1e3) {
        F24 g = F24(F) / fmax;
        GripUsage = g.Store();                                   // fst [170h]
        if (g.V > WheelK::One) GripUsage = WheelK::One;          // test ah,41h; jnz keeps <= 1 / NaN
    } else GripUsage = WheelK::One;
    if (Sliding != 0 || BrakeLocked != 0) GripUsage = WheelK::One;
    if (OnGround == 0) GripUsage = 0.0f;

    float lv[3];
    LocalVelocity(lv);
    float a = SlipAngle(lv);                                     // var_A8
    BWareMath::Sin((F24(a) + WheelK::HalfPi).Store());           // called and discarded (fstp st)
    F24 sfy = F24(BWareMath::Sin(a)) * Fy;
    if (std::fabs(lv[0]) > WheelK::One)                          // test ah,41h; jnz
        Fx = (F24(Fx) - sfy * WheelK::LatLongCoupling).Store();

    float m[16];
    Identity(m);
    switch (T().WheelForceFrame) {
        case 1:
            std::memcpy(m, World, 12 * sizeof(float));           // rows 0..2 (4 floats each, incl. w)
            break;
        case 2: {
            const float* n = GroundNormal;
            const float fwd[3] = {World[0], World[1], World[2]};
            const float side[3] = {World[4], World[5], World[6]};
            float s0 = (-(F24(n[2]) * World[2] + F24(n[1]) * World[1] + F24(World[0]) * n[0])).Store();
            float r[3];
            ScaleAdd(r, fwd, n, s0);
            m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
            float s1 = (-(F24(n[2]) * World[6] + F24(n[1]) * World[5] + F24(World[4]) * n[0])).Store();
            m[3] = 0.0f;
            ScaleAdd(r, side, n, s1);
            m[4] = r[0]; m[5] = r[1]; m[6] = r[2];
            m[9] = n[1]; m[7] = 0.0f; m[8] = n[0]; m[10] = n[2]; m[11] = 0.0f;
            break;
        }
        case 3: {
            const float* n = GroundNormal;
            const float fwd[3] = {World[0], World[1], World[2]};
            float s0 = (-(F24(World[2]) * n[2] + F24(World[1]) * n[1] + F24(n[0]) * World[0])).Store();
            float r[3];
            ScaleAdd(r, fwd, n, s0);
            float rn[3];
            BWareMath::Normalize(rn, r);
            std::memcpy(r, rn, sizeof r);
            m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
            m[8] = n[0]; m[9] = n[1]; m[10] = n[2];
            m[3] = 0.0f; m[11] = 0.0f;
            const float row2[3] = {m[8], m[9], m[10]};
            float row1[3];
            BWareMath::CrossX87(row1, row2, r);
            m[4] = row1[0]; m[5] = row1[1]; m[6] = row1[2];
            m[7] = 0.0f;
            break;
        }
        case 4:
            std::memcpy(m, World, 8 * sizeof(float));            // rows 0 and 1
            m[8] = GroundNormal[0]; m[9] = GroundNormal[1]; m[10] = GroundNormal[2]; m[11] = 0.0f;
            break;
        default:
            break;
    }
    const float f[3] = {Fx, Fy, Load};
    BWareMath::MulVector3(ForceWorld, m, f);
}

// ------------------------------------------------------------------ level 4 (world queries)

F24 Wheel::CheckTerrain(float* normalOut, int& terrOut, const float* pt, const float* carMat) {
    IWheelWorld* w = Env->Terrain;
    bool valid = false;
    F24 st = F24(w->GetElevation(Car->Topology, pt, terrOut, normalOut, valid)) + RideOffset;
    if (!valid) st = WheelK::NoElevation;
    float elev = st.Store();                                     // var_8
    float px = pt[0], py = pt[1];                                // var_10, var_C
    const float pxy[2] = {px, py};
    F24 noise = w->GetRoadNoise(pxy, terrOut);
    RoadNoise = noise.Store();                                   // fst [168h]
    elev = (noise + elev).Store();
    if (Env->World.Flags.DragByte != 0 || Env->World.Flags.TweakIsDragRace != 0) terrOut = 1;
    const float* n = normalOut;
    F24 up = F24(carMat[9]) * n[1] + F24(carMat[10]) * n[2] + F24(carMat[8]) * n[0];
    if (up.V < WheelK::Eps1e3)                                   // test ah,5; jp -> ok when >= / NaN
    {
        terrOut = 0;
        return WheelK::NoGround;
    }
    F24 dx = F24(px) - pt[0], dy = F24(py) - pt[1], dz = F24(elev) - pt[2];
    F24 sum = dy * n[1] + dx * n[0];
    sum = sum + dz * n[2];
    return sum / up;
}

// ------------------------------------------------------------------ helpers (bWare)

void Wheel::Identity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void Wheel::ScaleAdd(float* o, const float* a, const float* b, float s) {
    F24 y = F24(s) * b[1] + a[1];
    F24 z = F24(s) * b[2] + a[2];
    F24 x = F24(s) * b[0] + a[0];
    o[0] = x.Store(); o[1] = y.Store(); o[2] = z.Store();
}

} // namespace bb

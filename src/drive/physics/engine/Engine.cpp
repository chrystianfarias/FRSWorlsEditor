#include "drive/physics/engine/Engine.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/engine/DriveTrain.h"

#include <cmath>

namespace bb {

namespace {
inline float A(float x, float y) { return BWareMath::A(x, y); }
inline float S(float x, float y) { return BWareMath::S(x, y); }
inline float M(float x, float y) { return BWareMath::M(x, y); }
inline float D(float x, float y) { return BWareMath::D(x, y); }
} // namespace

float Engine::Table::GetValue(float x) const { return BWareMath::TableGetValue(Count, Min, InvStep, Y, x); }

void Engine::Construct(const EngineInfo* info, bb::CarState* carState) {
    float span;
    TorqueTable.Count = 9; TorqueTable.Min = info->IdleRpm; TorqueTable.Max = info->MaxRpm; TorqueTable.Y = info->Torque;
    span = S(info->MaxRpm, info->IdleRpm);
    TorqueTable.InvStep = D(EngineK::Eight, span);
    EngineBrakeTable.Count = 3; EngineBrakeTable.Min = info->IdleRpm; EngineBrakeTable.Max = info->MaxRpm; EngineBrakeTable.Y = info->EngineBrake;
    span = S(info->MaxRpm, info->IdleRpm);
    EngineBrakeTable.InvStep = D(EngineK::TwoTbl, span);
    BoostTable.Count = 9; BoostTable.Min = info->IdleRpm; BoostTable.Max = info->MaxRpm; BoostTable.Y = info->Boost;
    span = S(info->MaxRpm, info->IdleRpm);
    BoostTable.InvStep = D(EngineK::Eight, span);
    NosTable.Count = 9; NosTable.Min = info->IdleRpm; NosTable.Max = info->RedlineRpm; NosTable.Y = K.NosCurve.data();
    span = S(info->RedlineRpm, info->IdleRpm);
    NosTable.InvStep = D(EngineK::Eight, span);
    CarState = carState;
    Info = info;
    HpMultFlag = 0;
    NosLevel = 0;
    Heat = -1.0f;
    Unk94 = 0.0f;
    ThrottleGate = 1.0f;
    TorqueScale = 1.0f;
    RevLimiter = false;
    Boost = 0.0f; TurboState = 0; TurboTimer = 0.0f; BoostPrev = 0.0f; BoostAtRelease = 0.0f;
    MaxBoost = GetMaxBoost(info);
    DriveTrain = nullptr;
}

float Engine::GetMaxBoost(const EngineInfo* info) {
    float m = 0.0f;
    if ((X87::Fcom(info->Boost[0], 0.0f) & 0x41) == 0) m = info->Boost[0];          // b0 > 0
    for (int i = 1; i < 9; i++)
        if (!X87::ParityEven(X87::Fcom(m, info->Boost[i]) & 0x05)) m = info->Boost[i];  // m < bi
    return m;
}

float Engine::GetEffectiveThrottle() const { return RevLimiter ? 0.0f : M(ThrottleGate, Throttle); }

bool Engine::IsBlown() const { return !X87::ParityEven(X87::Fcom(Heat, EngineK::HeatBlown) & 0x44); }

float Engine::GetTotalEngineTorque() const {
    float rpm = M(Omega, EngineK::RadToRpm);                         // fstp var_4 (float argument)
    float t = M(TorqueTable.GetValue(rpm), TorqueScale);
    if (HpMultFlag != 0) t = M(t, K.TorqueHPMultiplier);
    float thr = RevLimiter ? 0.0f : M(ThrottleGate, Throttle);
    float acc = M(t, thr);                                           // var_8
    if (NosLevel > 0)                                                // jle (signed)
    {
        float mul = K.NosTorqueMultiplier[NosLevel];
        float n = M(M(NosTable.GetValue(rpm), CarState->Cpi->NosTorqueMul), mul);
        float thr2 = RevLimiter ? 0.0f : M(ThrottleGate, Throttle);
        acc = A(M(thr2, n), acc);
    }
    if ((X87::Fcom(Heat, K.HeatThreshold) & 0x41) == 0)              // heat > 0.6
    {
        float den = S(1.0f, K.HeatThreshold);
        float h = Heat;
        if (!X87::ParityEven(X87::Fcom(1.0f, h) & 0x05)) h = 1.0f;   // 1 < heat -> 1
        float x = D(S(h, K.HeatThreshold), den);
        x = S(1.0f, M(x, EngineK::HeatFade));
        acc = M(x, acc);
    }
    float thr3 = RevLimiter ? 0.0f : M(ThrottleGate, Throttle);      // var_4
    float brake = -M(EngineBrakeTable.GetValue(rpm), S(1.0f, thr3));
    if (RevLimiter) brake = M(brake, EngineK::Two);
    float r = A(brake, acc);
    if (!X87::ParityEven(X87::Fcom(r, 0.0f) & 0x05) && !RevLimiter)  // r < 0
        r = M(r, DriveTrain->Gear == 1 ? EngineK::NeutralDecel : EngineK::GearDecel);
    return r;
}

float Engine::GetDerivative() const {
    float a = A(GetTotalEngineTorque(), LoadTorque);
    float inertia = Info->Inertia;
    int cs = DriveTrain->ClutchState;
    if (cs == 2 || cs == 0) inertia = M(inertia, EngineK::InertiaMul);
    return D(a, inertia);
}

void Engine::ComputeAuxiliaries(float uglyTimestepHack) {
    float wMax = M(Info->MaxRpm, EngineK::RpmToRad);                 // var_8
    float c = M(Info->IdleRpm, EngineK::RpmToRad);
    float w = Omega;                                                 // var_4
    if ((X87::Fcom(w, c) & 0x41) == 0) c = w;                        // w > wMin
    if (!X87::ParityEven(X87::Fcom(wMax, c) & 0x05)) c = wMax;       // wMax < c
    Omega = c;
    float rpm = M(c, EngineK::RadToRpm);
    LoadTorque = 0.0f;
    if (RevLimiter) {
        float off = S(Info->MaxRpm, M(EngineK::LimiterHyst, Info->MaxRpm));
        if (!X87::ParityEven(X87::Fcom(rpm, off) & 0x05)) RevLimiter = false;   // rpm < max - 1.25%
    } else {
        float on = A(EngineK::LimiterOn, Info->MaxRpm);
        if ((X87::Fcom(rpm, on) & 0x41) == 0) RevLimiter = true;               // rpm > max - 25
    }
    UpdateTurbo(uglyTimestepHack);
    UpdateHeat(uglyTimestepHack);
}

void Engine::UpdateHeat(float dt) {
    if ((X87::Fcom(Heat, 0.0f) & X87::C0) != 0) return;                       // heat < 0 (or NaN)
    if (X87::ParityEven(X87::Fcom(Heat, EngineK::HeatBlown) & 0x05)) return;  // heat >= 2 (or NaN)
    if (DriveTrain->Gear == 2 && (X87::Fcom(DriveTrain->ShiftBonus, 0.0f) & 0x41) == 0) return;   // bonus > 0
    float rpm = M(Omega, EngineK::RadToRpm);
    float rate = X87::ParityEven(X87::Fcom(Info->RedlineRpm, rpm) & 0x05) ? EngineK::HeatFall : EngineK::HeatRise;
    float h = A(M(rate, dt), Heat);
    if ((X87::Fcom(0.0f, h) & 0x41) == 0) h = 0.0f;                             // 0 > h
    Heat = h;
    if ((X87::Fcom(h, EngineK::HeatLimit) & X87::C0) == 0) Heat = EngineK::HeatBlown;   // h >= 1.2
}

void Engine::UpdateTurbo(float dt) {
    if (!X87::ParityEven(X87::Fcom(MaxBoost, 0.0f) & 0x41)) return;  // maxBoost <= 0 ("jnp")
    if (!X87::ParityEven(X87::Fcom(dt, 0.0f) & 0x44)) return;        // dt == 0
    float rpm = M(Omega, EngineK::RadToRpm);                         // var_4
    float mb = D(MaxBoost, EngineK::TurboMaxDiv);                    // var_C
    float v8 = -M(EngineK::TurboIdle, mb);                           // var_8
    switch (TurboState) {
        case 0:
            if ((X87::Fcom(GetEffectiveThrottle(), EngineK::TurboThrottle) & X87::C0) == 0) { TurboState = 1; return; }
            {
                float q = D(S(rpm, Info->IdleRpm), S(Info->MaxRpm, Info->IdleRpm));
                q = A(q, q);
                Boost = M(BWareMath2::Min(1.0f, q), v8);
            }
            return;
        case 1:
            if ((X87::Fcom(GetEffectiveThrottle(), EngineK::TurboThrottle) & X87::C0) != 0) {
                if (RevLimiter) return;
                TurboState = 2;
                TurboTimer = dt;
                BoostAtRelease = Boost;
                return;
            }
            {
                float m1 = BWareMath2::Min(1.0f, GetEffectiveThrottle());
                float q = D(S(rpm, Info->IdleRpm), S(Info->MaxRpm, Info->IdleRpm));
                float y = A(M(M(M(m1, q), EngineK::TurboRate), dt), Boost);
                Boost = BWareMath2::Min(mb, y);
            }
            return;
        case 2: {
            float t = A(TurboTimer, EngineK::TurboT0);
            t = M(t, t);
            t = M(t, t);
            float s = BWareMath::Sin(A(M(t, EngineK::TurboFreq), EngineK::PiOver2));
            float lo = A(v8, v8);
            float v = A(M(M(D(D(BoostAtRelease, mb), TurboTimer), EngineK::TurboAmp), s), v8);
            float nb = BWareMath2::Clamp(v, lo, BoostAtRelease);
            bool settled = false;
            if (!X87::ParityEven(X87::Fcom(BoostPrev, 0.0f) & 0x44))            // prev == 0
                BoostPrev = nb;
            else {
                bool check;
                if ((X87::Fcom(nb, Boost) & 0x41) == 0 && !X87::ParityEven(X87::Fcom(nb, BoostPrev) & 0x05))
                    check = true;                                            // boost < nb < prev
                else
                    check = !X87::ParityEven(X87::Fcom(nb, Boost) & 0x05)    // nb < boost
                            && (X87::Fcom(nb, BoostPrev) & 0x41) == 0;       // and nb > prev
                if (check) {
                    float d = std::fabs(S(Boost, BoostPrev));
                    if (!X87::ParityEven(X87::Fcom(d, EngineK::TurboSettle) & 0x05)) settled = true;   // |d| < 0.15
                    else BoostPrev = Boost;
                }
            }
            float tn = A(dt, TurboTimer);
            TurboTimer = tn;
            Boost = nb;
            if (settled || (X87::Fcom(tn, EngineK::TurboMaxT) & 0x41) == 0) {   // or timer > 0.5
                TurboState = 0;
                BoostPrev = 0.0f;
            }
            return;
        }
        default:
            return;
    }
}

} // namespace bb

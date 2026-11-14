#include "drive/physics/engine/DriveTrain.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarState.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/engine/EngineLinks.h"
#include "drive/physics/wheel/Wheel.h"

#include <cmath>

namespace bb {

namespace {
inline float A(float x, float y) { return BWareMath::A(x, y); }
inline float S(float x, float y) { return BWareMath::S(x, y); }
inline float M(float x, float y) { return BWareMath::M(x, y); }
inline float D(float x, float y) { return BWareMath::D(x, y); }
} // namespace

float DriveTrain::Now(int worldTimer) { return EngineMath::FildMul(worldTimer, EngineK::TickToSec); }   // fild WorldTimer ; fmul 0.00025

// "fld split ; fcomp 0.001 ; test ah,1 ; jnz skip" -> rear axle driven
bool DriveTrain::RearDriven(float split) { return (X87::Fcom(split, EngineK::Split001) & X87::C0) == 0; }
// "fld split ; fcomp 0.999 ; test ah,41h ; jp skip" -> front axle driven
bool DriveTrain::FrontDriven(float split) { return !X87::ParityEven(X87::Fcom(split, EngineK::Split999) & 0x41); }

void DriveTrain::Construct(const DriveTrainInfo* info, const float* misc, bb::Engine* engine, Wheel* const* wheels,
                           bb::CarState* carState, bool shiftPointSearch, uint32_t& worldSeed, bool replayLocked) {
    MiscTunings = misc;
    Info = info;
    PerfType = 3;
    Engine = engine;
    for (int i = 0; i < 4; i++) Wheels[i] = wheels[i];
    CarState = carState;
    Gear = 1;
    TargetGear = 1;
    Clutch = 0.0f;
    ClutchState = 2;
    ShiftPending = 0;
    LastRpmDiff = 0.0f;
    RearSplit = info->RearTorqueSplit;
    ClutchMode = 0;
    ShiftLockTime = 0.0f;
    BonusTime = 0.0f;
    ShiftBonus = 0.0f;
    RpmAtShift = 0.0f;
    CalculateShiftPoints(info, engine->Info, ShiftUp, ShiftDown, shiftPointSearch);
    ChooseNewShiftModifiers(worldSeed, replayLocked);
    ShiftUp[1] = M(misc[4], ShiftUp[1]);
    ShiftModRange[0] = K.ShiftModRangeMin0;
    ShiftModRange[1] = EngineK::ModRangeMax;
    ShiftModRange[2] = EngineK::DefaultShiftUpMin;
    ShiftModRange[3] = EngineK::ModRangeMax;
    ShiftModRange[4] = EngineK::DefaultShiftUpMin;
    ShiftModRange[5] = EngineK::ModRangeMax;
    PerfectRangeMul = 1.0f;
    ClutchRateMul = 1.0f;
    ClutchPedal = false;
}

void DriveTrain::CalculateShiftPoints(const DriveTrainInfo* dti, const EngineInfo* ei, float* up, float* down, bool searchTorque) {
    // local Table {9, idle, max, 8/(max-idle), ei->torque}
    float inv = D(EngineK::Eight, S(ei->MaxRpm, ei->IdleRpm));
    up[1] = A(M(S(ei->RedlineRpm, ei->IdleRpm), dti->NeutralShiftFrac), ei->IdleRpm);
    int last = dti->NumGears + 1;
    int g = 2;
    for (; g < last; g++) {
        float r = dti->GearRatio[g], rNext = dti->GearRatio[g + 1];
        float u;
        if (searchTorque) {
            float x = M(A(ei->RedlineRpm, ei->IdleRpm), EngineK::Half);
            u = S(ei->RedlineRpm, EngineK::Hundred);
            if (!X87::ParityEven(X87::Fcom(x, ei->RedlineRpm) & 0x05))   // x < redline
            {
                float absR = std::fabs(r);
                while (true) {
                    float tCur = BWareMath::TableGetValue(9, ei->IdleRpm, inv, ei->Torque, x);
                    float tNext;
                    if ((X87::Fcom(absR, EngineK::Eps5) & 0x41) == 0) {
                        float rinv = D(1.0f, r);
                        tNext = BWareMath::TableGetValue(9, ei->IdleRpm, inv, ei->Torque, M(M(rinv, x), rNext));
                        tNext = M(M(tNext, rinv), rNext);
                    } else tNext = 0.0f;
                    if ((X87::Fcom(tNext, tCur) & 0x41) == 0) { u = x; break; }      // tNext > tCur
                    x = A(EngineK::Fifty, x);
                    if (X87::ParityEven(X87::Fcom(x, ei->RedlineRpm) & 0x05)) break;   // x >= redline
                }
            }
        } else u = S(ei->RedlineRpm, EngineK::Hundred);
        up[g] = u;
        if ((X87::Fcom(std::fabs(r), EngineK::Eps5) & 0x41) == 0)
            down[g + 1] = M(D(rNext, r), up[g]);
        else
            down[g + 1] = 0.0f;
    }
    up[g] = A(ei->MaxRpm, EngineK::FiveHundred);
}

void DriveTrain::ChooseNewShiftModifiers(uint32_t& worldSeed, bool replayLocked) {
    ShiftModifier[0] = 0.0f; ShiftModifier[1] = 0.0f; ShiftModifier[2] = 0.0f;
    if (replayLocked) return;   // pReplayManager->+0xA0 && [+0x41] != 0
    for (int i = 0; i < 3; i++)
        ShiftModifier[i] = A(World::BRandom(S(ShiftModRange[2 * i + 1], ShiftModRange[2 * i]), worldSeed), ShiftModRange[2 * i]);
}

float DriveTrain::GetDriveWheelSlippage() const {
    float x = 0.0f;
    float split = Info->RearTorqueSplit;
    if (RearDriven(split))
        x = M(M(A(Wheels[3]->Omega, Wheels[2]->Omega), EngineK::Half), Info->FinalDriveRear);
    if (FrontDriven(Info->RearTorqueSplit)) {
        x = A(M(M(A(Wheels[1]->Omega, Wheels[0]->Omega), EngineK::Half), Info->FinalDriveFront), x);
        if (RearDriven(Info->RearTorqueSplit)) x = M(x, EngineK::Half);
    }
    return M(x, Info->GearRatio[Gear]);
}

float DriveTrain::GetDifferentialAngularVelocity() const {
    float sum = 0.0f;
    int n = 0;
    if (RearDriven(Info->RearTorqueSplit)) { sum = A(Wheels[3]->SlipCopy, Wheels[2]->SlipCopy); n = 2; }
    if (FrontDriven(Info->RearTorqueSplit)) { n += 2; sum = A(A(Wheels[1]->SlipCopy, Wheels[0]->SlipCopy), sum); }
    return D(sum, static_cast<float>(n));
}

float DriveTrain::GetDriveWheelAngularAccel() const {
    float x = 0.0f;
    if (RearDriven(Info->RearTorqueSplit))
        x = M(M(A(WheelAccel(Wheels[3]), WheelAccel(Wheels[2])), EngineK::Half), Info->FinalDriveRear);
    if (FrontDriven(Info->RearTorqueSplit)) {
        x = A(M(M(A(WheelAccel(Wheels[1]), WheelAccel(Wheels[0])), EngineK::Half), Info->FinalDriveFront), x);
        if (RearDriven(Info->RearTorqueSplit)) x = M(x, EngineK::Half);
    }
    return M(x, Info->GearRatio[Gear]);
}

float DriveTrain::WheelAccel(const Wheel* w) { return D(S(w->Torque, M(w->Info->Radius, w->Fx)), w->Inertia); }

void DriveTrain::SetManualClutchEngaged(float c) {
    ClutchMode = 3;
    if ((X87::Fcom(c, EngineK::ClutchEngaged) & 0x41) == 0) { ClutchState = 3; Clutch = 1.0f; return; }     // c > 0.95
    if (!X87::ParityEven(X87::Fcom(c, EngineK::ClutchReleased) & 0x05)) { ClutchState = 1; Clutch = 0.0f; return; }  // c < 0.05
    if (!X87::ParityEven(X87::Fcom(c, 1.0f) & 0x44)) { ClutchState = 3; Clutch = c; return; }               // c == 1
    if (!X87::ParityEven(X87::Fcom(c, 0.0f) & 0x44)) { ClutchState = 1; Clutch = c; return; }               // c == 0
    ClutchState = (X87::Fcom(c, Clutch) & 0x41) == 0 ? 2 : 0;                                            // c > clutch
    Clutch = c;
}

float DriveTrain::GetDerivative() const {
    if (ClutchMode == 3) return 0.0f;
    float r = M(Info->ClutchRate, ClutchRateMul);
    if ((X87::Fcom(ShiftBonus, 0.0f) & 0x41) == 0) r = M(A(ShiftBonus, 1.0f), r);                      // bonus > 0
    else if (!X87::ParityEven(X87::Fcom(ShiftBonus, 0.0f) & 0x05) && TargetGear >= 3) r = M(r, EngineK::ShiftDownRate);
    return M(r, K.ClutchSpeed[ClutchState]);
}

bool DriveTrain::ShouldAutoDisengage() const {
    if (X87::ParityEven(X87::Fcom(Engine->Throttle, EngineK::AutoDisengageThrottle) & 0x05)) return false;   // throttle >= 0.3
    float idle = A(M(Engine->Info->IdleRpm, EngineK::RpmToRad), EngineK::IdleEps);
    return (X87::Fcom(idle, DriveOmega) & X87::C0) == 0;                                                // idle >= driveOmega
}

bool DriveTrain::IsClutchSynced() const {
    return !X87::ParityEven(X87::Fcom(std::fabs(S(Engine->Omega, DriveOmega)), K.ClutchSyncTolerance) & 0x41);
}

float DriveTrain::GetPerfectShiftRange(int gear) const { return M(K.PerfectShiftMaxRPMDiff[PerfType * 9 + gear], PerfectRangeMul); }

float DriveTrain::GetShiftUpRpm(int gear, bool alt, bool isDrag) const {
    float m = gear == 3 ? ShiftModifier[2] : ShiftModifier[0];
    if (alt) m = ShiftModRange[0];
    float x = A(m, ShiftUp[gear]);
    float limit = isDrag ? Engine->Info->MaxRpm : Engine->Info->RedlineRpm;
    return !X87::ParityEven(X87::Fcom(limit, x) & 0x05) ? limit : x;    // limit < x -> limit
}

float DriveTrain::GetShiftDownRpm(int gear, bool alt) const { return A(alt ? ShiftModRange[2] : ShiftModifier[1], ShiftDown[gear]); }

float DriveTrain::RateShift(float rpm, int gear, bool quantize) const {
    float d = S(ShiftUp[gear], rpm);
    float range = M(K.PerfectShiftMaxRPMDiff[PerfType * 9 + gear], PerfectRangeMul);
    if ((X87::Fcom(range, 0.0f) & 0x41) != 0) return 0.0f;                       // range <= 0 (or NaN)
    if ((X87::Fcom(d, 0.0f) & X87::C0) == 0)                                     // d >= 0
    {
        float q = S(1.0f, D(d, M(K.PerfectShiftSplit, range)));
        if ((X87::Fcom(0.0f, q) & 0x41) == 0) return 0.0f;                       // q < 0
        if ((X87::Fcom(q, 0.0f) & 0x41) != 0 || !quantize) return q;
        return X87::ParityEven(X87::Fcom(q, EngineK::Half) & 0x05) ? EngineK::GoodShift : EngineK::Half;
    }
    float lim = M(S(1.0f, K.PerfectShiftSplit), range);
    return (X87::Fcom(lim, std::fabs(d)) & 0x41) != 0 ? EngineK::MinusOne : 1.0f;   // lim <= |d| -> -1
}

void DriveTrain::RecordShiftQuality(float rpm, bool isDrag) {
    if (!isDrag) return;
    if (TargetGear < Gear) return;
    ShiftBonus = RateShift(rpm, Gear, true);
}

void DriveTrain::ApplyClutchTorque(float t) {
    if (Gear == 1) return;
    ClutchTorque = A(t, ClutchTorque);
    float k = M(static_cast<float>(CarState->WheelsOnGround), EngineK::Quarter);   // fild ; fmul 0.25 (small int, exact)
    if ((X87::Fcom(EngineK::Quarter, k) & 0x41) == 0) k = EngineK::Quarter;       // 0.25 > k
    Engine->LoadTorque = S(Engine->LoadTorque, M(k, t));
    float a = M(t, Info->GearRatio[Gear]);
    float front = M(M(S(1.0f, RearSplit), Info->FinalDriveFront), a);      // var_4
    float rear = M(M(Info->FinalDriveRear, RearSplit), a);                  // stays on the stack
    float bias = AxleBias(Wheels[0], Wheels[1]);
    if (Wheels[0]->BrakeLocked == 0) Wheels[0]->Torque = A(M(S(1.0f, bias), front), Wheels[0]->Torque);
    if (Wheels[1]->BrakeLocked == 0) Wheels[1]->Torque = A(M(bias, front), Wheels[1]->Torque);
    bias = AxleBias(Wheels[2], Wheels[3]);
    if (Wheels[2]->BrakeLocked == 0) Wheels[2]->Torque = A(M(bias, rear), Wheels[2]->Torque);
    if (Wheels[3]->BrakeLocked == 0) Wheels[3]->Torque = A(M(S(1.0f, bias), rear), Wheels[3]->Torque);
}

float DriveTrain::AxleBias(const Wheel* first, const Wheel* second) const {
    float l = first->OnGround != 0 && second->OnGround != 0 ? K.LimitedSlip : EngineK::Half;
    int mode = CarState->BurnoutCount;
    if (mode == 1) return S(1.0f, l);
    if (mode == 2) return l;
    // front: sum = w0 + w1, r = w1 / sum; rear: sum = w3 + w2, r = w2 / sum
    bool front = first == Wheels[0];
    float sum = front ? A(first->Omega, second->Omega) : A(second->Omega, first->Omega);
    if ((X87::Fcom(std::fabs(sum), EngineK::Eps5) & 0x41) != 0) return EngineK::Half;   // |sum| <= 1e-5
    float r = D(front ? second->Omega : first->Omega, sum);
    if (!X87::ParityEven(X87::Fcom(r, l) & 0x05)) return S(1.0f, M(K.LimitedSlip, EngineK::Half));   // r < l
    if ((X87::Fcom(r, S(1.0f, l)) & 0x41) == 0) return M(K.LimitedSlip, EngineK::Half);            // r > 1 - l
    return r;
}

void DriveTrain::GuessClutchTorque() {
    ClutchTorque = 0.0f;
    if (ClutchState == 3) { ApplyClutchTorque(Engine->GetTotalEngineTorque()); return; }
    if (ClutchState != 0 && ClutchState != 2) return;
    float d = S(Engine->Omega, DriveOmega);
    if ((X87::Fcom(d, K.ClutchSlipClamp) & 0x41) == 0) d = K.ClutchSlipClamp;
    else {
        float lo = -K.ClutchSlipClamp;
        if (!X87::ParityEven(X87::Fcom(d, lo) & 0x05)) d = lo;
    }
    float k = Info->ClutchStiffness;
    float rd = M(S(DriveOmega, Engine->Omega), EngineK::RadToRpm);
    if (X87::ParityEven(X87::Fcom(LastRpmDiff, 0.0f) & 0x44)                     // last != 0 ("jnp" = equal skips)
        && !X87::ParityEven(X87::Fcom(M(rd, LastRpmDiff), 0.0f) & 0x05)         // rd * last < 0
        && (X87::Fcom(std::fabs(rd), EngineK::RpmDiffBig) & 0x41) == 0
        && (X87::Fcom(std::fabs(LastRpmDiff), EngineK::RpmDiffBig) & 0x41) == 0)
        k = M(k, EngineK::HalfK);
    LastRpmDiff = rd;
    ApplyClutchTorque(M(M(k, Clutch), d));
}

void DriveTrain::ModifyClutchTorque() {
    if (ClutchState != 3) return;
    float a = A(Engine->GetTotalEngineTorque(), Engine->LoadTorque);
    float inertia = Engine->Info->Inertia;
    int cs = Engine->DriveTrain->ClutchState;
    if (cs == 2 || cs == 0) inertia = M(inertia, EngineK::InertiaMul);
    float aE = D(a, inertia);
    float num = S(aE, GetDriveWheelAngularAccel());
    float den = D(1.0f, Engine->Info->Inertia);
    if (FrontDriven(Info->RearTorqueSplit)) {
        float g = M(Info->GearRatio[Gear], Info->FinalDriveFront);
        den = A(den, M(M(M(D(S(1.0f, RearSplit), Wheels[0]->Inertia), g), g), EngineK::Half));
    }
    if (RearDriven(Info->RearTorqueSplit)) {
        float g = M(Info->GearRatio[Gear], Info->FinalDriveRear);
        den = A(den, M(M(M(D(RearSplit, Wheels[2]->Inertia), g), g), EngineK::Half));
    }
    ApplyClutchTorque(D(num, den));
}

void DriveTrain::FinalizeClutchTorque() {
    if (Gear != 1 && ClutchState == 3 && CarState->WheelsOnGround > 0) {
        float slip = S(GetDriveWheelSlippage(), Engine->Omega);
        if ((X87::Fcom(Engine->Throttle, EngineK::Point2) & 0x41) == 0
            && (X87::Fcom(GetDifferentialAngularVelocity(), EngineK::Point1) & 0x41) == 0) {
            float w = Engine->Omega;
            DriveOmega = w;
            SetDifferentialAngularVelocity(w);
        } else {
            float t = std::fabs(Engine->GetTotalEngineTorque());
            Engine->LoadTorque = A(BWareMath2::Clamp(M(slip, Info->ClutchStiffness), -t, t), Engine->LoadTorque);
        }
    }
    if (Gear == 2) {
        float t = Engine->GetTotalEngineTorque();
        float l = Engine->LoadTorque;
        float x = A(l, t);
        if (!X87::ParityEven(X87::Fcom(l, t) & 0x05) && !X87::ParityEven(X87::Fcom(x, 0.0f) & 0x05))   // l < t && x < 0
        {
            float g = 0.0f;   // the C# out parameter: only meaningful when the graph writes it
            BWareMath2::GraphGetValueTo(K.ClutchPlay.Points.data(), K.ClutchPlay.Count, x, g);
            Engine->LoadTorque = A(M(M(g, K.ClutchPlayFactor[PerfType]), x), Engine->LoadTorque);
        }
    }
}

void DriveTrain::SetDifferentialAngularVelocity(float w) {
    float delta = S(w, GetDriveWheelSlippage());
    float mph = M(CarState->Speed, EngineK::MsToMph);
    bool avg = !X87::ParityEven(X87::Fcom(mph, EngineK::Mph40) & 0x05);        // mph < 40
    if (RearDriven(Info->RearTorqueSplit)) {
        if (Wheels[2]->OnGround == 0 && Wheels[3]->OnGround == 0) avg = true;
        float dw = D(delta, M(Info->GearRatio[Gear], Info->FinalDriveRear));
        float w2 = Wheels[2]->Omega, w3 = Wheels[3]->Omega;
        float a = w2, b = w3;
        if (avg) { a = M(A(w3, w2), EngineK::Half); b = a; }
        Wheels[2]->Omega = A(a, dw);
        Wheels[3]->Omega = A(b, dw);
    }
    avg = !X87::ParityEven(X87::Fcom(mph, EngineK::Mph40) & 0x05);
    if (FrontDriven(Info->RearTorqueSplit)) {
        if (Wheels[0]->OnGround == 0 && Wheels[1]->OnGround == 0) avg = true;
        float dw = D(delta, M(Info->GearRatio[Gear], Info->FinalDriveFront));
        float w0 = Wheels[0]->Omega, w1 = Wheels[1]->Omega;
        float a = w0, b = w1;
        if (avg) { a = M(A(w1, w0), EngineK::Half); b = a; }
        Wheels[0]->Omega = A(a, dw);
        Wheels[1]->Omega = A(b, dw);
    }
}

void DriveTrain::ComputeAuxiliaries(int worldTimer, uint32_t& worldSeed, bool replayLocked) {
    DriveOmega = GetDriveWheelSlippage();
    if (ClutchMode == 3) {
        Engine->ThrottleGate = 1.0f;
        if (ClutchState == 3) {
            RpmAtShift = M(Engine->Omega, EngineK::RadToRpm);
            // fcomp qword 0.0 (dbl_786E60): exact for a float operand
            if ((X87::Fcom(ShiftBonus, 0.0f) & 0x41) == 0 && !X87::ParityEven(X87::Fcom(BonusTime, 0.0f) & 0x44))
                BonusTime = Now(worldTimer);
        } else if (ClutchState == 1 && ShiftPending != 0) {
            ShiftPending = 0;
            Gear = TargetGear;
        }
    } else {
        switch (ClutchState) {
            case 3: {
                if ((X87::Fcom(ShiftBonus, 0.0f) & 0x41) == 0 && !X87::ParityEven(X87::Fcom(BonusTime, 0.0f) & 0x44))
                    BonusTime = Now(worldTimer);
                Engine->ThrottleGate = 1.0f;
                int g = Gear;
                if (g != 1 && ShouldAutoDisengage()) ClutchState = 0;
                if (g != TargetGear) {
                    ShiftPending = 1;
                    RpmAtShift = M(Engine->Omega, EngineK::RadToRpm);
                    ClutchState = 0;
                } else ShiftPending = 0;
                if (ClutchPedal) ClutchState = 1;
                break;
            }
            case 2:
                Engine->ThrottleGate = ClutchPedal ? 1.0f : EngineK::EngagingGate;
                if ((X87::Fcom(Clutch, 1.0f) & X87::C0) == 0)                        // clutch >= 1
                {
                    Clutch = 1.0f;
                    if (IsClutchSynced() || Gear == 1) ClutchState = 3;
                }
                if (ShouldAutoDisengage() && Gear != 1) ClutchState = 0;
                if (ClutchPedal) ClutchState = 1;
                break;
            case 0:
                Engine->ThrottleGate = ClutchPedal ? 1.0f : 0.0f;
                if (!X87::ParityEven(X87::Fcom(Clutch, 0.0f) & 0x41)) { Clutch = 0.0f; ClutchState = 1; }   // clutch <= 0
                if (ClutchPedal) ClutchState = 1;
                break;
            case 1:
                Engine->ThrottleGate = ClutchPedal ? 1.0f : 0.0f;
                Gear = TargetGear;
                if (ShiftPending != 0 || !ShouldAutoDisengage()) {
                    if (!ClutchPedal) ClutchState = 2;
                    ChooseNewShiftModifiers(worldSeed, replayLocked);
                }
                break;
            default:
                break;
        }
    }
    if (X87::ParityEven(X87::Fcom(ShiftLockTime, 0.0f) & 0x44))                    // shiftLockTime != 0
    {
        Engine->ThrottleGate = 0.0f;
        if ((X87::Fcom(S(Now(worldTimer), ShiftLockTime), EngineK::ShiftLockSec) & X87::C0) == 0) ShiftLockTime = 0.0f;
        return;
    }
    if (ClutchState != 3) return;
    if ((X87::Fcom(BonusTime, 0.0f) & 0x41) != 0) return;                          // bonusTime <= 0
    float b = M(M(D(S(EngineK::BonusSec, S(Now(worldTimer), BonusTime)), EngineK::BonusSec), ShiftBonus), EngineK::BonusGain);
    if ((X87::Fcom(b, 0.0f) & 0x41) == 0) Engine->ThrottleGate = A(b, 1.0f);
    if ((X87::Fcom(S(Now(worldTimer), BonusTime), EngineK::BonusSec) & X87::C0) == 0) { BonusTime = 0.0f; ShiftBonus = 0.0f; }
}

int DriveTrain::UpdateGearSelection(int request, float rpm, int worldTimer, float worldTimeElapsed, bool isDrag) {
    if ((X87::Fcom(rpm, EngineK::MinusOne) & 0x41) != 0) rpm = M(Engine->Omega, EngineK::RadToRpm);   // rpm <= -1
    int ret = request;
    if (request == -1) {
        int g = Gear;
        LastRequest = 1;
        if (g <= 1) TargetGear = 2;
        else if (ClutchState == 3) {
            float up = GetShiftUpRpm(g, false, isDrag);
            float down = A(ShiftModifier[1], ShiftDown[g]);
            float alt = 0.0f;
            if (g != 2)
                alt = S(M(GetShiftUpRpm(g - 1, true, isDrag), D(Info->GearRatio[g], Info->GearRatio[g - 1])), EngineK::TwoHundred);
            if (g <= 2)
                down = X87::ParityEven(X87::Fcom(Engine->Throttle, EngineK::Point2) & 0x05) ? 0.0f : A(Engine->Info->IdleRpm, EngineK::Hundred);
            bool shiftedUp = false;
            if ((X87::Fcom(rpm, up) & X87::C0) == 0 && g < Info->NumGears + 1) {
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    const Wheel* w = Wheels[i];
                    bool good = w->OnGround != 0 && !X87::ParityEven(X87::Fcom(w->SlipVel, EngineK::SlipLimit) & 0x05);
                    ok &= good;
                }
                if (ok) TargetGear = g + 1;
                shiftedUp = true;
            }
            if (!shiftedUp
                && !X87::ParityEven(X87::Fcom(rpm, down) & 0x41)                 // rpm <= down
                && !X87::ParityEven(X87::Fcom(rpm, alt) & 0x41))                 // rpm <= alt
                TargetGear = g - 1;
        }
        if (ClutchState == 3 && TargetGear > g && g >= 1) RecordShiftQuality(rpm, isDrag);
        return ret;
    }
    if (request == -2) {
        float t = A(worldTimeElapsed, AutoTimer);
        AutoTimer = t;
        if (ClutchState != 3) return ret;
        float idle = Engine->Info->IdleRpm, max = Engine->Info->MaxRpm;
        int g = Gear;
        float x;
        if (g > 2) x = M(M(D(Info->GearRatio[g], Info->GearRatio[g - 1]), K.AIShiftRatio), max);
        else x = X87::ParityEven(X87::Fcom(Engine->Throttle, EngineK::Point2) & 0x05) ? 0.0f : A(idle, EngineK::Hundred);
        if (LastRequest != TargetGear) {
            GearAtRequest = g;
            if (LastRequest < TargetGear) {
                AutoTimer = 0.0f;
                if ((X87::Fcom(rpm, x) & 0x41) != 0) { TargetGear = g - 1; return ret; }   // rpm <= x
                LastRequest = g;
                TargetGear = g;
                return ret;
            }
            TargetGear = g + 1;
            AutoTimer = 0.0f;
            return ret;
        }
        if ((X87::Fcom(t, EngineK::TenSec) & 0x41) == 0) return -1;                       // timer > 10
        if (g > GearAtRequest && !X87::ParityEven(X87::Fcom(S(max, EngineK::ThreeHundred), rpm) & 0x05))
            return -1;                                                                   // max - 300 < rpm
        if ((X87::Fcom(A(idle, EngineK::Hundred), rpm) & 0x41) != 0) return ret;         // idle + 100 <= rpm
        if (g <= 2) return ret;
        TargetGear = g - 1;
        return -1;
    }
    if (ClutchMode == 3 && request != Gear) {
        if (ClutchState != 1) {
            ShiftPending = 0;
            ShiftLockTime = Now(worldTimer);
            return Gear;
        }
        if (ShiftPending != 0) return ret;
        ShiftPending = 1;
        TargetGear = request;
        if (request > Gear && Gear >= 1) RecordShiftQuality(RpmAtShift, isDrag);
        return ret;
    }
    if (request == Gear || request == TargetGear) return ret;
    TargetGear = request;
    if (request > Gear && Gear >= 1) RecordShiftQuality(rpm, isDrag);
    return ret;
}

} // namespace bb

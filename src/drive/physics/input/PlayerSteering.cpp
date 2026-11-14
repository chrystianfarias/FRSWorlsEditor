#include "drive/physics/input/PlayerSteering.h"

#include "drive/physics/input/RealDriver.h"
#include "drive/physics/timing/TimingFpu.h"

#include <cmath>
#include <stdexcept>

namespace bb {

using namespace TimingFpu;

namespace {
const float K_AngToDeg = X87::Bits(0x3BB40000u);            // flt_784248 360/65536
const float K_180 = X87::Bits(0x43340000u);                  // flt_784510 180.0
const float K_360 = X87::Bits(0x43B40000u);                  // "WorldAnimFrameCount" 0x78450C = 360.0
const float K_Minus45 = X87::Bits(0xC2340000u);              // flt_7844DC -45.0
const float K_45 = X87::Bits(0x42340000u);                   // flt_7844D4 45.0
const float K_SnapToZero = X87::Bits(0x00000000u);           // flt_783AF4 0.0 (|angle| below -> 0)
const float K_65536 = X87::Bits(0x47800000u);                // flt_784238 65536.0
const float K_PedalGain = X87::Bits(0x3F800000u);            // flt_783AE0 1.0
const float K_RecoveryTime = X87::Bits(0x3F800000u);         // flt_783AF8 1.0 s after a collision
const float K_0_7 = X87::Bits(0x3F333333u);                  // flt_7844D8 0.7
const float K_RadToDeg = X87::Bits(0x42652EE0u);             // flt_78424C 57.295776
const float K_YawOn = X87::Bits(0x41700000u);                // flt_7E9130 15.0 (.data) assist on above (deg/s)
const float K_YawOff = X87::Bits(0x40A00000u);               // flt_7E9134 5.0 (.data) assist off below
const float K_SpeedOn = X87::Bits(0x415692B6u);              // flt_7E9138 13.410818 (.data) = 30 mph
const float K_SpeedOff = X87::Bits(0x00000000u);             // flt_81CB90 0.0 (.data)
const float K_RelaxGate = X87::Bits(0x3F000000u);            // flt_783AEC 0.5
const float K_RelaxRate = X87::Bits(0x00000000u);            // flt_783AF0 0.0 (limit relax per s)
const float K_RateNormal = X87::Bits(0x43340000u);           // dword_783AE4 180.0 (rate scale)
const float K_RateSpecial = X87::Bits(0x42DC0000u);          // dword_783AE8 110.0
const float K_CurveWindow = X87::Bits(0x3F0CCCCDu);          // dword_783AD8 0.55 s
const float K_RateWindow = X87::Bits(0x3E19999Au);           // dword_783ADC 0.15 s
const float K_SampleRate = X87::Bits(0x42F00000u);           // dword_783AD4 120 samples/s

// WorldTimer * 0.00025 (fild WorldTimer ; fmul flt_784268).
float Now(const InputContext& ctx) { return FildMul(ctx.WorldTimer, InputConstants::K_1_4000); }
} // namespace

PlayerSteering::PlayerSteering()
    : CurveAverage(K_CurveWindow, K_SampleRate), RateAverage(K_RateWindow, K_SampleRate) {}

float PlayerSteering::SteerLimit(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx, const InputTables& tables) {
    const RaceModeFlags& fl = ctx.Flags;
    float spd = car.SpeedLong;
    float a = tables.SpeedLimit.GetValue(spd);                       // var_14
    float b = tables.SpeedFactor.GetValue(spd);
    float x = A(drv.Throttle, InputConstants::K_One);                // thr + 1
    float y = M(A(drv.EBrake, drv.Brake), InputConstants::K_Half);   // (ebrake + brake) * 0.5
    x = S(x, y);
    x = M(x, InputConstants::K_Half);
    x = S(InputConstants::K_One, x);                                 // fsubr 1.0
    float lim = M(A(M(M(b, x), K_PedalGain), InputConstants::K_One), a);
    lim = M(tables.CurveAverageGain.GetValue(std::fabs(CurveAverage.Average)), lim);
    if (!fl.DriftRace()) {
        int ts = car.LastCollisionTime;
        if (ts != 0 && ts != 0x7FFFFFFF) {
            float since = FildMul(static_cast<int>(static_cast<uint32_t>(ctx.WorldTimer) - static_cast<uint32_t>(ts)), InputConstants::K_1_4000);
            if (!Jp5(since, K_RecoveryTime)) {                       // since < 1 s
                float r = D(spd, M(car.TopSpeed, K_0_7));             // fdivr: speed / (top * 0.7)
                float v = Jp5(InputConstants::K_One, r) ? r : InputConstants::K_One;
                float g = tables.CollisionRecovery.GetValue(since).Store();
                float k = A(M(S(InputConstants::K_One, v), S(InputConstants::K_One, g)), g);
                lim = M(k, lim);
            }
        }
    }
    float yaw = M(car.Yaw, K_RadToDeg);
    float absYaw = std::fabs(yaw);
    float vel = std::sqrt(A(A(M(car.VelX, car.VelX), M(car.VelY, car.VelY)), M(car.VelZ, car.VelZ)));
    bool counter = !Jp5(M(input, yaw), InputConstants::K_Zero);      // input * yaw < 0: steering against the yaw
    if (CounterSteerActive) {
        if (Jnp5(absYaw, K_YawOff) || !Jp5(vel, K_SpeedOff)) CounterSteerActive = false;
    } else {
        if (!Jnz41(absYaw, K_YawOn) && !Jnz1(vel, K_SpeedOn)) CounterSteerActive = true;
    }
    bool relax = true;
    if (counter && (CounterSteerActive || !Jnz41(absYaw, lim))) {
        const InputTable& tbl = (!fl.Drag() && fl.DriftPhysics()) ? tables.CounterSteerDrift : tables.CounterSteer;
        float t = tbl.GetValue(std::fabs(input));
        lim = M(absYaw, t);
        float cap = M(t, K_45);
        if (!Jnz41(lim, cap)) lim = cap;
        relax = false;
    }
    if (relax && !Jnz1(std::fabs(CurveAverage.Average), K_RelaxGate)) {
        float floor = S(Limit, M(K_RelaxRate, dt));                 // limit decays at K_RelaxRate per second
        if (!Jz41(lim, floor)) lim = floor;
    }
    if (!Jnp5(lim, K_45)) lim = K_45;
    Limit = lim;
    return lim;
}

float PlayerSteering::SteerRate(float input, float dt, CarInputView& car, InputContext& ctx, const InputTables& tables) {
    float d = D(S(input, LastCurve), dt);                             // (in - last) / dt
    float now = Now(ctx);
    RateAverage.Record(tables.RateInput.GetValue(std::fabs(d)), now);
    float k = ctx.Flags.SpecialSteer() ? K_RateSpecial : K_RateNormal;
    float b = M(tables.SpeedFactor.GetValue(car.SpeedLong), k);
    float r = M(RateAverage.Average, b);
    return M(tables.RateCurveAverage.GetValue(std::fabs(CurveAverage.Average)), r);
}

float PlayerSteering::FilterPad(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx, const InputTables& tables) {
    const RaceModeFlags& fl = ctx.Flags;
    float curve = (fl.SpecialSteer() ? tables.SteerResponseSpecial : tables.SteerResponse).GetValue(input);
    float cur = M(static_cast<float>(Angle), K_AngToDeg);             // fild u16 ; fmul 360/65536
    if (!Jnz1(cur, K_180)) cur = S(cur, K_360);                       // -> [-180, 180)
    float maxDeg = car.SteerMaxDeg;                                   // cpi+0x270
    if (!fl.Drag() && fl.DriftPhysics() && !Jnz41(M(curve, car.Car128), InputConstants::K_Zero)) {
        float f = 0.0f;                                               // uninitialised stack slot in the original if no segment matches
        tables.DriftYaw.GetValueTo(f, std::fabs(car.Car128));
        maxDeg = M(f, maxDeg);
    }
    // 0x416C1C stored the curve over arg_0: sub_405DC0 / sub_4060E0 receive the curve value, not the raw input
    float target = M(M(SteerLimit(curve, dt, car, drv, ctx, tables), maxDeg), curve);
    if (Jnz41(target, K_Minus45)) target = K_Minus45;
    else if (!Jp5(K_45, target)) target = K_45;
    float rate = M(M(SteerRate(curve, dt, car, ctx, tables), maxDeg), dt);
    float hi = A(cur, rate);
    float st = S(cur, rate);                                          // fsubr var_C
    if (!Jnz41(target, st)) st = target;                              // max(target, lo)
    if (!Jp5(hi, st)) st = hi;                                        // min(.., hi)
    float outDeg = Jp5(std::fabs(st), K_SnapToZero) ? st : InputConstants::K_Zero;
    int q = Ftol(M(outDeg, K_65536));
    Angle = static_cast<uint16_t>(q / 360);                           // magic 0xB60B60B7: signed division, truncation
    return curve;
}

uint16_t PlayerSteering::DoSteering(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx,
                                    const InputTables& tables, bool wheelConnected) {
    if (wheelConnected) throw std::runtime_error("PlayerSteering wheel filter sub_4061C0 is not ported");
    float curve = FilterPad(input, dt, car, drv, ctx, tables);
    CurveAverage.Record(curve, Now(ctx));
    LastCurve = curve;
    return Angle;
}

} // namespace bb

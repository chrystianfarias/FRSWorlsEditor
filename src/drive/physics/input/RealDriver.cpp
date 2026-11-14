#include "drive/physics/input/RealDriver.h"

#include "drive/physics/timing/TimingFpu.h"

#include <cmath>

namespace bb {

using namespace TimingFpu;

RealDriver::RealDriver() {
    GearCommand = 1;
    ForceFullBrake = true;
}

void RealDriver::ResetJoyValues() {
    SteerLeft = SteerRight = 0x80;
    ThrottleDigital = ThrottleAnalog = ThrottleAnalog2 = 0;
    BrakeDigital = BrakeAnalog = BrakeAnalog2 = 0;
    ReverseDigital = ReverseAnalog = 0;
    EBrakeDigital = EBrakeAnalog = 0;
    SteerInput = 0.0f; ThrottleInput = 0.0f; BrakeInput = 0.0f; ReverseInput = 0.0f; EBrakeInput = 0.0f;
    IgnorePedals = false;
}

// ---------------------------------------------------------------------------------------- joystick handlers

void RealDriver::JoyHandlerSteer(int joyEvent, uint8_t value) {
    if (joyEvent == 0x8F) { SteerLeft = value; SteerRight = 0x80; }
    else if (joyEvent == 0x90) { SteerRight = value; SteerLeft = 0x80; }
    uint8_t v = SteerLeft;
    if (v == 0x80) v = SteerRight;
    if (v == 0xFF) { SteerInput = X87::Bits(0xBF800000u); return; }
    SteerInput = M(S(static_cast<float>(v), InputConstants::K_128), InputConstants::K_SteerScale);
}

void RealDriver::JoyHandlerThrottle(int joyEvent, uint8_t value, CarInputView& car, const InputTables& tables) {
    if (IgnorePedals) {
        ThrottleDigital = ThrottleAnalog = ThrottleAnalog2 = 0;
        ThrottleInput = 0.0f;
        return;
    }
    bool alt = false;                                                 // bl: set only when _return_0(port) != 0 (never on PC)
    if (joyEvent == 0x91) ThrottleDigital = value;
    else if (joyEvent == 0x92) {
        float x = static_cast<float>(value);                          // _return_0(port) == 0: no x2/x4 scaling
        if (!Jp5(InputConstants::K_255, x)) x = InputConstants::K_255;
        ThrottleAnalog = static_cast<uint8_t>(Ftol(x));
    } else if (joyEvent == 0x93) {
        float x = static_cast<float>(value);
        if (car.Gear != 1) x = M(x, InputConstants::K_1_07);
        if (!Jp5(InputConstants::K_255, x)) x = InputConstants::K_255;
        ThrottleAnalog2 = static_cast<uint8_t>(Ftol(x));
    }
    uint8_t v = ThrottleDigital;
    if (v == 0) v = ThrottleAnalog > ThrottleAnalog2 ? ThrottleAnalog : ThrottleAnalog2;
    float f = M(static_cast<float>(v), InputConstants::K_1_255);
    ThrottleInput = f;
    if (car.Gear == 1) {
        (alt ? tables.NeutralThrottleAlt : tables.NeutralThrottle).GetValueTo(f, f);   // sub_40C840
        ThrottleInput = f;
    }
}

void RealDriver::JoyHandlerBrake(int joyEvent, uint8_t value) {
    if (IgnorePedals) { BrakeInput = 0.0f; return; }
    if (joyEvent == 0x94) BrakeDigital = value;
    else if (joyEvent == 0x95) BrakeAnalog = value;
    else if (joyEvent == 0x96) BrakeAnalog2 = value;
    uint8_t v = BrakeDigital;
    if (v == 0) v = BrakeAnalog > BrakeAnalog2 ? BrakeAnalog : BrakeAnalog2;
    BrakeInput = M(static_cast<float>(v), InputConstants::K_1_255);
}

void RealDriver::JoyHandlerReverse(int joyEvent, uint8_t value, CarInputView& car) {
    if (IgnorePedals) { ReverseInput = 0.0f; return; }
    if (joyEvent == 0x97) ReverseDigital = value;
    else if (joyEvent == 0x98) {
        uint32_t v = value;
        if (car.Gear == 1) {
            float x = M(static_cast<float>(v), InputConstants::K_1_255);
            v = static_cast<uint32_t>(Ftol(M(M(M(x, x), x), InputConstants::K_255)));
        }
        ReverseAnalog = v > 0xFF ? static_cast<uint8_t>(0xFF) : static_cast<uint8_t>(v);
    }
    uint8_t b = ReverseDigital;
    if (b == 0) b = ReverseAnalog;
    ReverseInput = M(static_cast<float>(b), InputConstants::K_1_255);
}

void RealDriver::JoyHandlerEBrake(int joyEvent, uint8_t value) {
    if (joyEvent == 0x99) EBrakeDigital = value;
    else if (joyEvent == 0x9A) {
        float x = static_cast<float>(value);
        x = A(x, x);                                                  // fadd st, st
        if (!Jp5(InputConstants::K_255, x)) x = InputConstants::K_255;
        EBrakeAnalog = static_cast<uint8_t>(Ftol(x));
    }
    uint8_t b = EBrakeDigital;
    if (b == 0) b = EBrakeAnalog;
    EBrakeInput = M(static_cast<float>(b), InputConstants::K_1_255);
}

void RealDriver::JoyHandlerDisengageClutch(int joyEvent, uint8_t value) {
    if (ShiftMode != 3) return;
    if (joyEvent == 0x9F) { ManualClutch = value != 0 ? 0.0f : InputConstants::K_One; return; }
    if (joyEvent != 0xA0) return;
    float x = M(static_cast<float>(value), InputConstants::K_1_07);
    if (!Jnz41(x, InputConstants::K_255)) x = InputConstants::K_255;  // fcom 255 ; > 255 -> 255
    ManualClutch = S(InputConstants::K_One, M(x, InputConstants::K_1_255));
}

void RealDriver::JoyHandlerShiftUp(uint8_t value, CarInputView& car) {
    if (value == 0) return;
    int top = car.NumForwardGears + 1;
    switch (ShiftMode) {
    case 3:
        if (GearCommand == 0) return;
        { int g = car.Gear; if (g < top) g++; GearCommand = g; }
        return;
    case 1:
        if (GearCommand == 0 || GearCommand >= top) return;
        GearCommand++;
        return;
    case 2:
        if (car.MovementMode == 3) return;
        if (GearCommand != -2) {
            GearCommand = -2;
            int g = car.Gear; if (g < top) g++;
            CurrentGear = g;
        } else CurrentGear++;
        return;
    default:
        return;                                                       // mode 0: nullsub_15 (auto)
    }
}

void RealDriver::JoyHandlerShiftDown(uint8_t value, CarInputView& car) {
    if (value == 0) return;
    switch (ShiftMode) {
    case 3:
        if (GearCommand == 0) return;
        { int g = car.Gear; if (g >= 2) g--; GearCommand = g; }
        return;
    case 1:
        if (GearCommand == 0 || GearCommand < 2) return;
        GearCommand--;
        return;
    case 2:
        if (car.MovementMode == 3) return;
        if (GearCommand != -2) {
            GearCommand = -2;
            int g = car.Gear; if (g > 2) g--;
            CurrentGear = g;
        } else CurrentGear--;
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------------------------------- per-frame logic

void RealDriver::RequestForwardGear(InputContext& ctx) {
    if (ShiftMode != 1 && ShiftMode != 3) { GearCommand = -1; return; }
    GearCommand = 2;
    if (ctx.Flags.Drag())
        DragLaunchTimer = Ftol(A(M(InputConstants::K_DragLaunch, InputConstants::K_4000), InputConstants::K_Half));
}

bool RealDriver::CountDown(int& timer, float dt) {
    int t = timer;
    if (t == 0) return false;
    if (t != 0x7FFFFFFF) {
        t -= Ftol(M(dt, InputConstants::K_4000));
        timer = t;
        if (t < 0) { timer = 0; return false; }
    }
    t = timer;
    return t != 0 && t != 0x7FFFFFFF;
}

void RealDriver::DoThrottleBrakeReverse(float dt, CarInputView& car, InputContext& ctx, const InputTables& tables) {
    const RaceModeFlags& fl = ctx.Flags;
    float limit = InputConstants::K_Half;                             // var_4
    float speed = car.SpeedLong;                                      // var_C
    if (fl.Burnout())
        speed = D(speed, A(M(std::fabs(car.Car118), InputConstants::K_Four), InputConstants::K_One)); // fdivr
    float thrOut;                                                     // var_8
    float t = S(EBrakeTimer, dt);
    EBrakeTimer = t;
    if (!Jnz41(t, InputConstants::K_Zero) && !Jnz41(std::fabs(speed), InputConstants::K_20Mph)) {
        thrOut = 0.0f;
        EBrake = InputConstants::K_One;                               // forced handbrake above 20 mph
    } else {
        EBrakeTimer = 0.0f;
        thrOut = ThrottleInput;
        EBrake = EBrakeInput;
    }
    // reverse latch
    if (!Jnz41(BrakeInput, InputConstants::K_0_6) && !Jp5(ThrottleInput, InputConstants::K_Half))
        ReverseLatch = true;
    else if (!Jnz41(speed, InputConstants::K_Minus2))
        ReverseLatch = false;
    float rev, brakeOut;                                              // st0 , var_4
    if (Jz41(speed, limit) || !ReverseLatch) {
        rev = InputConstants::K_Zero; brakeOut = BrakeInput;          // 0x407BA4 (var_C = thrOut)
        speed = thrOut;
    } else if (Jnp5(speed, InputConstants::K_Minus30) || Jnp5(speed, InputConstants::K_Minus2)) {
        rev = BrakeInput; speed = 0.0f; brakeOut = thrOut;            // 0x407B8C: reversing, throttle pedal brakes
    } else {
        rev = BrakeInput; speed = thrOut; brakeOut = 0.0f;            // 0x407B74: slow, brake pedal = reverse throttle
    }
    float throttle = speed;                                           // var_C from here on
    rev = A(rev, rev);                                                // fadd st, st
    float revOut = rev;                                               // var_8
    if (!Jnz41(rev, InputConstants::K_One)) revOut = InputConstants::K_One;
    Brake = brakeOut;
    if (!Jnz41(EBrake, InputConstants::K_Half)) {
        int k = static_cast<int>(static_cast<uint32_t>(ctx.WorldLoopCounter) + 1u) % 8;   // and 80000007h with sign fixup = C remainder
        if (k == 0 && !Jnz41(car.Car430, InputConstants::K_Ten))
            if (ctx.ReplayEvent) ctx.ReplayEvent(InputConstants::K_EBrakeEventWeight, "EBrake");
    }
    if (fl.Drag() && CountDown(DragLaunchTimer, dt)) {
        float x = M(InputConstants::K_Two, throttle);
        if (!Jnz41(x, InputConstants::K_0_7))
            throttle = Jp5(InputConstants::K_One, x) ? x : InputConstants::K_One;
        else throttle = InputConstants::K_0_7;
    }
    if (!Jp5(A(throttle, InputConstants::K_0_2), revOut)) {
        Throttle = revOut;                                            // reverse: brake pedal drives backwards
        GearCommand = 0;
    } else {
        if (fl.Burnout()) throttle = tables.BurnoutThrottle.GetValue(throttle);
        bool rated = (fl.Drag() && car.Gear == 1) || fl.Burnout();
        float outT;
        bool clampLow = true;
        if (rated) {
            float prev = Throttle;
            float hi = A(M(InputConstants::K_RiseRate, dt), prev);   // fld 1.0 ; fmul dt ; fadd st, st1
            float lo = S(prev, M(InputConstants::K_FallRate, dt));  // fsubp st(1), st
            float st = lo;
            if (!Jnz41(throttle, lo)) st = throttle;
            if (!Jp5(hi, st)) st = hi;
            throttle = st;                                            // fst var_C
            if (!Jnz41(ThrottleInput, InputConstants::K_0_6)) {
                float rpm = Jnp44(ctx.TestRPM, InputConstants::K_100) ? car.Rpm : ctx.TestRPM;
                float idle = A(car.IdleRpm, InputConstants::K_1000);
                if (!Jp5(rpm, idle) && !Jnz41(InputConstants::K_0_6, st)) {
                    throttle = InputConstants::K_0_6;                 // launch anti-stall: hold 0.6 near idle
                    clampLow = false;                                 // jumps to 0x407E17 (upper clamp only)
                }
            }
        }
        if (clampLow && Jnz41(throttle, InputConstants::K_Zero)) outT = InputConstants::K_Zero;
        else outT = Jp5(InputConstants::K_One, throttle) ? throttle : InputConstants::K_One;
        Throttle = outT;
        if (GearCommand == 0) RequestForwardGear(ctx);                // vtbl+0x30
    }
    if (ctx.Online && ctx.RaceCoordinatorState == 6) {
        Throttle = 0.0f; Brake = InputConstants::K_Half;
    }
    car.ThrottleTelemetry = Throttle;
}

void RealDriver::Drive(float dt, CarInputView& car, InputContext& ctx, const InputTables& tables,
                       bool joystickPluggedIn, bool wheelConnected) {
    if (Port == -1) return;
    if (!joystickPluggedIn) ResetJoyValues();
    if (ForceStop != 0) { ThrottleInput = 0.0f; BrakeInput = InputConstants::K_One; }
    DoThrottleBrakeReverse(dt, car, ctx, tables);
    if (SteerEnabled) {
        float steerIn = IgnorePedals ? 0.0f : SteerInput;
        SteerAngle = Steering.DoSteering(steerIn, dt, car, *this, ctx, tables, wheelConnected);
    }
    if (ForceFullBrake) { GearCommand = 1; Brake = InputConstants::K_One; }
}

} // namespace bb

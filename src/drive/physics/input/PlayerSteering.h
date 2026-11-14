#pragma once

// Port of NFSU2Unity Core/Physics/Input/PlayerSteering.cs.

#include "drive/physics/input/InputTables.h"

#include <cstdint>

namespace bb {

class CarInputView;
class CarDriver;
class InputContext;

// PlayerSteering (embedded at RealDriver+0x220; ctor sub_405C60). Turns the -1..1 steering input into the
// u16 wheel angle (CarDriver+0x1E0) with a response curve, a speed-dependent angle limit with a
// counter-steer assist driven by the yaw rate, and a rate limiter. Gamepad/keyboard path (sub_416BB0);
// the Logitech wheel path (sub_4061C0) and the debug path (sub_406A40) are not ported.
class PlayerSteering {
public:
    uint8_t Field04 = 0;                    // +0x04
    float Field08 = 1.0f;                   // +0x08 (read by ApplyForceFeedback only)
    float Field0C = 7.0f;                   // +0x0C (read by ApplyForceFeedback only)
    bool CounterSteerActive = false;        // +0x10 assist hysteresis flag
    AverageWindow CurveAverage;             // +0x14 (its average +0x20: recent response-curve value)
    AverageWindow RateAverage;              // +0x48 (its average +0x54: recent input-speed factor)
    uint16_t Angle = 0;                     // +0x7C current wheel angle (u16, 65536 = 360 deg)
    float Limit = 45.0f;                    // +0x80 last angle limit
    float LastCurve = 0.0f;                 // +0x84 last response-curve value

    PlayerSteering();

    // sub_405DC0(float in, float dt, JoystickPort): steering limit factor (result saved in +0x80). Called by
    // sub_416BB0 with in = response-curve value.
    // Reads the car's driver (car+0x30) throttle/brake/handbrake: the RealDriver itself for a human car.
    float SteerLimit(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx, const InputTables& tables);

    // sub_4060E0(float in, float dt, JoystickPort): steering rate factor (records +0x48 average);
    // in = response-curve value (compared with the previous one, +0x84).
    float SteerRate(float input, float dt, CarInputView& car, InputContext& ctx, const InputTables& tables);

    // sub_416BB0(float in, float dt, JoystickPort): gamepad / keyboard filter. Returns the response
    // curve value; the new angle goes to +0x7C.
    float FilterPad(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx, const InputTables& tables);

    // PlayerSteering::DoSteering(float in, float dt, JoystickPort) 0x41DFC0. Returns the u16 angle.
    // ApplyForceFeedback (0x406460) only drives a Logitech wheel and writes nothing the physics reads.
    uint16_t DoSteering(float input, float dt, CarInputView& car, CarDriver& drv, InputContext& ctx,
                        const InputTables& tables, bool wheelConnected = false);
};

} // namespace bb

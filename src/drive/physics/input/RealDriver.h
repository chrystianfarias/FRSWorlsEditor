#pragma once

// Port of NFSU2Unity Core/Physics/Input/RealDriver.cs.

#include "drive/numerics/X87.h"
#include "drive/physics/World.h"
#include "drive/physics/input/InputTables.h"
#include "drive/physics/input/PlayerSteering.h"

#include <cstdint>
#include <functional>
#include <string>

namespace bb {

// Constants of the input chain (.rdata bit patterns unless marked .data).
namespace InputConstants {
inline const float K_Half = X87::Bits(0x3F000000u);          // flt_784260 / dword_783B28 0.5
inline const float K_One = X87::Bits(0x3F800000u);           // float_1 0x784250
inline const float K_Zero = 0.0f;                            // _zero 0x7D7D3C
inline const float K_Four = X87::Bits(0x40800000u);          // flt_783B34 4.0
inline const float K_20Mph = X87::Bits(0x410F0C79u);         // flt_784568 8.9405451 (20 mph in m/s)
inline const float K_0_6 = X87::Bits(0x3F19999Au);           // dword_784564 0.6
inline const float K_Minus2 = X87::Bits(0xC0000000u);        // flt_783B30 -2.0
inline const float K_Minus30 = X87::Bits(0xC1F00000u);       // flt_783B2C -30.0
inline const float K_Ten = X87::Bits(0x41200000u);           // flt_784560 10.0
inline const float K_EBrakeEventWeight = X87::Bits(0x3F800000u); // dword_783AFC 1.0
inline const float K_Two = X87::Bits(0x40000000u);           // flt_783B00 2.0
inline const float K_0_7 = X87::Bits(0x3F333333u);           // flt_7844D8 0.7
inline const float K_0_2 = X87::Bits(0x3E4CCCCDu);           // flt_784554 0.2
inline const float K_RiseRate = X87::Bits(0x3F800000u);      // flt_783B38 1.0 (throttle rise per s)
inline const float K_FallRate = X87::Bits(0x3FC00000u);      // flt_783B3C 1.5 (throttle fall per s)
inline const float K_100 = X87::Bits(0x42C80000u);           // flt_784258 100.0 (TestRPM "off" value)
inline const float K_1000 = X87::Bits(0x447A0000u);          // _one_thousand 0x784550
inline const float K_DragLaunch = X87::Bits(0x3E800000u);    // flt_783B04 0.25 s
inline const float K_4000 = X87::Bits(0x457A0000u);          // flt_784264 4000.0
inline const float K_1_4000 = X87::Bits(0x3983126Fu);        // flt_784268 0.00025
inline const float K_128 = X87::Bits(0x43000000u);           // flt_784540 128.0
inline const float K_SteerScale = X87::Bits(0xBC000000u);    // flt_78453C -1/128
inline const float K_1_07 = X87::Bits(0x3F88F5C3u);          // flt_783B1C 1.07
inline const float K_255 = X87::Bits(0x437F0000u);           // flt_784544 255.0
inline const float K_1_255 = X87::Bits(0x3B808081u);         // flt_784548 1/255
inline const float K_2p32 = X87::Bits(0x4F800000u);          // dword_78454C 2^32 (unsigned fixup, never used)
} // namespace InputConstants

// Per-frame globals and car values the input chain reads. The integrator wires these from World/Car/CarState;
// see docs/re/port-status-timing-input.md for the exact source of each.
class InputContext {
public:
    RaceModeFlags Flags;
    float TestRPM = X87::Bits(0x42C80000u);   // 0x7FBED8 (.data) = 100.0 -> use car+0x440
    int WorldLoopCounter = 0;                 // 0x7FB724
    int WorldTimer = 0;                       // 0x86518C
    bool Online = false;                      // sub_5F00F0(&dword_89CF48)
    int RaceCoordinatorState = 0;             // *pRaceCoordinator
    // ReplayManager::RegisterInterestingEvent(weight, "EBrake"): no effect on the simulation.
    std::function<void(float, const std::string&)> ReplayEvent;
};

// Car fields used by RealDriver / PlayerSteering (car = Car*, cpi = CarPhysicsInfo at car+0x20).
class CarInputView {
public:
    float SpeedLong = 0.0f;          // car+0x42C (= CarState+0x3EC) signed forward speed, m/s
    float Car118 = 0.0f;             // car+0x118 (burnout: speed divided by |x|*4+1; inferred wheel-spin term)
    float Car128 = 0.0f;             // car+0x128 (drift steering: sign vs input and |x| -> Graph 0x7E9F50; inferred yaw rate)
    float Car430 = 0.0f;             // car+0x430 (replay "EBrake" event when > 10; inferred speed)
    int Gear = 0;                    // car+0x434 (1 = neutral)
    float Rpm = 0.0f;                // car+0x440
    float ThrottleTelemetry = 0.0f;  // car+0x438 (written: = driver throttle)
    float VelX = 0.0f, VelY = 0.0f, VelZ = 0.0f;   // car+0x70 world velocity
    int MovementMode = 1;            // car+0x28
    int LastCollisionTime = 0;       // [car+0x3C]+0x140: WorldTimer stamp (0 / 0x7FFFFFFF = none)
    float Yaw = 0.0f;                // CarState::GetYaw() (car+0x40; 0x59FA10, CarState area)
    float TopSpeed = 0.0f;           // sub_5B5930([car+0x10]+0x10) (RidePhysicsInfo, inferred top speed)
    float SteerMaxDeg = 0.0f;        // cpi+0x270 (ControlInfo[0])
    float IdleRpm = 0.0f;            // cpi+0x1F0 (EngineInfo[0])
    int NumForwardGears = 0;         // cpi+0x1C8
};

// CarDriver (base) fields read by the physics (PhysicsMover::DelegateDriverInput).
class CarDriver {
public:
    virtual ~CarDriver() = default;

    uint16_t SteerAngle = 0;         // +0x1E0 (65536 = 360 deg)
    int GearCommand = 0;             // +0x1E4 (vtbl+0x20 get 0x405B20 / +0x28 set 0x405B40)
    int CurrentGear = 0;             // +0x1E8 (vtbl+0x24 get 0x405B30 / +0x2C set 0x405B50)
    float Hop[4] = {};               // +0x1EC hydraulic hop buttons
    float HydraulicHeight[4] = {};   // +0x1FC hydraulic height requests
    float Throttle = 0.0f;           // +0x20C SetThrottle 0x5EB910
    float EBrake = 0.0f;             // +0x210 SetEBrake 0x5EB900
    float Brake = 0.0f;              // +0x214 SetBrake 0x5EB8F0

    // vtbl+0x84: yaw angle requested from Car::MaybeForceYaw (RealDriver: 0x405BD0 returns 0).
    virtual uint16_t GetForcedYaw() { return 0; }
};

// RealDriver (vtbl 0x784FC8, ctor 0x425FC0): the human driver. Joystick handlers store raw values;
// Drive (0x41E2C0) turns them into CarDriver throttle/brake/handbrake/steering once per World::DoTimestep.
class RealDriver : public CarDriver {
public:
    PlayerSteering Steering;              // +0x220
    int Port = -1;                        // +0x2AC JoystickPort
    bool ForceFullBrake = false;          // +0x2B4 (byte; 1 unless pCurrentRace->state == 4 at creation)
    int ForceStop = 0;                    // +0x2B8 (throttle 0, brake 1)
    float EBrakeTimer = 0.0f;             // +0x2BC forced-handbrake timer (seconds)
    int ShiftMode = 0;                    // +0x2C0 (0 auto, 1, 2, 3 manual+clutch, 4 = 3 while shifting)
    uint8_t SteerLeft = 0x80, SteerRight = 0x80; // +0x2C4/+0x2C5
    float SteerInput = 0.0f;              // +0x2C8 (-1..1, left positive)
    int DragLaunchTimer = 0;              // +0x2CC Timer (4000 Hz ticks)
    uint8_t ThrottleDigital = 0, ThrottleAnalog = 0, ThrottleAnalog2 = 0; // +0x2D0..+0x2D2
    float ThrottleInput = 0.0f;           // +0x2D4
    uint8_t BrakeDigital = 0, BrakeAnalog = 0, BrakeAnalog2 = 0;          // +0x2D8..+0x2DA
    float BrakeInput = 0.0f;              // +0x2DC
    uint8_t ReverseDigital = 0, ReverseAnalog = 0; // +0x2E0/+0x2E1
    float ReverseInput = 0.0f;            // +0x2E4
    uint8_t EBrakeDigital = 0, EBrakeAnalog = 0;   // +0x2E8/+0x2E9
    float EBrakeInput = 0.0f;             // +0x2EC
    bool ReverseLatch = false;            // +0x2F0 brake pedal acts as reverse throttle
    float ManualClutch = 1.0f;            // +0x2F4 (1 = engaged)
    bool SteerEnabled = true;             // +0x2F8
    bool IgnorePedals = false;            // +0x2FB

    RealDriver();

    // RealDriver::ResetJoyValues 0x4076C0 (also +0x300..+0x310 hydraulic inputs, not modelled).
    void ResetJoyValues();

    // ---- joystick handlers
    // cdecl (JoystickPort, JoyEvent, uchar value, void* this); registered by AddJoyHandlers 0x41E060.

    // JoyHandler_Steer 0x407750 (events 0x8F left axis, 0x90 right axis; 0x80 = centre).
    void JoyHandlerSteer(int joyEvent, uint8_t value);
    // JoyHandler_Throttle 0x416DE0 (0x91 digital, 0x92 analog, 0x93 analog x1.07 outside neutral).
    void JoyHandlerThrottle(int joyEvent, uint8_t value, CarInputView& car, const InputTables& tables);
    // JoyHandler_Brake 0x407EB0 (0x94 digital, 0x95/0x96 analog).
    void JoyHandlerBrake(int joyEvent, uint8_t value);
    // JoyHandler_Reverse 0x4077D0 (0x97 digital, 0x98 analog; in neutral the analog is cubed).
    void JoyHandlerReverse(int joyEvent, uint8_t value, CarInputView& car);
    // JoyHandler_EBrake 0x407F40 (0x99 digital, 0x9A analog x2).
    void JoyHandlerEBrake(int joyEvent, uint8_t value);
    // JoyHandler_DisengageClutch 0x408180 (only in shift mode 3; 0x9F digital, 0xA0 analog).
    void JoyHandlerDisengageClutch(int joyEvent, uint8_t value);
    // JoyHandler_ShiftUp 0x407FC0 (events 0x9B, 0x9D; acts on press only).
    void JoyHandlerShiftUp(uint8_t value, CarInputView& car);
    // JoyHandler_ShiftDown 0x4080B0 (events 0x9C, 0x9E).
    void JoyHandlerShiftDown(uint8_t value, CarInputView& car);

    // ---- per-frame logic

    // RealDriver vtbl+0x30, sub_407310: leave reverse (first gear request) after reversing.
    void RequestForwardGear(InputContext& ctx);

    // RealDriver::DoThrottleBrakeReverse(float dt) 0x407A30, ported from the asm. Reads ThrottleInput (+0x2D4),
    // BrakeInput (+0x2DC), EBrakeInput (+0x2EC), the latch +0x2F0, timers +0x2BC/+0x2CC, the previous
    // Throttle (+0x20C) and car speed/gear/rpm; writes Throttle/Brake/EBrake, GearCommand (0 = reverse
    // requested) and car+0x438.
    void DoThrottleBrakeReverse(float dt, CarInputView& car, InputContext& ctx, const InputTables& tables);

    // RealDriver::Drive(float dt) 0x41E2C0 minus the parts owned elsewhere: IsJoystickPluggedIn/ResetJoyValues,
    // the transmission hand-off (shift modes 3/4, Car::IsShifting, DriveTrain::SetManualClutchEngaged,
    // drivetrain+0x74 in sub_401950 races) and DoHydraulics are left to the integrator (see the status doc).
    // The Logitech wheel steering filter (sub_4061C0) is not ported: wheelConnected must be false.
    void Drive(float dt, CarInputView& car, InputContext& ctx, const InputTables& tables,
               bool joystickPluggedIn = true, bool wheelConnected = false);

private:
    // Timer::CountDown(float dt) 0x577310 on +0x2CC: true while the timer is still running.
    static bool CountDown(int& timer, float dt);
};

} // namespace bb

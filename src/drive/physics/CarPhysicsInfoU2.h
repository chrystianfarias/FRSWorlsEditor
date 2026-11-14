#pragma once

// Port of NFSU2Unity Core/Physics/CarPhysicsInfoU2.cs.

#include "core/Math.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bb {

class CarTypeInfoU2;

// CarPhysicsInfo: the first 0x2B0 bytes of CompleatCarPhysicsInfo (CarTypeInfo+0x110), copied per car
// by BuildCarPhysicsInfo (0x5AC4F0). Field names come from their readers; see docs/re/physics-data.md 2.1.
// Values are kept as raw floats: the port must not reorder or re-derive anything.
class CarPhysicsInfoU2 {
public:
    static constexpr int Size = 0x2B0;
    static constexpr int CarTypeInfoOffset = 0x110;

    struct Wheel {          // cpi+0x10 + 0x30*i, FL FR RR RL
        float X = 0, Y = 0, RideHeight = 0, Unknown0C = 0, Radius = 0, Width = 0;
        int Id = 0;
        float RenderY = 0, WideBodyY = 0;
    };

    struct Tire {           // cpi+0xD0 front, +0xF0 rear
        float SlipAnglePeak = 0, LoadNorm = 0, LateralGrip = 0, GripCoef = 0, GripCoef2 = 0;
    };

    struct Suspension {     // cpi+0x170 front, +0x190 rear
        float Progression = 0, SpringRate = 0, BumpDamping = 0, ReboundDamping = 0, AntiRoll = 0, MaxCompression = 0, MaxExtension = 0, Unknown1C = 0;
    };

    ::Vec3 CG;                                         // +0x000
    Wheel Wheels[4];                                 // +0x010
    Tire FrontTire, RearTire;                        // +0x0D0, +0x0F0
    // RigidBodyInfo +0x110
    float Mass = 0;                                  // +0x110
    float BoxLength = 0, BoxWidth = 0, BoxHeight = 0; // +0x114..+0x11C
    float InertiaTensor[16] = {};                    // +0x120 (recomputed by CalculateInertiaTensor)
    float InertiaZZMul = 0;                          // +0x160
    Suspension FrontSuspension, RearSuspension;      // +0x170, +0x190
    // DriveTrainInfo +0x1B0
    float NeutralShiftFrac = 0;                      // +0x1B0
    float DriveTrain04 = 0;                          // +0x1B4 (no reader found)
    float FinalDrive0 = 0, FinalDrive1 = 0;          // +0x1B8, +0x1BC
    float TorqueSplitRear = 0;                       // +0x1C0 (0 FWD .. 1 RWD)
    float ClutchSlipCoef = 0;                        // +0x1C4
    int NumForwardGears = 0;                         // +0x1C8
    float ClutchRate = 0;                            // +0x1CC
    float GearRatios[8] = {};                        // +0x1D0: R, N, 1st..6th
    // EngineInfo +0x1F0
    float IdleRpm = 0, RedlineRpm = 0, MaxRpm = 0, EngineInertia = 0; // +0x1F0..+0x1FC
    float Torque[9] = {};                            // +0x200, idle..MaxRpm evenly spaced
    float TurboTorque[9] = {};                       // +0x224
    float EngineBraking[3] = {};                     // +0x248
    // AeroInfo +0x260
    float Drag = 0, AeroA = 0, AeroB = 0, AeroC = 0; // +0x260..+0x26C
    // ControlInfo +0x270
    float SteerParam = 0, BrakeTorque = 0, EBrakeTorque = 0, BrakeBiasRear = 0; // +0x270..+0x27C
    float Control280 = 0, Control284 = 0;            // +0x280, +0x284 (args of TuneWheelParams)
    float GroundDamping = 0, AirDamping = 0, Damping290 = 0; // +0x288, +0x28C, +0x290
    // Nitrous +0x2A0
    float NitrousCapacity = 0;                       // +0x2A0
    int NitrousShots = 0;                            // +0x2A4
    float Nitrous2A8 = 0;                            // +0x2A8 (no reader found)
    float NitrousTorqueMul = 0;                      // +0x2AC

    // The raw 0x2B0 bytes, for byte-exact comparisons.
    std::vector<uint8_t> Raw;

    // Throws std::runtime_error when buf[offset .. offset+Size) is out of range.
    static CarPhysicsInfoU2 Read(const uint8_t* buf, size_t bufSize, size_t offset);

    // Raw (un-upgraded) physics info of a car type, from its 0x890-byte CarTypeInfo record.
    static CarPhysicsInfoU2 FromCarType(const CarTypeInfoU2& carType);
};

} // namespace bb

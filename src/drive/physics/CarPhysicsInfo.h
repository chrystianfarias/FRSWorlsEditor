#pragma once

// Port of NFSU2Unity Core/Physics/CarPhysicsInfo.cs.

#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/Engine.h"
#include "drive/physics/rigidbody/Aerodynamics.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/wheel/WheelData.h"

#include <cstdint>
#include <memory>

namespace bb {

class CarTypeInfoU2;

// Per-car CarPhysicsInfo (0x2B0 bytes, RidePhysicsInfo+0x30) as the dynamics reads it: the sub-blocks are
// views on the same bytes in the game, here they are typed objects built once from the raw block. The
// physics never writes this block (it is rebuilt by RidePhysicsInfo::RebuildPhysicsInfo). Offsets: see
// docs/re/physics-overview.md section 3.1 and physics-data.md section 2.1.
// Components keep pointers into it: keep the object alive (and in place) while a mover uses it.
class CarPhysicsInfo {
public:
    static constexpr int Size = 0x2B0;

    uint8_t Raw[Size] = {};
    float Cg[3] = {};                              // +0x000 default CG (FixCarCenterOfGravity)
    WheelInfo Wheels[4];                           // +0x010 + 0x30*i
    TireInfo Tires[2];                             // +0x0D0 + 0x20*axle
    RigidBodyInfo RigidBody;                       // +0x110
    SuspensionInfo Suspensions[2];                 // +0x170 + 0x20*axle
    DriveTrainInfo DriveTrain;                     // +0x1B0
    EngineInfo Engine;                             // +0x1F0
    AeroInfo Aero;                                 // +0x260
    ControlInfo Control;                           // +0x270
    float BrakeBiasRear = 0;                       // +0x27C (ControlInfo+0xC)
    float RearGripBase = 0;                        // +0x280 RearGripBoost arg 3
    float RearGripCorner = 0;                      // +0x284 RearGripBoost arg 4
    float YawTorqueClampPilotOff = 0;              // +0x288 rb+0x200 when CarState+0x424 == 0
    float YawTorqueClampPilotOn = 0;               // +0x28C (0: 20 when counter-steering)
    float TopSpeed = 0;                            // +0x290 mph -> RigidBody+0x204
    float NosTorqueMul = 0;                        // +0x2AC nitrous torque factor

    float RearTorqueSplit() const { return DriveTrain.RearTorqueSplit; }   // +0x1C0

    // Builds the typed view of a 0x2B0-byte CarPhysicsInfo image (b + o .. b + o + Size).
    static std::unique_ptr<CarPhysicsInfo> FromBytes(const uint8_t* b, int o = 0);

    // Stock (un-upgraded) CarPhysicsInfo of a car type: CarTypeInfo+0x110. Throws std::runtime_error when the
    // record is too short.
    static std::unique_ptr<CarPhysicsInfo> FromCarType(const CarTypeInfoU2& carType);
};

} // namespace bb

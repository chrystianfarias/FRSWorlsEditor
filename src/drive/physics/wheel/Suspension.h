#pragma once

// Port of NFSU2Unity Core/Physics/Wheel/Suspension.cs (Suspension, TwoWheelAckermanSteering).

#include "drive/numerics/X87.h"
#include "drive/physics/wheel/WheelData.h"

#include <cstdint>

namespace bb {

class RigidBody;
class TwoWheelAckermanSteering;
class Wheel;

// Suspension (PhysicsComponent "Suspnsn", vtbl 0x7A210C, >= 0xB4 bytes) of SPEED2.EXE v1.2. Its state
// {compression, rate} is not integrated (GetDerivative 0x59E130 writes zeros): ComputeAuxiliaries rebuilds it
// from the terrain every sub-step. Spec: docs/re/physics-overview.md appendix A 1.3/3.2.
class Suspension {
public:
    const SuspensionInfo* Info = nullptr;         // +0x14
    Suspension* Partner = nullptr;                // +0x18 same axle (0<->1, 2<->3)
    float Compression = 0;                        // +0x1C STATE[0]
    float CompressionRate = 0;                    // +0x20 STATE[1]
    float SpringForce = 0;                        // +0x24 spring + damper + preload + anti-roll, >= 0
    float WheelPosLocal[4] = {};                  // +0x30 wheel centre relative to the CG, car space (w untouched)
    float LocalNoSteer[16] = {};                  // +0x40
    int Touching = 0;                             // +0x80
    float Preload = 0;                            // +0x88
    float Attach[3] = {};                         // +0x90 WheelInfo.pos - cgOffset, z -= rideHeight
    bb::Wheel* Wheel = nullptr;                   // +0xA0
    RigidBody* Body = nullptr;                    // +0xA4
    TwoWheelAckermanSteering* Steering = nullptr; // +0xA8 front wheels only
    int Index = 0;                                // +0xAC
    float ExchangeRet = 0;                        // +0xB0 return of Wheel::ExchangeZForces (0.0)

    // Suspension::Suspension(int, RigidBody*, Wheel*, Steering*, SuspensionInfo*) 0x5A86C0.
    Suspension(int index, RigidBody* rb, bb::Wheel* wheel, TwoWheelAckermanSteering* steering, const SuspensionInfo* si);

    // Suspension::Calibrate(float preload, float rideHeight) 0x59E0C0.
    void Calibrate(float preload, float rideHeight);

    // Suspension::GetDerivative(float* d) 0x59E130.
    void GetDerivative(float* d) const { d[0] = 0.0f; d[1] = 0.0f; }

    // Suspension::ComputeAuxiliaries() 0x5A8790: terrain contact, compression and rate, wheel matrices and
    // contact velocity, spring force. Calls Wheel::CheckTerrain (world queries through Wheel.Env.World).
    void ComputeAuxiliaries();

    // Suspension::ApplyGroundPenaltyImpulse() 0x59E1A0 (st0): impulse along the ground normal that cancels the
    // approach velocity of the wheel (bottoming out). Calls RigidBody::ApplyImpulse unless NoWheels.
    F24 ApplyGroundPenaltyImpulse();
};

// TwoWheelAckermanSteering (vtbl 0x7A2084, not a PhysicsComponent): rotation about Z of the two front wheels
// with an approximate Ackermann angle for the inner wheel. Spec: appendix A 1.4/3.3.
class TwoWheelAckermanSteering {
public:
    const WheelInfo* Wheels = nullptr;            // +0x04 CarPhysicsInfo* (reads cpi+0x10/+0x14/+0x44/+0xA0); 4 entries
    float Mat[2][16] = {};                        // +0x10, +0x50 (mats pointer +0x08 = this+0x10)
    uint16_t Angle = 0;                           // +0x90

    explicit TwoWheelAckermanSteering(const WheelInfo* wheels) : Wheels(wheels) {}

    // GetSteering() 0x5A4760.
    uint16_t GetSteering() const { return Angle; }

    // TwoWheelAckermanSteering::SetSteering(unsigned short angle) 0x59DEF0.
    void SetSteering(uint16_t angle);

private:
    static void Rot(float* m, float x);
};

} // namespace bb

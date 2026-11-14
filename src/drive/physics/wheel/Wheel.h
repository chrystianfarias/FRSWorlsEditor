#pragma once

// Port of NFSU2Unity Core/Physics/Wheel/Wheel.cs.

#include "drive/numerics/X87.h"
#include "drive/physics/wheel/WheelData.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cstdint>

namespace bb {

class CarState;
class PhysicsEnvironment;

// Wheel (PhysicsComponent "Wheel", vtbl 0x7A20E8, >= 0x19C bytes) of SPEED2.EXE v1.2: tyre forces, brakes and
// wheel spin. Ports follow the x87 instruction order of the original (operand order, comparison masks); values
// that stay on the x87 stack are F24 (PC=24 with the extended exponent range), every fstp dword is
// a Store(). Spec: docs/re/physics-overview.md appendix A 1.2/3.1; FPU rules: docs/re/fp-determinism.md.
class Wheel {
public:
    PhysicsEnvironment* Env = nullptr;
    const WheelInfo* Info = nullptr;                // +0x014
    const TireInfo* Tire = nullptr;                 // +0x018
    const ControlInfo* Control = nullptr;           // +0x01C
    CarState* Car = nullptr;                        // +0x020
    float Angle = 0;                                // +0x024 STATE[0] spin angle (rad), wrapped at +-628.318
    float Omega = 0;                                // +0x028 STATE[1] angular velocity (rad/s)
    float Inertia = 0;                              // +0x02C (WheelMomentOfInertia, PhysicsMover::Calibrate)
    float Spin[16] = {};                            // +0x030 rotation about Y by Angle
    float World[16] = {};                           // +0x070 world matrix (rows: forward, side, up, centre +0x0A0)
    float Local[16] = {};                           // +0x0B0 car-space matrix (with steering)
    float ContactVel[3] = {};                       // +0x0F0 world velocity of the wheel centre (v + w x r)
    float LastSlip = 0;                             // +0x100
    float LastLatVel = 0;                           // +0x104
    float Torque = 0;                               // +0x108 drive/brake torque accumulator
    int BrakeLocked = 0;                            // +0x10C
    int RollState = 0;                              // +0x110 0 forward, 1 ~stopped, 2 backward
    int Terrain = 0;                                // +0x114 TerrainType under the wheel
    float SlipVel = 0;                              // +0x118 R*omega - v_long
    uint8_t OnGround = 0;                           // +0x11C
    float NetTorqueFilt = 0;                        // +0x120 0.5 moving average of the net torque
    float Fx = 0;                                   // +0x124 longitudinal force (wheel frame)
    float Fy = 0;                                   // +0x128 lateral force (wheel frame)
    float ForceWorld[3] = {};                       // +0x130
    float ContactPt[3] = {};                        // +0x140 centre - R*normal
    float GroundNormal[3] = {};                     // +0x150
    uint8_t Sliding = 0;                            // +0x160 1 = dynamic (sliding) regime
    float Load = 0;                                 // +0x164 normal load Fz
    float RoadNoise = 0;                            // +0x168
    float SlipCopy = 0;                             // +0x16C copy of SlipVel (GetDifferentialAngularVelocity)
    float GripUsage = 0;                            // +0x170 |F|/Fmax in [0,1]
    float FlatDrag = 0;                             // +0x174 1.0 when the tyre is flat/damaged
    float Brake = 0;                                // +0x178
    float EBrake = 0;                               // +0x17C
    float GripScale = 0;                            // +0x180 TuneWheelParams / CalcBurnout
    float GripScale2 = 0;                           // +0x184 catch-up
    float LongGripScale = 0;                        // +0x188 DriftStabilizer
    float RideOffset = 0;                           // +0x18C added to the terrain elevation
    float LongForceScale = 0;                       // +0x190
    float LatForceScale = 0;                        // +0x194

    // Wheel::Wheel(WheelInfo*, TireInfo*, ControlInfo*, CarState*) 0x5A8160 (fields it initialises).
    Wheel(PhysicsEnvironment* env, const WheelInfo* wi, const TireInfo* ti, const ControlInfo* ci, CarState* cs);

    // ------------------------------------------------------------------ level 1 leaves

    // Wheel::GetPilotFactor() 0x59D490 (st0). Grip assist for the rear wheels above 30 mph.
    F24 GetPilotFactor() const;

    // Wheel::CheckForBrakeLock(float lateralForceAtContact) 0x59D400.
    void CheckForBrakeLock(float arg);

    // Wheel::GetHorizontalGroundForceInCarFrame(bVector3* out) 0x59DAF0 (force feedback only).
    void GetHorizontalGroundForceInCarFrame(float* o) const;

    // ------------------------------------------------------------------ level 2

    // Wheel::GetLateralForce(float load, float slipAngle) 0x59D510 (st0): static lateral force.
    F24 GetLateralForce(float load, float slipAngle) const;

    // Wheel::GetDerivative(float* d) 0x59D350: d = {omega, domega/dt}; updates NetTorqueFilt.
    void GetDerivative(float* d);

    // Wheel::ComputeAuxiliaries() 0x59D030: spin matrix, clears forces, wraps the angle, roll state.
    void ComputeAuxiliaries();

    // Wheel::ExchangeZForces(int touching, float springForce) 0x59D160 (IDA: Suspension::ResolveForces, called
    // with ecx = Wheel). Contact point and normal load from the spring force; returns 0.0 in st0.
    float ExchangeZForces(int touching, float springForce);

    // ------------------------------------------------------------------ level 3

    // Wheel::GuessGroundForce(bVector2* unused, int noAbs) 0x5A8280: brakes, slip, tyre forces Fx/Fy.
    void GuessGroundForce(int noAbs);

    // Wheel::FinalizeGroundForce(int unused) 0x59D600: friction circle, grip usage, world force.
    void FinalizeGroundForce();

    // ------------------------------------------------------------------ level 4 (world queries)

    // Wheel::CheckTerrain(bVector3* normalOut, TerrainType* terrOut, bVector3* pt, bMatrix4* carMat) 0x59D250
    // (st0): penetration of the point below the ground, measured along the car's up axis (row 2 of carMat).
    F24 CheckTerrain(float* normalOut, int& terrOut, const float* pt, const float* carMat);

    // ------------------------------------------------------------------ helpers (bWare)

    // bIdentity(bMatrix4*) 0x43C780.
    static void Identity(float* m);

    // bScaleAdd(bVector3* out, a, b, float s) 0x43D0C0: out = s*b + a (reads all inputs first).
    static void ScaleAdd(float* o, const float* a, const float* b, float s);

private:
    const WheelTweaks& T() const;
    // Wheel velocity in the wheel frame (x forward, y lateral).
    void LocalVelocity(float* lv) const;
    // Slip angle a (rad, folded to (-pi, pi]) as computed in GuessGroundForce/FinalizeGroundForce.
    float SlipAngle(const float* lv) const;
};

} // namespace bb

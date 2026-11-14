#pragma once

// Port of NFSU2Unity Core/Physics/Engine/Engine.cs.

#include "drive/physics/engine/EngineTables.h"

namespace bb {

class CarState;
class DriveTrain;

// EngineInfo (CarPhysicsInfo+0x1F0, 0x70 bytes). Field names from their readers (physics-data.md 2.1).
struct EngineInfo {
    float IdleRpm = 0;            // +0x00
    float RedlineRpm = 0;         // +0x04
    float MaxRpm = 0;             // +0x08
    float Inertia = 0;            // +0x0C
    float Torque[9] = {};         // +0x10, idle..MaxRpm evenly spaced
    float Boost[9] = {};          // +0x34 turbo torque (already summed into Torque)
    float EngineBrake[3] = {};    // +0x58
};

// Engine (vtbl 0x7A235C, "Engine", EngineSlotPool, 0xB0 bytes): a PhysicsComponent whose single state
// float is the crankshaft angular velocity. Port of SPEED2.EXE v1.2, x87 operation order preserved
// (docs/re/physics-overview.md appendix B, docs/re/fp-determinism.md).
class Engine {
public:
    // Table (0x14 bytes): {int n, float min, float max, float invStep, float* y}; Table::GetValue 0x5764D0.
    class Table {
    public:
        int Count = 0;              // +0x00
        float Min = 0;              // +0x04
        float Max = 0;              // +0x08 (not read by GetValue)
        float InvStep = 0;          // +0x0C
        const float* Y = nullptr;   // +0x10
        float GetValue(float x) const;
    };

    const EngineTables& K;

    float Omega = 0;                     // +0x14 STATE, crank rad/s (rpm = w * 9.549304)
    const EngineInfo* Info = nullptr;    // +0x18
    bb::CarState* CarState = nullptr;    // +0x1C
    bb::DriveTrain* DriveTrain = nullptr;// +0x20 (set by the PhysicsMover ctor)
    Table TorqueTable;                   // +0x24
    Table EngineBrakeTable;              // +0x38
    Table BoostTable;                    // +0x4C (built, never sampled by the physics)
    Table NosTable;                      // +0x60 (y = global NOS curve 0x7FBF38)
    int HpMultFlag = 0;                  // +0x74 (s8)CarTypeInfo+0xD8: torque *= TorqueHPMultiplier
    float Throttle = 0;                  // +0x78 written by DelegateDriverInput
    float LoadTorque = 0;                // +0x7C clutch reaction (0 in ComputeAuxiliaries, rebuilt by IterateForces)
    float ThrottleGate = 0;              // +0x80 1 normal, 0 shifting, 0.7 engaging, 1+bonus
    int NosLevel = 0;                    // +0x84 0..3 = CarState::IsNitrousOn
    bool RevLimiter = false;             // +0x88 (byte) ignition cut
    float Heat = 0;                      // +0x8C -1 off, >= 2 blown
    float TorqueScale = 0;               // +0x90
    float Unk94 = 0;                     // +0x94 (ctor 0)
    float MaxBoost = 0;                  // +0x98 GetMaxBoost(info)
    float Boost = 0;                     // +0x9C turbo value (UpdateTurbo; effects only)
    int TurboState = 0;                  // +0xA0 0 idle, 1 spooling, 2 blow-off
    float BoostAtRelease = 0;            // +0xA4
    float TurboTimer = 0;                // +0xA8
    float BoostPrev = 0;                 // +0xAC

    explicit Engine(const EngineTables& tables) : K(tables) {}

    // Engine::Engine(EngineInfo*, CarState*) 0x5AA5A0. Fields the ctor does not write (Omega, Throttle,
    // LoadTorque) keep whatever the object held (slot pool memory); DriveTrain is set to null (+0x20 = 0).
    void Construct(const EngineInfo* info, bb::CarState* carState);

    // sub_457B00 (GetMaxBoost, inferred): max(0, Boost[0..8]) with the x87 compare order.
    static float GetMaxBoost(const EngineInfo* info);

    // sub_5942E0 (GetEffectiveThrottle, inferred).
    float GetEffectiveThrottle() const;

    // sub_5942C0 (IsBlown, inferred): heat == 2.0 ("test ah,44h ; jp").
    bool IsBlown() const;

    // Engine::GetTotalEngineTorque 0x594300.
    float GetTotalEngineTorque() const;

    // Engine::GetDerivative(float*) 0x5A1290: d(omega)/dt.
    float GetDerivative() const;

    // Engine::ComputeAuxiliaries 0x5AA760 (h = UglyTimestepHack: the substep, 0 outside Move).
    void ComputeAuxiliaries(float uglyTimestepHack);

    // sub_5A11E0 (Engine::UpdateHeat, inferred).
    void UpdateHeat(float dt);

    // sub_5A0F30 (Engine::UpdateTurbo, inferred): turbo / blow-off state machine (effects only).
    void UpdateTurbo(float dt);
};

} // namespace bb

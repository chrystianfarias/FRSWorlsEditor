#pragma once

// Port of NFSU2Unity Core/Physics/RigidBody/Aerodynamics.cs.

namespace bb {

class RaceModeFlags;
class RigidBody;
class RigidBodyData;

// AeroInfo (CarPhysicsInfo+0x260, 0x10 bytes).
struct AeroInfo {
    float Drag = 0;            // +0x0  F = Drag * |v| * v
    float DownforceFront = 0;  // +0x4
    float DownforceRear = 0;   // +0x8
    float CalibK = 0;          // +0xC  only read by PhysicsMover::Calibrate
};

// Aerodynamics (AerodynamicsSlotPool, no vtable, not a PhysicsComponent): drag and downforce applied to
// the RigidBody from PhysicsMover::ResolveForces. Port of SPEED2.EXE v1.2 in x87 order.
class Aerodynamics {
public:
    static constexpr int Size = 0x38;
    const AeroInfo* Info = nullptr;           // +0x00
    RigidBody* Rb = nullptr;                  // +0x04
    float SpeedScale = 1.0f;                  // +0x08 catch-up speed scale (ctor 1.0)
    float DragScale = 1.0f;                   // +0x0C 1 - slipstream (ctor 1.0)
    float Throttle = 0;                       // +0x10
    float GroundFrac = 0;                     // +0x14 wheels on ground * 0.25
    float Unk18 = 0, Unk1C = 0;               // +0x18, +0x1C
    float DragForce[4] = {};                  // +0x20 last drag force
    float Enable = 1.0f;                      // +0x30 1.0; 0 on the dyno / mode 1
    float TerrainDrag = 0;                    // +0x34 mean terrain drag of the wheels on ground

    // Aerodynamics::ResolveForces(float *outDownforce) 0x59E620 (thiscall): drag force through the CG
    // (lever (0,0,(1-throttle)*-0.1) in world axes when on the ground), then front and rear downforce at
    // R*(+-1,0,0). outDownforce may be null (front + rear is written to [0]).
    void ResolveForces(float* outDownforce, const RaceModeFlags& mode, const RigidBodyData& data);
};

} // namespace bb

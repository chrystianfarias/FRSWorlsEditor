#pragma once

// Port of NFSU2Unity Core/Physics/Wheel/WheelData.cs.
// Read-only per-car data blocks of CarPhysicsInfo (cpi) seen by the wheel slice, laid out as in memory
// (offsets relative to each block). See docs/re/physics-overview.md appendix A section 1.1 and physics-data.md.

namespace bb {

// WheelInfo: cpi+0x10 + 0x30*i (i = 0 FL, 1 FR, 2 RR, 3 RL).
struct WheelInfo {
    float Pos[3] = {};       // +0x00 wheel position in car space (x forward, y left, z up)
    float Radius = 0;        // +0x10 tyre radius R
    int Index = 0;           // +0x18 wheel index 0..3 (>= 2: rear)
};

// TireInfo: cpi+0xD0 + 0x20*(i/2) (front, rear).
struct TireInfo {
    float MaxSlipAngle = 0;  // +0x00 static slip-angle limit; normaliser of the new lateral curves
    float LoadRef = 0;       // +0x04 load normaliser (GetLateralForce)
    float LateralScale = 0;  // +0x08 lateral force scale (new model)
    float MuLong = 0;        // +0x0C friction-circle mu (FinalizeGroundForce)
    float MuLat = 0;         // +0x10 dynamic mu (GuessGroundForce)
};

// SuspensionInfo: cpi+0x170 + 0x20*(i/2).
struct SuspensionInfo {
    float Progression = 0;    // +0x00 k_eff = (max(x,0)*progression + 1) * springRate
    float SpringRate = 0;     // +0x04
    float BumpDamping = 0;    // +0x08 compression rate > 0
    float ReboundDamping = 0; // +0x0C compression rate <= 0
    float AntiRoll = 0;       // +0x10 anti-roll bar stiffness (difference to the partner)
    float MaxCompression = 0; // +0x14 bump stop (clamp, still touching)
    float MinCompression = 0; // +0x18 full extension (below: wheel in the air)
    float DisplayMax = 0;     // +0x1C render clamp of the compression (UpdateCarState)
};

// ControlInfo: cpi+0x270.
struct ControlInfo {
    float SteerMaxDeg = 0;       // +0x00 maximum steering angle (degrees, PlayerSteering)
    float BrakeTorqueScale = 0;  // +0x04 x Wheel.Brake x BrakingTorque
    float EBrakeTorqueScale = 0; // +0x08 x Wheel.EBrake x EBrakingTorque
};

} // namespace bb

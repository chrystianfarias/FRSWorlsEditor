#pragma once

// Port of NFSU2Unity Core/Physics/RigidBody/RigidBody.cs.

#include "drive/numerics/X87.h"
#include "drive/physics/rigidbody/RigidBodyData.h"

#include <cstdint>
#include <functional>

namespace bb {

// RigidBodyInfo (CarPhysicsInfo+0x110, 0x60 bytes).
struct RigidBodyInfo {
    static constexpr int Size = 0x60;
    float Mass = 0;                                  // +0x00 (t)
    float BoxLength = 0, BoxWidth = 0, BoxHeight = 0; // +0x04..+0x0C (only used when building the tensor)
    float Inertia[16] = {};                          // +0x10 body inertia tensor (bMatrix4)
    float YawInertiaScale = 0;                       // +0x50 multiplies [2][2] for the yaw-scaled variant
    float Pad54 = 0, Pad58 = 0, Pad5C = 0;           // +0x54..+0x5C

    static RigidBodyInfo FromBytes(const uint8_t* b, int o = 0);
};

// RigidBody (0x210 bytes, vtbl 0x7A20C4, PhysicsComponent "Rigid"): the car body. Port of SPEED2.EXE v1.2
// following the x87 instruction order; values kept on the x87 stack are F24, every fstp dword is a
// Store(). Field names come from their use; the original offsets are in the comments.
// State = 32 floats at +0x20 (pos 4, R 16, P 4, L 4, restLevel + 3 pads), integrated by
// PhysicsSystem::IntegrateEuler (see IntegrateEuler).
class RigidBody {
public:
    static constexpr int Size = 0x210;
    static constexpr int StateSize = 32;      // GetSize 0x5A80B0

    const RigidBodyData& Data;
    // +0x00 vtbl, +0x04 bNode (next/prev), +0x0C name "Rigid": not modelled
    const RigidBodyInfo* Info = nullptr;          // +0x014
    int Unk18 = 0, Unk1C = 0;                     // +0x018, +0x01C (not used by the dynamics)
    // ---- state (32 floats)
    float Pos[4] = {};                            // +0x020 CG position (world); .w integrated but unused
    float R[16] = {};                             // +0x030 orientation rows (forward, side, up, (0,0,0,w))
    float P[4] = {};                              // +0x070 linear momentum
    float L[4] = {};                              // +0x080 angular momentum (world)
    float RestLevel = 0;                          // +0x090 "rest"/damping level (>= 0 after aux)
    float Pad94[3] = {};                          // +0x094..+0x09C (integrated, unused)
    // ---- auxiliaries
    float Vel[4] = {};                            // +0x0A0 P * invMass
    float AccelG[4] = {};                         // +0x0B0 force * invMass / 9.8
    float AngVel[4] = {};                         // +0x0C0 Iinv_world * L (|w| <= 250)
    float World[16] = {};                         // +0x0D0 R rows 0..2, row 3 = (pos, 1)
    float Force[4] = {};                          // +0x110 force accumulator (world)
    float Torque[4] = {};                         // +0x120 torque accumulator (world)
    float LastForce[4] = {};                      // +0x130 force before the reset in ComputeAuxiliaries
    float LastTorque[4] = {};                     // +0x140
    int RestDampingOn = 0;                        // +0x150
    int AngDampingOn = 0;                         // +0x154
    int UseYawScaledInertia = 0;                  // +0x158 1 -> InvInertiaYaw
    int RaceEngineFlag = 0;                       // +0x15C top speed x1.03
    float Gravity = 0;                            // +0x160
    float Mass = 0;                               // +0x164
    float InvMass = 0;                            // +0x168
    float InvInertia[16] = {};                    // +0x170
    float InvInertiaYaw[16] = {};                 // +0x1B0
    float CgOffset[4] = {};                       // +0x1F0
    float YawTorqueClamp = 0;                     // +0x200 0 = off
    float TopSpeed = 0;                           // +0x204 mph, 0 = off
    float TopSpeedScale = 0;                      // +0x208
    float Pad20C = 0;                             // +0x20C

    // Test/diagnostic observer of RigidBody::ApplyImpulse arguments (called before the impulse is applied).
    std::function<void(const float*, const float*)> ApplyImpulseTrace;

    explicit RigidBody(const RigidBodyData& data) : Data(data) {}

    // Element j (0..31) of the integrated state (+0x20 + 4*j).
    float& State(int j);

    // RigidBody::RigidBody(RigidBodyInfo*) 0x5A7F30 (fields it initialises; pos.w, P.w, L.w, the pads and the
    // world matrix keep the slot-pool content). gravity = the Gravity global (9.8).
    void Construct(const RigidBodyInfo* info, float gravity);

    // RigidBody::SetState(bVector3* pos, bMatrix4* R, bVector3* vel, bVector3* angVel, bMatrix4* outIw)
    // 0x59B9B0 with outIw = NULL: P = mass*vel, L = (R * I * R^T) * angVel with I = diag(1/InvInertia).
    void SetState(const float* pos, const float* r, const float* vel, const float* angVel);

    // RigidBody::CalculateInertiaTensor 0x59B940: mass, 1/mass, inverse tensors (plain and yaw-scaled).
    void CalculateInertiaTensor();

    // sub_59BC20 (RigidBody::AddTorque, inferred): torque += *t (no clamp).
    void AddTorque(const float* t);

    // RigidBody::ApplyForce(bVector3 *F, bVector3 *r) 0x59BB00: force += F clamped to |F| <= 1200;
    // torque += (r x F) with the ORIGINAL F, clamped to 100. Norm orders: |F| = (z*z + y*y) + x*x,
    // |t| = (y*y + z*z) + x*x.
    void ApplyForce(const float* f, const float* r);

    // RigidBody::ApplyImpulse(bVector3 *J, bVector3 *r) 0x59BC60: P += J clamped to 1000 (stored floats),
    // L += r x J_clamped clamped to 10. Norm orders: (y*y + z*z) + x*x for both.
    void ApplyImpulse(const float* jIn, const float* r);

    // RigidBody::ApplyImpulseWithScaledInverseInertiaTensor(J, r, s) 0x59BD80: as ApplyImpulse, but
    // |J| = (z*z + y*y) + x*x, and the clamped angular impulse is multiplied by s before L += .
    void ApplyImpulseWithScaledInverseInertiaTensor(const float* jIn, const float* r, float s);

    // sub_59B8B0 (RigidBody::SetCGOffset, inferred): a = R*newCg, b = R*cgOffset (eMulVector, including
    // R row 3); cgOffset = newCg; pos += a - b.
    void SetCGOffset(const float* newCg);

    // sub_59C120 (SkewTimesMatrix, cdecl(dst, w, R)): dst rows = w x R rows (3x3), dst[3]=dst[7]=dst[11]=0,
    // row 3 = (0,0,0,1). Writes 16 floats at d[o..].
    static void SkewTimesMatrix(float* d, int o, const float* w, const float* r);

    // RigidBody::GetDerivative(float *d) 0x59C1E0. d mirrors the 32-float state. Entries 3 (pos.w),
    // 23 (P.w), 27 (L.w) and 29..31 are NOT written (the original leaves whatever the caller's buffer
    // held). Not pure: the top speed limiter rewrites the force accumulator (+0x110).
    void GetDerivative(float* d);

    // RigidBody::ComputeAuxiliaries 0x59C5F0: v = P/m (|v| <= VelocityClamp when mass > 0.7 double),
    // accelG, w = (R^T * Iinv * R applied as row vector) * L (|w| <= 250), restLevel >= 0, world matrix,
    // lastForce/lastTorque, reset of the accumulators, gravity force through ApplyForce.
    void ComputeAuxiliaries();

    // The RigidBody part of PhysicsSystem::IntegrateEuler 0x5AA480: state[j] = h*d[j] + state[j] for the
    // 32 floats (fld h; fmul d; fadd s; fstp s). d is the GetDerivative buffer; its
    // entries 3, 23, 27, 29..31 are whatever the caller left there (stack garbage in the game).
    void IntegrateEuler(float h, const float* d, int offset = 0);

    // One RB-only substep as the game runs it when the RigidBody is the only component of a
    // PhysicsSystem whose ComputeAuxiliaries is PhysicsSystem::ComputeAuxiliaries 0x5A0D80:
    // GetDerivative, Euler update, ComputeAuxiliaries. scratch is the derivative
    // buffer (32 floats, kept between calls like the game's stack buffer).
    void Substep(float h, float* scratch);

    // Loads the fields from a 0x210-byte image (as in the game's memory).
    void ReadFrom(const uint8_t* b, int o = 0);

    // Writes the modelled fields into a 0x210-byte image (bytes +0x00..+0x17 are left untouched).
    void WriteTo(uint8_t* b, int o = 0) const;

private:
    void AddClampedTorque(const float* t);
    void ComputeVelocity();
};

} // namespace bb

#pragma once

// Port of NFSU2Unity Core/Physics/Collision/CollisionBody.cs: CollisionBody, DynamicCollisionBody and the track
// polygon body.

#include "drive/numerics/X87.h"
#include "drive/physics/collision/GjkShapes.h"
#include "drive/physics/collision/WorldCollision.h"

#include <cstdint>

namespace bb {

class CollisionWorld;
class Collision;
class RigidBody;

/// CollisionBody (base ctor sub_5A04F0, dtor sub_5A9C00; vtable layout in docs/re/world-collision.md). Only the
/// members the car-vs-world code reads are modelled; the virtual methods carry the vtable offset they port.
/// Types (+0x11): 0 car, 1 smokeable, 2 world object, 3 track polygon, 4 (not ported).
class CollisionBody {
public:
    virtual ~CollisionBody() = default;

    GjkShape* Shape = nullptr;          // +0x0C GJK volume
    uint8_t DefaultShape = 0;           // +0x10 1 = the default box (no hull found)
    uint8_t Type = 0;                   // +0x11
    uint8_t Active = 0;                 // +0x12 moves (DoTimestepMove) and starts witnesses
    int16_t Id = -1;                    // +0x14 number (pCollisionBodyNumberAllocator)
    uint8_t WitnessCount = 0;           // +0x16
    uint8_t Disabled = 0;               // +0x17
    int8_t Contacts = 0;                // +0x18 collisions in progress (BeginCollision / EndCollision)
    uint8_t Damping = 0;                // +0x19 awake ("physics mode"); written by the mover's damping logic
    float Radius = 0;                   // +0x1C
    float MinX = CollisionK::BigNegative, MinY = CollisionK::BigNegative;   // +0x20 / +0x24 plan box
    float MaxX = CollisionK::BigNegative, MaxY = CollisionK::BigNegative;   // +0x28 / +0x2C
    int WallContacts = 0;               // +0x120 (dynamic bodies): collisions with walls in progress
    int CarContacts = 0;                // +0x124 (dynamic bodies): collisions with cars in progress
    CollisionWorld* World = nullptr;    // (internal) the collision world the body is in
    bool InList = false;                // (internal)

    virtual float* GetPosition();                                     // +0x08
    virtual float* GetVelocity();                                     // +0x0C
    virtual float* GetRotation();                                     // +0x14
    virtual float* GetAngularVelocity();                              // +0x18
    virtual float* GetCenterOfGravity();                              // +0x1C
    virtual void SetPosition(const float* /*p*/) {}                   // +0x20
    virtual F24 GetMaxBackupTime() { return CollisionK::Thousand; }   // +0x28
    virtual void Move(float /*dt*/) {}                                // +0x2C
    virtual void Backup(float /*t*/) {}                               // +0x30
    virtual void Interpolate(float* pos, float* rot, float t);        // +0x34
    virtual void RecomputeAuxiliaries() {}                            // +0x38
    virtual void GetCurrentVelocities(float* lin, float* ang);        // +0x3C
    virtual void SetToPhysicsMode(CollisionBody* /*other*/) {}        // +0x48
    virtual F24 ConsiderImpulse(const float* /*j*/, const float* /*r*/) { return CollisionK::One; }   // +0x4C (CarCollisionBody's, also in the track vtable)
    virtual void ApplyImpulse(const float* /*j*/, const float* /*r*/) {}   // +0x50
    virtual bool AreCollisionsEnabled(CollisionBody* /*other*/) { return Disabled == 0; }   // +0x54
    virtual int GetTerrainType() { return 0; }                        // +0x58
    virtual float GetMassInverse() { return CollisionK::Zero; }       // +0x5C
    virtual float* GetInertiaTensorInverse() { return ZeroMatrix; }   // +0x60
    virtual bool IsMoveable() { return false; }                       // +0x64
    virtual bool IsBlinking() { return false; }                       // +0x6C
    virtual float GetBounciness(CollisionBody* /*other*/) { return X87::Bits(0x3A83126Fu); }   // +0x70 (track: flt_78435C 0.001)
    virtual float GetFriction(CollisionBody* /*other*/) { return X87::Bits(0x3A83126Fu); }     // +0x74
    virtual GjkShape* GetGjkShape(CollisionBody* /*other*/) { return Shape; }                   // +0x7C
    virtual void BeginCollision(Collision* /*c*/) { Contacts++; }    // +0x80 CollisionBody::BeginCollision 0x5A0590
    virtual void EndCollision(Collision* /*c*/) { Contacts--; }      // +0x84 CollisionBody::EndCollision 0x5929C0

    static inline float ZeroMatrix[16] = {};

    /// sub_592960(other, margin): closed plan-box overlap.
    bool Overlaps(const CollisionBody& o, float margin) const;

private:
    static inline float Zero3[4] = {};
    static inline float Identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

/// DynamicCollisionBody (ctor 0x5A9CE0): a body that moves, with a TopologyCoordinate (+0x60), the physics
/// pointers (+0x64 vehicle, +0x68 RigidBody, +0x5C enabled) and the start-of-step pose used to back up.
class DynamicCollisionBody : public CollisionBody {
public:
    void* Coordinate = nullptr;                     // +0x60 TopologyCoordinate*
    bool PhysicsEnabled = false;                    // +0x5C
    RigidBody* Rb = nullptr;                        // +0x68
    float BackupDt = 0;                             // +0x6C dt of the last BeginMove
    float StartRot[16] = {};                        // +0x70
    float StartPos[4] = {};                         // +0xB0
    float StartVel[4] = {};                         // +0xC0
    float ExtentZ = 0, ExtentRxy = 0, ExtentNegZ = 0;   // +0xD4, +0xD8, +0xD0 (BuildBoundingVolumes)
    float VolMin[3] = {}, VolMax[3] = {};           // +0xE0.., +0xF0..
    float Center[4] = {};                           // +0x100 world centre of the volume box
    float LocalCenter[4] = {};                      // +0x110

    /// GetMaxRadius 0x5A9D60 / GetMaxBackupTime 0x5A9D70.
    F24 GetMaxBackupTime() override { return BackupDt; }

    /// DynamicCollisionBody::SetPosition 0x5A9D40: writes through GetPosition.
    void SetPosition(const float* p) override;

    /// DynamicCollisionBody::BeginMove 0x593210: start-of-step rotation, position, velocity and dt.
    void BeginMove(float dt);

    /// DynamicCollisionBody::Interpolate 0x593280: the pose t seconds before the current one, linear between
    /// the start of the step and now (s = (dt - t) / dt); the current pose when dt < 1e-6.
    void Interpolate(float* pos, float* rot, float t) override;

    /// DynamicCollisionBody::Backup 0x593630: moves the RigidBody back by t: position, rotation and linear
    /// momentum = velocity * mass (angular momentum untouched).
    void Backup(float t) override;

    /// DynamicCollisionBody::GetCurrentVelocities 0x593710: the RigidBody's velocity and angular velocity.
    void GetCurrentVelocities(float* lin, float* ang) override;

    /// DynamicCollisionBody::UpdatePosition 0x5A9DE0: plan box = position +- radius (sub_5A0680 re-sorts the
    /// body list), and the volume centre = StartRot * LocalCenter + StartPos.
    void UpdatePosition();

    /// DynamicCollisionBody::BuildBoundingVolumes 0x5AE520: radius = largest |vertex| (+0x1C), the plan radius
    /// and z extents grown by 0.05, the extreme coordinates, and the local centre of the volume's box.
    void BuildBoundingVolumes();

protected:
    DynamicCollisionBody() { Active = 1; }

private:
    /// sub_593590: the velocity t seconds back (linear like Interpolate).
    void VelocityBack(float* o, float t);
};

/// A track polygon as a collision body (ctor sub_5AA0B0, dtor sub_5AEFA0, vtable 0x7A22A8): static, at the
/// origin with the identity rotation, no mass; its GJK polygon (+0x70) holds the polygon in world space.
/// Created by sub_5AE930 for the polygons near a moving body, deleted when no witness uses it.
class TrackPolygonCollisionBody : public CollisionBody {
public:
    GjkPolygon Polygon;                         // +0x70
    TrackPolygonShape* Source = nullptr;        // +0x60 expanded polygon
    bool GroupMember = false;                   // +0x64

    TrackPolygonCollisionBody() { Type = 3; Active = 0; Shape = &Polygon; }
    TrackPolygonCollisionBody(const TrackPolygonCollisionBody&) = delete;
    TrackPolygonCollisionBody& operator=(const TrackPolygonCollisionBody&) = delete;

    int GetTerrainType() override { return Source->Terrain; }                     // sub_593D90
    GjkShape* GetGjkShape(CollisionBody* /*other*/) override { return &Polygon; }  // sub_59E590
    bool AreCollisionsEnabled(CollisionBody* /*other*/) override { return Disabled == 0; }   // sub_5AA070
};

} // namespace bb

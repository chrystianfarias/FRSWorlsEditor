#pragma once

// Port of NFSU2Unity Core/Physics/RigidBody/CarCollisionBody.cs (CollisionBodyView, CarCollisionBody).

#include "drive/numerics/X87.h"
#include "drive/physics/collision/CollisionBody.h"
#include "drive/physics/collision/GjkShapes.h"

#include <cstdint>
#include <memory>

namespace bb {

class Car;
class CollisionWorld;

// What CarCollisionBody's surface getters read from the other CollisionBody: its type byte (+0x11;
// 3 = track polygon) and the z of the vector at [+0x60]+0x78 (the track polygon's normal z).
class CollisionBodyView {
public:
    uint8_t Type = 0;            // +0x11
    float NormalZ = 0;           // [+0x60]+0x78

    static CollisionBodyView Of(const CollisionBody& b);
};

// CarCollisionBody (car+0x3C, ctor 0x5AEC20, vtbl 0x7A2470): the car as a collision body. The scalar getters
// were ported first (GetMassInverse 0x593A00, GetFriction 0x5A0BE0, GetBounciness 0x5A0C90); the rest of the
// vtable used by the car-vs-world code follows (docs/re/world-collision.md).
class CarCollisionBody : public DynamicCollisionBody {
public:
    bb::Car* Car = nullptr;              // +0x130
    GjkBox* CarBox = nullptr;            // +0x134 box of the hull (used against cars)
    int LastContactTime = 0;             // +0x138 WorldTimer of the last contact
    int BlinkContactTime = 0;            // +0x13C
    int LastHitTime = 0;                 // +0x140 WorldTimer stamp of the last hit of a wall or a car (0 / 0x7FFFFFFF = none)
    float InertiaInverse[16] = {};       // +0x150 (GetInertiaTensorInverse)
    uint8_t CollideWithCars = 1;         // +0x190

    CarCollisionBody() { Type = 0; }
    CarCollisionBody(const CarCollisionBody&) = delete;
    CarCollisionBody& operator=(const CarCollisionBody&) = delete;

    // +0xD0 (BuildBoundingVolumes: 0.05 - lowest z of the hull); read by FixCarCenterOfGravity in mode 1.
    float HydraulicLow() const { return ExtentNegZ; }
    void SetHydraulicLow(float v) { ExtentNegZ = v; }
    // +0xD4 (BuildBoundingVolumes: 0.05 + highest z of the hull).
    float HydraulicHigh() const { return ExtentZ; }
    void SetHydraulicHigh(float v) { ExtentZ = v; }
    // +0x120: walls in contact (> 0: rear split 0.65 below 60 mph in TuneBoostAndDampers).
    int Field120() const { return WallContacts; }
    void SetField120(int v) { WallContacts = v; }

    // [+0x130] Car+0x424 (= CarState+0x3E4).
    int CarWheelsOnGround() const;
    void SetCarWheelsOnGround(int v);
    // [+0x130] Car+0x428 (= CarState+0x3E8).
    int Car428() const;
    void SetCar428(int v);

    // CarCollisionBody::GetMassInverse 0x593A00: rb ? min(rb.InvMass, 1/1.85) : 0 (NaN keeps invMass).
    float GetMassInverse() override;

    // CarCollisionBody::GetFriction(CollisionBody *other) 0x5A0BE0: 0.3; against a track polygon (type 3):
    // |nz| > 0.7 -> min(1, 0.3 * (0.17*(4 - wheelsOnGround) + 0*(4 - car+0x428) + 1));
    // |nz| < 0.1 -> min(1, 0.3*0.3); otherwise (or NaN) 0.3. other may be null.
    float GetFriction(const CollisionBodyView* other) const;

    // CarCollisionBody::GetBounciness(CollisionBody *other) 0x5A0C90: 0.1; 0.5*0.1 against a type-3 body with |nz| < 0.1.
    float GetBounciness(const CollisionBodyView* other) const;

    float GetFriction(CollisionBody* other) override;
    float GetBounciness(CollisionBody* other) override;

    // ------------------------------------------------------------------ vtable (car-vs-world)

    // CarCollisionBody::GetPosition 0x593B40: RigidBody+0x20, or Car+0x60 without physics.
    float* GetPosition() override;
    // GetVelocity 0x593B70: RigidBody+0xA0 / Car+0x70.
    float* GetVelocity() override;
    // GetRotation 0x593BB0: RigidBody+0x30 / Car+0x90.
    float* GetRotation() override;
    // GetAngularVelocity 0x593BD0: RigidBody+0xC0 / Car+0x110.
    float* GetAngularVelocity() override;
    // GetCenterOfGravity 0x593B60 = Car::GetCenterOfGravity 0x5ED400: RigidBody+0x1F0, or the CarPhysicsInfo CG.
    float* GetCenterOfGravity() override;

    // CarCollisionBody::SetupPhysicsPointers 0x5938C0 (movement mode 1: the PhysicsMover's RigidBody).
    void SetupPhysicsPointers();

    // CarCollisionBody::Move 0x5A9F10: BeginMove(dt); Car::Move(dt) (the mover); UpdatePosition().
    void Move(float dt) override;

    // CarCollisionBody::SetToPhysicsMode 0x593940: sub_5F4960 (switches AI/other movers to physics; nothing to
    // do for a player PhysicsMover), SetupPhysicsPointers, awake.
    void SetToPhysicsMode(CollisionBody* other) override;

    // DynamicCollisionBody::RecomputeAuxilliaries 0x593700: PhysicsVehicle vtbl+4 = PhysicsMover::ComputeAuxiliaries
    // 0x5B04D0 (component auxiliaries, BottomOut and ResolveForces).
    void RecomputeAuxiliaries() override;

    // CarCollisionBody::AreCollisionsEnabled 0x593980 (other = null: may this body start witnesses).
    bool AreCollisionsEnabled(CollisionBody* other) override;

    int GetTerrainType() override { return 0x1C; }   // CarCollisionBody::GetTerrainType 0x5AEED0

    // CarCollisionBody::IsMoveable 0x5AEE80: unless the mover is resting (GetDampState() == 2).
    bool IsMoveable() override;

    // CarCollisionBody::IsBlinking 0x5AEEA0: Car::IsBlinking or Car::IsPoofed (Car+0x998).
    bool IsBlinking() override;

    // CarCollisionBody::GetInertiaTensorInverse 0x593A40: the RigidBody's inverse tensor (yaw-scaled one when
    // +0x158 is set) times 1.0 (Car+0x38 == 1) or 0.4, rows 0..2 scaled, row 3 copied.
    float* GetInertiaTensorInverse() override;

    // CarCollisionBody::ApplyImpulse 0x5A0B00: J scaled by (1/1.85)/invMass when the inverse mass exceeds 1/1.85,
    // RigidBody::ApplyImpulseWithScaledInverseInertiaTensor(J, r, 1.0 or 0.4); AddDamage (visual car damage,
    // not modelled) and AnimateCollisionReaction (empty in U2).
    void ApplyImpulse(const float* j, const float* r) override;

    // CarCollisionBody::GetGJKPoly 0x5A0A80: the hull, or (cars of other slots with 4 wheels down, against a
    // track polygon or another such car) the box of the hull.
    GjkShape* GetGjkShape(CollisionBody* other) override;

    // The CarCollisionBody ctor 0x5AEC20 part that needs the world: hull (FindGJKPolytope of the volume name,
    // else the default "<CAR>_CV" name, else the default box), COG, the hull's box, bounding volumes, position.
    // hull is not owned (it belongs to the world data); the box and the default shape are owned by this body.
    void Attach(CollisionWorld* world, void* coordinate, GjkPolytope* hull, int worldTimer);

private:
    std::unique_ptr<GjkBox> carBoxOwned_;
    std::unique_ptr<GjkBox> defaultShapeOwned_;
};

} // namespace bb

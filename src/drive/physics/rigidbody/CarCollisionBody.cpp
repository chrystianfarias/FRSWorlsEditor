#include "drive/physics/rigidbody/CarCollisionBody.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/PhysicsMover.h"
#include "drive/physics/TrackTopology.h"
#include "drive/physics/World.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/collision/WorldCollision.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/rigidbody/RigidBodyData.h"

namespace bb {

namespace {
inline F24 F(float f) { return F24(f); }
} // namespace

CollisionBodyView CollisionBodyView::Of(const CollisionBody& b) {
    CollisionBodyView v;
    v.Type = b.Type;
    if (auto* t = dynamic_cast<const TrackPolygonCollisionBody*>(&b)) v.NormalZ = t->Source->Nz;
    return v;
}

int CarCollisionBody::CarWheelsOnGround() const { return Car->State.WheelsOnGround; }
void CarCollisionBody::SetCarWheelsOnGround(int v) { Car->State.WheelsOnGround = v; }
int CarCollisionBody::Car428() const { return Car->State.WheelsAttached; }
void CarCollisionBody::SetCar428(int v) { Car->State.WheelsAttached = v; }

float CarCollisionBody::GetMassInverse() {
    if (Rb == nullptr) return RigidBodyData::Zero;
    float c = (F(RigidBodyData::One) / RigidBodyData::CcbMaxInvMassDiv).Store();
    return BWareMath2::LeOrNaN(Rb->InvMass, c) ? Rb->InvMass : c;
}

float CarCollisionBody::GetFriction(const CollisionBodyView* other) const {
    F24 f = RigidBodyData::CcbFriction;
    if (other == nullptr || other->Type != 3) return f.Store();
    F24 nz = F24::Abs(other->NormalZ);
    F24 v;
    if (!BWareMath2::LeOrNaN(nz, RigidBodyData::CcbFlatNormalZ)) {
        F24 a = F24::MulDouble(RigidBodyData::CcbFrictionPerAirWheel,
                               static_cast<double>(static_cast<int>(4u - static_cast<uint32_t>(CarWheelsOnGround()))));   // fild; fmul
        F24 b = F24::MulDouble(RigidBodyData::CcbFriction428,
                               static_cast<double>(static_cast<int>(4u - static_cast<uint32_t>(Car428()))));
        v = a + b + RigidBodyData::One;
    } else {
        if (BWareMath2::GeOrNaN(nz, RigidBodyData::PointOne)) return f.Store();
        v = RigidBodyData::CcbWallFrictionMul;
    }
    v = v * RigidBodyData::CcbFriction;
    if (!BWareMath2::GeOrNaN(RigidBodyData::One, v)) v = RigidBodyData::One;
    return v.Store();
}

float CarCollisionBody::GetBounciness(const CollisionBodyView* other) const {
    if (other == nullptr || other->Type != 3) return RigidBodyData::PointOne;
    F24 nz = F24::Abs(other->NormalZ);
    if (BWareMath2::GeOrNaN(nz, RigidBodyData::PointOne)) return RigidBodyData::PointOne;
    return (F(RigidBodyData::CcbWallBounceMul) * RigidBodyData::PointOne).Store();
}

float CarCollisionBody::GetFriction(CollisionBody* other) {
    if (other == nullptr) return GetFriction(static_cast<const CollisionBodyView*>(nullptr));
    CollisionBodyView v = CollisionBodyView::Of(*other);
    return GetFriction(&v);
}

float CarCollisionBody::GetBounciness(CollisionBody* other) {
    if (other == nullptr) return GetBounciness(static_cast<const CollisionBodyView*>(nullptr));
    CollisionBodyView v = CollisionBodyView::Of(*other);
    return GetBounciness(&v);
}

// ------------------------------------------------------------------ vtable (car-vs-world)

float* CarCollisionBody::GetPosition() { return PhysicsEnabled ? Rb->Pos : Car->State.Pos; }
float* CarCollisionBody::GetVelocity() { return PhysicsEnabled ? Rb->Vel : Car->State.Vel; }
float* CarCollisionBody::GetRotation() { return PhysicsEnabled ? Rb->R : Car->State.R; }
float* CarCollisionBody::GetAngularVelocity() { return PhysicsEnabled ? Rb->AngVel : Car->State.AngVel; }
float* CarCollisionBody::GetCenterOfGravity() {
    return Car->Mover != nullptr && Car->Mover->Rb != nullptr ? Car->Mover->Rb->CgOffset : Car->Cpi->Cg;
}

void CarCollisionBody::SetupPhysicsPointers() {
    Rb = nullptr;
    if (!PhysicsEnabled) return;
    if (Car->MovementMode == 1 || Car->MovementMode == 3) Rb = Car->Mover != nullptr ? Car->Mover->Rb : nullptr;
}

void CarCollisionBody::Move(float dt) {
    BeginMove(dt);
    Car->Mover->Move(dt);
    UpdatePosition();
}

void CarCollisionBody::SetToPhysicsMode(CollisionBody* /*other*/) {
    SetupPhysicsPointers();
    Damping = 1;
}

void CarCollisionBody::RecomputeAuxiliaries() {
    if (PhysicsEnabled) Car->Mover->ComputeAuxiliaries();
}

bool CarCollisionBody::AreCollisionsEnabled(CollisionBody* other) {
    // (against a smokeable whose vtbl+0x8C returns 1 the game asks the smokeable first; smokeables are not ported)
    bool r = Disabled == 0 && World != nullptr && World->Topology != nullptr && World->Topology->IsConnected(Coordinate);
    if (CollideWithCars == 0 && other != nullptr && other->Type == 0) r = false;
    return r;
}

bool CarCollisionBody::IsMoveable() {
    if (Car->MovementMode != 1 && Car->MovementMode != 3) return true;
    return Car->Mover->GetDampState() != 2;
}

bool CarCollisionBody::IsBlinking() { return Car->IsBlinking(World->Clock->WorldTimer) || Car->Poofed != 0; }

float* CarCollisionBody::GetInertiaTensorInverse() {
    F24 s = Car->Mode == 1 ? F24(CollisionK::One) : F24(CollisionK::AiInertiaScale);
    const float* src = Rb->UseYawScaledInertia != 0 ? Rb->InvInertiaYaw : Rb->InvInertia;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            InertiaInverse[4 * r + c] = (F24(src[4 * r + c]) * s).Store();
    for (int c = 0; c < 4; c++) InertiaInverse[12 + c] = src[12 + c];
    return InertiaInverse;
}

void CarCollisionBody::ApplyImpulse(const float* j, const float* r) {
    float c = (F24(CollisionK::One) / CollisionK::MaxInvMassDiv).Store();
    float inv = Rb->InvMass;
    F24 scale = CollisionK::One;
    if ((BWareMath2::Fcom(inv, c) & 0x41) == 0) scale = F24(c) / inv;
    const float js[3] = {(scale * j[0]).Store(), (F24(j[1]) * scale).Store(), (F24(j[2]) * scale).Store()};
    float s = Car->Mode == 1 ? CollisionK::One : CollisionK::AiInertiaScale;
    Rb->ApplyImpulseWithScaledInverseInertiaTensor(js, r, s);
}

GjkShape* CarCollisionBody::GetGjkShape(CollisionBody* other) {
    if (Car->SlotType == 1 || CarBox == nullptr || other == nullptr || Car->State.WheelsOnGround != 4) return Shape;
    if (other->Type == 3) return CarBox;
    if (other->Type == 0) {
        auto* oc = dynamic_cast<CarCollisionBody*>(other);
        if (oc != nullptr && oc->Car->State.WheelsOnGround == 4 && oc->Car->SlotType != 1) return CarBox;
    }
    return Shape;
}

void CarCollisionBody::Attach(CollisionWorld* world, void* coordinate, GjkPolytope* hull, int worldTimer) {
    Coordinate = coordinate;
    LastContactTime = BlinkContactTime = LastHitTime = worldTimer;
    Damping = 1;
    CollideWithCars = 1;
    DefaultShape = 0;
    CarBox = nullptr;
    carBoxOwned_.reset();
    if (hull != nullptr) {
        Shape = hull;
        const float* cg = Car->Cpi->Cg;                                  // [car+0x20] (CarPhysicsInfo +0x0)
        hull->Cog[0] = cg[0]; hull->Cog[1] = cg[1]; hull->Cog[2] = cg[2];
        hull->CogSet = true;
        float min[3];
        float max[3];
        hull->GetBoundingBox(min, max);
        carBoxOwned_ = std::make_unique<GjkBox>(min[0], min[1], min[2], max[0], max[1], max[2]);
        CarBox = carBoxOwned_.get();
    } else {
        defaultShapeOwned_ = std::make_unique<GjkBox>(GjkBox::DefaultCar());
        Shape = defaultShapeOwned_.get();
        DefaultShape = 1;
    }
    PhysicsEnabled = Car->Mover != nullptr;                              // SetMovementMode: [car+0x3C]+0x5C = 1
    SetupPhysicsPointers();
    world->Add(this);
    BuildBoundingVolumes();
    UpdatePosition();
}

} // namespace bb

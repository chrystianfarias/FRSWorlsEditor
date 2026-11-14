#include "drive/physics/rigidbody/Aerodynamics.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/World.h"
#include "drive/physics/rigidbody/RigidBody.h"
#include "drive/physics/rigidbody/RigidBodyData.h"

namespace bb {

namespace {
inline F24 F(float f) { return F24(f); }
} // namespace

void Aerodynamics::ResolveForces(float* outDownforce, const RaceModeFlags& mode, const RigidBodyData& data) {
    RigidBody* rb = Rb;
    F24 vx = rb->Vel[0], vy = rb->Vel[1], vz = rb->Vel[2];
    float vzBits = rb->Vel[2];                                                   // var_54
    float s = (F24::Sqrt(vx * vx + vy * vy + vz * vz) / SpeedScale).Store();       // var_5C
    F24 k = F(TerrainDrag) * DragScale * Info->Drag * s;
    F24 q = (F(RigidBodyData::One) - Throttle) * (F(data.AeroThrottleDragMul) - RigidBodyData::One) + RigidBodyData::One;
    k = -(k * q * Enable);
    float vyNow = rb->Vel[1];
    DragForce[0] = (k * rb->Vel[0]).Store();
    DragForce[1] = (F(vyNow) * k).Store();
    DragForce[2] = (F(vzBits) * k).Store();
    float lever[3] = {};
    if (!BWareMath2::LtOrNaN(GroundFrac, RigidBodyData::Half))
        lever[2] = ((F(RigidBodyData::One) - Throttle) * data.AeroLiftLever).Store();
    rb->ApplyForce(DragForce, lever);

    F24 c = rb->R[10];                                                           // up.z (+0x58)
    if (!BWareMath2::GeOrNaN(c, RigidBodyData::Zero)) c = RigidBodyData::Zero;
    if (!BWareMath2::LtOrNaN(GroundFrac, RigidBodyData::Half)) c = RigidBodyData::One;
    F24 dx = RigidBodyData::One, dy = RigidBodyData::Zero, dz = RigidBodyData::Zero;
    if (!BWareMath2::LeOrNaN(s, RigidBodyData::AeroMinSpeed)) {
        F24 inv = F(RigidBodyData::One) / s;
        float invf = inv.Store();                                               // fst var_58
        dx = inv * rb->Vel[0];
        dy = F(rb->Vel[1]) * invf;
        dz = F(rb->Vel[2]) * invf;
    }
    F24 f = dz * rb->R[2] + dy * rb->R[1] + dx * rb->R[0];                          // dir . forward
    if (!BWareMath2::GeOrNaN(f, RigidBodyData::Zero)) f = RigidBodyData::Zero;
    if (mode.DriftAero()) f = f * f * f * f;
    else {
        f = f * f;
        if (!BWareMath2::LeOrNaN(RigidBodyData::AeroMinDownforceFactor, f)) f = RigidBodyData::AeroMinDownforceFactor;
    }
    float front, rear;
    if (BWareMath2::NeOrNaN(GroundFrac, RigidBodyData::Zero)) {
        front = (f * Info->DownforceFront * c * s).Store();                     // var_54
        rear = (f * Info->DownforceRear * c * s).Store();                       // var_58
    } else {
        F24 x = (F(Info->DownforceRear) + Info->DownforceFront) * f * c * s;
        front = (F(RigidBodyData::AeroMinDownforceFactor) * x).Store();
        rear = (x * RigidBodyData::AeroAirRearShare).Store();
    }
    const float fFront[3] = {0.0f, 0.0f, (-(F(front) * Enable)).Store()};      // var_40
    const float fRear[3] = {0.0f, 0.0f, (-(F(rear) * Enable)).Store()};        // var_30
    float force[3];
    float at[3];
    BWareMath2::MulVector3X87(force, rb->R, fFront);
    BWareMath2::MulVector3X87(at, rb->R, data.AeroFrontLever);
    rb->ApplyForce(force, at);
    BWareMath2::MulVector3X87(at, rb->R, fRear);
    BWareMath2::MulVector3X87(force, rb->R, data.AeroRearLever);
    rb->ApplyForce(at, force);
    if (outDownforce != nullptr) outDownforce[0] = (F(rear) + front).Store();
}

} // namespace bb

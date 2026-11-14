// Port of NFSU2Unity Core/Physics/Collision/Contact.cs.
#include "drive/physics/collision/Contact.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarState.h"
#include "drive/physics/collision/CollisionBody.h"
#include "drive/physics/collision/CollisionWitness.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"

#include <stdexcept>

namespace bb {

namespace {
const float IdentityM[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

float Contact::ComputeBackupTime(CollisionWorld& /*world*/)
{
    float rotA[16] = {};
    float rotB[16];
    for (int k = 0; k < 16; k++) rotB[k] = IdentityM[k];
    float posA[3] = {};
    float posB[3] = {};
    Sep[0] = Sep[1] = Sep[2] = 0.0f;
    F24 tb;
    float hi = A->GetMaxBackupTime().Store();
    tb = B->GetMaxBackupTime();
    if (BWareMath2::GeOrNaN(hi, tb)) hi = tb.Store();
    float lo = 0.0f;
    float best = CollisionK::MinusOne;
    CollisionWitness* w = Witness;
    if (w->Penetrating == 0) return CollisionK::Zero;
    float t = 0.0f;
    float result = 0.0f;
    bool touching = false;
    for (int k = 0; k < 8; k++) {
        t = ((F24(hi) - lo) * CollisionK::Half + lo).Store();
        A->Interpolate(posA, rotA, t);
        B->Interpolate(posB, rotB, t);
        w->SetTransforms(rotA, posA, rotB, posB);
        w->Update();
        if (w->Penetrating != 0) lo = t;
        else if (w->InContact == 0) {
            hi = t;
            if (!BWareMath2::NeOrNaN(best, CollisionK::MinusOne)) best = t;
            else if ((BWareMath2::Fcom(t, best) & 0x05) == 0x01) best = t;
        }
        else {
            touching = true;
            break;
        }
    }
    result = t;
    if (!touching) {
        if ((BWareMath2::Fcom(best, CollisionK::Zero) & 0x41) == 0) result = best;
        else PushOut(t);
    }
    if (A->Type == 3) static_cast<GjkPolygon*>(A->GetGjkShape(nullptr))->UseExtruded = 0;
    else if (B->Type == 3) static_cast<GjkPolygon*>(B->GetGjkShape(nullptr))->UseExtruded = 0;
    return result;
}

void Contact::PushOut(float t)
{
    float rotA[16];
    float rotB[16];
    float posA[3];
    float posB[3];
    A->Interpolate(posA, rotA, t);
    B->Interpolate(posB, rotB, t);
    float n[3] = {};
    if (A->Type == 3) Normal(static_cast<TrackPolygonCollisionBody*>(A), B, posB, n);
    else if (B->Type == 3) {
        Normal(static_cast<TrackPolygonCollisionBody*>(B), A, posA, n);
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }
    else if (A->Type == 2 || A->Type == 1 || B->Type == 2 || B->Type == 1)
        throw std::runtime_error("push-out against world objects / smokeables (sub_5B42F0 velocity branch) is not ported");
    else {
        n[0] = (F24(posB[0]) - posA[0]).Store();
        n[1] = (F24(posB[1]) - posA[1]).Store();
        n[2] = (F24(posB[2]) - posA[2]).Store();
        BWareMath2::NormalizeX87(n, n);
    }
    float dist = 0.0f, step = CollisionK::PushStep0;
    CollisionWitness* w = Witness;
    float lowZ = (-F24(CollisionK::PushZClamp)).Store();
    for (int i = 0; i < 20; i++) {
        dist = (F24(step) + dist).Store();
        if (!BWareMath2::GeOrNaN(step, CollisionK::PointOne)) step = (F24(step) + CollisionK::PushStepGrowth).Store();
        float sx = (F24(n[0]) * dist).Store();
        float sy = (F24(n[1]) * dist).Store();
        F24 z = F24(n[2]) * dist;
        if ((BWareMath2::Fcom(z, F24(lowZ)) & 0x41) != 0) z = lowZ;
        float sz = BWareMath2::GeOrNaN(CollisionK::PushZClamp, z) ? z.Store() : CollisionK::PushZClamp;
        float pa[3] = { posA[0], posA[1], posA[2] };
        float pb[3] = { posB[0], posB[1], posB[2] };
        if (A->IsMoveable()) {
            pa[0] = (F24(pa[0]) - sx).Store(); pa[1] = (F24(pa[1]) - sy).Store(); pa[2] = (F24(pa[2]) - sz).Store();
        }
        if (B->IsMoveable()) {
            pb[0] = (F24(pb[0]) + sx).Store(); pb[1] = (F24(pb[1]) + sy).Store(); pb[2] = (F24(pb[2]) + sz).Store();
        }
        w->SetTransforms(rotA, pa, rotB, pb);
        w->Update();
        Sep[0] = sx; Sep[1] = sy; Sep[2] = sz;
        if (w->InContact == 0 || w->Penetrating == 0) break;
    }
}

void Contact::Normal(TrackPolygonCollisionBody* t, CollisionBody* other, const float* otherPos, float* o)
{
    const TrackPolygonShape* src = t->Source;
    float nx = src->Nx, ny = src->Ny, nz = src->Nz;
    if (CollisionK::UseSidedBarrierNormals != 0 && !t->GroupMember) {
        o[0] = nx; o[1] = ny; o[2] = nz;
        return;
    }
    auto* poly = static_cast<GjkPolygon*>(t->GetGjkShape(nullptr));
    const float* c = poly->UseExtruded != 0 && (BWareMath2::Fcom(poly->Thickness, CollisionK::Zero) & 0x41) == 0 ? poly->Centroid : poly->FaceCentroid;
    const float* p = otherPos == nullptr || other->Type == 0 ? static_cast<DynamicCollisionBody*>(other)->Center : otherPos;
    int state = CollisionK::RememberBarrierSide == 0 ? -1 : other->Type == 1 ? Witness->BarrierSide : 0;
    if (state == 1) { o[0] = nx; o[1] = ny; o[2] = nz; return; }
    if (state == 2) { o[0] = -nx; o[1] = -ny; o[2] = -nz; return; }
    if (state > 2) return;
    F24 dx = F24(p[0]) - c[0], dy = F24(p[1]) - c[1], dz = F24(p[2]) - c[2];
    F24 s = F24(nz) * dz + dx * nx + dy * ny;
    bool front = BWareMath2::GeOrNaN(s, CollisionK::Zero);
    if (front) { o[0] = nx; o[1] = ny; o[2] = nz; }
    else { o[0] = -nx; o[1] = -ny; o[2] = -nz; }
    if (state == 0) Witness->BarrierSide = front ? 1 : 2;
}

void Contact::PointAndNormal(float* p, float* n)
{
    CollisionWitness* w = Witness;
    float ax = w->ClosestA(0), ay = w->ClosestA(1), az = w->ClosestA(2);
    float bx = w->ClosestB(0), by = w->ClosestB(1), bz = w->ClosestB(2);
    F24 sx = F24(bx) + ax, sy = F24(by) + ay;
    float sz = (F24(bz) + az).Store();
    p[0] = (sx * CollisionK::Half).Store();
    p[1] = (sy * CollisionK::Half).Store();
    p[2] = (F24(sz) * CollisionK::Half).Store();
    if (A->Active == 0) {
        const float bp[3] = { bx, by, bz };
        Normal(static_cast<TrackPolygonCollisionBody*>(A), B, bp, n);
        return;
    }
    if (B->Active == 0) {
        const float ap[3] = { ax, ay, az };
        Normal(static_cast<TrackPolygonCollisionBody*>(B), A, ap, n);
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
        return;
    }
    F24 dx = F24(bx) - ax, dy = F24(by) - ay;
    float dz = (F24(bz) - az).Store();
    float dxs = dx.Store();
    n[0] = dxs; n[1] = dy.Store(); n[2] = dz;
    F24 len = F24::Sqrt(F24(dz) * dz + dy * dy + F24(dxs) * dxs);
    if (BWareMath2::GeOrNaN(len, CollisionK::Eps1e6)) {
        F24 inv = F24(CollisionK::One) / len;
        n[0] = (F24(dxs) * inv).Store();
        n[1] = (dy * inv).Store();
        n[2] = (F24(dz) * inv).Store();
        return;
    }
    const float* pa = A->GetPosition();
    const float* pb = B->GetPosition();
    n[0] = (F24(pb[0]) - pa[0]).Store(); n[1] = (F24(pb[1]) - pa[1]).Store(); n[2] = (F24(pb[2]) - pa[2]).Store();
    BWareMath2::NormalizeX87(n, n);
}

bool Contact::ComesAfter(Contact& a, Contact& b)
{
    F24 diff = F24::Abs(F24(a.Time) - b.Time);
    if (BWareMath2::GeOrNaN(diff, CollisionK::ContactTimeTolerance))
        return (BWareMath2::Fcom(a.Time, b.Time) & 0x41) != 0;
    bool fa, fb;
    float da = a.Depth(fa);
    float db = b.Depth(fb);
    if (!fa || !fb) return true;
    return (BWareMath2::Fcom(da, db) & 0x41) != 0;
}

float Contact::Depth(bool& valid)
{
    float n[3];
    if (A->Type == 3) {
        if (B->Active == 0) { valid = false; return CollisionK::Zero; }
        Normal(static_cast<TrackPolygonCollisionBody*>(A), B, nullptr, n);
        valid = true;
        const float* p = B->GetPosition();
        return (F24(n[1]) * p[1] + F24(n[2]) * p[2] + F24(n[0]) * p[0]).Store();
    }
    if (B->Type == 3 && A->Active != 0) {
        Normal(static_cast<TrackPolygonCollisionBody*>(B), A, nullptr, n);
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
        valid = true;
        const float* p = A->GetPosition();
        return (F24(n[2]) * p[2] + F24(n[1]) * p[1] + F24(n[0]) * p[0]).Store();
    }
    valid = false;
    return CollisionK::Zero;
}

void Contact::Resolve(CollisionWorld& world)
{
    A->SetToPhysicsMode(B);
    B->SetToPhysicsMode(A);
    A->Backup(Time);
    B->Backup(Time);
    const float* pa0 = A->GetPosition();
    float pa[3] = { pa0[0], pa0[1], pa0[2] };
    const float* pb0 = B->GetPosition();
    float pb[3] = { pb0[0], pb0[1], pb0[2] };
    CollisionBody* keepZ = nullptr;
    if (A->Type == 0 && B->Type == 1 && B->WallContacts > 0) keepZ = A;
    else if (B->Type == 0 && A->Type == 1 && A->WallContacts > 0) keepZ = B;
    if (A->IsMoveable()) {
        pa[0] = (F24(pa[0]) - Sep[0]).Store(); pa[1] = (F24(pa[1]) - Sep[1]).Store(); pa[2] = (F24(pa[2]) - Sep[2]).Store();
        if (A == keepZ) pa[2] = A->GetPosition()[2];
    }
    if (B->IsMoveable()) {
        pb[0] = (F24(pb[0]) + Sep[0]).Store(); pb[1] = (F24(Sep[1]) + pb[1]).Store(); pb[2] = (F24(Sep[2]) + pb[2]).Store();
        if (B == keepZ) pb[2] = B->GetPosition()[2];
    }
    A->SetPosition(pa);
    B->SetPosition(pb);
    A->RecomputeAuxiliaries();
    B->RecomputeAuxiliaries();
    CollisionImpulse(world);
    A->RecomputeAuxiliaries();
    B->RecomputeAuxiliaries();
    A->Move(Time);
    B->Move(Time);
}

void Contact::Cross(float* o, const float* a, const float* b) { BWareMath::CrossX87(o, a, b); }

void Contact::CollisionImpulse(CollisionWorld& world)
{
    float P[3];
    float N[3];
    PointAndNormal(P, N);
    CollisionWitness* w = Witness;
    if (w->Collision == nullptr) world.BeginCollision(w, P);
    bb::Collision* coll = w->Collision;
    const float* pa = A->GetPosition();
    float rA[3] = { (F24(P[0]) - pa[0]).Store(), (F24(P[1]) - pa[1]).Store(), (F24(P[2]) - pa[2]).Store() };
    const float* pb = B->GetPosition();
    float rB[3] = { (F24(P[0]) - pb[0]).Store(), (F24(P[1]) - pb[1]).Store(), (F24(P[2]) - pb[2]).Store() };
    float vA[3], wA[3], vB[3], wB[3];
    float t1[3], t2[3];
    while (true) {
        A->GetCurrentVelocities(vA, wA);
        B->GetCurrentVelocities(vB, wB);
        Cross(t1, wA, rA);
        const float uA[3] = { (F24(t1[0]) + vA[0]).Store(), (F24(t1[1]) + vA[1]).Store(), (F24(t1[2]) + vA[2]).Store() };
        Cross(t2, wB, rB);
        float uBx = (F24(t2[0]) + vB[0]).Store();
        F24 uBy = F24(t2[1]) + vB[1];
        float uBz = (F24(t2[2]) + vB[2]).Store();
        const float urel[3] = { (F24(uBx) - uA[0]).Store(), (uBy - uA[1]).Store(), (F24(uBz) - uA[2]).Store() };
        F24 vnF = F24(urel[0]) * N[0] + F24(N[2]) * urel[2] + F24(N[1]) * urel[1];
        float vn = vnF.Store();
        float ut[3];
        BWareMath2::ScaleAdd(ut, urel, N, (-vnF).Store());
        if ((BWareMath2::Fcom(vn, CollisionK::Zero) & 0x01) == 0) return;          // separating
        float eA = A->GetBounciness(B);
        float eB = B->GetBounciness(A);
        float e;
        if (A->Type == 0 && B->Type == 3) e = eA;
        else if (B->Type == 0 && A->Type == 3) e = eB;
        else e = (BWareMath2::Fcom(eA, eB) & 0x41) != 0 ? eB : eA;
        float mA = A->GetMassInverse();
        float mB = B->GetMassInverse();
        float jn = (-((F24(e) + CollisionK::One) * vn)).Store();
        const float* IA = A->GetInertiaTensorInverse();
        float c1[3], c1r[3], c2[3];
        Cross(c1, rA, N);
        BWareMath2::MulVector3X87(c1r, IA, c1);
        Cross(c2, c1r, rA);
        float denom = (F24(mB) + mA + F24(c2[0]) * N[0] + F24(c2[2]) * N[2] + F24(c2[1]) * N[1]).Store();
        const float* IB = B->GetInertiaTensorInverse();
        float c3[3], c3r[3], c4[3];
        Cross(c3, rB, N);
        BWareMath2::MulVector3X87(c3r, IB, c3);
        Cross(c4, c3r, rB);
        denom = (F24(c4[0]) * N[0] + F24(c4[2]) * N[2] + F24(c4[1]) * N[1] + denom).Store();
        float m = (BWareMath2::Fcom(mA, mB) & 0x41) != 0 ? mB : mA;
        F24 jmin = F24(CollisionK::MinImpulseInvMass) / m;
        F24 jF = F24(jn) / denom;
        float j = jF.Store();
        if (!BWareMath2::GeOrNaN(j, jmin)) j = jmin.Store();
        const float J[3] = { (F24(N[0]) * j).Store(), (F24(N[1]) * j).Store(), (F24(N[2]) * j).Store() };
        F24 kB = B->ConsiderImpulse(J, rB);
        const float Jn[3] = { -J[0], -J[1], -J[2] };
        F24 kA = A->ConsiderImpulse(Jn, rA);
        if (!BWareMath2::NeOrNaN(kA, CollisionK::NoImpulse)) return;
        if (!BWareMath2::NeOrNaN(kB, CollisionK::NoImpulse)) return;
        bool again = false;
        if (!BWareMath2::GeOrNaN(kA, CollisionK::Zero)) kA = -kA;
        else if (!BWareMath2::GeOrNaN(kB, CollisionK::Zero)) kB = -kB;
        else again = true;
        float k = BWareMath2::GeOrNaN(kA, kB) ? kB.Store() : kA.Store();
        float jk = (F24(k) * j).Store();
        F24 f = Friction(A, B);
        float utLen = F24::Sqrt(F24(ut[0]) * ut[0] + F24(ut[2]) * ut[2] + F24(ut[1]) * ut[1]).Store();
        F24 ff = f * jk;
        if (!BWareMath2::GeOrNaN(utLen, CollisionK::FrictionSpeed)) ff = ff / CollisionK::FrictionSpeed * utLen;
        F24 fx, fz;
        float fy;
        if ((BWareMath2::Fcom(utLen, CollisionK::Eps1e5) & 0x41) == 0) {
            float sf = (F24(CollisionK::MinusOne) / utLen * ff).Store();
            fx = F24(ut[0]) * sf;
            fy = (F24(ut[1]) * sf).Store();
            fz = F24(ut[2]) * sf;
        }
        else {
            fy = CollisionK::Zero;
            fx = CollisionK::Zero;
            fz = CollisionK::Zero;
        }
        const float JN[3] = { (F24(N[0]) * jk).Store(), (F24(N[1]) * jk).Store(), (F24(N[2]) * jk).Store() };
        CollisionWitness* cw = coll->Witness;
        if ((cw->A->Type == 3 && !BWareMath2::GeOrNaN(F24::Abs(static_cast<TrackPolygonCollisionBody*>(cw->A)->Source->Nz), CollisionK::PointOne))
            || (cw->B->Type == 3 && !BWareMath2::GeOrNaN(F24::Abs(static_cast<TrackPolygonCollisionBody*>(cw->B)->Source->Nz), CollisionK::PointOne)))
            fz = fz * CollisionK::WallFrictionScale;
        float It[3] = { (fx + JN[0]).Store(), (F24(fy) + JN[1]).Store(), (F24(JN[2]) + fz).Store() };
        float Im[3] = { -It[0], -It[1], -It[2] };
        coll->AddImpulse(world, P, Im, utLen);
        if (coll->Wild != 0) ProjectOutWheelsDown(rA, rB);
        CollisionBody* keepZ = nullptr;
        if (A->Type == 0 && B->Type == 1 && B->WallContacts > 0) keepZ = A;
        else if (B->Type == 0 && A->Type == 1 && A->WallContacts > 0) keepZ = B;
        if (A == keepZ) Im[2] = DownScale(Im[2]);
        else if (B == keepZ) It[2] = DownScale(It[2]);
        B->ApplyImpulse(It, rB);
        A->ApplyImpulse(Im, rA);
        if (!BWareMath2::NeOrNaN(F24(k), CollisionK::One)) return;
        if (again) continue;
        return;
    }
}

float Contact::DownScale(float z)
{
    F24 v = F24(z) * CollisionK::SmokeableDownScale;
    return (BWareMath2::Fcom(F24(CollisionK::Zero), v) & 0x41) != 0 ? v.Store() : CollisionK::Zero;
}

void Contact::ProjectOutWheelsDown(float* rA, float* rB)
{
    Project(A, rA);
    Project(B, rB);
}

void Contact::Project(CollisionBody* b, float* r)
{
    auto* c = dynamic_cast<CarCollisionBody*>(b);
    if (c == nullptr || c->Car->State.WheelsOnGround != 4) return;
    const float* up = c->Car->State.R;                       // Car+0xB0 (CarState+0x70): rotation row 2
    float ux = up[8], uy = up[9], uz = up[10];
    float d = (F24(ux) * r[0] + F24(uz) * r[2] + F24(uy) * r[1]).Store();
    float dx = (F24(ux) * d).Store();
    F24 dy = F24(uy) * d, dz = F24(uz) * d;
    r[0] = (F24(r[0]) - dx).Store();
    r[1] = (F24(r[1]) - dy).Store();
    r[2] = (F24(r[2]) - dz).Store();
}

F24 Contact::Friction(CollisionBody* a, CollisionBody* b)
{
    float fa = a->GetFriction(b);
    float fb = b->GetFriction(a);
    if (a->Type == 0 && b->Type == 3) return fa;
    if (b->Type == 0 && a->Type == 3) return fb;
    return (BWareMath2::Fcom(fa, fb) & 0x41) != 0 ? fb : fa;
}

} // namespace bb

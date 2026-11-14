// Port of NFSU2Unity Core/Physics/Collision/CollisionWitness.cs.
#include "drive/physics/collision/CollisionWitness.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/World.h"
#include "drive/physics/collision/CollisionBody.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/collision/Contact.h"
#include "drive/physics/rigidbody/CarCollisionBody.h"

namespace bb {

void CollisionWitness::SetShapes(GjkShape* a, GjkShape* b)
{
    if (a == ShapeA && b == ShapeB) return;
    InContact = 0;
    Penetrating = 0;
    Dirty = 1;
    ShapeA = a;
    ShapeB = b;
    if (auto* pa = dynamic_cast<GjkPolygon*>(a)) OneSided = Extruded(*pa);
    else if (auto* pb = dynamic_cast<GjkPolygon*>(b)) OneSided = Extruded(*pb);
}

uint8_t CollisionWitness::Extruded(const GjkPolygon& p)
{
    return static_cast<uint8_t>(p.UseExtruded != 0 && (BWareMath2::Fcom(p.Thickness, CollisionK::Zero) & 0x41) == 0 ? 1 : 0);
}

void CollisionWitness::SetTransforms(const float* rotA, const float* posA, const float* rotB, const float* posB)
{
    if (!(CollisionK::Equal3(posA, 0, LastPosA, 0, CollisionK::Eps1e3) && CollisionK::Equal3(posB, 0, LastPosB, 0, CollisionK::Eps1e3)))
        Dirty = 1;
    if (Dirty == 0) return;
    BWareMath2::CopyMatrix(MatA, rotA);
    BWareMath2::TransposeMatrix(MatAT, rotA);
    MatA[12] = posA[0]; MatA[13] = posA[1]; MatA[14] = posA[2]; MatA[15] = CollisionK::One;
    BWareMath2::CopyMatrix(MatB, rotB);
    BWareMath2::TransposeMatrix(MatBT, rotB);
    MatB[12] = posB[0]; MatB[13] = posB[1]; MatB[14] = posB[2]; MatB[15] = CollisionK::One;
    LastPosA[0] = posA[0]; LastPosA[1] = posA[1]; LastPosA[2] = posA[2];
    LastPosB[0] = posB[0]; LastPosB[1] = posB[1]; LastPosB[2] = posB[2];
}

F24 CollisionWitness::Update()
{
    if (Dirty == 0) return LastDistance;
    auto* pa = dynamic_cast<GjkPolygon*>(ShapeA);
    auto* pb = dynamic_cast<GjkPolygon*>(ShapeB);
    if (pa != nullptr && Extruded(*pa) != OneSided) {
        OneSided = Extruded(*pa);
        Simplex.SetInitialised(false);
    }
    else if (pb != nullptr &&Extruded(*pb) != OneSided) {
        OneSided = Extruded(*pb);
        Simplex.SetInitialised(false);
    }
    F24 d = Gjk();
    LastDistance = d.Store();
    Penetrating = static_cast<uint8_t>(X87::ParityEven(BWareMath2::Fcom(d, F24(CollisionK::GjkZero)) & 0x41) ? 0 : 1);
    InContact = static_cast<uint8_t>(X87::ParityEven(BWareMath2::Fcom(d, F24(CollisionK::GjkContact)) & 0x41) ? 0 : 1);
    Dirty = 0;
    return d;
}

F24 CollisionWitness::Gjk()
{
    GjkSimplex& s = Simplex;
    GjkVertex va;
    GjkVertex vb;
    float d[3] = {};
    float nd[3] = {};
    float dist2;
    float t[3];
    if (!s.Initialised()) {
        Vertex(ShapeA, 0, va);
        { const float p[3] = { va.X, va.Y, va.Z }; BWareMath2::MulVector3X87(t, MatA, p); }
        va.X = t[0]; va.Y = t[1]; va.Z = t[2];
        Vertex(ShapeB, 0, vb);
        { const float p[3] = { vb.X, vb.Y, vb.Z }; BWareMath2::MulVector3X87(t, MatB, p); }
        vb.X = t[0]; vb.Y = t[1]; vb.Z = t[2];
        d[0] = (F24(vb.X) - va.X).Store();
        d[1] = (F24(vb.Y) - va.Y).Store();
        d[2] = (F24(vb.Z) - va.Z).Store();
        nd[0] = -d[0]; nd[1] = -d[1]; nd[2] = -d[2];
        ShapeA->Support(d, va, MatA, MatAT);
        ShapeB->Support(nd, vb, MatB, MatBT);
        s.SetPointA(0, va.X, va.Y, va.Z);
        s.SetPointB(0, vb.X, vb.Y, vb.Z);
        s.SetI(0xB0, va.Id);
        s.SetI(0xC4, vb.Id);
        s.SetCountB(1);
        s.SetCountA(1);
        for (int k = 0; k < 3; k++) { s.SetF(0x80 + 4 * k, s.F(4 * k)); s.SetF(0x90 + 4 * k, s.F(0x40 + 4 * k)); }
        d[0] = (F24(s.F(0x90)) - s.F(0x80)).Store();
        d[1] = (F24(s.F(0x94)) - s.F(0x84)).Store();
        d[2] = (F24(s.F(0x98)) - s.F(0x88)).Store();
        dist2 = (F24(d[0]) * d[0] + F24(d[2]) * d[2] + F24(d[1]) * d[1]).Store();
        if (!X87::ParityEven(BWareMath2::Fcom(dist2, CollisionK::GjkZero) & 0x41)) return CollisionK::Zero;
        s.SetInitialised(true);
    }
    else {
        for (int i = 0; i < s.CountA(); i++) {
            Vertex(ShapeA, s.IdA(i), va);
            if (ShapeA->Type() != 2) {
                const float p[3] = { va.X, va.Y, va.Z };
                BWareMath2::MulVector3X87(t, MatA, p);
                s.SetPointA(i, t[0], t[1], t[2]);
            }
            else s.SetPointA(i, va.X, va.Y, va.Z);
        }
        for (int i = 0; i < s.CountB(); i++) {
            Vertex(ShapeB, s.IdB(i), vb);
            if (ShapeB->Type() != 2) {
                const float p[3] = { vb.X, vb.Y, vb.Z };
                BWareMath2::MulVector3X87(t, MatB, p);
                s.SetPointB(i, t[0], t[1], t[2]);
            }
            else s.SetPointB(i, vb.X, vb.Y, vb.Z);
        }
        dist2 = GjkSimplexSolver::Solve(s, d).Store();
        if (!X87::ParityEven(BWareMath2::Fcom(dist2, CollisionK::GjkZero) & 0x41)) return CollisionK::Zero;
    }
    float best = dist2;
    GjkSimplex saved;
    saved.CopyFrom(s);
    while (true) {
        nd[0] = -d[0]; nd[1] = -d[1]; nd[2] = -d[2];
        ShapeA->Support(d, va, MatA, MatAT);
        ShapeB->Support(nd, vb, MatB, MatBT);
        bool newA = true;
        for (int i = 0; i < s.CountA(); i++) if (s.IdA(i) == va.Id) { newA = false; break; }
        if (newA) {
            int n = s.CountA();
            s.SetI(0xB0 + 4 * n, va.Id);
            s.SetPointA(n, va.X, va.Y, va.Z);
            s.SetCountA(n + 1);
        }
        bool newB = true;
        for (int i = 0; i < s.CountB(); i++) if (s.IdB(i) == vb.Id) { newB = false; break; }
        if (newB) {
            int n = s.CountB();
            s.SetI(0xC4 + 4 * n, vb.Id);
            s.SetPointB(n, vb.X, vb.Y, vb.Z);
            s.SetCountB(n + 1);
        }
        if (!newA && !newB) break;
        dist2 = GjkSimplexSolver::Solve(s, d).Store();
        if (!BWareMath2::NeOrNaN(dist2, CollisionK::Zero)) return CollisionK::Zero;   // test ah,44h; jnp: equal
        if ((BWareMath2::Fcom(dist2, best) & 0x01) != 0) {
            best = dist2;
            saved.CopyFrom(s);
            continue;
        }
        dist2 = best;
        s.CopyFrom(saved);
        break;
    }
    float dir[3] = { s.F(0xA0), s.F(0xA4), s.F(0xA8) };
    BWareMath2::NormalizeX87(dir, dir);
    s.SetF(0xA0, dir[0]); s.SetF(0xA4, dir[1]); s.SetF(0xA8, dir[2]);
    return F24::Sqrt(dist2);
}

void CollisionWitness::TestForContact(CollisionWorld& world, std::vector<std::unique_ptr<Contact>>& contacts)
{
    if (A->Type == 4 && B->Type != 0 && B->Type != 1) return;
    if (B->Type == 4 && A->Type != 0 && A->Type != 1) return;
    if (!A->AreCollisionsEnabled(B)) return;
    if (!B->AreCollisionsEnabled(A)) return;
    // sub_592F00 only acts on track polygon vs smokeable (type 1) pairs: not ported (no smokeables)
    if (auto* ha = dynamic_cast<GjkPolytope*>(ShapeA)) {
        const float* c = A->GetCenterOfGravity();
        ha->Cog[0] = c[0]; ha->Cog[1] = c[1]; ha->Cog[2] = c[2];
        ha->CogSet = true;
    }
    if (auto* hb = dynamic_cast<GjkPolytope*>(ShapeB)) {
        const float* c = B->GetCenterOfGravity();
        hb->Cog[0] = c[0]; hb->Cog[1] = c[1]; hb->Cog[2] = c[2];
        hb->CogSet = true;
    }
    SetTransforms(A->GetRotation(), A->GetPosition(), B->GetRotation(), B->GetPosition());
    Update();
    // sub_593010
    if (A->Type == 3) static_cast<GjkPolygon*>(A->GetGjkShape(nullptr))->UseExtruded = 0;
    else if (B->Type == 3) static_cast<GjkPolygon*>(B->GetGjkShape(nullptr))->UseExtruded = 0;
    if (InContact == 0) return;
    int now = world.Clock->WorldTimer;
    if ((A->IsBlinking() || B->IsBlinking()) && A->Active != 0 && B->Active != 0) {
        if (auto* ca0 = dynamic_cast<CarCollisionBody*>(A)) ca0->BlinkContactTime = now;
        if (auto* cb0 = dynamic_cast<CarCollisionBody*>(B)) cb0->BlinkContactTime = now;
        return;
    }
    if (auto* ca = dynamic_cast<CarCollisionBody*>(A)) {
        if (HitWorthy(B)) ca->LastHitTime = now;
        ca->LastContactTime = now;
    }
    if (auto* cb = dynamic_cast<CarCollisionBody*>(B)) {
        if (HitWorthy(A)) cb->LastHitTime = now;
        cb->LastContactTime = now;
    }
    auto contact = std::make_unique<Contact>(A, B, this);
    contact->Time = contact->ComputeBackupTime(world);
    contact->LoopCounter = world.Clock->WorldLoopCounter;
    size_t at = contacts.size();
    for (size_t i = 0; i < contacts.size(); i++)
        if (!Contact::ComesAfter(*contact, *contacts[i])) { at = i; break; }
    contacts.insert(contacts.begin() + static_cast<std::ptrdiff_t>(at), std::move(contact));
}

bool CollisionWitness::HitWorthy(CollisionBody* b)
{
    if (b->Type <= 2) return true;
    if (b->Type == 3) return (static_cast<TrackPolygonCollisionBody*>(b)->Source->Flags & 2) != 0;
    return false;
}

} // namespace bb

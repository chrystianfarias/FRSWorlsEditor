// Port of NFSU2Unity Core/Physics/Collision/CollisionWorld.cs.
#include "drive/physics/collision/CollisionWorld.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/World.h"
#include "drive/physics/collision/Contact.h"

#include <algorithm>
#include <stdexcept>

namespace bb {

// ---------------------------------------------------------------------------------------------- Collision

bool Collision::IsWallInvolved() const
{
    const CollisionWitness* w = Witness;
    if (w->A->Type == 3 && !BWareMath2::GeOrNaN(F24::Abs(static_cast<TrackPolygonCollisionBody*>(w->A)->Source->Nz), CollisionK::PointOne)) return true;
    if (w->B->Type == 3 && !BWareMath2::GeOrNaN(F24::Abs(static_cast<TrackPolygonCollisionBody*>(w->B)->Source->Nz), CollisionK::PointOne)) return true;
    return false;
}

bool Collision::IsFlatGroundInvolved() const
{
    const CollisionWitness* w = Witness;
    if (w->A->Type == 3 && (BWareMath2::Fcom(F24::Abs(static_cast<TrackPolygonCollisionBody*>(w->A)->Source->Nz), F24(CollisionK::FlatNormalZ)) & 0x41) == 0) return true;
    if (w->B->Type == 3 && (BWareMath2::Fcom(F24::Abs(static_cast<TrackPolygonCollisionBody*>(w->B)->Source->Nz), F24(CollisionK::FlatNormalZ)) & 0x41) == 0) return true;
    return false;
}

void Collision::AddImpulse(CollisionWorld& world, const float* point, const float* impulse, float speed)
{
    ImpulseSum[0] = (F24(ImpulseSum[0]) + impulse[0]).Store();
    ImpulseSum[1] = (F24(impulse[1]) + ImpulseSum[1]).Store();
    ImpulseSum[2] = (F24(impulse[2]) + ImpulseSum[2]).Store();
    F24 mag = F24::Sqrt(F24(impulse[0]) * impulse[0] + F24(impulse[2]) * impulse[2] + F24(impulse[1]) * impulse[1]);
    MagnitudeSum = (mag + MagnitudeSum).Store();
    LastTime = world.Clock->WorldTimer;
    Point[0] = point[0]; Point[1] = point[1]; Point[2] = point[2];
    Speed = speed;
    if (Witness->A->Type == 0 && Witness->B->Type == 0)
        throw std::runtime_error("car-vs-car collisions (CheckForWildCollision 0x59FB70) are not ported");
    float d[3];
    BWareMath2::NormalizeX87(d, impulse);
    Direction[0] = d[0]; Direction[1] = d[1]; Direction[2] = d[2];
    world.RaiseImpact(*this, point, impulse, mag.Store());
}

bool Collision::Update(int worldTimer) const
{
    F24 age = F24::MulDouble(F24(CollisionK::TickToSec),
                             static_cast<double>(static_cast<int>(static_cast<uint32_t>(worldTimer) - static_cast<uint32_t>(LastTime))));
    return !BWareMath2::GeOrNaN(age, CollisionK::CollisionLifetime);
}

// ---------------------------------------------------------------------------------------------- CollisionWorld

CollisionWorld::CollisionWorld(World* clock, IWorldCollision* worldData, ITrackTopology* topology)
    : Clock(clock), WorldData(worldData), Topology(topology),
      witnessBits(static_cast<size_t>(CollisionK::MaxBodies) * CollisionK::MaxBodies, 0)
{
}

CollisionWorld::~CollisionWorld()
{
    for (auto& t : trackBodies) if (t->Source != nullptr) t->Source->Body = nullptr;
    for (CollisionBody* b : bodies) {
        b->World = nullptr;
        b->InList = false;
        b->WitnessCount = 0;
    }
}

void CollisionWorld::RaiseImpact(const Collision& c, const float* p, const float* imp, float mag)
{
    if (!Impact) return;
    const CollisionWitness* w = c.Witness;
    TrackPolygonCollisionBody* track = dynamic_cast<TrackPolygonCollisionBody*>(w->A);
    if (track == nullptr) track = dynamic_cast<TrackPolygonCollisionBody*>(w->B);
    CollisionImpact h;
    h.X = p[0]; h.Y = p[1]; h.Z = p[2];
    h.ImpulseX = imp[0]; h.ImpulseY = imp[1]; h.ImpulseZ = imp[2];
    h.Magnitude = mag;
    h.Terrain = track != nullptr ? track->Source->Terrain : 0;
    h.Wall = c.IsWallInvolved();
    h.PolygonFlags = track != nullptr ? track->Source->Flags : static_cast<uint8_t>(0);
    Impact(h);
}

// ------------------------------------------------------------------ body numbers, list

void CollisionWorld::MarkId(int id)
{
    if (nextId == id) {
        int k = id + 1;
        while (k < CollisionK::MaxBodies && ids[k]) k++;
        nextId = k;
    }
    ids[id] = true;
}

void CollisionWorld::FreeId(int id)
{
    ids[id] = false;
    if (id < nextId) nextId = id;
}

void CollisionWorld::Add(CollisionBody* b)
{
    if (b->InList) return;
    b->World = this;
    b->Id = static_cast<int16_t>(nextId);
    if (b->Id >= CollisionK::MaxBodies) throw std::runtime_error("collision body numbers exhausted");
    MarkId(b->Id);
    b->MinX = b->MinY = b->MaxX = b->MaxY = CollisionK::BigNegative;
    bodies.insert(bodies.begin(), b);
    b->InList = true;
}

void CollisionWorld::SetBox(CollisionBody* b, float minX, float minY, float maxX, float maxY)
{
    float width = (F24(maxX) - minX).Store();
    UpdateMaxWidth(b, width);
    float old = b->MaxX;
    b->MinX = minX; b->MinY = minY; b->MaxY = maxY; b->MaxX = maxX;
    if (!BWareMath2::NeOrNaN(old, maxX)) return;
    auto it = std::find(bodies.begin(), bodies.end(), b);
    if (it == bodies.end()) return;
    int i = static_cast<int>(it - bodies.begin());
    int n = static_cast<int>(bodies.size());
    while (i > 0 && Swap(bodies[i - 1], b)) { bodies[i] = bodies[i - 1]; bodies[i - 1] = b; i--; }
    while (i < n - 1 && Swap(b, bodies[i + 1])) { bodies[i] = bodies[i + 1]; bodies[i + 1] = b; i++; }
}

bool CollisionWorld::Swap(const CollisionBody* first, const CollisionBody* second)
{
    int c = BWareMath2::Fcom(first->MaxX, second->MaxX);
    if ((c & 0x41) == 0) return true;                     // first.maxX > second.maxX
    if ((c & 0x05) == 0x01) return false;                 // less
    return first->Id > second->Id;
}

void CollisionWorld::UpdateMaxWidth(CollisionBody* b, float width)
{
    float my = (F24(b->MaxX) - b->MinX).Store();
    if (!BWareMath2::NeOrNaN(width, my)) return;
    if ((BWareMath2::Fcom(width, my) & 0x41) == 0 && (BWareMath2::Fcom(width, MaxWidth) & 0x41) == 0) {
        MaxWidth = width;
        return;
    }
    if (BWareMath2::GeOrNaN(width, my)) return;
    if (BWareMath2::NeOrNaN(my, MaxWidth)) return;
    float best = 0.0f;
    for (CollisionBody* o : bodies) {
        if (o == b) continue;
        F24 w = F24(o->MaxX) - o->MinX;
        if ((BWareMath2::Fcom(w, F24(best)) & 0x41) == 0) best = w.Store();
    }
    MaxWidth = best;
}

void CollisionWorld::RemoveBody(CollisionBody* b)
{
    UpdateMaxWidth(b, 0.0f);
    bodies.erase(std::remove(bodies.begin(), bodies.end(), b), bodies.end());
    b->InList = false;
    if (b->WitnessCount != 0) {
        std::vector<CollisionWitness*> snapshot;
        for (auto& w : witnesses) snapshot.push_back(w.get());
        for (CollisionWitness* w : snapshot)
            if (w->A == b || w->B == b) DeleteWitness(w);
    }
    if (b->Id >= 0) FreeId(b->Id);
}

// ------------------------------------------------------------------ witnesses

bool CollisionWorld::WitnessExists(const CollisionBody* a, const CollisionBody* b) const
{
    return a->Id >= 0 && b->Id >= 0 && witnessBits[static_cast<size_t>(a->Id) * CollisionK::MaxBodies + b->Id] != 0;
}

CollisionWitness* CollisionWorld::NewWitness(CollisionBody* a, CollisionBody* b)
{
    if (static_cast<int>(witnesses.size()) >= CollisionK::MaxWitnesses) return nullptr;   // CollisionBodyWitnessSlotPool full: bOMalloc returns 0
    auto w = std::make_unique<CollisionWitness>();
    w->A = a;
    w->B = b;
    w->Simplex.SetInitialised(false);
    GjkShape* sb = b->GetGjkShape(a);
    GjkShape* sa = a->GetGjkShape(b);
    w->SetShapes(sa, sb);
    CollisionWitness* r = w.get();
    witnesses.push_back(std::move(w));
    witnessBits[static_cast<size_t>(a->Id) * CollisionK::MaxBodies + b->Id] = 1;
    witnessBits[static_cast<size_t>(b->Id) * CollisionK::MaxBodies + a->Id] = 1;
    a->WitnessCount++;
    b->WitnessCount++;
    return r;
}

void CollisionWorld::DeleteWitness(CollisionWitness* w)
{
    if (w->Collision != nullptr) EndCollision(w);
    w->A->WitnessCount--;
    w->B->WitnessCount--;
    witnessBits[static_cast<size_t>(w->A->Id) * CollisionK::MaxBodies + w->B->Id] = 0;
    witnessBits[static_cast<size_t>(w->B->Id) * CollisionK::MaxBodies + w->A->Id] = 0;
    auto it = std::find_if(witnesses.begin(), witnesses.end(), [w](const std::unique_ptr<CollisionWitness>& x) { return x.get() == w; });
    if (it != witnesses.end()) witnesses.erase(it);    // frees w
}

bool CollisionWorld::ContainsWitness(const CollisionWitness* w) const
{
    for (auto& x : witnesses) if (x.get() == w) return true;
    return false;
}

bool CollisionWorld::Expired(const CollisionWitness& w)
{
    if (w.Collision != nullptr) return false;
    if (w.A->Overlaps(*w.B, 0.0f) && (w.A->Damping != 0 || w.B->Damping != 0) && w.A->AreCollisionsEnabled(w.B) && w.B->AreCollisionsEnabled(w.A))
        return false;
    return true;
}

// ------------------------------------------------------------------ collisions in progress

void CollisionWorld::BeginCollision(CollisionWitness* w, const float* point)
{
    auto owned = std::make_unique<Collision>();
    Collision* c = owned.get();
    c->Witness = w;
    collisions.push_back(std::move(owned));
    if (point != nullptr) { c->Point0[0] = point[0]; c->Point0[1] = point[1]; c->Point0[2] = point[2]; }
    c->StartTime = c->LastTime = Clock->WorldTimer;
    if (c->IsWallInvolved()) {
        CollisionBody* b = w->A->Type == 3 ? w->B : w->A;
        if (b != nullptr) b->WallContacts++;
    }
    else if (w->A->Type == 0 && w->B->Type == 0) {
        w->A->CarContacts++;
        w->B->CarContacts++;
    }
    w->Collision = c;
    w->A->BeginCollision(c);
    w->B->BeginCollision(c);
}

void CollisionWorld::EndCollision(CollisionWitness* w)
{
    Collision* c = w->Collision;
    w->A->EndCollision(c);
    w->B->EndCollision(c);
    std::unique_ptr<Collision> keep;   // alive until the end of the function
    auto it = std::find_if(collisions.begin(), collisions.end(), [c](const std::unique_ptr<Collision>& x) { return x.get() == c; });
    if (it != collisions.end()) {
        keep = std::move(*it);
        collisions.erase(it);
    }
    if (c->IsWallInvolved()) {
        CollisionBody* b = w->A->Type == 3 ? w->B : w->A;
        if (b != nullptr) b->WallContacts = std::max(b->WallContacts - 1, 0);
    }
    else if (w->A->Type == 0 && w->B->Type == 0) {
        w->A->CarContacts = std::max(w->A->CarContacts - 1, 0);
        w->B->CarContacts = std::max(w->B->CarContacts - 1, 0);
    }
    w->Collision = nullptr;
}

Collision* CollisionWorld::NthCollision(CollisionBody* b, int i) const
{
    int n = 0;
    for (auto& w : witnesses) {
        if (w->Collision == nullptr || (w->A != b && w->B != b)) continue;
        if (n == i) return w->Collision;
        n++;
    }
    return nullptr;
}

// ------------------------------------------------------------------ track polygon bodies

int CollisionWorld::Bucket(const TrackPolygonShape& p)
{
    return X87::FtoL2Lo(((F24(p.V[4]) + p.V[0]) * CollisionK::Thousand).V) & 0xFF;
}

int CollisionWorld::CreateTrackBodies(DynamicCollisionBody* b)
{
    if (WorldData == nullptr) return 0;
    WorldData->UpdateInterest(b->Coordinate, b->MinX, b->MinY, b->MaxX, b->MaxY, b->GetVelocity());
    int n = WorldData->InterestCount(b->Coordinate);
    std::vector<TrackPolygonShape*> list;
    for (int i = 0; i < n; i++) {
        TrackPolygonShape* p = WorldData->Interest(b->Coordinate, i);
        if (p->Body != nullptr) continue;
        if (X87::ParityEven(BWareMath2::Fcom(b->MinX, p->MaxX) & 0x41)) continue;
        if ((BWareMath2::Fcom(b->MaxX, p->MinX) & 0x01) != 0) continue;
        if (X87::ParityEven(BWareMath2::Fcom(b->MinY, p->MaxY) & 0x41)) continue;
        if ((BWareMath2::Fcom(b->MaxY, p->MinY) & 0x01) != 0) continue;
        if (!NearPolygon(b, p)) continue;
        if (static_cast<int>(list.size()) < CollisionK::MaxTrackCandidates) list.push_back(p);
    }
    if (list.empty()) return 0;
    for (bool swapped = true; swapped;) {
        swapped = false;
        for (size_t i = 0; i + 1 < list.size(); i++)
            if (list[i + 1]->SortKey < list[i]->SortKey) { std::swap(list[i], list[i + 1]); swapped = true; }
    }
    for (size_t i = 0; i < list.size(); i++) {
        TrackPolygonShape* p = list[i];
        StatCandidates++;
        if (buckets[Bucket(*p)] == 0) continue;
        StatBucketHits++;
        for (CollisionBody* o : bodies) {
            if (o->Type != 3) continue;
            const TrackPolygonShape* s = static_cast<TrackPolygonCollisionBody*>(o)->Source;
            if (s->Count != p->Count) continue;
            int k = 0;
            while (k < p->Count && CollisionK::Equal3(p->V, 3 * k, s->V, 3 * k, CollisionK::Eps1e3)) k++;
            if (k != p->Count) continue;
            StatDuplicates++;
            list[i] = nullptr;
            break;
        }
    }
    int made = 0;
    for (TrackPolygonShape* p : list) {
        if (p == nullptr) continue;
        if (static_cast<int>(trackBodies.size()) >= CollisionK::MaxTrackBodies) break;   // TrackPolygonCollisionBodySlotPool full
        NewTrackBody(p);
        made++;
    }
    return made;
}

void CollisionWorld::NewTrackBody(TrackPolygonShape* p)
{
    auto owned = std::make_unique<TrackPolygonCollisionBody>();
    TrackPolygonCollisionBody* t = owned.get();
    t->Source = p;
    t->GroupMember = p->GroupMember;
    Add(t);
    trackBodies.push_back(std::move(owned));
    p->Body = t;
    buckets[Bucket(*p)]++;
    const float zero[3] = {};
    t->Polygon.Build(*p, zero, CollisionK::PolygonThickness);
    SetBox(t, p->MinX, p->MinY, p->MaxX, p->MaxY);
}

void CollisionWorld::DeleteTrackBody(TrackPolygonCollisionBody* t)
{
    buckets[Bucket(*t->Source)]--;
    t->Source->Body = nullptr;
    RemoveBody(t);
    auto it = std::find_if(trackBodies.begin(), trackBodies.end(), [t](const std::unique_ptr<TrackPolygonCollisionBody>& x) { return x.get() == t; });
    if (it != trackBodies.end()) trackBodies.erase(it);   // frees t
}

void CollisionWorld::DeleteUnusedTrackBodies()
{
    std::vector<TrackPolygonCollisionBody*> snapshot;
    for (auto& t : trackBodies) snapshot.push_back(t.get());
    for (TrackPolygonCollisionBody* t : snapshot)
        if (t->WitnessCount == 0) DeleteTrackBody(t);
}

void CollisionWorld::PurgeDeadTrackBodies()
{
    std::vector<TrackPolygonCollisionBody*> snapshot;
    for (auto& t : trackBodies) snapshot.push_back(t.get());
    for (TrackPolygonCollisionBody* t : snapshot)
        if (!t->Source->Alive) DeleteTrackBody(t);
}

bool CollisionWorld::NearPolygon(DynamicCollisionBody* b, const TrackPolygonShape* p)
{
    const float* rot = b->GetRotation();
    F24 sF = F24(rot[10]) * p->Nz + F24(rot[9]) * p->Ny + F24(rot[8]) * p->Nx;
    float s = sF.Store();
    if ((BWareMath2::Fcom(sF, F24(CollisionK::One)) & 0x41) == 0) s = CollisionK::One;
    float sinA = F24::Sqrt(F24(CollisionK::One) - F24(s) * s).Store();
    const float* pos = b->GetPosition();
    F24 dist = F24(pos[2]) * p->Nz + F24(pos[1]) * p->Ny + F24(pos[0]) * p->Nx - p->D;
    F24 h = BWareMath2::GeOrNaN(F24(s) * dist, CollisionK::Zero) ? F24(b->ExtentNegZ) : F24(b->ExtentZ);
    F24 reach = F24::Abs(s) * h + F24(sinA) * b->Radius;
    if (BWareMath2::GeOrNaN(F24::Abs(dist), reach)) return false;
    float d = PlanDistance(p, pos[0], pos[1], (F24(b->Radius) + CollisionK::PointOne).Store());
    return (BWareMath2::Fcom(d, b->Radius) & 0x41) != 0;
}

float CollisionWorld::PlanDistance(const TrackPolygonShape* p, float px, float py, float max)
{
    if ((p->Flags & 2) == 0) {
        if (auto* et = dynamic_cast<const IEdgeTest*>(p)) {
            F24 r = et->EdgeTest(px, py);
            if (!X87::ParityEven(BWareMath2::Fcom(r, F24(CollisionK::Zero)) & 0x41)) return CollisionK::Zero;
        }
    }
    float best = max;
    int n = p->Count;
    for (int k = 0; k < n; k++) {
        int j = (k + 1) % n;
        F24 d = CollisionK::SegmentDistance(px, py, p->V[3 * k], p->V[3 * k + 1], p->V[3 * j], p->V[3 * j + 1]);
        if (BWareMath2::GeOrNaN(d, best)) continue;
        best = d.Store();
        if ((BWareMath2::Fcom(d, F24(max)) & 0x05) == 0x01) break;
    }
    return best;
}

// ------------------------------------------------------------------ the step

void CollisionWorld::MaybeCreateWitnesses(DynamicCollisionBody* b)
{
    if (!b->AreCollisionsEnabled(nullptr)) return;
    CreateTrackBodies(b);
    auto it = std::find(bodies.begin(), bodies.end(), static_cast<CollisionBody*>(b));
    int i = it == bodies.end() ? -1 : static_cast<int>(it - bodies.begin());
    int lo = i - 1;
    while (lo >= 0 && (BWareMath2::Fcom(bodies[lo]->MaxX, b->MinX) & 0x01) == 0) lo--;
    float limit = (F24(MaxWidth) + b->MaxX).Store();
    int hi = i + 1;
    while (hi < static_cast<int>(bodies.size()) && !X87::ParityEven(BWareMath2::Fcom(bodies[hi]->MaxX, limit) & 0x41)) hi++;
    std::vector<CollisionBody*> range(bodies.begin() + (lo + 1), bodies.begin() + hi);
    for (CollisionBody* c : range) {
        if (c == b) continue;
        if (X87::ParityEven(BWareMath2::Fcom(b->MinX, c->MaxX) & 0x41)) continue;
        if ((BWareMath2::Fcom(b->MaxX, c->MinX) & 0x01) != 0) continue;
        if (X87::ParityEven(BWareMath2::Fcom(b->MinY, c->MaxY) & 0x41)) continue;
        if ((BWareMath2::Fcom(b->MaxY, c->MinY) & 0x01) != 0) continue;
        if (WitnessExists(b, c)) continue;
        if (!c->AreCollisionsEnabled(b)) continue;
        if (c->Type == 2) throw std::runtime_error("world object bodies (sub_5B31F0) are not ported");
        if (!b->Overlaps(*c, 0.0f)) continue;
        NewWitness(b, c);
    }
}

void CollisionWorld::DoTimestepCollisions(float /*dt*/)
{
    PurgeDeadTrackBodies();
    {
        std::vector<CollisionWitness*> snapshot;
        for (auto& w : witnesses) snapshot.push_back(w.get());
        for (CollisionWitness* w : snapshot)
            if (ContainsWitness(w) && Expired(*w)) DeleteWitness(w);
    }
    {
        std::vector<CollisionBody*> snapshot = bodies;
        for (CollisionBody* b : snapshot) {
            if (b->Active == 0 || b->Damping == 0 || !b->InList) continue;
            if (auto* d = dynamic_cast<DynamicCollisionBody*>(b)) MaybeCreateWitnesses(d);
        }
    }
    std::vector<std::unique_ptr<Contact>> contacts;
    {
        std::vector<CollisionWitness*> snapshot;
        for (auto& w : witnesses) snapshot.push_back(w.get());
        for (CollisionWitness* w : snapshot) {
            w->Resolutions = 0;
            w->TestForContact(*this, contacts);
        }
    }
    int n = 0;
    while (!contacts.empty() && n < CollisionK::MaxContactResolutions) {
        std::unique_ptr<Contact> c = std::move(contacts.front());
        contacts.erase(contacts.begin());     // RemoveAll below drops it anyway (its bodies are a and b)
        c->Resolve(*this);
        c->Witness->Resolutions++;
        CollisionBody* a = c->A;
        CollisionBody* b = c->B;
        if (a->Active != 0)
            contacts.erase(std::remove_if(contacts.begin(), contacts.end(),
                [a](const std::unique_ptr<Contact>& x) { return x->A == a || x->B == a; }), contacts.end());
        if (b->Active != 0)
            contacts.erase(std::remove_if(contacts.begin(), contacts.end(),
                [b](const std::unique_ptr<Contact>& x) { return x->A == b || x->B == b; }), contacts.end());
        if (a->Active == 0 && b->Active == 0) {
            // the resolved contact itself stays listed in the original when neither body is active
            contacts.insert(contacts.begin(), std::move(c));
        }
        std::vector<DynamicCollisionBody*> affected;
        if (a->Active != 0 && a->Damping != 0) if (auto* da = dynamic_cast<DynamicCollisionBody*>(a)) affected.push_back(da);
        if (b->Active != 0 && b->Damping != 0) if (auto* db = dynamic_cast<DynamicCollisionBody*>(b)) affected.push_back(db);
        for (DynamicCollisionBody* d : affected) MaybeCreateWitnesses(d);
        std::vector<CollisionWitness*> snapshot;
        for (auto& w : witnesses) snapshot.push_back(w.get());
        for (CollisionWitness* w : snapshot)
            for (DynamicCollisionBody* d : affected)
                if ((w->A == d || w->B == d) && w->Resolutions < CollisionK::MaxContactsPerFrame)
                    w->TestForContact(*this, contacts);
        n++;
    }
    LastResolutions = n;
    {
        std::vector<Collision*> snapshot;
        for (auto& c : collisions) snapshot.push_back(c.get());
        for (Collision* coll : snapshot)
            if (!coll->Update(Clock->WorldTimer)) EndCollision(coll->Witness);
    }
    DeleteUnusedTrackBodies();
}

} // namespace bb

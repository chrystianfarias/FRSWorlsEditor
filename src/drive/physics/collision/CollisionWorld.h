#pragma once

// Port of NFSU2Unity Core/Physics/Collision/CollisionWorld.cs: Collision (a collision in progress), the impact
// report, the collision world of World::DoTimestepCollisions and the topology's edge test interface.

#include "drive/numerics/X87.h"
#include "drive/physics/collision/CollisionBody.h"
#include "drive/physics/collision/CollisionWitness.h"
#include "drive/physics/collision/WorldCollision.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bb {

class World;
class ITrackTopology;
class Contact;

/// Collision (the record of a collision in progress; ctor sub_5A9430, lives CollisionLifetime 0.3 s after its last
/// impulse; World+8 list). Effects, sounds, style points and rumble of Collision::AddImpulse are not modelled.
class Collision {
public:
    int StartTime = 0, LastTime = 0;            // +0x08, +0x0C WorldTimer
    int Wild = 0;                               // +0x10 set by CheckForWildCollision (car vs car)
    float Point0[3] = {};                       // +0x20 first contact point
    float Point[3] = {};                        // +0x30 last contact point
    float Speed = 0;                            // +0x40 tangential speed of the last impulse
    float ImpulseSum[3] = {};                   // +0x50
    float MagnitudeSum = 0;                     // +0x60
    CollisionWitness* Witness = nullptr;        // +0x64
    float Direction[3] = {};                    // +0x70 unit direction of the last impulse

    /// Collision::IsWallInvolved 0x5A0160: a track polygon with |nz| < 0.1.
    bool IsWallInvolved() const;

    /// sub_5A01B0: a track polygon with |nz| > 0.7 (supports the car).
    bool IsFlatGroundInvolved() const;

    /// Collision::AddImpulse 0x5A9620 (physics part): sums, time, point and speed; CheckForWildCollision 0x59FB70
    /// (only acts on car-vs-car collisions: not ported); the normalised impulse (the game stores it when a player
    /// is within 50 of the point, which holds for the player's own car).
    void AddImpulse(CollisionWorld& world, const float* point, const float* impulse, float speed);

    /// Collision::Update 0x592BD0: alive while less than CollisionLifetime seconds since the last impulse.
    bool Update(int worldTimer) const;
};

/// A collision impulse, reported for sounds and effects (the game triggers them in Collision::AddImpulse).
struct CollisionImpact {
    float X = 0, Y = 0, Z = 0;              // contact point (game space)
    float ImpulseX = 0, ImpulseY = 0, ImpulseZ = 0;
    float Magnitude = 0;
    int Terrain = 0;
    bool Wall = false;
    /// Record flags of the track polygon hit (0x02 vertical, 0x04 barrier), 0 when no track polygon.
    uint8_t PolygonFlags = 0;
};

/// The collision world of World::DoTimestepCollisions 0x5FFA50: the body list sorted by box (CollisionBodyList),
/// witnesses (CollisionBodyWitnessList), collisions in progress (World+8), the track polygon bodies (0x86B5A0),
/// body numbers (pCollisionBodyNumberAllocator, 214) and the witness existence bit field.
/// The car bodies added with Add are not owned; witnesses, collisions and track polygon bodies are.
class CollisionWorld {
public:
    World* const Clock;
    IWorldCollision* const WorldData;
    ITrackTopology* const Topology;
    /// flt_86B29C: widest body box (bounds the sweep in MaybeCreateWitnesses).
    float MaxWidth = 0;
    int StatCandidates = 0, StatBucketHits = 0, StatDuplicates = 0;   // dword_86AF7C / 80 / 84
    /// Contacts resolved in the last DoTimestepCollisions.
    int LastResolutions = 0;
    /// Raised for every impulse (sounds / effects).
    std::function<void(const CollisionImpact&)> Impact;

    CollisionWorld(World* clock, IWorldCollision* worldData, ITrackTopology* topology);
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    const std::vector<CollisionBody*>& Bodies() const { return bodies; }
    const std::vector<std::unique_ptr<CollisionWitness>>& Witnesses() const { return witnesses; }
    const std::vector<std::unique_ptr<Collision>>& Collisions() const { return collisions; }

    /// (internal) Reports an impulse to Impact.
    void RaiseImpact(const Collision& c, const float* p, const float* imp, float mag);

    /// CollisionBody ctor sub_5A04F0: number (next free), empty box, front of the body list.
    void Add(CollisionBody* b);

    /// sub_5A0680(min, max): new plan box (sub_5A05A0 first updates the widest-box bound from the old box), then
    /// the body moves through the list until it is ordered by (max x, number).
    void SetBox(CollisionBody* b, float minX, float minY, float maxX, float maxY);

    /// (internal) sub_5A9B10 + Collision ctor sub_5A9430: the witness's collision record; wall/car counters;
    /// BeginCollision.
    void BeginCollision(CollisionWitness* w, const float* point);

    /// sub_5A0820(i): the collision of the i-th witness (list order) of the body that has one.
    Collision* NthCollision(CollisionBody* b, int i) const;

    /// DynamicCollisionBody::MaybeCreateWitnesses 0x5B23B0: when the body may collide, makes the track bodies
    /// around it, then sweeps the bodies whose boxes can overlap (the list is ordered by max x) and starts a
    /// witness for each new overlapping pair that allows it.
    void MaybeCreateWitnesses(DynamicCollisionBody* b);

    /// World::DoTimestepCollisions 0x5FFA50: expire witnesses; new witnesses for the awake moving bodies; test
    /// every witness (contacts ordered by backup time); resolve up to MAX_CONTACT_RESOLUTIONS contacts (each
    /// resolution drops the other contacts of its bodies and re-tests their witnesses, at most
    /// MAX_CONTACTS_PER_FRAME per witness); age the collisions; delete unused track bodies.
    void DoTimestepCollisions(float dt);

    /// Takes a (car) body out of the world: its witnesses go, its number is freed (CollisionBody dtor sub_5A9C00).
    void Remove(CollisionBody* b) { if (b->InList) RemoveBody(b); }

private:
    std::vector<CollisionBody*> bodies;
    std::vector<std::unique_ptr<CollisionWitness>> witnesses;
    std::vector<std::unique_ptr<Collision>> collisions;
    std::vector<std::unique_ptr<TrackPolygonCollisionBody>> trackBodies;
    bool ids[CollisionK::MaxBodies] = {};
    int nextId = 0;
    std::vector<uint8_t> witnessBits;
    uint8_t buckets[256] = {};          // byte_86AFA8

    void MarkId(int id);
    void FreeId(int id);
    static bool Swap(const CollisionBody* first, const CollisionBody* second);
    void UpdateMaxWidth(CollisionBody* b, float width);
    void RemoveBody(CollisionBody* b);
    bool WitnessExists(const CollisionBody* a, const CollisionBody* b) const;
    CollisionWitness* NewWitness(CollisionBody* a, CollisionBody* b);
    void DeleteWitness(CollisionWitness* w);
    bool ContainsWitness(const CollisionWitness* w) const;
    static bool Expired(const CollisionWitness& w);
    void EndCollision(CollisionWitness* w);
    static int Bucket(const TrackPolygonShape& p);
    int CreateTrackBodies(DynamicCollisionBody* b);
    void NewTrackBody(TrackPolygonShape* p);
    void DeleteTrackBody(TrackPolygonCollisionBody* t);
    void DeleteUnusedTrackBodies();
    void PurgeDeadTrackBodies();
    static bool NearPolygon(DynamicCollisionBody* b, const TrackPolygonShape* p);
    static float PlanDistance(const TrackPolygonShape* p, float px, float py, float max);
};

/// The topology's sub_5D3330 edge test (implemented by the topology's polygons).
class IEdgeTest {
public:
    virtual ~IEdgeTest() = default;
    virtual F24 EdgeTest(float px, float py) const = 0;
};

} // namespace bb

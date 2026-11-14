#pragma once

// Port of NFSU2Unity Core/Physics/Collision/Contact.cs.

#include "drive/numerics/X87.h"

namespace bb {

class CollisionBody;
class CollisionWitness;
class CollisionWorld;
class TrackPolygonCollisionBody;

/// Contact (0x34 bytes; ctor sub_5B4F90): two bodies in contact, the time to back them up (+0x08) and the
/// separation to apply (+0x20). Resolved by Contact::Resolve 0x5B2610.
class Contact {
public:
    float Time = 0;                             // +0x08
    CollisionBody* const A;                     // +0x0C
    CollisionBody* const B;                     // +0x10
    CollisionWitness* const Witness;            // +0x14
    float Sep[3] = {};                          // +0x20
    int LoopCounter = 0;                        // +0x30 WorldLoopCounter

    Contact(CollisionBody* a, CollisionBody* b, CollisionWitness* w) : A(a), B(b), Witness(w) {}

    /// sub_5B4BE0: when the bodies interpenetrate, bisects (8 steps) the time they must be backed up over
    /// [0, min(max backup times)]: penetrating moves the low end, separated moves the high end and records the
    /// smallest separated time, touching stops. No separated time found: push them apart (sub_5B42F0).
    float ComputeBackupTime(CollisionWorld& world);

    /// sub_5A0EB0(a, b) (sub_43BEE0's comparator): true when a goes after b: backup times further apart than
    /// flt_7FBF10 (always, it is negative) order by decreasing time.
    static bool ComesAfter(Contact& a, Contact& b);

    /// Contact::Resolve 0x5B2610: both bodies to physics mode, backed up by Time, pushed apart by the separation,
    /// auxiliaries recomputed, CollisionImpulse, auxiliaries again, then both moved forward by Time again (for
    /// the car this runs PhysicsMover::Move(Time) a second time in the frame).
    void Resolve(CollisionWorld& world);

private:
    /// sub_5B42F0(t): push-out along the track polygon's normal (or between the positions).
    void PushOut(float t);
    /// sub_593DF0(out, track, other, otherPos): the track polygon's normal, sided for barrier-group polygons.
    void Normal(TrackPolygonCollisionBody* t, CollisionBody* other, const float* otherPos, float* o);
    /// sub_594000(point, normal): the contact point and normal.
    void PointAndNormal(float* p, float* n);
    /// sub_5A0DC0: the moving body's position along the track polygon's (outward) normal.
    float Depth(bool& valid);
    /// Contact::CollisionImpulse 0x5AF0E0.
    void CollisionImpulse(CollisionWorld& world);
    /// max(0, z * 0.25) (the smokeable case of CollisionImpulse).
    static float DownScale(float z);
    /// CollisionImpulse after a "wild" collision (car-car only): lever arms lose their component along up.
    void ProjectOutWheelsDown(float* rA, float* rB);
    static void Project(CollisionBody* b, float* r);
    /// sub_5941C0: the car's friction against a track polygon, else the larger of the two.
    static F24 Friction(CollisionBody* a, CollisionBody* b);
    static void Cross(float* o, const float* a, const float* b);
};

} // namespace bb

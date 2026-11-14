#pragma once

// Port of NFSU2Unity Core/Physics/Collision/CollisionWitness.cs.

#include "drive/numerics/X87.h"
#include "drive/physics/collision/GjkShapes.h"
#include "drive/physics/collision/GjkSimplexSolver.h"

#include <memory>
#include <vector>

namespace bb {

class CollisionBody;
class CollisionWorld;
class Collision;
class Contact;

/// CollisionBodyWitness (0x250 bytes; ctor sub_5A0340, dtor sub_5B22B0): a pair of bodies whose boxes overlap,
/// with the GJK state between them. Field offsets are the game's.
class CollisionWitness {
public:
    float LastDistance = 0;                                     // +0x10
    uint8_t InContact = 0;                                      // +0x14 distance <= 0.001
    uint8_t Penetrating = 0;                                    // +0x15 distance <= 1e-5
    float LastPosA[4] = {}, LastPosB[4] = {};                   // +0x20, +0x30
    GjkSimplex Simplex;                                         // +0x40 (its +0xD8 is +0x118 "initialised")
    float MatA[16] = {}, MatAT[16] = {};                        // +0x120 (row 3 = position), +0x160
    float MatB[16] = {}, MatBT[16] = {};                        // +0x1A0, +0x1E0
    uint8_t Dirty = 0;                                          // +0x220
    uint8_t OneSided = 0;                                       // +0x221
    GjkShape* ShapeA = nullptr;                                 // +0x224
    GjkShape* ShapeB = nullptr;                                 // +0x228
    CollisionBody* A = nullptr;                                 // +0x238
    CollisionBody* B = nullptr;                                 // +0x23C
    int Resolutions = 0;                                        // +0x240 resolved contacts this frame
    bb::Collision* Collision = nullptr;                         // +0x244
    int BarrierSide = 0;                                        // +0x248 (sub_593DF0)

    /// Closest point on A / on B (witness +0xC0 / +0xD0) and the direction (+0xE0) of the last GJK.
    float ClosestA(int k) const { return Simplex.F(0x80 + 4 * k); }
    float ClosestB(int k) const { return Simplex.F(0x90 + 4 * k); }

    /// sub_598590(shapeA, shapeB): new shapes reset the contact state; one-sidedness of a track polygon.
    void SetShapes(GjkShape* a, GjkShape* b);

    /// sub_598620(rotA, posA, rotB, posB): when a position moved by more than 0.001 the pose matrices (rotation
    /// rows plus a position row) and their transposed rotations are rebuilt and the witness marked dirty.
    void SetTransforms(const float* rotA, const float* posA, const float* rotB, const float* posB);

    /// sub_5B3D20: the distance between the bodies at the current transforms (GJK when dirty; a change of the
    /// polygon's one-sidedness restarts the GJK). Sets Penetrating (<= 1e-5) and InContact (<= 0.001).
    F24 Update();

    /// CollisionBodyWitness::TestForContact 0x5B5050: when both bodies allow it, sets the hulls' COG, the poses,
    /// runs the distance test; when in contact (and neither body is blinking) stamps the contact times and adds
    /// a Contact (sub_5B4F90) to the list, ordered by backup time (sub_43BEE0 with sub_5A0EB0).
    void TestForContact(CollisionWorld& world, std::vector<std::unique_ptr<Contact>>& contacts);

private:
    /// sub_5928E0: the polygon's extruded half is in use (flag and positive thickness).
    static uint8_t Extruded(const GjkPolygon& p);
    /// The vertex of a shape by id (sub_5AAD20 dispatch).
    static void Vertex(GjkShape* s, int id, GjkVertex& v) { s->GetPoint(id, v); }
    /// sub_5B3390: GJK distance between the two shapes at MatA / MatB.
    F24 Gjk();
    /// sub_592ED0: bodies of types 0..2, and vertical track polygons, count as a hit (Car+0x140).
    static bool HitWorthy(CollisionBody* b);
};

} // namespace bb

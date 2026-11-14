#pragma once

// Port of NFSU2Unity Core/Physics/Collision/WorldCollision.cs: the track polygon as the collision code reads it,
// the world side of car-vs-world collision (IWorldCollision) and the collision constants (CollisionK).

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cstdint>
#include <string>

namespace bb {

class GjkPolytope;

/// What the collision code reads of a track polygon: the 0x80-byte expanded polygon of the topology
/// (sub_5D7F10; bb::TopologyPolygon derives from this). Offsets are the game's.
class TrackPolygonShape {
public:
    virtual ~TrackPolygonShape() = default;

    /// +0x09: corner count (3, or 4 for quads).
    int Count = 0;
    /// +0x0A: record flags (0x02 vertical, 0x04 barrier, 0x10 quad, ...).
    uint8_t Flags = 0;
    /// +0x0B: TerrainType.
    int8_t Terrain = 0;
    /// +0x14..+0x20: plan bounding box.
    float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    /// +0x30: corners (x, y, z per corner, 4 corners).
    float V[12] = {};
    /// +0x70: unit normal and plane distance.
    float Nx = 0, Ny = 0, Nz = 0, D = 0;
    /// +0x10: the track polygon collision body made from it (a TrackPolygonCollisionBody*), or null.
    void* Body = nullptr;
    /// Record byte 0 != 0: the polygon belongs to a barrier group (track body +0x64).
    bool GroupMember = false;
    /// Order of the source records in memory (sub_5AE930 sorts candidates by the record address): section
    /// table index * 2^20 + record index. The real address order of different sections depends on the
    /// streaming allocator; within a section it is the record order.
    int64_t SortKey = 0;
    /// False once the polygon's section was unloaded (its body must go, as sub_5E0C40 destroys it).
    bool Alive = true;
};

/// The world side of car-vs-world collision, implemented by bb::TrackWorld. When the ground given to VehicleSim
/// implements it (and ITrackTopology), the car collides with the track polygons (World::DoTimestepCollisions
/// 0x5FFA50); FlatGround and TestGround do not implement it.
class IWorldCollision {
public:
    virtual ~IWorldCollision() = default;

    /// sub_5E7100 (TopologyCoordinate): sets the interest box from a body's plan box and velocity (grown by 0.1
    /// and by the clamped velocity), re-checks the section at its centre and refills the interest polygons
    /// when the box left the previous one. Returns false when nothing changed. velocity may be null.
    virtual bool UpdateInterest(void* coordinate, float minX, float minY, float maxX, float maxY, const float* velocity) = 0;

    /// TopologyCoordinate+0x2E0: number of interest polygons.
    virtual int InterestCount(void* coordinate) = 0;

    /// TopologyCoordinate+0x3C[i].
    virtual TrackPolygonShape* Interest(void* coordinate, int i) = 0;

    /// FindGJKPolytope 0x5A18C0: the collision volume named by nameHash among the loaded ones
    /// (the region's chunk 0x80034020 and the car's CARS\<carName>\GEOMETRY.BIN); null when absent.
    virtual GjkPolytope* FindCollisionVolume(const std::string& carName, uint32_t nameHash) = 0;
};

/// .rdata / .data constants of the collision code (bit patterns; Load checks them).
namespace CollisionK {
inline const float Zero = X87::Bits(0x00000000u);              // _zero
inline const float One = X87::Bits(0x3F800000u);               // float_1
inline const float MinusOne = X87::Bits(0xBF800000u);          // _float_minus1
inline const float Half = X87::Bits(0x3F000000u);              // flt_784260
inline const float PointOne = X87::Bits(0x3DCCCCCDu);          // _zero_point_one
inline const float Eps1e3 = X87::Bits(0x3A83126Fu);            // immediate 0.001 (bEqual) / flt_7A1928
inline const float GjkZero = X87::Bits(0x3727C5ACu);           // flt_7A192C 1e-5 (touching distance)
inline const float GjkContact = X87::Bits(0x3A83126Fu);        // flt_7A1928 0.001 (in contact)
inline const float Eps1e6 = X87::Bits(0x358637BDu);            // flt_787594 1e-6
inline const float Eps1e5 = X87::Bits(0x3727C5ACu);            // flt_7845A0 1e-5
inline const float PolygonThickness = X87::Bits(0x3F800000u);  // dword_7A1878 1.0
inline const float WitnessContactAge = X87::Bits(0x3D4CCCCDu); // flt_7A187C 0.05 s
inline const float TickToSec = X87::Bits(0x3983126Fu);         // flt_784268 1/4000
inline const float PushZClamp = X87::Bits(0x3E19999Au);        // flt_7A189C 0.15
inline const float PushStep0 = X87::Bits(0x3BA3D70Au);         // immediate 0.005
inline const float PushStepGrowth = X87::Bits(0x3C23D70Au);    // flt_78425C 0.01
inline const float RadiusMargin = X87::Bits(0x3D4CCCCDu);      // flt_7A1880 0.05
inline const float AiInertiaScale = X87::Bits(0x3ECCCCCDu);    // dword_7A1888 0.4
inline const float WallFrictionScale = X87::Bits(0x3F800000u); // flt_7A18A4 1.0
inline const float SmokeableDownScale = X87::Bits(0x3E800000u);// flt_7A18A8 0.25
inline const float MaxInvMassDiv = X87::Bits(0x3FECCCCDu);     // flt_7A1884 1.85
inline const float NoImpulse = X87::Bits(0xC760E700u);         // flt_7A24FC -57575.0
inline const float FlatNormalZ = X87::Bits(0x3F333333u);       // flt_7844D8 0.7
inline const float Thousand = X87::Bits(0x447A0000u);          // _one_thousand
inline const float InterestVelocityScale = X87::Bits(0x3E4CCCCDu); // flt_784554 0.2
inline const float InterestVelocityMax = X87::Bits(0x40400000u);   // flt_787ADC 3.0
inline const float InterestVelocityMin = X87::Bits(0xC0400000u);   // flt_78BAFC -3.0
inline const float SupportSentinel = X87::Bits(0xFCDC84DCu);   // flt_7A206C
inline const float BigNegative = X87::Bits(0xCB18967Fu);       // immediate -9999999 (empty body box)
// .data (game tweakables, read from the exe)
inline float MinImpulseInvMass = X87::Bits(0x3E99999Au);          // flt_7FBF24 0.3
inline float FrictionSpeed = X87::Bits(0x3F800000u);              // flt_7FBF30 1.0
inline float CollisionLifetime = X87::Bits(0x3E99999Au);          // CollisionLifetime 0x7FBEE0 0.3
inline float ContactTimeTolerance = X87::Bits(0xC0A000D2u);       // flt_7FBF10 -5.0001
inline int UseSidedBarrierNormals = 1;                            // dword_7FBF18
inline int RememberBarrierSide = 1;                               // dword_7FBF14
inline int MaxContactResolutions = 20;                            // MAX_CONTACT_RESOLUTIONS 0x802080
inline int MaxContactsPerFrame = 2;                               // MAX_CONTACTS_PER_FRAME 0x802084
constexpr int MaxBodies = 0xD6;                                   // pCollisionBodyNumberAllocator capacity
constexpr int MaxTrackBodies = 0xA0;                              // TrackPolygonCollisionBodySlotPool
constexpr int MaxWitnesses = 0x80;                                // CollisionBodyWitnessSlotPool
constexpr int MaxTrackCandidates = 0x30;                          // sub_5AE930 local list

/// bEqual(a, b, eps) 0x43D190: |a.k - b.k| <= eps for each component (unordered counts as equal).
bool Equal3(const float* a, int ao, const float* b, int bo, float eps);

/// Distance from (px, py) to the segment a-b as the game computes it in sub_5D3450 / sub_5D7270 / sub_5D70D0:
/// along = d . unit(e) (bNormalize(bVector2) 0x43CE50), perp = -ex'*dy + dx*ey'; before the start: |(perp,
/// along)|, past the end: |(along - len, perp)|, else |perp|.
F24 SegmentDistance(float px, float py, float ax, float ay, float bx, float by);

/// Loads the .data tweakables from the exe and checks the .rdata bit patterns (throws std::runtime_error).
void Load(const Speed2Image& exe);
} // namespace CollisionK

} // namespace bb

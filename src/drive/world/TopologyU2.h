#pragma once

// Port of NFSU2Unity Core/World/TopologyU2.cs: the game's track topology (TopologyManager, TopologyTree,
// TopologyCoordinate, the visible-section boundaries) and its ground query NewGetElevation.
// See NFSU2Unity docs/re/world-collision.md.
//
// Editor adaptation: a TopologyTree is built from a section's table index and its collision pack
// (TrackCollisionU2) instead of a streamed WorldSectionU2.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"
#include "drive/physics/collision/CollisionWorld.h"
#include "drive/physics/collision/WorldCollision.h"
#include "drive/world/TrackCollisionU2.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bb {

/// .rdata constants and immediates of the topology code of SPEED2.EXE v1.2 (bit patterns; checked against the
/// installed exe by Verify). See docs/re/world-collision.md.
namespace TopologyK {
inline const float One = X87::Bits(0x3F800000u);            // float_1 0x784250
inline const float MinusOne = X87::Bits(0xBF800000u);       // _float_minus1 0x7842A0
inline const float Zero = X87::Bits(0x00000000u);           // _zero 0x7D7D3C
inline const float Half = X87::Bits(0x3F000000u);           // flt_784260 0.5
inline const float Four = X87::Bits(0x40800000u);           // flt_7844F8 4.0
inline const float Eight = X87::Bits(0x41000000u);          // flt_7881F4 8.0
inline const float MinusEighth = X87::Bits(0xBE000000u);    // flt_7A52D0 -0.125
inline const float InvCell = X87::Bits(0x3D000000u);        // flt_7A5180 1/32
inline const float Cell = X87::Bits(0x42000000u);           // flt_7875B4 32.0
inline const float InvSubCell = X87::Bits(0x3E000000u);     // flt_788164 0.125
inline const float Fixed = X87::Bits(0x47800000u);          // flt_784238 65536.0
inline const float InvFixed = X87::Bits(0x37800000u);       // flt_784278 1/65536
inline const float PointOne = X87::Bits(0x3DCCCCCDu);       // _zero_point_one 0x784358
inline const float NoBestElevation = X87::Bits(0xC7C34F80u); // immediate -99999.0 (NewGetElevation)
inline const float FarDistance = X87::Bits(0x4B18967Fu);    // immediate 9999999.0 (sub_5DB770)
inline const float InitialRadius = X87::Bits(0xC479C000u);  // immediate -999.0 (TopologyCoordinate ctor)
inline const float BarrierLow = X87::Bits(0xC5000000u);     // immediate -2048.0 (sub_5D7F10)
inline const float BarrierHigh = X87::Bits(0x45000000u);    // immediate 2048.0 (sub_5D7F10)
inline const float HasTopologyZ = X87::Bits(0x47C34F8Du);   // immediate 99999.1 (HasTopology 0x5E7E80)
inline const float FltMax = X87::Bits(0x7F7FFFFFu);         // immediate FLT_MAX
inline const float SectionSlack = X87::Bits(0x41100000u);   // immediate 9.0 (NewGetElevation -> SetSectionPosition)
inline const float VelocityScale = X87::Bits(0x3E4CCCCDu);  // flt_784554 0.2 (sub_5E7100)
inline const float VelocityMax = X87::Bits(0x40400000u);    // flt_787ADC 3.0
inline const float VelocityMin = X87::Bits(0xC0400000u);    // flt_78BAFC -3.0

/// Throws std::runtime_error when a .rdata constant of the installed exe differs from the bit pattern above.
void Verify(const Speed2Image& exe);
} // namespace TopologyK

/// One section boundary of chunk 0x00034152 (child of 0x80034150 in L4Rx.BUN), as linked by
/// LoaderVisibleSections 0x5EA0F0 / sub_5E9C10 into TheVisibleSectionManager 0x883E70. Record layout:
/// +0x00/+0x04 list links, +0x08 i16 section number, +0x0A i16 point count, +0x0C min (x, y),
/// +0x14 max (x, y), +0x1C 8 bytes (not read by the topology), +0x24 points (x, y) floats.
class SectionBoundaryU2 {
public:
    int16_t Section = 0;
    float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    std::vector<float> X, Y;
    int Count() const { return static_cast<int>(X.size()); }
};

/// Chunk 0x80034150 of L4Rx.BUN as far as the topology reads it: the drivable section limit
/// (0x00034151, first dword -> dword_800DFC, 20 in L4RA) and the section boundaries (0x00034152).
class VisibleSectionsU2 {
public:
    static constexpr uint32_t ChunkHeader = 0x00034151, ChunkBoundaries = 0x00034152;

    /// dword_800DFC: main-set sections are numbered boundary + limit (C29 = 329 for boundary 309).
    int DrivableLimit = 10;
    /// All boundaries in file order.
    std::vector<SectionBoundaryU2> Boundaries;

    static VisibleSectionsU2 Read(const std::vector<uint8_t>& layoutFile);
};

/// Chunk 0x0003412F of L4Rx.BUN (TopologyManager+0x82C, dword_88FF94): 32-byte records naming the
/// barrier groups (PLAYER_BARRIERS_xxxx, BARRIERS_xxxx, ...) that polygon byte 0 refers to.
/// +0x00 hash, +0x04 u32 (polygon count), +0x08 s8 enabled (run-time), +0x09 name.
/// TopologyManager::Init 0x5D8350 enables entry 0 and disables every other one.
class TopologyGroupsU2 {
public:
    static constexpr uint32_t ChunkId = 0x0003412F;
    std::vector<uint32_t> Hash;
    std::vector<std::string> Name;

    static TopologyGroupsU2 Read(const std::vector<uint8_t>& layoutFile);
};

class TopologyTree;
class TopologyCoordinate;

/// The 0x80-byte expanded polygon the topology builds on demand from a 32-byte collision record
/// (sub_5E0C90 -> sub_5D7F10 0x5D7F10, corners by sub_5D2CC0 0x5D2CC0). Field offsets are the game's.
class TopologyPolygon : public TrackPolygonShape, public IEdgeTest {
public:
    /// +0x0C: the source record (tree and index).
    TopologyTree* Tree = nullptr;
    int Index = 0;

    float X(int k) const { return V[3 * k]; }
    float Y(int k) const { return V[3 * k + 1]; }
    float Z(int k) const { return V[3 * k + 2]; }

    /// sub_5D3330 (thiscall, st0): the largest edge function of the point against the polygon's edges
    /// v0-v1, v1-v2, v3-v0 and (quads) v2-v3, starting from -1 and returning early once positive.
    /// Negative = strictly inside, 0 = on an edge, positive = outside.
    F24 EdgeTest(float px, float py) const override;

    /// The plane height at (px, py) exactly as NewGetElevation computes it: (n.x*x + n.y*y - d) * (-1 / n.z).
    F24 PlaneHeight(float px, float py) const;
};

/// TopologyTree (slot pool "TopologyTreeSlotPool", 0x70 bytes): one section's collision pack as the game
/// addresses it (sub_5D8140 0x5D8140). +0x08 polygons (32-byte records), +0x18 cell references
/// (u16 polygon, u16 sub-cell mask), +0x20 cells (u16 first, u16 count), +0x30 section number,
/// +0x34/+0x36 grid width/height, +0x38/+0x3C grid origin, +0x40/+0x50 bounds; +0x60 list of its
/// expanded polygons, +0x68 list of the TopologyCoordinates connected to it.
class TopologyTree {
public:
    /// Section table index (the C# Section.Info.Index; the polygons' SortKey high bits).
    const int SectionIndex;
    /// The collision pack (not owned: it must outlive the tree).
    const TrackCollisionU2* const Pack;
    const int16_t Number, Width, Height;
    const float OriginX, OriginY;
    const std::vector<uint8_t>& Raw;
    const int PolygonCount;
    /// (internal) the coordinates connected to this tree.
    std::vector<TopologyCoordinate*> Coordinates;
    /// (internal) temporary "already listed" mark (the game sets record bit 0x20 while it fills an interest list).
    std::vector<uint8_t> Listed;

    TopologyTree(int sectionIndex, const TrackCollisionU2* pack);
    TopologyTree(const TopologyTree&) = delete;
    TopologyTree& operator=(const TopologyTree&) = delete;

    uint8_t Attr0(int i) const { return Raw[32 * i]; }
    uint8_t CornerCode(int i) const { return Raw[32 * i + 1]; }
    uint8_t TerrainByte(int i) const { return Raw[32 * i + 2]; }
    uint8_t Flags(int i) const { return Raw[32 * i + 3]; }
    int16_t Base(int i) const { return S16(32 * i + 4); }
    int16_t Xs(int i, int k) const { return S16(32 * i + 8 + 2 * k); }
    int16_t Ys(int i, int k) const { return S16(32 * i + 0x10 + 2 * k); }
    int16_t Zs(int i, int k) const { return S16(32 * i + 0x18 + 2 * k); }

    int CellFirst(int cell) const { return Pack->CellFirst[cell]; }
    int CellCount(int cell) const { return Pack->CellCount[cell]; }
    int RefPolygon(int r) const { return Pack->RefPolygon[r]; }
    int RefMask(int r) const { return Pack->RefMask[r]; }

    /// (internal) The section went away: its expanded polygons (and the collision bodies made of them) are void.
    void MarkDead();

    /// Has the record been expanded already (record +6 >= 0 in the game)?
    TopologyPolygon* Expanded(int index) const { return expanded[index].get(); }

    /// sub_5E0C90 (thiscall on the tree): the expanded polygon of a record, built on first use by
    /// sub_5D7F10 and kept while the section is loaded (the game recycles unused ones from a
    /// 1024-entry pool; the values are a pure function of the record, so caching them is equivalent).
    TopologyPolygon* Expand(int index);

    /// Expands a single 32-byte collision record (sub_5D7F10) outside any section (tools and tests).
    /// The result's Tree is null.
    static std::unique_ptr<TopologyPolygon> ExpandRecord(const uint8_t* record32);

    /// Fixed-point corner (16-bit << 13) to float: fild exact; fmul 2^-16 exact; one rounding.
    static float FixedToFloat(int v) { return static_cast<float>(static_cast<double>(v) / 65536.0); }
    /// v << 13 with 32-bit wrap (the game's shl).
    static int Shl13(int v) { return static_cast<int>(static_cast<uint32_t>(v) << 13); }

private:
    std::vector<std::unique_ptr<TopologyPolygon>> expanded;

    int16_t S16(size_t o) const { int16_t v; std::memcpy(&v, Raw.data() + o, 2); return v; }

    /// sub_5D7F10: header bytes, bounding box, corners (sub_5D2CC0), barrier heights, plane.
    void Build(TopologyPolygon& p, int i) const;
};

/// TopologyCoordinate (0x314 bytes; ctor 0x5E0B40): a client's view of the topology. The car's one is
/// embedded at Car+0x5C4 (CarState+0x4 points to it) and shared by the wheels (GetElevation) and the
/// car's collision body (interest box).
class TopologyCoordinate {
public:
    static constexpr int MaxInterest = 160, CacheSize = 4;

    // +0x0C VisibleSectionCoordinate: last position, boundary, radius (how far it may move before re-checking)
    float PosX = 0, PosY = 0;
    SectionBoundaryU2* Boundary = nullptr;
    float Radius = TopologyK::InitialRadius;
    /// +0x08: the tree (section) it is connected to, or null.
    TopologyTree* Tree = nullptr;
    /// +0x1C/+0x24: interest box as requested; +0x2C/+0x34: the box used (grown by 0.1 and by the velocity).
    float BoxMinX = TopologyK::FltMax, BoxMinY = TopologyK::FltMax, BoxMaxX = -TopologyK::FltMax, BoxMaxY = -TopologyK::FltMax;
    float InterestMinX = TopologyK::FltMax, InterestMinY = TopologyK::FltMax, InterestMaxX = -TopologyK::FltMax, InterestMaxY = -TopologyK::FltMax;
    /// +0x3C: polygons of the interest box: ground first, then vertical (flag 0x02), then flag 0x08.
    TopologyPolygon* Interest[MaxInterest] = {};
    /// +0x2E0 total, +0x2E4 ground, +0x2E8 vertical, +0x2EC flag-0x08 counts.
    int InterestCount = 0, InterestGround = 0, InterestVertical = 0, InterestOther = 0;
    /// +0x2BC / +0x2C0: most-recently-used polygons tried first by NewGetElevation.
    TopologyPolygon* Cache[CacheSize] = {};
    int CacheCount = 0;
    /// +0x2F0 / +0x2F4: barrier groups enabled for this client while it queries (sub_5D80F0).
    std::vector<int> Groups;
    /// (internal)
    bool Registered = false;
};

/// The topology state shared by every client: TheTopologyManager 0x88F768 (trees by slot, barrier
/// group flags) and TheVisibleSectionManager 0x883E70 (drivable section boundaries, most recently used
/// first). Ported routines cite their addresses; see docs/re/world-collision.md.
class TopologyManagerU2 {
public:
    VisibleSectionsU2 Sections;
    TopologyGroupsU2 Groups;
    /// Group flag at record +8 of the 0x3412F table (Init: 1 for entry 0, 0 for the others).
    std::vector<int8_t> GroupEnabled;
    /// dword_800DFC.
    const int DrivableLimit;
    /// TheVisibleSectionManager+0: drivable boundaries (number % 100 in [1, limit)), MRU order.
    std::vector<SectionBoundaryU2*> Drivable;

    TopologyManagerU2(VisibleSectionsU2 sections, TopologyGroupsU2 groups);
    TopologyManagerU2(const TopologyManagerU2&) = delete;
    TopologyManagerU2& operator=(const TopologyManagerU2&) = delete;

    /// The loaded trees, in load order.
    std::vector<TopologyTree*> Trees() const;

    /// sub_5D3C10: tree slot of a section number (alternate numbers fold onto the main ones).
    int Slot(int16_t number) const;

    /// TopologyManager::GetIndex 0x591380.
    TopologyTree* GetIndex(int16_t number) const;

    /// A new client, owned by the manager.
    TopologyCoordinate* CreateCoordinate();

    /// sub_5E78A0 + sub_5E6980: a tree was loaded; waiting clients inside its boundary connect to it.
    void LoadTree(TopologyTree* tree);

    /// sub_5E8210 / sub_5E0C40: a tree was unloaded; its clients are disconnected.
    void UnloadTree(TopologyTree* tree);

    // ------------------------------------------------------------------ TopologyCoordinate

    /// sub_5DC9C0: drops the cache and the interest list and leaves the tree.
    void Disconnect(TopologyCoordinate* c);

    /// TopologyCoordinate::ConnectToTree 0x5E6890.
    void Connect(TopologyCoordinate* c, TopologyTree* tree);

    /// sub_5E7100(min, max, velocity): re-checks the section at the box centre (slack 0), stores the box and, when
    /// it is not inside the current interest box, makes the interest box the body box grown by 0.1 and, on the
    /// side the velocity points to, by 0.2 * velocity clamped to +-3; then refills the interest polygons.
    bool UpdateInterest(TopologyCoordinate* c, float minX, float minY, float maxX, float maxY, const float* vel);

    /// sub_5E5890: rebuilds the interest list from the interest box (when one is set).
    void RefreshInterest(TopologyCoordinate* c);

    /// TopologyCoordinate::SetSectionPosition 0x5E68D0: once the point has moved (Manhattan distance)
    /// more than slack + radius from where the section was last checked, re-evaluates the boundary
    /// (VisibleSectionCoordinate::FullUpdate) and (re)connects to the tree of the boundary's section.
    void SetSectionPosition(TopologyCoordinate* c, float px, float py, float slack);

    /// sub_5DB850: the drivable boundary containing the point (moved to the front), else the nearest one within 0.1.
    SectionBoundaryU2* FindBoundary(float px, float py);

    /// sub_5DB770: the boundary containing the point (distance 0, moved to the front) or the nearest one.
    SectionBoundaryU2* Nearest(float px, float py, float& distance);

    /// sub_5D7090: box and crossing-number test.
    static bool Contains(const SectionBoundaryU2& b, float px, float py);

    /// Distance from the point to the segment a-b as sub_5D7270 / sub_5D70D0 / sub_5D3450 compute it.
    static F24 SegmentDistance(float px, float py, float ax, float ay, float bx, float by);

    /// TopologyCoordinate::NewGetElevation 0x5E72E0 (GetElevation 0x5E7E70 jumps to it): ground height
    /// under the point. Re-checks the section (slack 9), tries the 4 most recently used polygons, then
    /// the polygons of the point's grid cell/sub-cell in two passes (first the highest surface at most
    /// z + 1, then any). Writes terrain, normal (may be null) and valid; without a hit returns z with terrain 1,
    /// normal (0, 0, -1) and valid false.
    F24 NewGetElevation(TopologyCoordinate* c, float px, float py, float pz, int& terrain, float* normal, bool& valid);

    /// TopologyCoordinate::HasTopology 0x5E7E80: NewGetElevation at z = 99999.1, returns valid.
    bool HasTopology(TopologyCoordinate* c, float px, float py);

private:
    std::unordered_map<int, TopologyTree*> slots;
    std::vector<int> slotOrder;                    // load order of the slots (C# Dictionary enumeration order)
    std::vector<std::unique_ptr<TopologyCoordinate>> coordinates;

    static constexpr int MaxListed = TopologyCoordinate::MaxInterest;

    static void ClearInterest(TopologyCoordinate* c);
    void FillInterest(TopologyTree* t, TopologyCoordinate* c, float minX, float minY, float maxX, float maxY);
    static int Ftol(F24 v) { return X87::FtoL2Lo(v.V); }
    bool FullUpdate(TopologyCoordinate* c, float px, float py, float slack);
    void MoveToFront(int i);
    static bool InBox(const SectionBoundaryU2& b, float px, float py, float eps);
    static bool PointInPolygon(const SectionBoundaryU2& b, float px, float py);
    static bool Crosses(float yi, float yj, float py);
    static F24 InsideDistance(const SectionBoundaryU2& b, float px, float py);
    static F24 OutsideDistance(const SectionBoundaryU2& b, float px, float py, float max);
};

} // namespace bb

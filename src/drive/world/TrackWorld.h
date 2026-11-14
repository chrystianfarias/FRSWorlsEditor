#pragma once

// Port of NFSU2Unity Core/World/TrackWorld.cs: the game's track topology over a region, for the wheels
// (TopologyCoordinate::GetElevation 0x5E7E70 / NewGetElevation 0x5E72E0, GetRoadNoise 0x5D3CA0), the per-car
// TopologyCoordinate and the track polygons the car collides with.
//
// Editor adaptation: Unity streams a WorldRegionU2 and mirrors its loaded sections into the topology on every
// query. In the editor every section of the region is in memory, so TrackWorld is built once from plain data:
// the layout file (L4Rx.BUN, with the editor's layout spans applied) and each section's collision pack as a raw
// 0x80034130 chunk (nfs::EncodeCollision of the pack as edited). Every section counts as loaded; the slot ->
// section choice (main set first, then the first alternate) is the Unity one.

#include "drive/io/Vec3.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"
#include "drive/physics/TrackTopology.h"
#include "drive/physics/collision/GjkShapes.h"
#include "drive/physics/collision/WorldCollision.h"
#include "drive/physics/wheel/WheelEnvironment.h"
#include "drive/world/TopologyU2.h"
#include "drive/world/TrackCollisionU2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bb {

/// One section's collision as TrackWorld takes it (the editor's replacement for a loaded WorldSectionU2).
struct SectionCollisionInput {
    /// Section number (table entry; e.g. 329 for C29).
    int Number = 0;
    /// Index of the section in the region's section table (orders the track polygons, see SortKey).
    int Index = 0;
    /// Second set of sections (ordering key >= 20000): only used for a slot no main-set section fills.
    bool IsAlternate = false;
    /// The complete 0x80034130 chunk (8-byte header included), e.g. from nfs::EncodeCollision.
    std::vector<uint8_t> Collision;
};

/// The game's track topology over a region: TopologyCoordinate::GetElevation 0x5E7E70 (NewGetElevation
/// 0x5E72E0) and GetRoadNoise 0x5D3CA0 for the wheels, the per-car TopologyCoordinate (section boundaries,
/// 4-entry polygon cache) and the track polygons the car collides with.
class TrackWorld : public IWheelWorld, public ITrackTopology, public IWorldCollision {
public:
    // TerrainTypeInfoTable 0x800EE0 (stride 0x98): +0x18 road-noise frequency, +0x1C amplitude; noise lattice 0x800BF8
    static constexpr uint32_t VA_TerrainTypeInfoTable = 0x800EE0, VA_RoadNoiseTable = 0x800BF8;
    static constexpr int TerrainStride = 0x98, TerrainCount = 29;

    /// The topology (TheTopologyManager + TheVisibleSectionManager).
    std::unique_ptr<TopologyManagerU2> Topology;

    /// exe: SPEED2.EXE (tables, constants; verified, throws std::runtime_error on a mismatch).
    /// layoutFile: TRACKS\<REGION>.BUN as the game reads it (GJK volumes 0x80034020, boundaries 0x80034150,
    /// barrier groups 0x3412F). sections: the positioned, non-placeholder sections that have a collision pack.
    /// gameDir: the game folder, for the cars' CARS\<CAR>\GEOMETRY.BIN hulls (FindCollisionVolume); may be empty.
    TrackWorld(const Speed2Image& exe, const std::vector<uint8_t>& layoutFile,
               const std::vector<SectionCollisionInput>& sections, const std::wstring& gameDir = std::wstring());
    ~TrackWorld() override;
    TrackWorld(const TrackWorld&) = delete;
    TrackWorld& operator=(const TrackWorld&) = delete;

    /// Loads the trees of the slots (sub_5E78A0 / sub_5E6980): a slot holds its main-set section, else the first
    /// alternate. Every section is loaded in the editor, so this does its work once (the constructor calls it).
    void Sync();

    // ------------------------------------------------------------------ ITrackTopology

    void* CreateTopologyCoordinate() override;
    void SetSectionPosition(void* coordinate, float x, float y, float slack) override;
    bool IsConnected(void* coordinate) override;

    // ------------------------------------------------------------------ IWorldCollision

    bool UpdateInterest(void* coordinate, float minX, float minY, float maxX, float maxY, const float* velocity) override;
    int InterestCount(void* coordinate) override;
    TrackPolygonShape* Interest(void* coordinate, int i) override;

    /// FindGJKPolytope 0x5A18C0 over the volumes loaded so far (L4Rx.BUN's chunk 0x80034020, then the cars' own
    /// CARS\<car>\GEOMETRY.BIN, loaded on the first request), first match in load order.
    GjkPolytope* FindCollisionVolume(const std::string& carName, uint32_t nameHash) override;

    // ------------------------------------------------------------------ IWheelWorld

    /// TopologyCoordinate::GetElevation 0x5E7E70 (= NewGetElevation 0x5E72E0) on the given coordinate (null: a shared one).
    float GetElevation(void* topology, const float* point, int& terrain, float* normal, bool& valid) override;

    /// GetRoadNoise(bVector2 *, TerrainType) 0x5D3CA0: bilinear value noise on the integer lattice of
    /// (x, y) * frequency, lattice values flt_800BF8[(-5*i*j - 3*(i+1)*(j+1)) & 15], times the amplitude
    /// (TerrainTypeInfoTable +0x18 / +0x1C). The x87 order of the original is kept.
    float GetRoadNoise(const float* pointXY, int terrain) override;

    // ------------------------------------------------------------------ helpers

    /// Ground under (x, y) for spawning: the game's elevation query at z = zHint (its first pass takes the
    /// highest multi-level surface not above zHint + 1; single-level polygons are taken at any height), through
    /// the section boundary of the point; when the point lies outside every loaded section boundary, the loaded
    /// sections' grids are tried in turn.
    bool TryFindGround(float x, float y, float zHint, float& z);

    /// Centre of the collision ground polygon nearest (in plan) to (x, y) over the packs the topology uses
    /// (WorldViewer.TryNearestGround / TrackCollisionU2.TryNearestGround); false when there is no ground.
    bool TryNearestGround(float x, float y, Vec3& point) const;

    /// Viewer helper (TrackCollisionU2.TryGetGroundHeight over the packs the topology uses): the highest ground
    /// polygon under (x, y) not above maxZ; not the game's query.
    bool TryGroundHeight(float x, float y, float maxZ, float& z) const;

    /// Section number of the tree a coordinate is connected to, or 0.
    int ConnectedSection(void* coordinate);

private:
    float noiseFrequency[TerrainCount] = {}, noiseAmplitude[TerrainCount] = {};
    float noiseLattice[16] = {};

    TopologyCoordinate* defaultCoordinate = nullptr;
    TopologyCoordinate* spawnCoordinate = nullptr;

    /// The collision packs by section table index (owned).
    std::unordered_map<int, std::unique_ptr<TrackCollisionU2>> packs;
    /// Tree slot -> candidate table indices (main set first), in first-seen slot order.
    std::vector<std::pair<int, std::vector<int>>> slotSections;
    std::unordered_map<int, size_t> slotIndex;
    std::unordered_map<int, std::unique_ptr<TopologyTree>> slotTree;
    bool synced = false;

    std::vector<std::unique_ptr<GjkPolytope>> volumes;
    std::unordered_set<std::string> carsLoaded;
    std::wstring gameDir;

    void Candidate(int number, int index);
    TopologyCoordinate* Coordinate(void* o) const;
    void ResetCoordinate(TopologyCoordinate* c);
    static int Hash(int i, int j);
};

} // namespace bb

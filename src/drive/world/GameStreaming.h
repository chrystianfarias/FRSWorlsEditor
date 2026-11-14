#pragma once

// The game's streaming rules (SPEED2.EXE v1.2 TrackStreamer + VisibleSectionManager) as a pure data model, for
// the test drive and for the editor's "what would the game load here" questions. See docs/STREAMING-STUDY.md;
// the section numbers (§) below refer to it.
//
// Three zones are tracked at once, each looked up in the drivable outlines of chunk 0x00034152:
//  - LOADING zone (§1.2): the car, or where it will be 1.5 s later (at most 100 m) inside a streamer prediction
//    zone (PathsFreeRoam.bin, type 0x0C); a 0x34112 streaming barrier crossed by pos->pred cancels the prediction.
//    It decides what is in memory: WantedSet (§1.3), trimmed by PoolFit (§1.5).
//  - DRAWING zone (§1.7): the camera. It decides which LOADED sections are drawn (DrawSet).
//  - COLLISION zone (§3.1): the car, no prediction, kept while inside. Its tree is the pack whose header number
//    folds to the zone's slot, if a loaded section holds one (CollisionPackFor).
//
// The model is built from the layout file as the editor will WRITE it (the shipped L4Rx.BUN with the editor's
// layout spans laid over: edited zone lists, repointed/resized sections) and, when a World is given, from the
// editor's parsed sections (collision pack header numbers, model hashes) as they are edited right now.
//
// Exact (read in the code or measured): zone lookups (the ported topology's MRU + 0.1 rule), the prediction
// constants (read from the exe), the wanted-set rule, the drawing rule, the slot rule, the pool sizes, the drop
// scores. Approximated (marked [I] where they are used): sizes held in the pool (see SizeModel), the order in
// which equal-score sections are dropped, the "last loaded wins" order of two packs on one slot, the
// FindZone polygon test of the prediction zones, the segment test of the barriers.

#include "drive/world/TopologyU2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfs { class World; struct FreeRoamZone; }

namespace bb {

class Speed2Image;

/// Section numbers are letter * 100 + n (A = 1, ..., X = 24, Y = 25, Z = 26), e.g. 324 = C24.
std::string StreamSectionName(int number);

/// One 0x00034110 table entry (0x50 bytes) as TrackStreamer reads it (§1.1).
struct StreamSection {
    int16_t Number = 0;
    std::string Name;
    int TableIndex = -1;
    uint32_t FullSize = 0;       // +0x18: allocation size while loading
    uint32_t PermanentSize = 0;  // +0x1C: size without pixel data [I: what stays resident after activation]
    uint32_t OrderKey = 0;       // +0x20: 10 X0, 20 Z0, 30 Y0, 2001.. Y, 3001.. X, 10000+ main, 20000+ alternate
    float CentreX = 0, CentreY = 0, Radius = 0;   // +0x24, +0x28, +0x2C
    /// Header section numbers of the collision packs the section holds (the editor's parsed packs; empty
    /// when the model was built without a World).
    std::vector<int16_t> PackHeaders;
};

/// A streamer prediction zone: PathsFreeRoam.bin chunk 0x3414A record of type 0x0C (§1.2).
struct PredictionZone {
    float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    std::vector<float> X, Y;
    int Zones[4] = {};           // +0x30..+0x3C: the zones a prediction may switch to (0 = unused)
};

/// A 0x00034155 "CC" group (76 bytes): zones that load the union of their lists, plus extra sections.
struct CcGroup {
    std::string Name;
    std::vector<int16_t> Zones;      // +0x18 count, +0x1A zones[16]
    std::vector<int16_t> Sections;   // +0x3A count, +0x3C sections[8]
};

/// A 0x00034154 record (32 bytes): sections loaded for one track id (races; none in free roam).
struct TrackSections {
    int TrackId = 0;
    std::vector<int16_t> Sections;
};

/// A 0x00034112 streaming barrier: a 2D segment that cancels a prediction crossing it.
struct StreamBarrier {
    float X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
};

/// Which size a section holds in the pool.
enum class SizeModel {
    Full,        // +0x18 for every section: the allocation while loading, the study's per-zone figures
    Permanent,   // +0x1C for every section [I: resident after activation; to confirm in the game, E3]
};

/// §1.5 drop priority of a wanted section when the pool overflows; higher is dropped first, -1 never.
int DropScore(int number, int base, bool loaded);

struct PoolFitResult {
    std::vector<int16_t> Kept, Dropped;   // in wanted order / in drop order
    uint64_t WantedBytes = 0;             // everything wanted
    uint64_t KeptBytes = 0;
    uint64_t PoolBytes = 0;
    bool Fits() const { return Dropped.empty(); }
};

struct CollisionPackRef {
    int16_t Section = 0;   // the section that holds the pack
    int16_t Header = 0;    // the pack's own header number
    int Slot = 0;          // Slot(Header)
};

/// The collision a zone gets from a loaded set (§2.3, §3.1).
struct CollisionPackStatus {
    int Zone = 0;
    int Slot = -1;
    /// Loaded packs whose header folds to the zone's slot, in the [I] load order (the last one is the tree the
    /// game keeps: "last loaded wins").
    std::vector<CollisionPackRef> Providers;
    /// Loaded packs whose header number is not in [base, 2 base) nor [1, base): they alias into another
    /// letter's slots (C41 -> E1) and overwrite whatever tree is there.
    std::vector<CollisionPackRef> Aliases;
    bool Missing() const { return Zone > 0 && Providers.empty(); }
    bool Conflict() const { return Providers.size() > 1; }
};

/// What the loading-zone lookup decided (§1.2).
struct LoadingZoneResult {
    int Zone = 0;                 // the loading zone (0: outside every outline)
    int ZoneAtCar = 0;            // the zone of the position itself
    int PredictionZone = -1;      // index into PredictionZones() of the zone that holds the position, or -1
    bool Predicted = false;       // a predicted zone other than ZoneAtCar was taken
    bool BarrierCancelled = false;// a prediction was taken, then a streaming barrier cancelled it
    float PredX = 0, PredY = 0;   // the predicted point
    int PredictedZone = 0;        // the zone of the predicted point
};

/// The game's streaming state for one position (one player).
struct StreamingState {
    LoadingZoneResult Loading;
    int DrawingZone = 0;
    int CollisionZone = 0;
    const CcGroup* Group = nullptr;          // the CC group the loading zone belongs to
    std::vector<int16_t> Wanted;             // WantedSet(loading zone)
    PoolFitResult Fit;                       // Wanted trimmed to the pool
    std::vector<int16_t> Drawn;              // DrawSet(drawing zone, Fit.Kept)
    CollisionPackStatus Collision;           // CollisionPackFor(collision zone, Fit.Kept)
};

class GameStreamingModel {
public:
    /// The game's constants, from SPEED2.EXE when available (else the v1.2 values).
    struct Constants {
        uint32_t PoolKiB[3] = { 60000, 43000, 36000 };   // TrackStreamingPoolSizes 0x79DC54
        float PredictSeconds = 1.5f;                      // flt_7A47A4
        float PredictMax = 100.0f;                        // flt_7A47A8
        float PredictMaxSpeed = 178.8109131f;             // flt_7A52F4 (400 mph)
        bool FromExe = false;
    };

    /// Builds the model from a layout file as the game reads it. freeRoam: PathsFreeRoam.bin zone records (may
    /// be empty). region: e.g. L"L4RA" (L4RG gets the third pool size). exe may be null.
    GameStreamingModel(const std::vector<uint8_t>& layout, const std::vector<nfs::FreeRoamZone>& freeRoam,
                       const std::wstring& region, const Speed2Image* exe);

    /// Builds it from the editor's open region as it will be written (layout spans applied), reading the
    /// region's PathsFreeRoam.bin from gameFolder, and attaches the sections' collision headers and model
    /// hashes (AttachWorld). Null with `error` when no region is open or the layout has no visibility data.
    static std::unique_ptr<GameStreamingModel> FromWorld(nfs::World& world, const std::wstring& gameFolder,
                                                         const Speed2Image* exe, std::wstring& error);

    /// The editor's layout file with its layout spans laid over the shipped bytes.
    static std::vector<uint8_t> EditedLayout(const nfs::World& world);

    /// Reads the collision pack headers and the model hashes of the editor's parsed sections (World::LoadSection
    /// on every table entry not parsed yet).
    void AttachWorld(nfs::World& world);
    bool HasWorld() const { return worldAttached_; }

    // ---- data ------------------------------------------------------------------------------------------
    int Base() const { return base_; }
    const Constants& Consts() const { return k_; }
    uint64_t PoolBytes() const { return poolBytes_; }
    const std::vector<StreamSection>& Sections() const { return sections_; }
    const StreamSection* Section(int number) const;
    const std::vector<int16_t>* ListOf(int zone) const;   // the zone's 0x34153 list, or null
    const std::unordered_map<int, std::vector<int16_t>>& Lists() const { return lists_; }
    const std::vector<CcGroup>& Groups() const { return groups_; }
    const std::vector<TrackSections>& Tracks() const { return tracks_; }
    const std::vector<StreamBarrier>& Barriers() const { return barriers_; }
    const std::vector<PredictionZone>& PredictionZones() const { return prediction_; }
    const VisibleSectionsU2& Outlines() const { return outlines_; }
    /// Drivable outline ids (letter A..T, 1 <= n % 100 < base), file order.
    const std::vector<int>& DrivableZones() const { return drivable_; }
    bool IsDrivableZone(int number) const;

    // ---- the rules -------------------------------------------------------------------------------------
    /// A zone finder with its own most-recently-used order (the game keeps one per manager): a
    /// TopologyManagerU2 over the drivable outlines, without trees.
    std::unique_ptr<TopologyManagerU2> NewFinder() const;

    /// sub_5E00D0 / sub_5DB850: the outline that holds the point (MRU first), else the nearest one within
    /// 0.1, else 0 ("outside every outline": the game unloads the whole city there).
    static int ZoneAt(TopologyManagerU2& finder, float x, float y);

    /// sub_5E4F20 (§1.2): the loading zone of a position moving at vel (xy, m/s).
    LoadingZoneResult LoadingZone(TopologyManagerU2& finder, float x, float y, float vx, float vy) const;

    /// §1.7: the zone of the camera (same lookup, its own MRU).
    static int DrawingZone(TopologyManagerU2& finder, float x, float y) { return ZoneAt(finder, x, y); }

    /// §3.1: the collision zone as the car's TopologyCoordinate keeps it (SetSectionPosition, slack 0 as
    /// sub_5E7100 calls it): kept while inside, then the containing / nearest-within-0.1 outline. `coordinate`
    /// comes from finder.CreateCoordinate().
    static int CollisionZone(TopologyManagerU2& finder, TopologyCoordinate* coordinate, float x, float y);

    /// §1.3: X0, Y0, Z0, the 0x34154 sections of trackId (0 = free roam), then the zone's list, or the union
    /// of its CC group's lists plus the group's sections. Deduplicated, in the game's order, numbers without a
    /// table entry skipped. `group` receives the CC group used, if any.
    std::vector<int16_t> WantedSet(int zone, int trackId = 0, const CcGroup** group = nullptr) const;

    /// The CC group a zone belongs to (the first that names it [I]), or null.
    const CcGroup* GroupOf(int zone) const;

    uint64_t SectionSize(int number, SizeModel model) const;
    uint64_t TotalSize(const std::vector<int16_t>& numbers, SizeModel model) const;

    /// §1.5: drops wanted sections, highest DropScore first (first in wanted order on a tie [I]), until
    /// size + 16 KiB fits the pool. Sections in `alreadyLoaded` count at PermanentSize and score as loaded,
    /// the others at `newModel` (Full: the allocation while they load).
    PoolFitResult PoolFit(const std::vector<int16_t>& wanted, SizeModel newModel = SizeModel::Full,
                          const std::unordered_set<int>* alreadyLoaded = nullptr) const;

    /// §1.7: the loaded sections the camera's zone draws: every loaded Z section and every loaded section of
    /// the zone's own 0x34153 list; with no zone, only loaded sections whose n % 100 < 10.
    std::vector<int16_t> DrawSet(int drawingZone, const std::vector<int16_t>& loaded) const;

    /// §3.1 sub_5D3C10: tree slot of a section number.
    int Slot(int number) const;

    /// §2.3: which loaded pack the zone's collision comes from. Requires AttachWorld.
    CollisionPackStatus CollisionPackFor(int zone, const std::vector<int16_t>& loaded) const;

    /// Model hashes held by the given loaded sections (§2.1: the game binds models over every loaded pack).
    /// Requires AttachWorld.
    std::unordered_set<uint32_t> LoadedModels(const std::vector<int16_t>& loaded) const;

    /// Everything above for one position: car position and velocity, camera position. The finders and the
    /// coordinate carry the MRU and "keep while inside" state from call to call.
    struct Tracker {
        std::unique_ptr<TopologyManagerU2> Loading, Drawing, Collision;
        TopologyCoordinate* Coordinate = nullptr;
    };
    Tracker NewTracker() const;
    void Update(Tracker& tracker, float carX, float carY, float velX, float velY, float camX, float camY,
                StreamingState& out, const std::unordered_set<int>* alreadyLoaded = nullptr) const;

    // ---- static audits (self-test, tools) -------------------------------------------------------------
    /// Ordered pairs (a, b) of drivable outlines within maxDistance of each other (adj.py's measure: the
    /// smaller of the two vertex-to-edge distances), and those where b's geometry section (b + base) is not in
    /// a's own list / not in a's wanted set (CC unions applied).
    struct AdjacencyAudit {
        int Pairs = 0;
        std::vector<std::pair<int, int>> NotInList, NotWanted;
    };
    AdjacencyAudit AuditAdjacency(float maxDistance = 1.0f) const;

private:
    Constants k_;
    int base_ = 20;
    uint64_t poolBytes_ = 0;
    std::vector<StreamSection> sections_;
    std::unordered_map<int, size_t> byNumber_;
    VisibleSectionsU2 outlines_;
    std::vector<int> drivable_;
    std::unordered_map<int, std::vector<int16_t>> lists_;
    std::vector<int> listOrder_;
    std::vector<CcGroup> groups_;
    std::vector<TrackSections> tracks_;
    std::vector<StreamBarrier> barriers_;
    std::vector<PredictionZone> prediction_;
    bool worldAttached_ = false;
    std::unordered_map<int, std::vector<uint32_t>> models_;   // section number -> model hashes

    void ReadLayout(const std::vector<uint8_t>& layout);
};

/// FRS_STREAM_SELFTEST=1: reproduces the study's L4RA figures from the shipped layout and from the layout as
/// the editor will write it, and logs them (FRSWorldEditor.log, "stream selftest:" lines).
void RunGameStreamingSelfTest(nfs::World& world, const std::wstring& gameFolder);

} // namespace bb

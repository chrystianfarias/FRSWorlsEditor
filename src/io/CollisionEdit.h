#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/Collision.h"

#include <string>
#include <vector>

namespace nfs { class World; struct Section; }

// Editing the world's collision inside the editor.
//
// Two halves. The first makes polygons the way the game stores them: ground
// (triangles and quads), barriers (walls of unbounded height) and vertical
// walls (a real height, which a car can jump), from points in world space.
// The second puts them into the world: each polygon goes into every
// section's pack whose ground lies under it -- the game stores the polygons
// along a section edge in both neighbours, and a car near the edge may be
// using either -- and removing or changing a polygon reaches every copy of
// it, for the same reason. Changes are kept in the loaded sections
// (LoadedSection::collisionChanged) until the next save writes them
// (PlanPlacementSave), and each one can be undone.
//
// Everything made here carries race barrier group 0, so it exists in free
// roam as well as in races (see World::RaceBarrierGroups).
namespace collision {

// --- making polygons ---------------------------------------------------------

// A triangle (3 corners) or a quad (4, perimeter order, convex in plan).
nfs::CollisionPolygon MakeGround(const Vec3* corners, int count, u8 surface);

// A barrier from `from` to `to`: blocks cars on its left-hand side only
// (looking from `from` towards `to`); AppendWall writes both directions.
nfs::CollisionPolygon MakeBarrier(const Vec3& from, const Vec3& to, u8 surface);

// A vertical wall standing from `bottom` up to `bottom + height` at each end
// (z of the two points is the foot). Stored as the game stores its kerbs and
// building sides: (from foot, from top, to top, to foot).
nfs::CollisionPolygon MakeVerticalWall(const Vec3& from, const Vec3& to, float height, u8 surface);

enum class WallKind {
    Barrier,     // unbounded height: nothing gets over it
    Vertical,    // real height: a car that flies high enough clears it
};

struct WallOptions {
    WallKind kind    = WallKind::Barrier;
    u8       surface = nfs::kSurfaceSolidWall;
    float    height  = 3.0f;     // vertical walls: above the points
    float    sink    = 0.5f;     // vertical walls: how far the foot goes below the points
    bool     bothSides = true;   // a twin facing the other way, so it blocks from either side
    float    maxPiece  = 8.0f;   // longer segments are cut into pieces of at most this
};

// A wall along a polyline of points on the ground: one piece per segment
// (cut up when long). Repeat the first point at the end for a closed loop.
void AppendWall(const std::vector<Vec3>& points, const WallOptions& options,
                std::vector<nfs::CollisionPolygon>& out);

// Ground over the area a closed outline encloses (points in order, either
// winding, no repeat of the first). Three points make a triangle, four that
// are convex in plan a quad, anything else is cut into triangles. False when
// the outline encloses nothing usable.
bool AppendGround(const std::vector<Vec3>& outline, u8 surface,
                  std::vector<nfs::CollisionPolygon>& out);

// A strip of ground between two edges with the same number of points --
// a road, `left[i]` and `right[i]` facing each other across it. One quad per
// step, cut in two where it is not convex in plan.
void AppendStrip(const std::vector<Vec3>& left, const std::vector<Vec3>& right, u8 surface,
                 std::vector<nfs::CollisionPolygon>& out);

// --- changing the world -------------------------------------------------------

// One polygon of one loaded section's pack. Indices into World::Loaded(), the
// section's `collision` and the pack's `polygons`.
struct PolygonRef {
    u32 section = 0;
    u32 pack    = 0;
    u32 polygon = 0;
    bool operator==(const PolygonRef& o) const {
        return section == o.section && pack == o.pack && polygon == o.polygon;
    }
};

// One change kept, as the polygons it took out of and put into each pack.
// Packs are named by section and their place among the section's packs, so
// an entry outlives the session that made it.
struct PackDelta {
    std::string section;
    u32 pack = 0;
    std::vector<nfs::CollisionPolygon> removed, added;
};
struct LogEntry {
    std::wstring label;
    std::vector<PackDelta> packs;
    size_t Polygons() const;
};

// The log as a text file (models\collision-<REGION>.txt).
bool SaveLog(const std::wstring& path, const std::vector<LogEntry>& log);
bool LoadLog(const std::wstring& path, std::vector<LogEntry>& log);

class Editor {
public:
    // Forgets the undo history: its indices belong to the world it was made in.
    void SetWorld(nfs::World* world);
    nfs::World* World() const { return world_; }

    // Puts polygons into the world. Each goes into every pack whose ground
    // lies under it (or, over no ground at all, the pack nearest to it). A
    // polygon the pack already holds, shape for shape, is not added twice.
    // Returns every polygon added, copies included. `label` names the step
    // for Undo.
    std::vector<PolygonRef> Add(const std::vector<nfs::CollisionPolygon>& polygons,
                                const std::wstring& label);

    // The packs a polygon would go into: (loaded section, pack) pairs.
    std::vector<std::pair<u32, u32>> PacksFor(const nfs::CollisionPolygon& polygon) const;

    // `refs` and every stored copy of the same polygon in other packs.
    std::vector<PolygonRef> WithCopies(const std::vector<PolygonRef>& refs) const;
    // How many packs hold the same polygon as `ref`, itself included.
    std::vector<PolygonRef> CopiesOf(const PolygonRef& ref) const;

    // Each of these reaches every copy, and returns how many polygons changed.
    size_t Remove(const std::vector<PolygonRef>& refs);
    size_t SetSurface(const std::vector<PolygonRef>& refs, u8 surface);
    size_t SetRaceGroup(const std::vector<PolygonRef>& refs, u8 group);

    // Moving a selection: Begin records it (copies included), Update places
    // it `offset` away from where it started, End keeps or puts it back.
    bool BeginMove(const std::vector<PolygonRef>& refs);
    void UpdateMove(const Vec3& offset);
    void EndMove(bool keep);
    bool Moving() const { return moving_; }

    // Reshaping (the edit mode's vertices, edges and faces): Begin records
    // exactly `refs` -- the caller includes the copies -- as one undo step,
    // SetShape gives one of them new corners, EndMove keeps or puts back.
    bool BeginReshape(const std::vector<PolygonRef>& refs);
    void SetShape(const PolygonRef& ref, const nfs::CollisionPolygon& polygon);

    // A polygon as it is now, or nullptr.
    const nfs::CollisionPolygon* Polygon(const PolygonRef& ref) const;
    // Where a barrier stands on screen: its quad with the ground's heights.
    bool BarrierQuad(const PolygonRef& ref, Vec3 out[4]) const;

    // Counters of undo steps recorded and taken back, for a history that
    // interleaves these with other kinds of edit.
    u64 StepsRecorded() const { return recorded_; }
    u64 StepsUndone() const { return undone_; }

    // Every change kept since the shipped collision, one entry per step --
    // the Edits tab's collision rows. Reading it closes the step in progress.
    const std::vector<LogEntry>& Log();
    void SetLog(std::vector<LogEntry> log) { log_ = std::move(log); openStep_ = false; }
    // Steps that change the collision without an entry: a tool keeping its
    // own record of what it made (the roads' bake) turns this off around it.
    void SetLogging(bool on) { FinishOpenStep(); logging_ = on; }
    // Takes entries back: each pack they touched is rebuilt from the shipped
    // pack with every other entry replayed in order. Clears the undo history.
    size_t RevertLog(const std::vector<size_t>& entries, std::wstring& summary);

    bool CanUndo() const { return !undo_.empty(); }
    std::wstring UndoLabel() const { return undo_.empty() ? std::wstring() : undo_.back().label; }
    bool Undo();
    void ClearHistory() { FinishOpenStep(); undo_.clear(); }

    // The loaded sections whose collision changed since the last call, so the
    // caller can redraw them.
    std::vector<u32> TakeTouchedSections();

private:
    struct PackState {
        u32 section = 0, pack = 0;
        nfs::CollisionPack data;
        bool sectionChanged = false;
        std::vector<u32> changedPacks;
    };
    struct Step {
        std::wstring label;
        std::vector<PackState> packs;
        long logIndex = -1;     // its entry in log_, once closed
        bool logged = true;
    };
    void FinishOpenStep();
    std::vector<LogEntry> log_;
    bool openStep_ = false;     // undo_.back() still changing, not in the log yet
    bool logging_ = true;

    nfs::CollisionPack* Pack(u32 section, u32 pack) const;
    // Records the packs about to change as one undo step.
    void Remember(const std::wstring& label, std::vector<std::pair<u32, u32>> packs);
    void Changed(u32 section, u32 pack);

    nfs::World* world_ = nullptr;
    std::vector<Step> undo_;
    u64 recorded_ = 0, undone_ = 0;
    std::vector<u32> touched_;

    bool moving_ = false;
    std::vector<PolygonRef> moveRefs_;
    std::vector<nfs::CollisionPolygon> moveStart_;
};

// --- saving ------------------------------------------------------------------

// For each pack of the section, as the loader lists them (empty packs are
// skipped), its position among all the section's collision chunks -- which
// is what SectionEdit::order counts for a collision edit.
bool CollisionChunkOrders(const nfs::World& world, const nfs::Section& section,
                          std::vector<u32>& orders);

} // namespace collision

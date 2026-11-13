#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "roads/GameCut.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Procedural roads: a graph of nodes joined by Bezier segments.
//
// The editor keeps roads as this description, never as a mesh: a segment is
// two nodes and a cross-section (lanes, sidewalks, walls, the game textures
// each part is painted with), and the geometry is generated from it whenever
// something changes. Where three or more segments meet at a node, the node is
// a junction: the roads are pulled back from it and the gap filled with a
// surface whose corners are rounded between neighbouring roads.
//
// Nothing here is the game's. Baking (roads/RoadBake.h) turns the generated
// mesh into models of the game's own kind, in the sections it stands over.
namespace roads {

// What a segment looks like across its width.
struct RoadProfile {
    // Lanes in the segment's own direction (node a to node b), on the right of
    // the centre line, and against it, on the left.
    int   lanesForward  = 1;
    int   lanesBackward = 1;
    float laneWidth     = 4.0f;

    // The game's lane textures carry their marking on the left edge: the
    // lane beside the centre line is painted with `laneCentre` (a solid
    // yellow line), every other lane with `laneInner` (a dashed white one).
    std::string laneCentre = "RDP_LANEA_YS_AA_KT";
    std::string laneInner  = "RDP_LANEB_WD_AA_KT";
    float textureLength    = 32.0f;   // road length one repeat of a lane texture covers

    // Raised sidewalks on either edge; width 0 leaves them out.
    float sidewalkWidth   = 0.0f;
    float sidewalkHeight  = 0.2f;
    std::string sidewalkTexture = "ARC_SIDEWALK_E";

    // Walls along the outer edges.
    bool  wallLeft  = false;
    bool  wallRight = false;
    float wallHeight = 1.2f;
    std::string wallTexture = "ARC_BARRIERTYPEFWY_WH";
    float wallTextureLength = 8.0f;

    // Street lights: a game model stood along the road every `poleSpacing`,
    // on the right, the left, both sides or alternating. Empty: none.
    std::string poleModel;
    float poleSpacing = 30.0f;
    int   poleSides   = 0;        // 0 right, 1 left, 2 both, 3 alternating, 4 on the median
    float poleInset   = 0.8f;     // in from the outer edge (on the sidewalk), or out from the road edge
    float poleTurn    = 0.0f;     // degrees added to the pole's heading, which faces the road

    // The light baked into the vertex colours, worked out from the poles the
    // way the game's own roads are lit: a dim night ambient everywhere and a
    // pool of light under each lamp. The game draws texture x colour x 2.
    u32   ambient      = 0x383C42;   // RRGGBB
    u32   lampColor    = 0xFFD9A0;   // RRGGBB, at full strength
    float lampRadius   = 14.0f;
    float lampStrength = 1.0f;

    // A bridge: the road stands on a deck of its own thickness, held up by
    // pillars every `pillarSpacing` down to the ground (0 for none), and the
    // ground is not fitted under it.
    bool  bridge = false;
    float deckThickness = 1.2f;
    float pillarSpacing = 24.0f;
    float pillarWidth   = 1.8f;
    std::string deckTexture   = "ARC_CONCRETE_LIGHT2";
    std::string pillarTexture = "ARC_PILLARCONCRETE";

    // Leave the game's ground alone beside this road: set on the stretches
    // that stand in for one of the game's own roads, whose surroundings are
    // already right.
    bool keepGround = false;

    // Banking: the road tilted into its curves. Auto works the angle out of
    // the curve's radius and the speed it is built for, tan = v^2 / (g r),
    // never past `bankMax`; fixed holds `bankAngle` (positive: the right
    // edge up). Either way it eases to flat where the road meets a junction
    // or ends, so the junction stays level.
    int   bankMode = 0;            // 0 off, 1 auto, 2 fixed
    float bankMax = 10.0f;         // degrees
    float designSpeed = 120.0f;    // km/h
    float bankAngle = 6.0f;        // degrees, fixed mode

    // A median between the two directions: 0 none, 1 painted stripes, 2 a
    // raised concrete island, 3 a raised grass one, 4 a concrete barrier.
    int   medianType = 0;
    float medianWidth = 2.0f;
    float medianHeight = 0.2f;     // raised kinds; a barrier's height
    std::string medianTexture;     // empty: the kind's own

    // Sidewalks on each side, for roads whose inner side has none -- the two
    // halves of a split highway.
    bool sidewalkLeft = true, sidewalkRight = true;

    // How a wall is built: 0 flat, 1 a steel guardrail, 2 a game model
    // repeated along the edge every `wallModelSpacing`, turned to follow it.
    int   wallStyle = 0;
    std::string wallModel;
    float wallModelSpacing = 4.0f;
    float wallModelTurn = 0.0f;

    // Traffic and the GPS: whether the game's traffic drives this road, whether
    // the player's GPS (and the racers) may route over it, and the speed the
    // traffic keeps on its straights. Written into the game's road graph at
    // every bake, joined to the game's own roads automatically.
    bool  traffic = true;
    bool  gps = true;
    float speedLimit = 60.0f;      // km/h

    int   Lanes() const { return lanesForward + lanesBackward; }
    float MedianWidth() const { return medianType ? medianWidth : 0.0f; }
    float HalfRoad() const { return Lanes() * laneWidth * 0.5f + MedianWidth() * 0.5f; }
    float HalfWidth() const { return HalfRoad() + sidewalkWidth; }
    // Across the road, from its left edge: where the median starts.
    float MedianLeft() const { return -HalfRoad() + lanesBackward * laneWidth; }
    std::string MedianTexture() const {
        if (!medianTexture.empty()) return medianTexture;
        switch (medianType) {
            case 1:  return "RDP_ROADSTRIP_MC";
            case 3:  return "ARC_SIDEWALKGRASS1";
            case 4:  return "ARC_BARRIERTYPEFWY_WH";
            default: return "ARC_CONCRETE_LIGHT2";
        }
    }
    bool  Walk(int side) const { return sidewalkWidth > 0.01f && (side < 0 ? sidewalkLeft : sidewalkRight); }
    bool  Wall(int side) const { return side < 0 ? wallLeft : wallRight; }
    // The outer edge on one side, sidewalk included.
    float Edge(int side) const { return HalfRoad() + (Walk(side) ? sidewalkWidth : 0.0f); }
};

// A street light as generated: the model and where it stands, and the lamp
// it lights the road with.
struct Pole {
    std::string model;
    Vec3  position;
    float heading = 0.0f;     // degrees
    u32   segment = 0;
    Vec3  lamp;               // where the light comes from
    u32   color = 0;
    float radius = 0.0f;
    float strength = 0.0f;
};

struct Node {
    u32  id = 0;
    Vec3 position;
    // A node joining two segments turns its curve smoothly through itself; a
    // heading set by hand (R in the viewport) fixes the direction instead.
    bool  hasHeading = false;
    float heading = 0.0f;    // degrees, 0 = +X, counter-clockwise
    // At a junction: a pedestrian crossing painted across the end of every
    // road that meets here.
    bool  crosswalks = false;
    // Stands at a fixed place beside another node and turns with it: the
    // ends of the two halves of a split highway, kept on the road they leave.
    u32   follow = 0;
    float followOffset = 0.0f;     // to the right of the followed node's heading
};

// The game's crossing paint, and how much road it covers.
constexpr const char* kCrosswalkTexture = "RDP_INTERSECT_AA_WH";
constexpr float kCrosswalkDepth = 4.0f;
constexpr float kCrosswalkTile = 3.0f;     // across the road, one repeat of the stripes

struct Segment {
    u32 id = 0;
    u32 a = 0, b = 0;        // node ids
    RoadProfile profile;
    // How far the curve's handles reach from each end; 0 is automatic (a
    // third of the way). Their direction is the node's heading.
    float handleA = 0.0f, handleB = 0.0f;
};

// The cubic Bezier a segment follows.
struct Curve {
    Vec3 p0, p1, p2, p3;
    Vec3 At(float t) const;
    Vec3 Tangent(float t) const;   // not normalised
    Vec3 Second(float t) const;    // the second derivative
};

// One vertex of generated geometry: world position, normal, texture
// coordinate (Direct3D's V direction) and colour.
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    u32  color = 0xFF808080u;
};

// Triangles that share a texture.
struct MeshGroup {
    std::string texture;
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;      // counter-clockwise seen from the front
};

// What a car drives on and bumps into, in shapes the collision editor makes
// polygons of (io/CollisionEdit.h). Surfaces are the game's surface table
// indices: 0x01 road, 0x10 sidewalk, 0x14 solid wall.
constexpr u8 kSurfaceRoad = 0x01;
constexpr u8 kSurfaceSidewalk = 0x10;
constexpr u8 kSurfaceGrass = 0x0F;
constexpr u8 kSurfaceWall = 0x14;

struct CollisionStrip { std::vector<Vec3> left, right; u8 surface = kSurfaceRoad; };
struct CollisionArea  { std::vector<Vec3> outline; u8 surface = kSurfaceRoad; };
struct CollisionWall {
    std::vector<Vec3> points;
    bool  barrier = true;      // unbounded height; false: a kerb of `height`
    float height = 0.0f;
    u8    surface = kSurfaceWall;
};

// A piece of road that becomes one model when baked: a stretch of a segment
// or a junction. Kept short so each lands in the section it stands over.
struct MeshPiece {
    std::string name;              // ROAD_S<segment>_<n> or ROAD_J<node>
    u32 segment = 0;               // owner (0 for a junction)
    u32 node = 0;
    std::vector<MeshGroup> groups;
    std::vector<CollisionStrip> strips;
    std::vector<CollisionArea> areas;
    std::vector<CollisionWall> walls;

    MeshGroup& Group(const std::string& texture);
    size_t VertexCount() const;
    Aabb Bounds() const;
};

class Network {
public:
    void Clear() { nodes_.clear(); segments_.clear(); cuts_.clear(); gameJunctions_.clear(); nextId_ = 1; SetDirty(); }

    const std::vector<Node>& Nodes() const { return nodes_; }
    const std::vector<Segment>& Segments() const { return segments_; }

    const Node* FindNode(u32 id) const;
    Node* FindNode(u32 id);
    const Segment* FindSegment(u32 id) const;
    Segment* FindSegment(u32 id);
    int NodeIndex(u32 id) const;
    int SegmentIndex(u32 id) const;

    u32 AddNode(const Vec3& position);
    // Null when either node is missing, they are the same, or already joined.
    Segment* AddSegment(u32 a, u32 b, const RoadProfile& profile);

    // Removes the node with its segments, and any node left with none.
    void RemoveNode(u32 id);

    // Puts every following node beside the node it follows. True when one moved.
    bool UpdateFollowers();

    // Stretches of the game's roads replaced by procedural ones, and the
    // boxes the junctions made into them take out.
    std::vector<GameCut> Cuts() const;
    void AddCut(const GameCut& cut) { cuts_.push_back(cut); SetDirty(); }

    // Junctions made into the game's own roads.
    const std::vector<GameJunction>& GameJunctions() const { return gameJunctions_; }
    // Takes out a junction made into a game road (the road stays the game's
    // there again), or a stretch cut out of one.
    void RemoveGameJunction(size_t index) {
        if (index < gameJunctions_.size()) gameJunctions_.erase(gameJunctions_.begin() + (ptrdiff_t)index);
        SetDirty();
    }
    void RemoveCut(size_t index) {
        if (index < cuts_.size()) cuts_.erase(cuts_.begin() + (ptrdiff_t)index);
        SetDirty();
    }
    const std::vector<GameCut>& StoredCuts() const { return cuts_; }

    // The junction whose mouth `node` is, or null.
    const GameJunction* FindGameJunction(u32 node) const {
        for (const GameJunction& j : gameJunctions_)
            if (j.node == node) return &j;
        return nullptr;
    }
    GameJunction* FindGameJunction(u32 node) {
        for (GameJunction& j : gameJunctions_)
            if (j.node == node) return &j;
        return nullptr;
    }
    void AddGameJunction(const GameJunction& junction) { gameJunctions_.push_back(junction); SetDirty(); }
    // The new road's width at a junction: (half kerb to kerb, sidewalk).
    void MouthOf(const GameJunction& junction, float& half, float& walk) const;

    // Where the light comes from besides the network's own street lights:
    // `sample` reads the light the game baked into its own ground under a
    // point -- its street lights and signs, already in the vertex colours --
    // and `lamps` are the game's street lights, for ground with nothing of the
    // game's under it.
    struct LightSources {
        std::function<bool(const Vec3& point, u32& colour)> sample;
        std::vector<Pole> lamps;
    };
    void SetLightSources(std::shared_ptr<const LightSources> sources) { light_ = std::move(sources); SetDirty(); }
    const LightSources* Light() const { return light_.get(); }
    // Removes the segment, and either end left with nothing.
    void RemoveSegment(u32 id);

    // Hands every segment of `from` to `into` and removes `from`. Segments
    // that would join a node to itself, or duplicate another, go.
    void MergeNodes(u32 from, u32 into);

    // Splits the segment at `t` with a new node, which it returns (0 on failure).
    u32 SplitSegment(u32 segment, float t);

    std::vector<u32> SegmentsAt(u32 node) const;

    // The curve of a segment, with tangents worked out from its neighbours.
    Curve CurveOf(const Segment& segment) const;

    // The nearest point on a segment to a point (horizontal distance): `t`
    // and the distance.
    bool NearestOnSegment(const Segment& segment, const Vec3& point, float& t, float& distance) const;

    // Generates every piece. Junction trims are worked out first, so a
    // segment's ends meet the junction surface exactly.
    // `poles`, when given, receives the street lights; the vertex colours are
    // lit by them either way.
    std::vector<MeshPiece> BuildMesh(std::vector<Pole>* poles = nullptr) const;

    // Where the ground is under a point, for the pillars of bridges: the
    // highest ground below `top`. False when there is none.
    using GroundQuery = std::function<bool(float x, float y, float top, float& height)>;
    void SetGroundQuery(GroundQuery query) { ground_ = std::move(query); }

    // Save and load, as text.
    bool Save(const std::wstring& path) const;
    bool Load(const std::wstring& path);

    // Changes since the mesh was last built, and since the last bake.
    bool Dirty() const { return dirty_; }
    void SetDirty() { dirty_ = true; ++revision_; }
    void ClearDirty() { dirty_ = false; }
    u64  Revision() const { return revision_; }

private:
    std::vector<Node> nodes_;
    std::vector<Segment> segments_;
    std::vector<GameCut> cuts_;
    std::vector<GameJunction> gameJunctions_;
    GroundQuery ground_;
    std::shared_ptr<const LightSources> light_;
    u32 nextId_ = 1;
    bool dirty_ = true;
    u64 revision_ = 1;
};

// Lights `pieces` the way BuildMesh lights the roads: `ambient` (RRGGBB)
// everywhere, and the pool of every lamp in reach.
void ApplyLighting(std::vector<MeshPiece>& pieces, const std::vector<Pole>& poles, u32 ambient,
                   const Network::LightSources* sources = nullptr);

// Where the roads are, for ground that has to fit around them: the nearest
// road to a point, its height there and how far outside its edge the point
// is (negative inside). Built from the curves sampled every unit or so.
class RoadField {
public:
    // `include`, when given, picks the segments that count.
    void Build(const Network& network, const std::function<bool(const Segment&)>& include = nullptr);
    bool Empty() const { return samples_.empty(); }
    // False when no road is within `reach` of the edge.
    bool Query(float x, float y, float reach, float& height, float& outside) const;

private:
    struct Sample { Vec3 a, b; float half; };     // a stretch of centre line
    std::vector<Sample> samples_;
    std::vector<std::vector<u32>> cells_;
    float cellSize_ = 16.0f;
    float minX_ = 0, minY_ = 0;
    int   columns_ = 0, rows_ = 0;
    float widest_ = 0.0f;
};

} // namespace roads

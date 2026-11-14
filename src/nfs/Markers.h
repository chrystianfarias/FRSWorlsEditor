#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/FreeRoam.h"

#include <string>
#include <vector>

namespace nfs {

class World;

// The beacons the game stands in free roam: shops, the car lot, the garage and
// the race starts -- the region's own, and the ones placed in the editor.
//
// None of them is geometry. Each is a zone in the region's free-roam road
// graph (TRACKS\ROUTES<region>\PathsFreeRoam.bin, chunk 0x3414A) -- a centre,
// a facing, a radius and a footprint polygon, all in plan -- named by a hash
// that the career data in GLOBAL\GLOBALB.BUN points at: a shop record names
// its zone at +0x3C, a race its start trigger at +0x28. See
// docs/NFSU2-FORMATS.md, "Shops and map markers".
//
// A shop placed in the editor is a working copy of a shipped shop of its kind;
// a race start placed in the editor is a trigger that career races are moved
// to. Both live in <game>\models\markers-<region>.frsmarkers until
// io/MarkerWrite hands them to the game.

enum class MarkerKind {
    Graphics,        // PAINTSHOP01.BIN
    BodyShop,        // PARTSSHOP01.BIN
    Performance,     // PERFSHOP01.BIN
    CarSpecialties,  // AUDIOSHOP01.BIN
    CarLot,          // CARLOT01.BIN
    Garage,          // CRIB01.BIN
    OtherShop,       // a shop whose scene file is none of the above
    RaceStart,       // a race's start trigger
    Placed,          // placed in the editor; Marker::style says as what
};

const wchar_t* MarkerKindName(MarkerKind kind);

// The part of a shop's effect names that says which shop: "BODYSHOP_" in
// XEE_Event_BODYSHOP_IconA_01. Null for kinds with no effects of their own.
const char* MarkerEffectPrefix(MarkerKind kind);

// What the Library offers to place: the game's own beacons. Shops are working
// copies of a shipped shop of the kind (door, prompt and all); a race start is
// a trigger the chosen career races are moved to.
constexpr MarkerKind kPlaceableKinds[] = {
    MarkerKind::BodyShop, MarkerKind::Performance, MarkerKind::Graphics,
    MarkerKind::CarSpecialties, MarkerKind::CarLot, MarkerKind::Garage, MarkerKind::RaceStart,
};

// The game's own picture of a kind: SHOP_ICON_BODY and so on (GLOBALB), or
// MINIMAP_ICON_CIRCUIT for a race start (InGameCommon). Null for none.
const char* MarkerKindIcon(MarkerKind kind);

// The colour a kind is drawn with, 0xRRGGBB: the hue of the game's own ring
// and smoke for the shops, orange for a race start.
u32 MarkerKindColour(MarkerKind kind);

struct Marker {
    MarkerKind  kind = MarkerKind::Placed;
    std::string name;         // UC_BODYSHOP, a race trigger's name, or FRS_BODYSHOP_1
    std::string sceneFile;    // shops: the front-end scene the game opens
    u32  shopType = 0;        // shops: +0x50 of the career record
    u32  stage = 0;           // shops: the last word of the career record
    u32  hash = 0;
    Vec3 position;            // plan centre, z found on the collision
    float heading = 0.0f;     // degrees, from the zone's facing
    float radius = 0.0f;
    std::vector<Vec2> polygon;
    int  zone = -1;           // index into MarkerSet::Zones(), -1 for placed ones
    // Race starts: every race that starts here. On a race start placed in the
    // editor, the career races moved to it (by name, as CareerRaces()).
    std::vector<std::string> races;

    // Placed in the editor only.
    MarkerKind style = MarkerKind::BodyShop;   // what it was placed as
    // The shipped shop a placed shop is a working copy of: what it sells, its
    // scene, its prompt and its door.
    std::string opens;
    // The garage door, moved from where it stands to the copied shop -- along
    // the marker's facing, to its right and up, in units, and turned, in
    // degrees. All zero leaves it as at that shop.
    float doorForward = 0.0f, doorRight = 0.0f, doorUp = 0.0f, doorTurn = 0.0f;

    bool IsPlaced() const { return kind == MarkerKind::Placed; }
    // What it is in the game: its kind, or for a placed one the kind it was
    // placed as.
    MarkerKind GameKind() const { return IsPlaced() ? style : kind; }
    bool IsRaceStart() const { return GameKind() == MarkerKind::RaceStart; }
    u32 DrawColour() const { return MarkerKindColour(GameKind()); }
};

// A placed shop's garage door: the door of the shop it copies (`templateDoor`,
// that door's matrix), turned and moved with the marker as it stood to its own
// shop, then moved by the marker's door adjustments. Row vectors, as the game's.
Mat4 PlacedDoorMatrix(const Mat4& templateDoor, const Marker& shop, const Marker& placed);

class MarkerSet {
public:
    // Reads the region's zones and the career's shop and race lists. A region
    // without a free-roam graph (every closed track) simply has no markers.
    bool Load(const std::wstring& gameFolder, const std::wstring& region);
    void Clear();

    // Stands each marker on the collision under its centre.
    void PlaceOnGround(const World& world);

    const std::vector<FreeRoamZone>& Zones() const { return zones_; }
    const std::vector<Marker>& Markers() const { return markers_; }
    Marker* Mutable(size_t index) { return index < markers_.size() ? &markers_[index] : nullptr; }

    // A beacon of one of kPlaceableKinds: a shop copies the first shipped shop
    // of the kind; a race start has no races until some are chosen.
    size_t AddPlaced(MarkerKind kind, const Vec3& position, float heading);
    bool   RemovePlaced(size_t index);
    bool   PlacedChanged() const { return placedChanged_; }
    void   MarkPlacedChanged() { placedChanged_ = true; }
    size_t PlacedCount() const;

    // Every race of the career, by the name the career gives it (or its
    // hash, %08X, for the few with none), in the career's order.
    const std::vector<std::string>& CareerRaces() const { return careerRaces_; }

    // A shipped shop by name, and the first one of a kind.
    const Marker* FindShop(const std::string& name) const;
    const Marker* FirstShopOfKind(MarkerKind kind) const;

    // <game>\models\markers-<region>.frsmarkers. Written as text, one marker a
    // line, so it can be read and edited by hand.
    std::wstring PlacedPath() const;
    bool SavePlaced();

    // Height of the collision ground at a point in plan: the lowest surface
    // there, since shops and race starts stand on the street under any bridge.
    static bool GroundHeight(const World& world, float x, float y, float& height) {
        return nfs::GroundHeight(world, x, y, height);
    }

private:
    bool LoadCareer(const std::wstring& path);
    void LoadPlaced();

    std::wstring gameFolder_, region_;
    std::vector<FreeRoamZone> zones_;
    std::vector<Marker> markers_;
    std::vector<std::string> careerRaces_;
    // Lines of the file this editor does not place -- markers with an action
    // of their own, from builds that had them -- kept as they are and written
    // back unchanged, but not shown and not handed to the game.
    std::vector<std::string> keptLines_;
    bool placedChanged_ = false;
};

} // namespace nfs

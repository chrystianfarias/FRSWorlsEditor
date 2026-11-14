#pragma once

#include "core/Math.h"
#include "nfs/ChunkReader.h"

#include <unordered_map>
#include <vector>

namespace nfs {

// The game's own answer to "what is visible from here".
//
// L4Rx.BUN carries a precomputed visibility set in chunk 0x80034150. Two of its
// children matter: 0x00034152 gives each zone a polygon on the ground plane,
// and 0x00034153 lists, for each zone, the sections the game draws while the
// car is inside it. The lists run from eleven to fifty-five sections out of
// several hundred, which is why the running game never shows the pile of
// overlapping districts that drawing everything produces.
//
// Records in both children begin with the same eight-byte marker and are
// variable length, so they are found by scanning for it rather than by stride.
class Preculler {
public:
    // Parses the visibility container out of an already-mapped L4Rx.BUN.
    bool Parse(ByteSpan bunFile);

    bool IsEmpty() const { return zones_.empty(); }

    size_t ZoneCount() const { return zones_.size(); }

    // Section numbers drawn while standing at `point` on the ground plane.
    //
    // Zones overlap, and the car is only ever in one of them, so the most
    // specific -- the one with the smallest footprint -- is the answer.
    // Merging every zone that claims the point instead inflates the set to
    // fifty-odd sections and puts back the pile this exists to remove.
    // Returns false when no zone claims the point at all.
    bool VisibleAt(const Vec3& point, std::vector<u32>& sections) const;

    // Where no zone claims a point, the game keeps the zone the car came
    // from: the lists of every zone within `radius` of the point, any of
    // which may be the one in force there.
    void ListsNear(const Vec3& point, float radius, std::vector<std::vector<u32>>& lists) const;

    // The same choices by zone id: the zone the game is in at a point (the
    // smallest with a list; -1 for none), and every zone with a list within
    // `radius` of it.
    int ListedZoneAt(const Vec3& point) const;
    void ZoneIdsNear(const Vec3& point, float radius, std::vector<u32>& ids) const;

    // Whether any zone's list names the section: what the game ever loads.
    bool Listed(u32 sectionNumber) const;

    struct Zone {
        Vec3              boundsMin;
        Vec3              boundsMax;
        std::vector<Vec3> ring;       // ground-plane polygon, z unused
        u32               id = 0;
        // Where its outline record sits in the layout file: the body after
        // its marker -- u16 id, u16 point count, the bounding box as two
        // points (min, max), then the ring, x and y floats.
        u64               recordOffset = 0;
        u32               recordLength = 0;
    };
    const std::vector<Zone>& Zones() const { return zones_; }
    // The zone the game is in standing at a point (the smallest holding it),
    // as an index into Zones(); -1 where none is.
    int ZoneAt(float x, float y) const;
    // A zone's section list, or nullptr.
    const std::vector<u32>* ListOf(u32 zoneId) const;
    // Where the zone's list record sits in the layout file: the body after
    // its marker -- u16 id, two words, u32 count at +8, u16 section numbers
    // from +12 -- and how long it is up to the next record. The zeros after
    // the numbers are room the list can grow into without moving anything.
    bool ListRecord(u32 zoneId, u64& fileOffset, u32& length) const;
    bool Contains(const Zone& zone, float x, float y) const;

private:

    std::vector<Zone>                          zones_;
    std::unordered_map<u32, std::vector<u32>>  visible_;   // zone id -> section numbers
    std::unordered_map<u32, std::pair<u64, u32>> records_;  // zone id -> list record (offset, length)
};

} // namespace nfs

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

private:
    struct Zone {
        Vec3              boundsMin;
        Vec3              boundsMax;
        std::vector<Vec3> ring;       // ground-plane polygon, z unused
        u32               id = 0;
    };

    bool Contains(const Zone& zone, float x, float y) const;

    std::vector<Zone>                          zones_;
    std::unordered_map<u32, std::vector<u32>>  visible_;   // zone id -> section numbers
};

} // namespace nfs

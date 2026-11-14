#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <vector>

namespace nfs {

// Scenery: how the world is actually populated.
//
// A region stores each distinct prop once, in a shared library section, and then
// places it with a table of lightweight instances per world block. L4RA holds
// 10 704 solid objects but 44 158 instances, so a renderer that draws only the
// objects shows a city stripped of almost everything except its roads.

// One entry of the per-section model table. Instances index into this.
struct SceneryInfo {
    std::string name;
    u32         solidHash = 0;  // the SolidObject this places
    float       radius    = 0.0f;
    std::vector<u8> raw;        // the 68-byte entry, for placing the model elsewhere
};

// One placement.
struct SceneryInstance {
    u32  infoIndex = 0;
    u32  flags     = 0;
    u32  sceneryNumber = 0;   // the scenery chunk it came from (its header's number)
    u32  record        = 0;   // its index among that chunk's records
    Mat4 transform;     // rotation and scale from the fixed-point matrix, plus translation
    Aabb worldBounds;   // as stored in the file: tighter than transforming the model bounds
};

// The scenery of one streaming section.
struct Scenery {
    std::vector<SceneryInfo>     infos;
    std::vector<SceneryInstance> instances;

    bool Empty() const { return instances.empty(); }

    // Hash of the model an instance places, or 0 when the index is out of range.
    u32 ModelHash(const SceneryInstance& instance) const {
        return instance.infoIndex < infos.size() ? infos[instance.infoIndex].solidHash : 0;
    }
};

// Parses one 0x80034100 scenery chunk. Underground 1 stores the same chunk
// with smaller records: a 72-byte SceneryInfo without a name (the model's
// hash for each of six detail levels), and 48-byte instances whose bounds are
// whole units in i16. Its infos come out unnamed; World names them after the
// model they place.
bool ParseScenery(const Chunk& sceneryChunk, Scenery& out, bool underground1 = false);

} // namespace nfs

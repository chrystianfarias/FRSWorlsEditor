#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <string>
#include <vector>

namespace nfs {

// Adding scenery instances to a section.
//
// A section's scenery chunk (0x80034100) is the game's own list of "this model,
// here": a table of the models it places (0x00034102), one 64-byte record per
// placement (0x00034103) and a bounding-volume tree over those records
// (0x00034104) the game culls with. New placements are appended after the
// shipped ones, so every index anything else holds into the list stays valid,
// and the tree takes them in with the smallest change that keeps it a tree:
// the shipped nodes are kept, bounds grow along the path the new record goes
// down, and a new node is made only where every slot on the way is full.
// Everything else in the chunk -- the header, the 0x34105 word, the 0x34106
// masks -- is copied as it is. See docs/NFSU2-FORMATS.md, section 5.

// One placement, as the editor keeps it and as it is written.
struct PlacedInstance {
    u32         modelHash = 0;
    std::string modelName;
    Mat4        transform;       // rotation and scale rows, translation in row 3
    Aabb        modelBounds;     // the model's own box, for the record's world box
    u16         flags = 0;
    // The model's triangle count and bounding radius, for a model the pack
    // does not list yet. `infoTemplate` is the 68-byte entry another section
    // already uses for the same model; copying it is preferred when there is
    // one, because some of its fields are still unexplained.
    u32             triangles = 0;
    float           radius    = 0.0f;
    std::vector<u8> infoTemplate;

    Aabb WorldBounds() const;
};

// Facts about one scenery chunk as it sits in a file.
struct SceneryChunkInfo {
    u32    number = 0;          // the section number in its header
    size_t infoCount = 0;
    size_t instanceCount = 0;
};

// `chunk` is a whole 0x80034100 chunk, header included, that sits at
// `fileOffset`. Returns false when it is not one.
bool ReadSceneryChunk(ByteSpan chunk, u64 fileOffset, SceneryChunkInfo& out);

// Reads the records from index `first` on back into placements -- how the
// editor recovers the instances an earlier save appended.
bool ReadSceneryPlacements(ByteSpan chunk, u64 fileOffset, size_t first,
                           std::vector<PlacedInstance>& out);

// Writes `shipped` with `added` appended, as a whole chunk that will sit at
// `fileOffset` (the instance records are aligned by their position in the
// file). With nothing added the result is the input, byte for byte.
//
// `replaced` swaps shipped records for edited ones, index by index -- a
// deleted object keeps its record, with its matrix zeroed, so no index moves.
// When a replaced record's box changes, the tree's boxes are recomputed.
struct ReplacedRecord {
    size_t index = 0;
    std::vector<u8> record;     // 64 bytes
};
bool AppendScenery(ByteSpan shipped, u64 fileOffset, const std::vector<PlacedInstance>& added,
                   std::vector<u8>& out, std::wstring& error,
                   const std::vector<ReplacedRecord>& replaced = {});

// The 64-byte records of a scenery chunk, as stored.
bool ReadSceneryRecords(ByteSpan chunk, u64 fileOffset, std::vector<std::vector<u8>>& out);

// The name of the model a record places.
std::string SceneryRecordModel(ByteSpan chunk, u64 fileOffset, size_t index);

// A shipped record with its matrix zeroed: drawn as nothing, still in place.
std::vector<u8> DeletedRecord(const std::vector<u8>& record);
bool IsDeletedRecord(const std::vector<u8>& record);

// Checks what the game relies on: every record in the tree exactly once,
// every node but the root referenced exactly once, every node's box holding
// its children's, every model index in range. Run on anything before it is
// written.
bool ValidateScenery(ByteSpan chunk, u64 fileOffset, std::string& why);

// Rotation and scale for a placement, as the editor exposes them: turns in
// degrees about the world axes, applied roll (x) then pitch (y) then heading
// (z), and one uniform scale.
Mat4 ComposePlacement(const Vec3& position, float headingDegrees, float pitchDegrees,
                      float rollDegrees, float scale);
void DecomposePlacement(const Mat4& transform, Vec3& position, float& headingDegrees,
                        float& pitchDegrees, float& rollDegrees, float& scale);

} // namespace nfs

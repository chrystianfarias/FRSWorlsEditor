#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "nfs/ChunkReader.h"

#include <string>
#include <vector>

namespace nfs {

// One streaming section of a track region.
//
// The game splits a region into a few hundred sections and pages them in as the
// car drives around. The table lives in L4Rx.BUN (chunk 0x00034110) and points
// at byte ranges inside the matching STREAML4Rx.BUN.
struct Section {
    std::string name;        // 4-char tag such as "C29", stored as ASCII in the table
    u32         number = 0;  // unique section number within the region
    u32         id     = 0;  // raw value of the name field, kept for round-tripping

    u64 streamOffset = 0;    // byte offset into STREAML4Rx.BUN, always 2 KiB aligned
    u32 streamSize   = 0;    // total bytes, including any texture pack
    u32 geometrySize = 0;    // bytes excluding the texture pack; equals streamSize when there is none

    u32 order = 0;           // streaming priority / ordering key

    // Where this entry sits in L4Rx.BUN. A mesh that outgrows its section
    // moves, and moving it means correcting the offset and size right here.
    u64 tableEntryOffset = 0;

    // What the shipped table said, before any edit repointed it. Rebuilding a
    // section has to start from the bytes the game came with: starting from an
    // edit already in force stacks one rebuild on the last, and each pass drags
    // in whatever followed the section the time before.
    u32 originalStreamOffset = 0;
    u32 originalStreamSize   = 0;
    u32 originalOtherSize    = 0;
    u32 hash  = 0;

    // Three floats at +0x24. The first two are the section centre on the ground
    // plane; the third is either the centre height or a bounding radius, so it
    // is kept raw and only used for display until geometry confirms which.
    Vec3 position;

    bool HasTexturePack() const { return geometrySize != streamSize; }

    // A region ships two sets of sections. The ordering key separates them:
    // 10000-19999 is one set of 255 sections, 20000-29999 another of 112.
    // The second set is NOT simply an alternative to the first: the game's
    // visibility lists load many of them alongside -- M42 carries the scenery
    // of the Arco tower, whose windows are in M31 -- so a section of the
    // second set is only an alternative when no list ever loads it
    // (World::IsUnusedAlternate, which asks the visibility lists).
    bool IsAlternate() const { return order >= 20000; }
};

// The full section table of one track region.
class SectionTable {
public:
    // Parses chunk 0x00034110 out of an already-mapped L4Rx.BUN.
    bool Parse(ByteSpan bunFile);

    const std::vector<Section>& Sections() const { return sections_; }

    // Applies a span of replacement bytes to the table as it was read.
    //
    // A section whose mesh outgrew it moves, and the move is recorded as bytes
    // over its entry. Rather than decoding what those bytes mean, the entry's
    // own fields are re-read after they land -- which keeps this honest if the
    // patch ever covers something other than the offset and the size.
    void ApplyBytes(u64 fileOffset, const u8* data, size_t size);

    const Section* FindByName(const std::string& name) const;
    const Section* FindByNumber(u32 number) const;

    // Bounds of every positioned section centre. This is the game's own
    // description of where the drivable world lies, and it is a far better box
    // to frame a camera on than the geometry: terrain and props reach well past
    // the blocks they are stored in, by up to 7000 units in some regions.
    Aabb Bounds() const;

    // Median distance between a section centre and its nearest neighbour, which
    // is roughly how wide one section is. Used to pad the centre bounds out to
    // cover the geometry that belongs to the edge sections.
    float TypicalSectionSpacing() const;

private:
    std::vector<Section> sections_;
};

} // namespace nfs

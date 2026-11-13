#pragma once

#include "core/Types.h"
#include "io/MeshOverlay.h"

#include <string>
#include <vector>

namespace nfs { class World; struct Section; }

// Rewriting whole top-level chunks of a streaming section, and turning the
// result into span overlays.
//
// This is what the collision import and the placement writer share. Each
// replaces one or more chunks of a section with re-encoded ones and keeps the
// rest byte for byte. Two things make that safe:
//
// - Everything after a replaced chunk moves by however much it changed, and
//   the vertex and texture blocks there are aligned to 128 bytes by their
//   position in the file. A zero-id padding chunk after each replacement makes
//   every shift a whole number of 128-byte units, so they stay aligned without
//   being touched. Zero-id chunks already following the replaced one are
//   absorbed first, so saving the same section again and again does not pile
//   up padding.
// - The rebuild starts from the bytes the editor shows (World::SectionBytes),
//   so edits of other kinds already in the section -- a mesh, the collision --
//   stay in, and the span they came from is replaced rather than stacked on.

struct SectionSpan {
    std::string     name;
    std::wstring    key;          // names the file in the models folder
    u64             fileOffset = 0;
    bool            layoutFile = false;
    std::vector<u8> bytes;
};

// One chunk to replace: [start, end) in the section's bytes, which must cover
// whole top-level chunks, and what goes there instead.
struct ChunkEdit {
    size_t          start = 0;
    size_t          end   = 0;
    std::vector<u8> bytes;
};

// Applies `edits` (non-overlapping, any order) to `section`. A replacement
// chunk lands at a position congruent to `start` modulo 128, so anything
// inside it that is aligned by file position can be encoded against
// `sectionFileOffset + start`.
std::vector<u8> SpliceSection(ByteSpan section, std::vector<ChunkEdit> edits);

struct SectionSpanPlan {
    std::vector<SectionSpan>  spans;
    std::vector<std::wstring> superseded;   // span files the new ones replace
    u64  offset = 0;                        // where the section now sits
    u32  size   = 0;
    bool moved  = false;                    // past the end of the stream
    bool tableChanged = false;
};

// Where `bytes` -- the new contents of `section` -- go: in place when they fit
// before the next section, past the end of the stream otherwise, with the
// region table corrected whenever it no longer matches the shipped entry.
// `existing` is the models folder's current spans for the region.
void PlanSectionSpans(const nfs::World& world, const nfs::Section& section,
                      std::vector<u8> bytes, const std::vector<SpanOverlayInfo>& existing,
                      const std::string& label, SectionSpanPlan& out);

// Writes the spans, then removes the ones they supersede -- in that order, so
// a failure part way leaves the old edit in force rather than none. Returns
// how many were written.
size_t WriteSectionSpans(const std::vector<SectionSpan>& spans,
                         const std::vector<std::wstring>& superseded,
                         const std::wstring& modelsFolder, const std::wstring& region);

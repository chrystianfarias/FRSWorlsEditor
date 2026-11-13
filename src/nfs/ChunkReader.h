#pragma once

#include "core/Types.h"
#include "nfs/ChunkIds.h"

namespace nfs {

// One chunk inside a container.
struct Chunk {
    u32      id          = 0;
    u64      headerOffset = 0;  // file offset of the 8-byte chunk header
    u64      dataOffset   = 0;  // file offset of the payload, filler removed
    ByteSpan raw;               // payload including the 0x11 alignment filler
    ByteSpan data;              // payload with the filler removed -- what parsers read

    bool IsContainer() const { return nfs::IsContainer(id); }

    // Number of filler bytes skipped. A writer needs this to reproduce the
    // original layout byte for byte.
    u32 FillerBytes() const { return (u32)(raw.size - data.size); }
};

// Forward iterator over the chunks of a buffer.
//
// The alignment filler is computed from the absolute file offset, so every
// reader has to know where its buffer starts within the file. Nested containers
// inherit that offset through Children().
//
// Malformed sizes stop the iteration rather than throwing: the streams contain
// runs of zero padding between sections, and a strict parser would choke on
// perfectly loadable files.
class ChunkReader {
public:
    ChunkReader(ByteSpan buffer, u64 fileOffset)
        : buffer_(buffer), fileOffset_(fileOffset) {}

    // Walks the children of a container chunk.
    static ChunkReader Children(const Chunk& container) {
        return ChunkReader(container.raw, container.headerOffset + 8);
    }

    // Reads the next chunk. Returns false once the buffer is exhausted.
    bool Next(Chunk& out);

    // Skips the zero-filled gaps the streaming layout leaves between sections.
    // Enabled by default; turn it off inside containers whose children are known
    // to be contiguous.
    void SetSkipZeroPadding(bool enabled) { skipZeroPadding_ = enabled; }

    u64 Position() const { return position_; }

private:
    ByteSpan buffer_;
    u64      fileOffset_      = 0;
    u64      position_        = 0;
    bool     skipZeroPadding_ = true;
};

// Returns the first child of `container` with the given id. The returned chunk
// has id 0 when no such child exists.
Chunk FindChunk(const Chunk& container, u32 id);

} // namespace nfs

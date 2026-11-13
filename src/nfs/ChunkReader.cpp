#include "nfs/ChunkReader.h"

namespace nfs {

namespace {

// Rounds `value` up to the next multiple of `alignment` (a power of two).
u64 AlignUp(u64 value, u32 alignment) {
    return (value + alignment - 1) & ~(u64)(alignment - 1);
}

bool AllZero(const u8* p, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (p[i] != 0) return false;
    return true;
}

// Alignment padding is written as a run of 0x11 bytes.
bool IsFiller(const u8* p, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (p[i] != 0x11) return false;
    return true;
}

} // namespace

bool ChunkReader::Next(Chunk& out) {
    while (position_ + 8 <= buffer_.size) {
        const u32 id   = buffer_.read<u32>(position_);
        const u32 size = buffer_.read<u32>(position_ + 4);

        // Sections are padded out to a 2 KiB boundary with zeros. Walk past the
        // run and resume at the next real chunk header.
        if (skipZeroPadding_ && id == 0 && size == 0) {
            position_ += 8;
            while (position_ + 8 <= buffer_.size &&
                   AllZero(buffer_.data + position_, 8)) {
                position_ += 8;
            }
            continue;
        }

        if (position_ + 8 + size > buffer_.size)
            return false;  // truncated chunk: stop instead of reading past the end

        const u64 rawFileOffset = fileOffset_ + position_ + 8;
        const ByteSpan raw = buffer_.sub(position_ + 8, size);

        // The filler is only present when the writer needed it. Some chunks sit
        // on an unaligned offset and start with real data that happens to be
        // zero, so the padding is accepted only when every byte really is 0x11.
        u64 filler = AlignUp(rawFileOffset, PayloadAlignment(id)) - rawFileOffset;
        if (filler > size || !IsFiller(raw.data, (size_t)filler))
            filler = 0;

        out.id           = id;
        out.headerOffset = fileOffset_ + position_;
        out.raw          = raw;
        out.dataOffset   = rawFileOffset + filler;
        out.data         = raw.sub(filler, size - filler);

        position_ += 8 + size;
        return true;
    }
    return false;
}

Chunk FindChunk(const Chunk& container, u32 id) {
    ChunkReader reader = ChunkReader::Children(container);
    Chunk child;
    while (reader.Next(child)) {
        if (child.id == id) return child;
    }
    return {};
}

} // namespace nfs

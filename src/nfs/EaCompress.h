#pragma once

#include "core/Types.h"

#include <vector>

namespace nfs {

// The EA block compression of compressed texture packs (TRACKS\LOC*DYNTEX.BIN).
// Each block starts with a 16-byte header:
//   +0x0 "JDLZ" | "HUFF" | "RAWW"   +0x4 version   +0x5 header size (0x10)
//   +0x8 uncompressed size          +0xC compressed size
// Returns false when the block is not one of those or does not decode to the
// size its header promises.
bool EaDecompress(ByteSpan block, std::vector<u8>& out);

} // namespace nfs

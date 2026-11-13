#pragma once

#include "core/Types.h"

#include <vector>

namespace nfs {

// JDLZ, the LZ compression of the game's .lzc files and of the FE packages in
// chunk 0x30210 (a u32 package hash, then the JDLZ block).
//
// Header: "JDLZ", 0x02, 0x10, u16 0, u32 unpacked size, u32 packed size
// (header included). Then two interleaved flag streams, one bit per step: the
// first says literal byte or back-reference; the second picks the reference's
// form -- a near one (offset 1..16, length 3..4098) or a far one (offset
// 17..2064, length 3..34). tools/jdlz.py is the same decoder.

// The unpacked bytes of the block at `data`, or empty when it is not JDLZ or
// does not decode to its stated size.
std::vector<u8> JdlzDecompress(ByteSpan data);

// A JDLZ block holding `data` as literals only: a flag byte per eight bytes,
// about an eighth larger than the input, and read by the game's decoder like
// any other block.
std::vector<u8> JdlzStore(ByteSpan data);

} // namespace nfs

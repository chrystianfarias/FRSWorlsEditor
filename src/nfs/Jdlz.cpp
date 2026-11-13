#include "nfs/Jdlz.h"

#include <cstring>

namespace nfs {

namespace {

constexpr size_t kHeader = 16;

u32 Read32(const u8* at) {
    u32 value = 0;
    std::memcpy(&value, at, 4);
    return value;
}

void Write32(u8* at, u32 value) { std::memcpy(at, &value, 4); }

} // namespace

std::vector<u8> JdlzDecompress(ByteSpan data) {
    if (data.size < kHeader || std::memcmp(data.data, "JDLZ", 4) != 0) return {};
    const size_t size = Read32(data.data + 8);
    const size_t packed = Read32(data.data + 12);
    if (packed > data.size || packed < kHeader) return {};

    std::vector<u8> out(size);
    const u8* in = data.data;
    size_t pos = kHeader, o = 0;
    unsigned flags1 = 1, flags2 = 1;
    while (pos < packed && o < size) {
        if (flags1 == 1) {
            if (pos >= packed) break;
            flags1 = in[pos++] | 0x100u;
        }
        if (flags2 == 1) {
            if (pos >= packed) break;
            flags2 = in[pos++] | 0x100u;
        }
        if (flags1 & 1) {
            if (pos + 2 > packed) break;
            const unsigned b0 = in[pos], b1 = in[pos + 1];
            pos += 2;
            size_t length, offset;
            if (flags2 & 1) {
                length = (b1 | ((b0 & 0xF0u) << 4)) + 3;
                offset = (b0 & 0x0Fu) + 1;
            } else {
                offset = (b1 | ((b0 & 0xE0u) << 3)) + 17;
                length = (b0 & 0x1Fu) + 3;
            }
            if (offset > o) return {};
            for (size_t k = 0; k < length && o < size; ++k, ++o) out[o] = out[o - offset];
            flags2 >>= 1;
        } else {
            out[o++] = in[pos++];
        }
        flags1 >>= 1;
    }
    if (o != size) return {};
    return out;
}

std::vector<u8> JdlzStore(ByteSpan data) {
    // The decoder reads the first flag byte of each stream up front, then the
    // literal stream's again every eight steps; the reference stream's never
    // moves, so it is read once.
    std::vector<u8> out(kHeader);
    std::memcpy(out.data(), "JDLZ", 4);
    out[4] = 0x02;
    out[5] = 0x10;
    Write32(out.data() + 8, (u32)data.size);
    for (size_t at = 0; at < data.size; at += 8) {
        out.push_back(0);                   // eight literals
        if (at == 0) out.push_back(0);      // the reference stream's only byte
        const size_t run = data.size - at < 8 ? data.size - at : 8;
        out.insert(out.end(), data.data + at, data.data + at + run);
    }
    Write32(out.data() + 12, (u32)out.size());
    return out;
}

} // namespace nfs

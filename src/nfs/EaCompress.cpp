#include "nfs/EaCompress.h"

#include <algorithm>
#include <cstring>

namespace nfs {

namespace {

// --- JDLZ v2: LZ77 with two flag streams ------------------------------------
bool Jdlz(ByteSpan in, std::vector<u8>& out) {
    const u32 outLength = in.read<u32>(8);
    const u32 inLength = std::min<u32>(in.read<u32>(12), (u32)in.size);
    out.assign(outLength, 0);
    size_t ip = 16, op = 0;
    u32 f1 = 1, f2 = 1;
    while (ip < inLength && op < outLength) {
        if (f1 == 1) { if (ip >= inLength) break; f1 = in.data[ip++] | 0x100u; }
        if (f2 == 1) { if (ip >= inLength) break; f2 = in.data[ip++] | 0x100u; }
        if (f1 & 1) {
            if (ip + 2 > inLength) return false;
            const u32 b0 = in.data[ip], b1 = in.data[ip + 1];
            ip += 2;
            size_t length, distance;
            if (f2 & 1) {            // near: distance 1..16, length 3..4098
                length = (((b0 >> 4) << 8) | b1) + 3;
                distance = (b0 & 0x0F) + 1;
            } else {                 // far: distance 17..2064, length 3..34
                distance = (((b0 >> 5) << 8) | b1) + 17;
                length = (b0 & 0x1F) + 3;
            }
            f2 >>= 1;
            if (distance > op || op + length > outLength) return false;
            for (size_t i = 0; i < length; ++i) out[op + i] = out[op - distance + i];   // may overlap
            op += length;
        } else {
            out[op++] = in.data[ip++];
        }
        f1 >>= 1;
    }
    return op == outLength;
}

// --- HUFF v1: EA's canonical Huffman with run lengths ---------------------------
struct Bits {
    const u8* data;
    size_t size;
    size_t p = 0;   // bit position, most significant bit first

    u32 Peek(int n) const {
        if (n <= 0) return 0;
        const size_t i = p >> 3;
        u64 window = 0;
        for (size_t k = 0; k < 8; ++k) window = (window << 8) | (i + k < size ? data[i + k] : 0);
        return (u32)((window >> (64 - (p & 7) - n)) & ((1ull << n) - 1));
    }
    u32 Get(int n) { const u32 v = Peek(n); p += (size_t)n; return v; }
    bool Over() const { return p > size * 8 + 64; }

    // '1xx' is 0..3; otherwise z zeros, a one, and 2+z bits.
    bool Number(u32& value) {
        if (Peek(1)) { value = Get(3) - 4; return true; }
        int zeros = 0;
        while (Peek(1) == 0) {
            ++p;
            if (++zeros > 28 || Over()) return false;
        }
        ++p;
        const int n = 2 + zeros;
        value = Get(n) + (1u << n) - 4;
        return true;
    }
};

bool Huff(ByteSpan in, std::vector<u8>& out) {
    const u32 expected = in.read<u32>(8);
    Bits bits{ in.data + 16, in.size - 16 };
    u32 type = bits.Get(16);
    u32 length;
    if (type & 0x8000) {
        if (type & 0x100) bits.Get(32);
        length = bits.Get(32);
    } else {
        if (type & 0x100) bits.Get(24);
        length = bits.Get(24);
    }
    type &= ~0x100u;
    const u32 clue = bits.Get(8);

    // Code lengths: how many symbols use 1, 2, 3... bits.
    u32 counts[64] = {}, firsts[64] = {};
    u32 symbolCount = 0, base = 0;
    int width = 1;
    for (;;) {
        if (width >= 25) return false;
        base <<= 1;
        firsts[width] = base;
        u32 count = 0;
        if (!bits.Number(count)) return false;
        counts[width] = count;
        symbolCount += count;
        base += count;
        const u32 compare = count && width <= 16 ? ((base << (16 - width)) & 0xFFFF) : 1;
        ++width;
        if (count && !compare) break;
    }
    const int longest = width - 1;
    if (symbolCount == 0 || symbolCount > 256) return false;

    // The symbols, as skips over the ones already used.
    bool used[256] = {};
    std::vector<u8> symbols;
    u32 next = 255;
    for (u32 s = 0; s < symbolCount; ++s) {
        u32 skip = 0;
        if (!bits.Number(skip)) return false;
        u32 left = skip + 1;
        for (int guard = 0;; ++guard) {
            if (guard > 256 * 256) return false;
            next = (next + 1) & 0xFF;
            if (!used[next]) --left;
            if (left == 0) break;
        }
        used[next] = true;
        symbols.push_back((u8)next);
    }

    // A direct lookup over the longest code.
    std::vector<u8> tableSymbol((size_t)1 << longest), tableLength((size_t)1 << longest);
    size_t index = 0;
    for (int bitsUsed = 1; bitsUsed <= longest; ++bitsUsed) {
        for (u32 k = 0; k < counts[bitsUsed]; ++k) {
            const size_t low = (size_t)(firsts[bitsUsed] + k) << (longest - bitsUsed);
            const size_t high = low + ((size_t)1 << (longest - bitsUsed));
            if (high > tableSymbol.size()) return false;
            for (size_t j = low; j < high; ++j) {
                tableSymbol[j] = symbols[index];
                tableLength[j] = (u8)bitsUsed;
            }
            ++index;
        }
    }

    out.clear();
    out.reserve(length);
    for (;;) {
        if (bits.Over() || out.size() > (size_t)length + 4096) return false;
        const u32 code = bits.Peek(longest);
        const int codeLength = tableLength[code];
        if (!codeLength) return false;
        bits.p += (size_t)codeLength;
        const u8 symbol = tableSymbol[code];
        if (symbol != clue) { out.push_back(symbol); continue; }
        u32 run = 0;
        if (!bits.Number(run)) return false;
        if (run) {
            if (out.empty() || run > length) return false;
            out.insert(out.end(), run, out.back());
            continue;
        }
        if (bits.Get(1)) break;
        out.push_back((u8)bits.Get(8));
    }
    if (out.size() < length) return false;
    out.resize(length);

    if (type == 0x32FB || type == 0xB2FB) {          // one delta pass
        u8 sum = 0;
        for (u8& b : out) { sum = (u8)(sum + b); b = sum; }
    } else if (type == 0x34FB || type == 0xB4FB) {   // two
        u8 sum = 0, sum2 = 0;
        for (u8& b : out) { sum = (u8)(sum + b); sum2 = (u8)(sum2 + sum); b = sum2; }
    }
    return out.size() == expected;
}

} // namespace

bool EaDecompress(ByteSpan block, std::vector<u8>& out) {
    out.clear();
    if (block.size < 16) return false;
    if (std::memcmp(block.data, "JDLZ", 4) == 0) return Jdlz(block, out);
    if (std::memcmp(block.data, "HUFF", 4) == 0) return Huff(block, out);
    if (std::memcmp(block.data, "RAWW", 4) == 0) {
        const u32 size = block.read<u32>(8);
        if (16 + (size_t)size > block.size) return false;
        out.assign(block.data + 16, block.data + 16 + size);
        return true;
    }
    return false;
}

} // namespace nfs

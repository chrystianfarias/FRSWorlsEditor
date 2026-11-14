// Port of NFSU2Unity Core/Audio/GameAdpcm.cs.

#include "drive/audio/GameAdpcm.h"

#include <array>

namespace bb {
namespace GameAdpcm {

namespace {

// 0x819668 (SND EA-XA) and 0x7ED92C (Ginsu) hold the same layout: K0[4], K1[4], then (0x819688 /
// 0x7ED950) the nibble table T[shift 0..15][nibble 0..15] = signed nibble << (12 - shift) (0 for shift > 12).
// Filter indexes >= 4 are not range-checked by the exe and read on into the next table; Tbl reproduces that.
std::array<float, 8 + 256> BuildTable()
{
    std::array<float, 8 + 256> t{};
    const float k0[] = {0.0f, 0.9375f, 1.796875f, 1.53125f};
    const float k1[] = {0.0f, 0.0f, -0.8125f, -0.859375f};
    for (int i = 0; i < 4; i++) {
        t[i] = k0[i];
        t[4 + i] = k1[i];
    }
    for (int s = 0; s < 16; s++)
        for (int n = 0; n < 16; n++) {
            int sn = n < 8 ? n : n - 16;
            t[8 + s * 16 + n] = s <= 12 ? sn * static_cast<float>(1 << (12 - s)) : 0.0f;
        }
    return t;
}

const std::array<float, 8 + 256> Tbl = BuildTable();

inline float K0(int i) { return Tbl[i]; }
inline float K1(int i) { return Tbl[4 + i]; }
inline float T(int shift, int nib) { return Tbl[8 + shift * 16 + nib]; }

inline double Op(double v, bool pc53) { return pc53 ? v : static_cast<double>(static_cast<float>(v)); }

inline int16_t S16Be(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>(p[0] << 8 | p[1])); }

} // namespace

void EaXaV2::DecodeFrames(const uint8_t* d, float* dst, int index, int stride, int frames)
{
    for (int f = 0; f < frames; f++) {
        int fi = d[Offset];
        if (fi == 0xEE) {
            H1 = S16Be(d + Offset + 1);
            H2 = S16Be(d + Offset + 3);
            for (int i = 0; i < 28; i++, index += stride)
                dst[index] = S16Be(d + Offset + 5 + 2 * i);
            Offset += 0x3D;
            continue;
        }
        int c = fi >> 4, sh = fi & 0x0F;
        float k0 = K0(c), k1 = K1(c);
        float h1 = H1, h2 = H2;
        for (int b = 0; b < 14; b++) {
            int by = d[Offset + 1 + b];
            float s0 = h1 * k0 + T(sh, by >> 4) + h2 * k1;
            dst[index] = s0;
            index += stride;
            float s1 = s0 * k0 + T(sh, by & 0x0F) + h1 * k1;
            dst[index] = s1;
            index += stride;
            h2 = s0;
            h1 = s1;
        }
        H1 = h1;
        H2 = h2;
        Offset += 0x0F;
    }
}

void DecodeGinsuFrame(const uint8_t* d, int o, float* m, int mo, bool pc53)
{
    m[mo + 0] = static_cast<int16_t>(static_cast<uint16_t>((d[o + 0] & 0xF0) | (d[o + 1] << 8)));
    m[mo + 1] = static_cast<int16_t>(static_cast<uint16_t>((d[o + 2] & 0xF0) | (d[o + 3] << 8)));
    int c = d[o] & 0x0F, sh = d[o + 2] & 0x0F;
    double k0 = K0(c), k1 = K1(c);
    int p = mo + 2;
    for (int b = 0; b < 15; b++, p += 2) {
        int by = d[o + 4 + b];
        double th = T(sh, by >> 4), tl = T(sh, by & 0x0F);
        double h2 = m[p - 2], h1 = m[p - 1];
        double e, od;
        switch (b % 5) {
        case 1:
            e = Op(Op(Op(k0 * h1, pc53) + th, pc53) + Op(k1 * h2, pc53), pc53);
            break;
        default:
            e = Op(Op(Op(k1 * h2, pc53) + th, pc53) + Op(k0 * h1, pc53), pc53);
            break;
        }
        m[p] = static_cast<float>(e);
        double h1o = m[p - 1]; // odd sample: h2 = previous sample (memory)
        switch (b % 5) {
        case 0:
        case 1:
            od = Op(Op(Op(k1 * h1o, pc53) + tl, pc53) + Op(e * k0, pc53), pc53);
            break;
        default:
            od = Op(Op(Op(e * k0, pc53) + tl, pc53) + Op(k1 * h1o, pc53), pc53);
            break;
        }
        m[p + 1] = static_cast<float>(od);
    }
}

std::vector<float> DecodeGinsu(const uint8_t* d, int offset, int numSamples, bool pc53)
{
    int frames = (numSamples + 31) / 32;
    std::vector<float> buf(static_cast<size_t>(frames) * 32);
    for (int f = 0; f < frames; f++) DecodeGinsuFrame(d, offset + f * 0x13, buf.data(), f * 32, pc53);
    buf.resize(static_cast<size_t>(numSamples));
    return buf;
}

} // namespace GameAdpcm
} // namespace bb

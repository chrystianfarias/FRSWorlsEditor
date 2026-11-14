#pragma once

// Port of NFSU2Unity Core/Audio/GameAdpcm.cs.

#include <cstdint>
#include <vector>

namespace bb {

// The game's own ADPCM decoders, transcribed from SPEED2.EXE v1.2. Unlike vgmstream they work in float,
// keep float history (no int16 clamp/truncation) and output float samples in the int16 scale
// (+-32768, unclamped) that the SND mixer consumes.
namespace GameAdpcm {

// SND EA-XA v2 decoder, SSE path (sub_734AFF, chosen when CPU caps bit 1 is set at 0x8B4A20; the x87
// twin is sub_734BF4). Per sample, single precision: s = (h1 * K0 + T[shift][nibble]) + h2 * K1.
// 0xEE frames (sub_734A81): h1 = s16be, h2 = s16be, then 28 raw s16be samples. History starts at 0.
// Decodes whole 28-sample frames; the caller trims.
class EaXaV2 {
public:
    int Offset = 0;
    float H1 = 0, H2 = 0;

    void DecodeFrames(const uint8_t* d, float* dst, int index, int stride, int frames);
};

// Ginsu (.gin) EA-XAS v0 frame decoder, sub_466590 (x87). Frame = 0x13 bytes -> 32 floats: h2, h1 from the
// header ((b0 & 0xF0) | b1 << 8 and (b2 & 0xF0) | b3 << 8), filter = b0 & 0x0F, shift = b2 & 0x0F, then 30
// decoded samples. The loop is unrolled by 5 bytes with a different operation order per byte, transcribed
// below; history is re-read from the float output (rounded), except the even sample of each pair, which
// the odd sample reuses straight from the FPU register.
// pc53: x87 precision control 53-bit (Windows default for new threads) - each operation rounded to double;
// false = 24-bit (as after Direct3D sets single precision), i.e. float math.
void DecodeGinsuFrame(const uint8_t* d, int o, float* m, int mo, bool pc53 = true);

// Decodes a whole .gin sample stream with DecodeGinsuFrame.
std::vector<float> DecodeGinsu(const uint8_t* d, int offset, int numSamples, bool pc53 = true);

} // namespace GameAdpcm
} // namespace bb

#pragma once

// Port of NFSU2Unity Core/Audio/EaPatchHeader.cs.

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

// EA "codec2" ids (PT tag 0xA0). Only EaXa, MicroTalk5 and EaLayer3 occur in U2 PC.
enum class EaCodec : int {
    None = -1,
    Pcm16LEInt = 0x00,
    Pcm16BEInt = 0x01,
    Pcm8Int = 0x02,
    EaXaInt = 0x03,
    MicroTalk10 = 0x04,
    Vag = 0x05,
    N64 = 0x06,
    Pcm16BE = 0x07,
    Pcm16LE = 0x08,
    Pcm8 = 0x09,
    EaXa = 0x0A,
    ImaInt = 0x0D,
    Layer2 = 0x0F,
    Layer3 = 0x10,
    GcAdpcm = 0x12,
    XboxAdpcm = 0x14,
    MicroTalk5 = 0x16,
    EaLayer3 = 0x17,
    // Not a PT codec: EA-XAS v0 as used by .gin granular engine files.
    EaXasV0 = 0x100,
};

// One raw tag of a PT header, kept for documentation/debug.
struct EaPatchTag {
    uint8_t Tag = 0;
    // Absolute offset of the tag byte.
    int Offset = 0;
    // Byte count of the value (0..4), or -1 for blobs (>4 bytes / 0xFF-sized user data).
    int Size = 0;
    uint32_t Value = 0;
};

// EA "PT\0\0" variable patch header (sx.exe / SND 9). Used inside BNKl banks (one per sound)
// and as the payload of SCHl stream headers. Format: 'P','T', u16 platform (LE), then a list of
// tags: u8 tag, u8 n, n bytes big-endian value; 0xFC/0xFD are markers without value, 0xFF ends
// (0xFE ends the first info section). Defaults follow vgmstream's parse_variable_header.
class EaPatchHeader {
public:
    static constexpr int PlatformPC = 0;

    int Platform = 0;
    // Tag 0x80; -1 when absent (then platform default: PC = 0).
    int Version = -1;
    int Codec1 = -1;
    EaCodec Codec = EaCodec::None;
    int BitsPerSample = 0;
    int Channels = 0;
    int SampleRate = 0;
    int NumSamples = 0;
    // Loop start sample (tag 0x86); valid when Looped().
    int LoopStart = 0;
    // Loop end, exclusive (tag 0x87 + 1, as sx.exe/vgmstream); 0 = no loop.
    int LoopEnd = 0;
    // Channel data offsets (tags 0x88/0x89/0x94/0x95/0xA2/0xA3). In banks they are relative to the BNKl start.
    uint32_t Offsets[6] = {};
    // Relative loop byte offsets (tags 0x1A, 0x26..0x2A).
    uint32_t LoopOffsets[6] = {};
    int Priority = -1;      // 0x06
    int EffectBus = -1;     // 0x13
    uint32_t Flags = 0;     // 0x8C
    int Azimuth = -1;       // 0x9C (ch1)
    bool UsePcmBlocks = false;
    // Offset just past the end tag.
    int End = 0;
    std::vector<EaPatchTag> Tags;

    bool Looped() const { return LoopEnd != 0; }

    // Parses a PT header at offset ("PT\0\0" or "GSTR"). Throws std::runtime_error on a bad header.
    static EaPatchHeader Parse(const uint8_t* d, int dLength, int offset, int maxLength = 0x7FFFFFFF);

    std::string ToString() const;

private:
    static uint32_t ReadPatch(const uint8_t* d, int& o, int& size);
    void Apply(uint8_t tag, uint32_t v);
    void ResolveDefaults();
};

} // namespace bb

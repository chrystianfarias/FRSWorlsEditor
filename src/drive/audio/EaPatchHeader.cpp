// Port of NFSU2Unity Core/Audio/EaPatchHeader.cs.

#include "drive/audio/EaPatchHeader.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace bb {

uint32_t EaPatchHeader::ReadPatch(const uint8_t* d, int& o, int& size)
{
    int n = d[o++];
    if (n == 0xFF) {
        uint32_t len = static_cast<uint32_t>(d[o]) << 24 | static_cast<uint32_t>(d[o + 1]) << 16 |
                       static_cast<uint32_t>(d[o + 2]) << 8 | d[o + 3];
        o += 4 + static_cast<int>(len);
        size = -1;
        return 0;
    }
    if (n > 4) {
        o += n;
        size = -1;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | d[o++];
    size = n;
    return v;
}

EaPatchHeader EaPatchHeader::Parse(const uint8_t* d, int dLength, int offset, int maxLength)
{
    EaPatchHeader h;
    int o = offset;
    if (offset < 0 || offset + 8 > dLength) throw std::runtime_error("PT header out of range");
    if (d[o] == 'P' && d[o + 1] == 'T') {
        h.Platform = d[o + 2] | d[o + 3] << 8;
        o += 4;
    } else if (d[o] == 'G' && d[o + 1] == 'S' && d[o + 2] == 'T' && d[o + 3] == 'R') {
        h.Platform = 8; // generic
        o += 8;
    } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "no PT header at 0x%X", offset);
        throw std::runtime_error(buf);
    }

    long long limit = std::min(static_cast<long long>(dLength), static_cast<long long>(offset) + maxLength);
    bool end = false;
    while (!end && o < limit) {
        uint8_t tag = d[o++];
        EaPatchTag t;
        t.Tag = tag;
        t.Offset = o - 1;
        switch (tag) {
        case 0xFF:
        case 0xFE:
            end = true;
            break;
        case 0xFC:
        case 0xFD:
            break;
        default: {
            int size = 0;
            uint32_t v = ReadPatch(d, o, size);
            t.Size = size;
            t.Value = v;
            h.Apply(tag, v);
            break;
        }
        }
        h.Tags.push_back(t);
    }
    h.End = o;
    h.ResolveDefaults();
    return h;
}

void EaPatchHeader::Apply(uint8_t tag, uint32_t v)
{
    switch (tag) {
    case 0x06: Priority = static_cast<int>(v); break;
    case 0x13: EffectBus = static_cast<int>(v); break;
    case 0x80: Version = static_cast<int>(v); break;
    case 0x81: BitsPerSample = static_cast<int>(v); break;
    case 0x82: Channels = static_cast<int>(v); break;
    case 0x83: Codec1 = static_cast<int>(v); break;
    case 0x84: SampleRate = static_cast<int>(v); break;
    case 0x85: NumSamples = static_cast<int>(v); break;
    case 0x86: LoopStart = static_cast<int>(v); break;
    case 0x87: LoopEnd = static_cast<int>(v) + 1; break;
    case 0x88: Offsets[0] = v; break;
    case 0x89: Offsets[1] = v; break;
    case 0x94: Offsets[2] = v; break;
    case 0x95: Offsets[3] = v; break;
    case 0xA2: Offsets[4] = v; break;
    case 0xA3: Offsets[5] = v; break;
    case 0x1A: LoopOffsets[0] = v; break;
    case 0x26: LoopOffsets[1] = v; break;
    case 0x27: LoopOffsets[2] = v; break;
    case 0x28: LoopOffsets[3] = v; break;
    case 0x29: LoopOffsets[4] = v; break;
    case 0x2A: LoopOffsets[5] = v; break;
    case 0x8C: Flags = v; break;
    case 0x9C: Azimuth = static_cast<int>(v); break;
    case 0xA0: Codec = static_cast<EaCodec>(static_cast<int>(v)); break;
    default: break;
    }
}

void EaPatchHeader::ResolveDefaults()
{
    if (Channels <= 0) Channels = 1;
    if (Channels > 6) throw std::runtime_error("PT: too many channels");
    bool bigEndian = Platform == 2 || Platform == 3 || Platform == 4 || Platform == 6 || Platform == 8 ||
                     Platform == 9 || Platform == 0x0E || Platform == 0x10;
    if (Version < 0) {
        switch (Platform) {
        case 0: case 1: case 2: case 3: case 4: Version = 0; break;
        case 5: Version = 1; break;
        case 6: case 7: case 8: Version = 2; break;
        default: Version = 3; break;
        }
    }
    if (Codec1 < 0 && Version == 0) {
        switch (Platform) {
        case 1: Codec1 = 6; break;   // VAG
        case 2: Codec1 = 5; break;   // N64
        default: Codec1 = 0; break;  // PCM
        }
    }
    if (Codec1 >= 0 && Codec == EaCodec::None) {
        switch (Codec1) {
        case 0:
            if (Platform == PlatformPC)
                Codec = BitsPerSample == 8 ? EaCodec::Pcm8Int : bigEndian ? EaCodec::Pcm16BEInt : EaCodec::Pcm16LEInt;
            else
                Codec = BitsPerSample == 8 ? EaCodec::Pcm8 : bigEndian ? EaCodec::Pcm16BE : EaCodec::Pcm16LE;
            break;
        case 2: Codec = EaCodec::ImaInt; break;
        case 5: Codec = EaCodec::N64; break;
        case 6: Codec = EaCodec::Vag; break;
        case 7: Codec = Platform == PlatformPC || Platform == 3 ? EaCodec::EaXaInt : EaCodec::EaXa; break;
        case 9: Codec = EaCodec::MicroTalk10; break;
        default: break;
        }
    }
    if (Codec == EaCodec::None) {
        switch (Platform) {
        case 1: case 5: Codec = EaCodec::Vag; break;
        case 2: Codec = EaCodec::N64; break;
        case 6: Codec = EaCodec::Pcm16BE; break;
        case 7: Codec = EaCodec::Pcm16LE; break;
        case 0x10: case 0x14: Codec = EaCodec::GcAdpcm; break;
        default: Codec = EaCodec::EaXa; break; // PC, Mac, generic, X360, PSP, PS3
        }
    }
    if (SampleRate == 0) {
        switch (Platform) {
        case 8: SampleRate = 48000; break;
        case 6: case 7: SampleRate = 24000; break;
        case 9: case 0x0E: SampleRate = 44100; break;
        case 0x10: case 0x14: SampleRate = 32000; break;
        default: SampleRate = 22050; break;
        }
    }
    // EA-XA and MicroTalk revisions with PCM frames (sx.exe 2.30+)
    UsePcmBlocks = Version == 3 || (Version == 2 && (Platform == 0 || Platform == 3 || Platform == 8));
}

std::string EaPatchHeader::ToString() const
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "codec 0x%X %dch %dHz %d samples", static_cast<int>(Codec), Channels, SampleRate,
                  NumSamples);
    std::string s = buf;
    if (Looped()) {
        std::snprintf(buf, sizeof buf, " loop %d..%d", LoopStart, LoopEnd);
        s += buf;
    }
    return s;
}

} // namespace bb

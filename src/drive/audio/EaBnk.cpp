// Port of NFSU2Unity Core/Audio/EaBnk.cs.

#include "drive/audio/EaBnk.h"

#include "drive/audio/GameAdpcm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace bb {

namespace {
int I32(const std::vector<uint8_t>& d, int o)
{
    if (o < 0 || o + 4 > static_cast<int>(d.size())) throw std::runtime_error("BNKl: read out of range");
    int32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}
} // namespace

EaBnk::EaBnk(const std::vector<uint8_t>& data, int offset) : Data(&data), Offset(offset)
{
    if (offset < 0 || offset + 0x14 > static_cast<int>(data.size()) || data[offset] != 'B' ||
        data[offset + 1] != 'N' || data[offset + 2] != 'K' || data[offset + 3] != 'l') {
        char buf[64];
        std::snprintf(buf, sizeof buf, "no BNKl at 0x%X", offset);
        throw std::runtime_error(buf);
    }
    Version = data[offset + 4];
    int count = data[offset + 6] | data[offset + 7] << 8;
    Size = I32(data, offset + 8);
    int table;
    switch (Version) {
    case 2: table = 0x0C; break;
    case 4: case 5: table = 0x14; break;
    default: throw std::runtime_error("BNK version " + std::to_string(Version));
    }
    for (int i = 0; i < count; i++) {
        int entry = offset + table + 4 * i;
        int rel = I32(data, entry);
        EaBnkSound s;
        s.Index = i;
        if (rel != 0) {
            s.HeaderOffset = entry + rel;
            s.Header = std::make_shared<EaPatchHeader>(EaPatchHeader::Parse(
                data.data(), static_cast<int>(data.size()), s.HeaderOffset,
                static_cast<int>(data.size()) - s.HeaderOffset));
            s.DataOffsets.resize(static_cast<size_t>(s.Header->Channels));
            for (int c = 0; c < s.Header->Channels; c++)
                s.DataOffsets[c] = offset + static_cast<int>(s.Header->Offsets[c]);
        }
        Sounds.push_back(std::move(s));
    }
}

std::unique_ptr<PcmSound> EaBnk::Decode(int index) const
{
    const EaBnkSound& s = Sounds.at(static_cast<size_t>(index));
    if (s.IsEmpty()) return nullptr;
    return DecodeFlat(*Data, *s.Header, Offset, "#" + std::to_string(index));
}

std::unique_ptr<PcmSound> EaBnk::DecodeGame(int index) const
{
    const EaBnkSound& s = Sounds.at(static_cast<size_t>(index));
    if (s.IsEmpty()) return nullptr;
    const EaPatchHeader& h = *s.Header;
    if (h.Codec != EaCodec::EaXa || !h.UsePcmBlocks || h.Version == 0) return Decode(index);
    int ch = h.Channels, n = h.NumSamples, frames = (n + 27) / 28;
    // Bounds: worst case every frame is an 0xEE (0x3D bytes) frame.
    for (int c = 0; c < ch; c++)
        if (s.DataOffsets[c] < 0 || s.DataOffsets[c] >= static_cast<int>(Data->size()))
            throw std::runtime_error("BNKl: sample data out of range");
    std::vector<float> tmp(static_cast<size_t>(frames) * 28 * ch);
    // Pad the source so a truncated last frame cannot read past the buffer.
    std::vector<uint8_t> padded;
    const uint8_t* src = Data->data();
    size_t need = 0;
    for (int c = 0; c < ch; c++)
        need = std::max(need, static_cast<size_t>(s.DataOffsets[c]) + static_cast<size_t>(frames) * 0x3D);
    if (need > Data->size()) {
        padded = *Data;
        padded.resize(need, 0);
        src = padded.data();
    }
    for (int c = 0; c < ch; c++) {
        GameAdpcm::EaXaV2 x;
        x.Offset = s.DataOffsets[c];
        x.DecodeFrames(src, tmp.data(), c, ch, frames);
    }
    auto p = std::make_unique<PcmSound>();
    p->Name = "#" + std::to_string(index);
    p->Channels = ch;
    p->SampleRate = h.SampleRate;
    p->Float.assign(tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(n) * ch);
    p->Looped = h.Looped();
    p->LoopStart = h.LoopStart;
    p->LoopEnd = h.LoopEnd;
    p->Codec = h.Codec;
    return p;
}

std::unique_ptr<PcmSound> EaBnk::DecodeFlat(const std::vector<uint8_t>& d, const EaPatchHeader& h, int baseOffset,
                                            const std::string& name)
{
    int ch = h.Channels, n = h.NumSamples;
    std::vector<int> offs(static_cast<size_t>(ch));
    bool v0 = h.Version == 0;
    for (int c = 0; c < ch; c++)
        offs[c] = baseOffset + static_cast<int>(v0 ? h.Offsets[0] : h.Offsets[c]);

    auto p = std::make_unique<PcmSound>();
    p->Float.assign(static_cast<size_t>(n) * ch, 0.0f);
    auto at = [&](int o) -> int {
        if (o < 0 || o >= static_cast<int>(d.size())) throw std::runtime_error("BNKl: sample data out of range");
        return d[o];
    };
    switch (h.Codec) {
    case EaCodec::Pcm16LE:
    case EaCodec::Pcm16BE:
    case EaCodec::Pcm16LEInt:
    case EaCodec::Pcm16BEInt: {
        bool be = h.Codec == EaCodec::Pcm16BE || h.Codec == EaCodec::Pcm16BEInt;
        bool inter = h.Codec == EaCodec::Pcm16LEInt || h.Codec == EaCodec::Pcm16BEInt;
        for (int c = 0; c < ch; c++) {
            int start = v0 ? offs[0] + (inter ? 2 * c : n * 2 * c) : offs[c];
            int step = v0 && inter ? 2 * ch : 2;
            for (int i = 0; i < n; i++) {
                int q = start + i * step;
                uint16_t u = be ? static_cast<uint16_t>(at(q) << 8 | at(q + 1))
                                : static_cast<uint16_t>(at(q) | at(q + 1) << 8);
                p->Float[static_cast<size_t>(i) * ch + c] = static_cast<int16_t>(u);
            }
        }
        break;
    }
    case EaCodec::Pcm8:
    case EaCodec::Pcm8Int: {
        bool inter = h.Codec == EaCodec::Pcm8Int;
        for (int c = 0; c < ch; c++) {
            int start = v0 ? offs[0] + (inter ? c : n * c) : offs[c];
            int step = v0 && inter ? ch : 1;
            for (int i = 0; i < n; i++)
                p->Float[static_cast<size_t>(i) * ch + c] =
                    static_cast<int16_t>(static_cast<int8_t>(at(start + i * step)) * 256);
        }
        break;
    }
    default:
        throw std::runtime_error("bank codec not supported by the port: " + h.ToString());
    }
    p->Name = name;
    p->Channels = ch;
    p->SampleRate = h.SampleRate;
    p->Looped = h.Looped();
    p->LoopStart = h.LoopStart;
    p->LoopEnd = h.LoopEnd;
    p->Codec = h.Codec;
    return p;
}

} // namespace bb

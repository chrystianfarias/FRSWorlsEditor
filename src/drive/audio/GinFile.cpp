// Port of NFSU2Unity Core/Audio/GinFile.cs.

#include "drive/audio/GinFile.h"

#include <cstring>
#include <stdexcept>

namespace bb {

GinFile::GinFile(std::vector<uint8_t> d) : Data(std::move(d))
{
    const std::vector<uint8_t>& b = Data;
    if (b.size() < 0x20 || b[0] != 'G' || b[1] != 'n' || b[2] != 's' || b[3] != 'u')
        throw std::runtime_error("not a Gnsu .gin");
    auto i32 = [&](size_t o) {
        if (o + 4 > b.size()) throw std::runtime_error(".gin: header out of range");
        int32_t v;
        std::memcpy(&v, b.data() + o, 4);
        return v;
    };
    std::memcpy(&MinRpm, b.data() + 0x08, 4);
    std::memcpy(&MaxRpm, b.data() + 0x0C, 4);
    int na = i32(0x10), nb = i32(0x14);
    NumSamples = i32(0x18);
    SampleRate = i32(0x1C);
    if (na < 0 || nb < 0 || NumSamples < 0 || SampleRate <= 0) throw std::runtime_error(".gin: bad header");
    RpmPositions.resize(static_cast<size_t>(na) + 1);
    GrainBounds.resize(static_cast<size_t>(nb) + 1);
    size_t o = 0x20;
    for (int i = 0; i <= na; i++, o += 4) RpmPositions[i] = i32(o);
    for (int i = 0; i <= nb; i++, o += 4) GrainBounds[i] = i32(o);
    DataOffset = static_cast<int>(o);
    size_t frames = (static_cast<size_t>(NumSamples) + 31) / 32;
    if (o + frames * 0x13 > b.size()) {
        // Short file: pad with zeros so the decoder never reads past the buffer (the game would read garbage).
        Data.resize(o + frames * 0x13, 0);
    }
}

} // namespace bb

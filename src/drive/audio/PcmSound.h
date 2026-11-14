#pragma once

// Port of the parts of NFSU2Unity Core/Audio/PcmSound.cs the engine sound uses (decoded sound container).

#include "drive/audio/EaPatchHeader.h"

#include <string>
#include <vector>

namespace bb {

struct PcmSound {
    std::string Name;
    int Channels = 0;
    int SampleRate = 0;
    // Game-path float samples, int16 scale (unclamped), interleaved.
    std::vector<float> Float;
    bool Looped = false;
    int LoopStart = 0, LoopEnd = 0;
    EaCodec Codec = EaCodec::None;

    int Frames() const { return Channels == 0 ? 0 : static_cast<int>(Float.size()) / Channels; }
};

} // namespace bb

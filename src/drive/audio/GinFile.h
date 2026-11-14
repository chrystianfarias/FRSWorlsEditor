#pragma once

// Port of NFSU2Unity Core/Audio/GinFile.cs (header parsing; the PcmSound decode helpers are not ported).

#include <cstdint>
#include <vector>

namespace bb {

// EA granular engine sample (".gin", magic "Gnsu" + "20\0\0"), all little-endian:
// 0x00 'Gnsu', 0x04 "20\0\0", 0x08 f32 min RPM, 0x0C f32 max RPM, 0x10 u32 nA, 0x14 u32 nB,
// 0x18 u32 sample count, 0x1C u32 sample rate, 0x20 u32 A[nA+1] (sample position for nA+1 RPM points evenly
// spaced between min and max RPM), u32 B[nB+1] (grain boundaries: B[0] = 0 .. B[nB] = samples - 1),
// then mono EA-XAS v0 data (0x13 bytes per 32 samples).
class GinFile {
public:
    std::vector<uint8_t> Data;
    float MinRpm = 0, MaxRpm = 0;
    int NumSamples = 0, SampleRate = 0;
    std::vector<int> RpmPositions;
    std::vector<int> GrainBounds;
    int DataOffset = 0;

    // Throws std::runtime_error when the bytes are not a Gnsu file.
    explicit GinFile(std::vector<uint8_t> d);
};

} // namespace bb

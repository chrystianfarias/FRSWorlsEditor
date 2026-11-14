#pragma once

// Port of NFSU2Unity Core/Audio/Engine/GinPlayer.cs.

#include "drive/audio/GinFile.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bb {

// A loaded Ginsu (.gin) recording as the game's decoder object holds it (0x466770): header tables plus the
// sample stream decoded with the game's own float ADPCM (sub_466590, GameAdpcm::DecodeGinsu).
// Lookups transcribed from 0x466840 / 0x466AF0 / 0x466910 / 0x4669D0 / 0x466C60.
class GinSource {
public:
    GinFile File;
    float MinRpm = 0, MaxRpm = 0;
    int NumRpm = 0, NumGrains = 0, NumSamples = 0, Rate = 0;
    std::vector<int> RpmPos, Grain;
    // Game-path decoded samples (float, int16 scale, unclamped).
    std::vector<float> Samples;
    double MinGrainLen = 1;

    explicit GinSource(std::vector<uint8_t> data, bool pc53 = true);

    // 0x466840: rpm -> sample position inside the rpm-sweep recording.
    int RpmToPos(double rpm) const;
    // 0x466AF0: position -> fractional grain (engine cycle) index; interpolation search.
    double PosToGrain(double pos) const;
    // 0x466910: fractional grain index -> position.
    int GrainToPos(double gf) const;
    // 0x4669D0: local cycle length (samples) at a fractional grain index.
    double GrainLen(double gf) const;
    // 0x466C60: copies count int16 samples from pos; zeros when out of range.
    void Read(int pos, int count, int16_t* dst, int dstOffset) const;
};

// Ginsu grain player (object at CARSFX_PlayerEngine+0xD0 / +0xD4), transcription of Render 0x466D30 and
// SetRPM 0x467230 (same as GinPlayer in docs/re/tools/audio_engine_ref.py). Plays the recording at its
// natural rate in 11 ms blocks and jumps by whole engine cycles, with a 0.5 ms linear crossfade, so the read
// position follows the rpm target. Output is int16 at the GIN's native rate.
class GinPlayer {
public:
    static constexpr double BlockSeconds = 0.011;   // flt_788890 / flt_788894
    static constexpr double XfadeSeconds = 0.0005;  // flt_788898

    std::shared_ptr<const GinSource> Gin;
    int Nmax = 0, N = 0, X = 0;
    int Pos = 0, TgtSmooth = 0, Tgt = 0, Glide = 1, Left = 0;
    double TgtGrain = 0;

    explicit GinPlayer(std::shared_ptr<const GinSource> gin);

    // Largest block Render can produce (N + X).
    int MaxBlock() const { return N + X; }

    // 0x467230 SetRPM(rpm, smoothMs): the game passes 60.0.
    void SetRpm(double rpm, double smoothMs = 60.0);

    // rate / grainLen(tgtGrain + 0.5) * 120: the rpm the stream is actually playing.
    double HeardRpm() const;

    // Renders one block into dst (capacity MaxBlock()); returns its length.
    int Render(int16_t* dst);

private:
    std::vector<int16_t> xa_, xb_;
};

} // namespace bb

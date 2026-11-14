#pragma once

// Port of NFSU2Unity Core/Audio/Engine/GinVoice.cs.

#include "drive/audio/engine/EngineAudioController.h"
#include "drive/audio/engine/GinPlayer.h"

#include <memory>
#include <mutex>
#include <vector>

namespace bb {

// One SND stream voice fed by a GinPlayer (the GIN layer of the player engine). The game thread
// sets the target rpm and the voice parameters (volume 0..127, pitch 4.12, low-pass Hz) once per audio tick;
// the audio thread pulls 11 ms blocks from the grain player and resamples them to the output rate.
// All state shared between the two sides goes through sync_ (the game uses the SND lock
// 0x721F99/0x721FA7 the same way).
// Approximations (SND library internals not reversed): linear-interpolation resampling, a one-pole
// low-pass (bypassed at >= 20 kHz, i.e. "open"), and volume changes ramped across one output buffer.
class GinVoice : public IGinTarget {
public:
    GinPlayer Player;

    explicit GinVoice(std::shared_ptr<const GinSource> gin);

    float MinRpm() const override { return Player.Gin->MinRpm; }
    int NativeRate() const { return Player.Gin->Rate; }

    void SetTargetRpm(double rpm) override;
    double HeardRpm() override;

    // SND_volume (0..127), SND_pitch (4.12), SND_lowpass (Hz).
    void SetParams(int vol127, int pitch, int lowpassHz);

    // Audio thread: adds (or writes) frames mono samples, scaled to [-1, 1], into dst with the given channel
    // stride, at outRate Hz.
    void Render(float* dst, int offset, int frames, int stride, int outRate, bool add, float masterGain = 1.0f);

private:
    float NextNative();

    std::mutex sync_;
    std::vector<int16_t> block_;
    int blockLen_ = 0, blockPos_ = 0;
    // pending (game thread) -> applied (audio thread)
    double pendingRpm_;
    bool hasPending_ = false;
    int volume127_ = 0, pitch4096_ = 4096, lpfHz_ = 24000;
    double heardRpm_ = 0;
    // audio-thread state
    double frac_ = 0;
    float s0_ = 0, s1_ = 0;
    bool primed_ = false;
    float curGain_ = 0;
    double lp_ = 0;
};

} // namespace bb

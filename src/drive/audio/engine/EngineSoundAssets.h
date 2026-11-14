#pragma once

// Port of NFSU2Unity Core/Audio/Engine/EngineSoundAssets.cs.

#include "drive/audio/AbkBank.h"
#include "drive/audio/engine/AemsCarBank.h"
#include "drive/audio/engine/EngineAudioController.h"
#include "drive/audio/engine/GinPlayer.h"
#include "drive/audio/engine/GinVoice.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bb {

// One AEMS voice sample of the CAR bank, decoded with the game's float EA-XA path.
struct EngineVoiceSample {
    int Slot = 0, BnkIndex = 0;
    // Mono samples in int16 scale (game float decode, unclamped). Empty for an empty slot.
    std::vector<float> Data;
    int Rate = 0;
    bool Looped = false;
    int LoopStart = 0, LoopEnd = 0;
};

// Reads a game-relative file ("SOUND\\ENGINE\\x.abk"); throws std::runtime_error when it cannot.
using GameFileReader = std::function<std::vector<uint8_t>(const std::string&)>;

// Everything the player-engine sound loads for one car (CARSFX_PlayerEngine::SetupLoadData 0x465C80):
// the EE engine bank, the accel GIN and, for dual engines, the decel GIN. Files are read through read
// (relative game paths), never copied anywhere.
class EngineSoundAssets {
public:
    EngineSelection Selection;
    std::unique_ptr<AbkBank> Bank;
    std::shared_ptr<const AemsCarBank> Tables;
    EngineVoiceSample Voices[AemsCarBank::Voices];
    std::shared_ptr<const GinSource> Gin, Dcl;

    EngineSoundAssets(const EngineSelection& sel, const GameFileReader& read);
};

// Plays one bank sample with its EA loop points (linear-interpolation resampling).
class SampleVoicePlayer {
public:
    explicit SampleVoicePlayer(const EngineVoiceSample* sample) : s_(sample) {}

    void Start()
    {
        pos_ = 0;
        stopped_ = s_->Data.empty();
    }
    void Stop() { stopped_ = true; }

    // Adds frames scaled by a gain ramp g0 -> g1 (linear) at pitch (4.12) into dst.
    void Mix(float* dst, int offset, int frames, int outRate, int pitch4096, float g0, float g1);

private:
    float At(int i) const { return i < static_cast<int>(s_->Data.size()) ? s_->Data[static_cast<size_t>(i)] : 0.0f; }

    const EngineVoiceSample* s_;
    double pos_ = 0;
    bool stopped_ = true;
};

// Offline (deterministic) render of the player-engine sound: controller at 60 Hz, AEMS voices on the 20 ms
// AEMS timer, GIN streams through GinVoice. Mono float output in [-1, 1] scale (unclamped).
// Port addition: Tick is split into Control (controller tick + AEMS timer, game thread) and Mix (audio), so
// the real-time EngineSound can run the same code with the mix on its audio thread.
class EngineOfflineRenderer {
public:
    std::unique_ptr<EngineAudioController> Ctl;
    std::unique_ptr<AemsCarVoices> Aems;
    std::unique_ptr<GinVoice> GinV, DclV;
    const int OutRate;
    // Layer gains (debug): AEMS bank voices and GIN streams.
    float AemsGain = 1.0f, GinGain = 1.0f;

    // The assets must outlive the renderer (the voice players point at their samples).
    EngineOfflineRenderer(const EngineSoundAssets& a, float idleRpm, float maxRpm, const int* cosTable,
                          const EngineAudioOptions& opt = EngineAudioOptions(), int outRate = 44100);

    // Runs one 60 Hz tick and appends its output samples to sink (returns count).
    int Tick(const EngineAudioInput& input, std::vector<float>& sink);

    // Port split of Tick: the controller tick, GIN voice parameters and the AEMS timer.
    const EngineAudioFrame& Control(const EngineAudioInput& input);
    // Port split of Tick: renders frames of the current state into dst (overwritten, mono).
    void Mix(float* dst, int frames);

private:
    std::vector<std::unique_ptr<SampleVoicePlayer>> players_;
    int lastStart_[AemsCarBank::Voices] = {};
    float lastGain_[AemsCarBank::Voices] = {};
    double frameAcc_ = 0, lp_ = 0;
    std::vector<float> tmp_;
};

} // namespace bb

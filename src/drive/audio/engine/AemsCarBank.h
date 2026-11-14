#pragma once

// Port of NFSU2Unity Core/Audio/Engine/AemsCarBank.cs.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bb {

// Steady-state behaviour of the AEMS class "CAR" of one engine bank (CAR_xx_ENG_MB_EE.abk): per voice, the gain
// for load 0 and load 1024 and the pitch, as functions of rpm (d01). The tables were obtained by running the
// bank's own x86 module with SPEED2.EXE's node library (docs/re/tools/audio/bake_engine_tables.py, Unicorn)
// on a 50-rpm grid; between grid points the gains are interpolated linearly (as audio_engine_ref.py does on
// its 250-rpm grid). Load law, d05 attenuation and d25 (limiter layer) are applied analytically
// (docs/re/audio-engine.md 2.6): vol = (g0 cos(pi/2 L) + g1 (1 - cos(pi/2 L))) (32767 + d05) / 32767.
class AemsCarBank {
public:
    static constexpr int Voices = 8;
    static constexpr int GridStep = 50, GridPoints = 10000 / GridStep + 1;
    // Sample-table slot played by each voice (slot 7 is never played).
    static constexpr int SampleSlot[Voices] = {0, 1, 2, 3, 4, 5, 6, 8};
    // Pitch table marker for "no pitch parameter" (sample plays at its native rate).
    static constexpr uint16_t NoPitch = 0xFFFF;

    const int BankIndex;

    // Number of banks with baked tables (CAR_00..CAR_40).
    static int Count();

    // Tables of CAR_<bank>_ENG_MB_EE/SPU (EE and SPU give identical outputs). Cached, thread-safe.
    // Throws std::out_of_range for a bank without tables.
    static std::shared_ptr<const AemsCarBank> Get(int bank);

    // "CAR_23_ENG_MB_EE.abk" -> 23 (-1 when the name has no CAR_nn).
    static int IndexFromName(const std::string& bankName);

    // Raw baked gain of a voice at a grid point (load 0 or 1024).
    int GridGain(int point, int voice, bool loaded) const
    {
        return (loaded ? gain1_ : gain0_)[static_cast<size_t>(point) * Voices + voice];
    }
    int GridPitch(int point, int voice) const { return pitch_[static_cast<size_t>(point) * Voices + voice]; }

    // True when the voice has a pitch parameter that follows rpm.
    bool IsRpmPitched(int voice) const;

    // Steady-state voice gains (0..32767) and pitches (4.12) for the CAR data d (d01 rpm, d03 load,
    // d05 attenuation, d16 pitch offset, d25 limiter layer). step = grid spacing used for interpolation
    // (50, or 250 to match audio_engine_ref.py).
    void Evaluate(const int* d, int* vol, int* pitchOut, int step = GridStep) const;

    AemsCarBank(int bank, std::vector<uint16_t> g0, std::vector<uint16_t> g1, std::vector<uint16_t> p)
        : BankIndex(bank), gain0_(std::move(g0)), gain1_(std::move(g1)), pitch_(std::move(p)) {}

private:
    std::vector<uint16_t> gain0_, gain1_, pitch_;      // [point * 8 + voice]
};

// Voice gating of the CAR patch on the AEMS timer (20 ms tick, inferred SND default): a voice keys on when its
// gain exceeds ~1 % of full scale and then ramps 0 -> target over 5 ticks (100 ms); while on, gain changes are
// instant; when the gain falls below the threshold the volume drops at once and the voice stops 4 ticks later.
// Pitch follows d01 in the same tick. (audio-engine.md 2.6; the threshold rule is measured, not derived.)
class AemsCarVoices {
public:
    static constexpr double TickSeconds = 0.020;
    static constexpr int GateThreshold = 330, AttackTicks = 5, ReleaseTicks = 4;

    std::shared_ptr<const AemsCarBank> Bank;
    // Steady-state gain from the tables (before the gate).
    int Target[AemsCarBank::Voices] = {};
    // Gated volume 0..32767 sent to the voice.
    int Volume[AemsCarBank::Voices] = {};
    int Pitch[AemsCarBank::Voices] = {};
    bool Playing[AemsCarBank::Voices] = {};
    // Incremented each time a voice is (re)started from the beginning of its sample.
    int StartCount[AemsCarBank::Voices] = {};
    long long AemsTicks = 0;

    explicit AemsCarVoices(std::shared_ptr<const AemsCarBank> bank) : Bank(std::move(bank)) {}

    // Advances the AEMS timer by dt seconds with the latest committed CAR data.
    int Advance(double dt, const int* d);
    void Tick(const int* d);

private:
    bool on_[AemsCarBank::Voices] = {};
    int attack_[AemsCarBank::Voices] = {}, release_[AemsCarBank::Voices] = {};
    double acc_ = 0;
};

} // namespace bb

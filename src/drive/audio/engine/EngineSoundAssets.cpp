// Port of NFSU2Unity Core/Audio/Engine/EngineSoundAssets.cs.

#include "drive/audio/engine/EngineSoundAssets.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bb {

EngineSoundAssets::EngineSoundAssets(const EngineSelection& sel, const GameFileReader& read) : Selection(sel)
{
    Bank = std::make_unique<AbkBank>(read(sel.BankPath()));
    Tables = AemsCarBank::Get(AemsCarBank::IndexFromName(sel.Engine->BankEE));
    if (Bank->Modules.empty() || Bank->Modules[0].PlayerSampleTables.empty() || Bank->Bnk == nullptr)
        throw std::runtime_error("engine bank without a sample table: " + sel.BankPath());
    const std::vector<AbkSampleRef>& table = Bank->SampleTableEntries.at(Bank->Modules[0].PlayerSampleTables[0]);
    for (int v = 0; v < AemsCarBank::Voices; v++) {
        int slot = AemsCarBank::SampleSlot[v];
        EngineVoiceSample& vs = Voices[v];
        vs.Slot = slot;
        if (slot < static_cast<int>(table.size()) && table[slot].Type == 0 && table[slot].Index != 0 &&
            table[slot].Index < Bank->Bnk->Sounds.size()) {
            vs.BnkIndex = static_cast<int>(table[slot].Index);
            std::unique_ptr<PcmSound> s = Bank->Bnk->DecodeGame(vs.BnkIndex);
            if (s != nullptr) {
                vs.Data = std::move(s->Float);
                vs.Rate = s->SampleRate;
                vs.Looped = s->Looped;
                vs.LoopStart = s->LoopStart;
                vs.LoopEnd = s->LoopEnd > 0 ? s->LoopEnd : static_cast<int>(vs.Data.size()) / std::max(1, s->Channels);
            }
        }
    }
    if (!sel.Engine->Gin.empty()) Gin = std::make_shared<const GinSource>(read(sel.GinPath()));
    if (sel.Dual != nullptr && !sel.Dual->Gin.empty()) Dcl = std::make_shared<const GinSource>(read(sel.DclPath()));
}

void SampleVoicePlayer::Mix(float* dst, int offset, int frames, int outRate, int pitch4096, float g0, float g1)
{
    if (stopped_ || s_->Data.empty()) return;
    double step = s_->Rate * (pitch4096 / 4096.0) / outRate;
    int end = s_->Looped ? s_->LoopEnd : static_cast<int>(s_->Data.size());
    float dg = (g1 - g0) / std::max(1, frames);
    for (int i = 0; i < frames; i++) {
        int i0 = static_cast<int>(pos_);
        double f = pos_ - i0;
        int i1 = i0 + 1;
        if (s_->Looped && i1 >= end) i1 = s_->LoopStart + (i1 - end);
        float x = static_cast<float>(At(i0) + (At(i1) - At(i0)) * f);
        dst[offset + i] += x * (g0 + dg * i);
        pos_ += step;
        if (pos_ >= end) {
            if (s_->Looped && end > s_->LoopStart) {
                while (pos_ >= end) pos_ -= end - s_->LoopStart;
            } else {
                stopped_ = true;
                break;
            }
        }
    }
}

EngineOfflineRenderer::EngineOfflineRenderer(const EngineSoundAssets& a, float idleRpm, float maxRpm,
                                             const int* cosTable, const EngineAudioOptions& opt, int outRate)
    : OutRate(outRate)
{
    Ctl = std::make_unique<EngineAudioController>(a.Selection, idleRpm, maxRpm, cosTable, opt);
    Aems = std::make_unique<AemsCarVoices>(a.Tables);
    if (a.Gin != nullptr) {
        GinV = std::make_unique<GinVoice>(a.Gin);
        Ctl->Gin = GinV.get();
    }
    if (a.Dcl != nullptr) {
        DclV = std::make_unique<GinVoice>(a.Dcl);
        Ctl->Dcl = DclV.get();
    }
    for (int v = 0; v < AemsCarBank::Voices; v++) players_.push_back(std::make_unique<SampleVoicePlayer>(&a.Voices[v]));
}

const EngineAudioFrame& EngineOfflineRenderer::Control(const EngineAudioInput& input)
{
    const EngineAudioFrame& f = Ctl->Tick(input);
    if (GinV) GinV->SetParams(f.GinVol127, f.GinPitch, f.GinLpf);
    if (DclV) DclV->SetParams(f.DclVol127, f.GinPitch, f.GinLpf);
    // AEMS timer (20 ms) with the CAR data just committed; 0 or 1 AEMS ticks per 60 Hz tick.
    Aems->Advance(EngineAudioController::TickSeconds, f.D);
    return f;
}

void EngineOfflineRenderer::Mix(float* dst, int frames)
{
    std::fill(dst, dst + frames, 0.0f);
    const EngineAudioFrame& f = Ctl->Frame;
    for (int v = 0; v < static_cast<int>(players_.size()); v++) {
        if (Aems->StartCount[v] != lastStart_[v]) {
            lastStart_[v] = Aems->StartCount[v];
            players_[v]->Start();
            lastGain_[v] = 0;
        }
        if (!Aems->Playing[v]) {
            players_[v]->Stop();
            lastGain_[v] = 0;
            continue;
        }
        float g1 = Aems->Volume[v] / 32767.0f * AemsGain / 32768.0f;
        players_[v]->Mix(dst, 0, frames, OutRate, Aems->Pitch[v], lastGain_[v], g1);
        lastGain_[v] = g1;
    }
    // AEMS low-pass (d21 * 0.97592, type 6 param), one pole on the layer sum; open at >= 20 kHz
    double fc = f.D[21] * 0.97592;
    double a = fc >= 20000 || fc <= 0 ? 1.0 : 1.0 - std::exp(-2.0 * 3.14159265358979323846 * fc / OutRate);
    for (int i = 0; i < frames; i++) {
        lp_ += a * (dst[i] - lp_);
        dst[i] = static_cast<float>(lp_);
    }
    if (GinV) GinV->Render(dst, 0, frames, 1, OutRate, true, GinGain);
    if (DclV) DclV->Render(dst, 0, frames, 1, OutRate, true, GinGain);
}

int EngineOfflineRenderer::Tick(const EngineAudioInput& input, std::vector<float>& sink)
{
    Control(input);
    frameAcc_ += OutRate * EngineAudioController::TickSeconds;
    int frames = static_cast<int>(frameAcc_);
    frameAcc_ -= frames;
    if (static_cast<int>(tmp_.size()) < frames) tmp_.resize(static_cast<size_t>(frames));
    Mix(tmp_.data(), frames);
    sink.insert(sink.end(), tmp_.begin(), tmp_.begin() + frames);
    return frames;
}

} // namespace bb

// Port of NFSU2Unity Core/Audio/Engine/GinVoice.cs.

#include "drive/audio/engine/GinVoice.h"

#include <algorithm>
#include <cmath>

namespace bb {

GinVoice::GinVoice(std::shared_ptr<const GinSource> gin) : Player(std::move(gin)), pendingRpm_(0)
{
    // The C# block is MaxBlock long (Render can never exceed it there without throwing); extra room here so a
    // malformed .gin cannot write past the buffer.
    block_.assign(static_cast<size_t>(Player.MaxBlock()) * 4 + 64, 0);
}

void GinVoice::SetTargetRpm(double rpm)
{
    std::lock_guard<std::mutex> lock(sync_);
    pendingRpm_ = rpm;
    hasPending_ = true;
}

double GinVoice::HeardRpm()
{
    std::lock_guard<std::mutex> lock(sync_);
    return heardRpm_;
}

void GinVoice::SetParams(int vol127, int pitch, int lowpassHz)
{
    std::lock_guard<std::mutex> lock(sync_);
    volume127_ = vol127 < 0 ? 0 : vol127 > 127 ? 127 : vol127;
    pitch4096_ = pitch;
    lpfHz_ = lowpassHz;
}

float GinVoice::NextNative()
{
    if (blockPos_ >= blockLen_) {
        // The stream callback asks for the next block; the target set since the last block applies now.
        if (hasPending_) {
            Player.SetRpm(pendingRpm_, 60.0);
            hasPending_ = false;
        }
        blockLen_ = Player.Render(block_.data());
        blockPos_ = 0;
        heardRpm_ = Player.HeardRpm();
        if (blockLen_ == 0) return 0.0f;
    }
    return block_[static_cast<size_t>(blockPos_++)];
}

void GinVoice::Render(float* dst, int offset, int frames, int stride, int outRate, bool add, float masterGain)
{
    std::lock_guard<std::mutex> lock(sync_);
    double step = Player.Gin->Rate * (pitch4096_ / 4096.0) / outRate;
    if (step <= 0) step = 1e-6;
    float target = volume127_ / 127.0f * masterGain / 32768.0f;
    float g0 = primed_ ? curGain_ : target, dg = (target - g0) / std::max(1, frames);
    double a = lpfHz_ >= 20000 || lpfHz_ <= 0 ? 1.0 : 1.0 - std::exp(-2.0 * 3.14159265358979323846 * lpfHz_ / outRate);
    if (!primed_) {
        s0_ = NextNative();
        s1_ = NextNative();
        frac_ = 0;
        primed_ = true;
    }
    int idx = offset;
    for (int i = 0; i < frames; i++, idx += stride) {
        while (frac_ >= 1.0) {
            s0_ = s1_;
            s1_ = NextNative();
            frac_ -= 1.0;
        }
        double x = s0_ + (s1_ - s0_) * frac_;
        frac_ += step;
        lp_ += a * (x - lp_);
        float v = static_cast<float>(lp_ * (g0 + dg * i));
        if (add) dst[idx] += v;
        else dst[idx] = v;
    }
    curGain_ = target;
}

} // namespace bb

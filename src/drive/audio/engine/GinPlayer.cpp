// Port of NFSU2Unity Core/Audio/Engine/GinPlayer.cs.

#include "drive/audio/engine/GinPlayer.h"

#include "drive/audio/GameAdpcm.h"
#include "drive/audio/engine/EngineAudioMath.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace bb {

using namespace EngineAudioMath;

GinSource::GinSource(std::vector<uint8_t> data, bool pc53) : File(std::move(data))
{
    const GinFile& f = File;
    MinRpm = f.MinRpm;
    MaxRpm = f.MaxRpm;
    RpmPos = f.RpmPositions;
    Grain = f.GrainBounds;
    NumRpm = static_cast<int>(RpmPos.size()) - 1;
    NumGrains = static_cast<int>(Grain.size()) - 1;
    NumSamples = f.NumSamples;
    Rate = f.SampleRate;
    Samples = GameAdpcm::DecodeGinsu(f.Data.data(), f.DataOffset, f.NumSamples, pc53);
    int m = INT_MAX;
    for (int i = 0; i < NumGrains; i++) m = std::min(m, Grain[i + 1] - Grain[i]);
    MinGrainLen = NumGrains > 0 ? m : 1;
}

int GinSource::RpmToPos(double rpm) const
{
    int n = NumRpm;
    if (n < 1) return 0;
    if (rpm <= MinRpm) return RpmPos[0];
    if (rpm >= MaxRpm) return RpmPos[n];
    double x = (rpm - MinRpm) * n / (static_cast<double>(MaxRpm) - MinRpm);
    int i = FloorX87(x);
    int a = RpmPos[i], b = RpmPos[i + 1];
    return Rint((b - a) * (x - i) + a);
}

double GinSource::PosToGrain(double pos) const
{
    const std::vector<int>& g = Grain;
    int n = NumGrains;
    if (n < 1 || pos <= g[0]) return 0.0;
    if (pos >= g[n]) return n;
    int lo = 0, hi = n;
    for (int guard = 0; guard < 4096; guard++) {
        int i = lo + FloorX87((pos - g[lo]) / static_cast<double>(g[hi] - g[lo]) * (hi - lo));
        if (pos < g[i]) {
            int k = i - CeilX87((g[i] - pos) / MinGrainLen);
            if (k > lo) lo = k;
            hi = i;
        } else if (pos >= g[i + 1]) {
            int k = i + 1 + CeilX87((pos - g[i]) / MinGrainLen);
            lo = i + 1;
            if (k < hi) hi = k;
        } else {
            return i + (pos - g[i]) / static_cast<double>(g[i + 1] - g[i]);
        }
    }
    return lo;
}

int GinSource::GrainToPos(double gf) const
{
    const std::vector<int>& g = Grain;
    int n = NumGrains;
    if (n < 1) return 0;
    if (gf <= 0.0) return g[0];
    if (n <= gf) return g[n];
    int i = FloorX87(gf);
    return Rint((g[i + 1] - g[i]) * (gf - i) + g[i]);
}

double GinSource::GrainLen(double gf) const
{
    const std::vector<int>& g = Grain;
    int n = NumGrains;
    if (n < 1) return 0.0;
    int i = FloorX87(gf);
    double l0, l1;
    if (i < 1) {
        if (i < 0) {
            gf = 0.0;
            i = 0;
        }
        l0 = g[1] - g[0];
        l1 = (g[2] - g[0]) * 0.5;
    } else if (i >= n - 1) {
        if (i >= n) {
            gf = n;
            i = n - 1;
        }
        l0 = (g[n] - g[n - 2]) * 0.5;
        l1 = g[n] - g[n - 1];
    } else {
        l0 = (g[i + 1] - g[i - 1]) * 0.5;
        l1 = (g[i + 2] - g[i]) * 0.5;
    }
    return l0 + (l1 - l0) * (gf - i);
}

void GinSource::Read(int pos, int count, int16_t* dst, int dstOffset) const
{
    if (count <= 0) return;
    if (pos < 0 || pos + count - 1 >= NumSamples) {
        std::memset(dst + dstOffset, 0, sizeof(int16_t) * static_cast<size_t>(count));
        return;
    }
    for (int k = 0; k < count; k++) {
        float f = Samples[static_cast<size_t>(pos) + k];
        double r = std::nearbyint(static_cast<double>(f));
        int v = r > 32767 || r < -32768 ? (f > 0 ? 32767 : -32768) : static_cast<int>(r);
        dst[dstOffset + k] = static_cast<int16_t>(v);
    }
}

GinPlayer::GinPlayer(std::shared_ptr<const GinSource> gin) : Gin(std::move(gin))
{
    Nmax = Rint(Gin->Rate * BlockSeconds);
    N = Rint(Gin->Rate * BlockSeconds);
    X = Rint(Gin->Rate * XfadeSeconds);
    xa_.assign(static_cast<size_t>(std::max(X, 1)), 0);
    xb_.assign(static_cast<size_t>(std::max(X, 1)), 0);
}

void GinPlayer::SetRpm(double rpm, double smoothMs)
{
    Tgt = Gin->RpmToPos(rpm);
    Glide = FloorX87(smoothMs / (BlockSeconds * 1000.0)) + 1;
}

double GinPlayer::HeardRpm() const
{
    double l = Gin->GrainLen(TgtGrain + 0.5);
    return l == 0 ? 0.0 : Gin->Rate / l * 120.0;
}

int GinPlayer::Render(int16_t* dst)
{
    const GinSource& g = *Gin;
    int n = N, x = X, o = 0;
    TgtGrain = g.PosToGrain(TgtSmooth);
    double step = (Tgt - TgtSmooth) / static_cast<double>(Glide);
    int jump = 0;
    double frac = 1.0;
    if (Left < n) {
        double cur = g.PosToGrain(Pos);
        double a = n / g.GrainLen(cur);
        double b = step / g.GrainLen(TgtGrain);
        double d = TgtGrain - cur;
        double e = b - a;
        double f = Left / static_cast<double>(n);
        double gg = f * e + d;
        double h = e + d;
        if (FloorX87(gg) != FloorX87(h)) {
            jump = gg < h ? CeilX87(gg) : FloorX87(gg);
            frac = (jump - d) / e;
        } else {
            int k = CeilX87(std::fabs(BlockSeconds / BlockSeconds * e));
            if (k < std::fabs(gg)) {
                frac = f;
                jump = gg >= 0 ? FloorX87(gg) : CeilX87(gg);
            }
        }
        if (jump == 0) frac = 1.0;
    }
    int n1 = Rint(n * frac);
    g.Read(Pos, n1, dst, o);
    o += std::max(0, n1);
    Pos += n1;
    Left = std::max(0, Left - n1);
    if (jump != 0) {
        g.Read(Pos, x, xa_.data(), 0);
        Pos = g.GrainToPos(g.PosToGrain(Pos) + jump);
        g.Read(Pos, x, xb_.data(), 0);
        double t = 0.0, dt = 1.0 / x;
        for (int i = 0; i < x; i++) {
            dst[o++] = static_cast<int16_t>(Rint((xb_[i] - xa_[i]) * t + xa_[i]));
            t += dt;
        }
        Pos += x;
        Left = Nmax - x;
        int rest = n - (n1 + x);
        if (rest > 0) {
            g.Read(Pos, rest, dst, o);
            o += rest;
            Pos += rest;
            Left -= rest;
        }
    }
    TgtSmooth = Rint(TgtSmooth + step);
    if (Glide > 1) Glide--;
    return o;
}

} // namespace bb

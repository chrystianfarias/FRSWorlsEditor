#pragma once

// Port of NFSU2Unity Core/Audio/Engine/EngineAudioMath.cs.

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace bb {

// Small per-call helpers of the car-audio controllers. Everything is evaluated per call (per audio tick),
// never scaled by dt, exactly like the game. Intermediates are double (x87 with 53-bit precision control),
// stored fields are float like the game's members.
// The C# overloads Clamp(double..)/Clamp(int..) are ClampD/ClampI here (C++ promotion rules differ).
namespace EngineAudioMath {

inline double ClampD(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
inline int ClampI(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

// sub_468E30: clamp(target, cur - step, cur + step).
inline double Slew(double cur, double target, double step) { return ClampD(target, cur - step, cur + step); }

inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// _ftol: truncation toward zero (C# (int) cast: out of range / NaN -> int.MinValue on x86/x64).
inline int Ftol(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
    return static_cast<int>(x);
}

// x87 fistp in the default mode: round to nearest, ties to even.
inline int Rint(double x) { return Ftol(std::nearbyint(x)); }

// Idiom fist / fisub / sign test = floor.
inline int FloorX87(double x)
{
    int i = Rint(x);
    return x - i < 0 ? i - 1 : i;
}

inline int CeilX87(double x)
{
    int i = Rint(x);
    return i - x < 0 ? i + 1 : i;
}

// Mixer curve lookup on table T with the game's "bits 1..4" fraction (0x468160).
int CurveT(const int* t, int x);

// Mixer curves 0..9 (0 cos, 1 sin, 2 cos^2, 3 sin^2, 4 1-sin, 5 1-cos, 6 (1-sin)^2, 7 (1-cos)^2, 8 linear, 9 1-x).
int MixerCurve(const int* t, int type, int x);

// Fallback T table when no exe is at hand (the exe's table is round(32767 cos(pi/2 i/512))).
std::vector<int> SyntheticCosTable();

// bSin: u16 angle -> [-1, 1].
inline double BSin(int phase16)
{
    return std::sin((phase16 & 0xFFFF) * (2.0 * 3.14159265358979323846 / 65536.0));
}

} // namespace EngineAudioMath

// Moving average of the last N calls (0x579F20 / 0x576970), with Flush.
class AudioAverage {
public:
    float Mean = 0;

    explicit AudioAverage(int n) : v_(static_cast<size_t>(n), 0.0f) {}

    void Record(double x)
    {
        v_[i_] = static_cast<float>(x);
        i_ = (i_ + 1) % static_cast<int>(v_.size());
        double s = 0;
        for (float e : v_) s += e;
        Mean = static_cast<float>(s / static_cast<int>(v_.size()));
    }

    void Flush(double x)
    {
        for (float& e : v_) e = static_cast<float>(x);
        Mean = static_cast<float>(x);
    }

private:
    std::vector<float> v_;
    int i_ = 0;
};

// Ramp (Set 0x459990 / Update 0x46B100): dur = ms*0.001 (0 -> 0.01 s); curve 1 t^2, 2 1-(1-t)^2, else linear.
class AudioRamp {
public:
    float T = 0, Dur = 0.01f, A = 0, B = 0, Cur = 0;
    int Type = 0;
    bool Done = true;

    explicit AudioRamp(float v = 0.0f) { A = B = Cur = v; }

    void Set(double a, double b, int ms, int type = 0)
    {
        T = 0.0f;
        Dur = ms > 0 ? static_cast<float>(ms * 0.001) : 0.01f;
        A = static_cast<float>(a);
        B = static_cast<float>(b);
        Type = type;
        Cur = static_cast<float>(a);
        Done = false;
    }

    // Snaps the ramp to a constant value (finished).
    void Reset(double v)
    {
        T = 0;
        Dur = 0.01f;
        A = B = Cur = static_cast<float>(v);
        Done = true;
    }

    float Update(double dt)
    {
        T = static_cast<float>(T + dt);
        if (T > Dur) {
            Cur = B;
            Done = true;
            return Cur;
        }
        double u = T / static_cast<double>(Dur);
        if (Type == 1) u = u * u;
        else if (Type == 2) u = 1 - (u - 1) * (u - 1);
        Cur = static_cast<float>(A + (B - A) * u);
        return Cur;
    }
};

// Graph::GetValue 0x576790, piecewise linear (inferred beyond the first branch).
class AudioGraph {
public:
    AudioGraph() = default;
    AudioGraph(std::initializer_list<std::pair<double, double>> pts)
    {
        for (const auto& p : pts) {
            xs_.push_back(p.first);
            ys_.push_back(p.second);
        }
    }

    double Value(double x) const
    {
        if (x <= xs_[0]) return ys_[0];
        for (size_t i = 1; i < xs_.size(); i++)
            if (x <= xs_[i])
                return xs_[i] == xs_[i - 1] ? ys_[i - 1]
                                            : ys_[i - 1] + (ys_[i] - ys_[i - 1]) * (x - xs_[i - 1]) / (xs_[i] - xs_[i - 1]);
        return ys_.back();
    }

private:
    std::vector<double> xs_, ys_;
};

// MSVC rand() (LCG 214013/2531011, 15-bit output), used where the game rolls dice.
class MsvcRand {
public:
    explicit MsvcRand(uint32_t seed = 1) : s_(seed) {}
    int Next()
    {
        s_ = s_ * 214013u + 2531011u;
        return static_cast<int>((s_ >> 16) & 0x7FFF);
    }
    // bRandom-like helper: integer in [0, n).
    int Next(int n) { return n <= 0 ? 0 : Next() % n; }

private:
    uint32_t s_;
};

} // namespace bb

#pragma once

// Port of NFSU2Unity Core/Numerics/X87.cs: helpers that mirror the x87 idioms
// SPEED2.EXE was compiled to, under the game's FPU state (control word 0x007F:
// 24-bit precision, round to nearest, all exceptions masked).
//
// Under PC=24 every x87 fadd/fsub/fmul/fdiv/fsqrt of float operands rounds its
// result once to a 24-bit significand, which is IEEE binary32 as long as the
// result is a normal float. Plain float arithmetic reproduces it provided every
// intermediate is rounded to float and nothing is contracted into an FMA
// (the target builds with /fp:precise, which does not contract on x64).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace bb {

namespace X87 {

// fnstsw ax / test ah, imm8 flag bits
constexpr int C0 = 0x01, C2 = 0x04, C3 = 0x40;

// Condition bits (in AH) after fcom/fcomp st0 = a with operand b.
inline int Fcom(float a, float b) {
    if (a < b) return C0;
    if (a > b) return 0;
    if (a == b) return C3;
    return C0 | C2 | C3;   // unordered
}

// PF after "test ah, mask": true when the masked byte has an even number of set bits (jp taken).
inline bool ParityEven(int ahMasked) {
    int v = ahMasked & 0xFF;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) == 0;
}

// MSVC 7.1 __ftol2 (0x75BED4): truncation toward zero to a 64-bit integer. NaN,
// infinities and |v| >= 2^63 give the x87 "integer indefinite" 0x8000000000000000.
inline int64_t FtoL2(double v) {
    if (std::isnan(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(v);
}

// __ftol2 result as used by "call __ftol2; mov reg, eax".
inline int FtoL2Lo(double v) { return static_cast<int>(static_cast<uint32_t>(static_cast<uint64_t>(FtoL2(v)))); }

inline float Bits(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

} // namespace X87

// A value on the x87 stack under PC=24: a 24-bit significand with the (much
// wider) extended exponent range, stored in a double. Each arithmetic operator
// computes in double and rounds the significand to 24 bits (ties to even)
// without clamping the exponent. Converting to float (= fstp dword) rounds once
// more, to the float grid. Implicit from float (fld dword); Store() = fstp dword.
struct F24 {
    double V = 0.0;

    F24() = default;
    F24(float f) : V(f) {}
    // A raw stack value (already on the 24-bit grid).
    static F24 Raw(double v) { F24 r; r.V = v; return r; }

    static F24 Load(float f) { return F24(f); }
    float Store() const { return static_cast<float>(V); }

    static double Round24(double d) {
        int64_t b; std::memcpy(&b, &d, 8);
        if ((b & 0x7FF0000000000000LL) == 0x7FF0000000000000LL) return d;   // inf / NaN
        int64_t lsb = (b >> 29) & 1;
        b += 0x0FFFFFFFLL + lsb;                                             // ties to even, carries into the exponent
        b &= ~0x1FFFFFFFLL;
        double r; std::memcpy(&r, &b, 8);
        return r;
    }

    // x87 NaN result of an operation whose host result r is NaN (see X87.cs).
    static double Nan(double r, double a, double b) {
        if (!std::isnan(r)) return r;
        bool na = std::isnan(a), nb = std::isnan(b);
        const int64_t Quiet = 0x0008000000000000LL, Mant = 0x000FFFFFFFFFFFFFLL;
        auto fromBits = [](int64_t x) { double d; std::memcpy(&d, &x, 8); return d; };
        auto toBits = [](double d) { int64_t x; std::memcpy(&x, &d, 8); return x; };
        if (!na && !nb) return fromBits(static_cast<int64_t>(0xFFF8000000000000ULL));
        int64_t qa = toBits(a) | Quiet, qb = toBits(b) | Quiet;
        if (!nb) return fromBits(qa);
        if (!na) return fromBits(qb);
        int64_t ma = qa & Mant, mb = qb & Mant;
        if (ma != mb) return fromBits(ma > mb ? qa : qb);
        return fromBits(qa >= 0 ? qa : qb);
    }

    static F24 Sqrt(F24 a) { return Raw(Round24(Nan(std::sqrt(a.V), a.V, a.V))); }
    static F24 Abs(F24 a) { return Raw(std::fabs(a.V)); }

    // fmul qword [c] under PC24: one rounding of the exact 24x53-bit product.
    static F24 MulDouble(F24 a, double c) {
        double p = Nan(a.V * c, a.V, c);
        double e = TwoProductError(a.V, c, p);
        int64_t b; std::memcpy(&b, &p, 8);
        if (e != 0 && (b & 0x1FFFFFFFLL) == 0x10000000LL) {
            int64_t t = b & ~0x1FFFFFFFLL;
            bool up = (e > 0) == (p > 0);
            int64_t r = up ? t + 0x20000000LL : t;
            double d; std::memcpy(&d, &r, 8);
            return Raw(d);
        }
        return Raw(Round24(p));
    }

    static double TwoProductError(double a, double b, double p) {
        const double split = 134217729.0;   // 2^27 + 1
        volatile double ca = split * a; double ah = ca - (ca - a), al = a - ah;
        volatile double cb = split * b; double bh = cb - (cb - b), bl = b - bh;
        return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    }
};

inline F24 operator+(F24 a, F24 b) { return F24::Raw(F24::Round24(F24::Nan(a.V + b.V, a.V, b.V))); }
inline F24 operator-(F24 a, F24 b) { return F24::Raw(F24::Round24(F24::Nan(a.V - b.V, a.V, b.V))); }
inline F24 operator*(F24 a, F24 b) { return F24::Raw(F24::Round24(F24::Nan(a.V * b.V, a.V, b.V))); }
inline F24 operator/(F24 a, F24 b) { return F24::Raw(F24::Round24(F24::Nan(a.V / b.V, a.V, b.V))); }
inline F24 operator-(F24 a) { return F24::Raw(-a.V); }
// Mixed float/F24: the float operand is loaded exactly (fld dword).
inline F24 operator+(F24 a, float b) { return a + F24(b); }
inline F24 operator-(F24 a, float b) { return a - F24(b); }
inline F24 operator*(F24 a, float b) { return a * F24(b); }
inline F24 operator/(F24 a, float b) { return a / F24(b); }
inline F24 operator+(float a, F24 b) { return F24(a) + b; }
inline F24 operator-(float a, F24 b) { return F24(a) - b; }
inline F24 operator*(float a, F24 b) { return F24(a) * b; }
inline F24 operator/(float a, F24 b) { return F24(a) / b; }

} // namespace bb

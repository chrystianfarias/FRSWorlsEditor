#pragma once

// Port of NFSU2Unity Core/Physics/Timing/TimingFpu.cs: x87 idioms shared by the timing and input ports
// (game FPU state: CW 0x007F, PC=24, round to nearest). Every arithmetic helper rounds once to binary32,
// which equals the x87 PC24 result for normal floats (docs/re/fp-determinism.md 2.2). Branch helpers
// reproduce "fcom; fnstsw ax; test ah, mask; jcc" literally (NaN included); the name says when the jump
// is TAKEN for st0 = a, operand = b.

#include "drive/numerics/X87.h"

#include <cstdint>

namespace bb {

namespace TimingFpu {

inline float A(float x, float y) { return x + y; }
inline float S(float x, float y) { return x - y; }
inline float M(float x, float y) { return x * y; }
inline float D(float x, float y) { return x / y; }

// "fild int; fmul dword c" (or fimul): the exact integer times c, rounded once to 24 bits.
// Plain (float)i * c rounds twice when |i| > 2^24; F24::MulDouble rounds the exact product once.
inline float FildMul(int64_t i, float c) { return F24::MulDouble(F24::Load(c), static_cast<double>(i)).Store(); }

// call __ftol2 ; use eax.
inline int Ftol(float f) { return X87::FtoL2Lo(f); }

// ---- jump-taken predicates ("st0 = a, operand = b")
// test ah,41h ; jnz  -> a <= b or unordered.
inline bool Jnz41(float a, float b) { return (X87::Fcom(a, b) & 0x41) != 0; }
// test ah,41h ; jz   -> a > b (ordered).
inline bool Jz41(float a, float b) { return (X87::Fcom(a, b) & 0x41) == 0; }
// test ah,41h ; jp   -> a > b or unordered.
inline bool Jp41(float a, float b) { return X87::ParityEven(X87::Fcom(a, b) & 0x41); }
// test ah,5 ; jp     -> a >= b or unordered.
inline bool Jp5(float a, float b) { return X87::ParityEven(X87::Fcom(a, b) & 0x05); }
// test ah,5 ; jnp    -> a < b (ordered).
inline bool Jnp5(float a, float b) { return !X87::ParityEven(X87::Fcom(a, b) & 0x05); }
// test ah,1 ; jnz    -> a < b or unordered.
inline bool Jnz1(float a, float b) { return (X87::Fcom(a, b) & 0x01) != 0; }
// test ah,44h ; jp   -> a != b or unordered.
inline bool Jp44(float a, float b) { return X87::ParityEven(X87::Fcom(a, b) & 0x44); }
// test ah,44h ; jnp  -> a == b (ordered).
inline bool Jnp44(float a, float b) { return !X87::ParityEven(X87::Fcom(a, b) & 0x44); }

} // namespace TimingFpu

} // namespace bb

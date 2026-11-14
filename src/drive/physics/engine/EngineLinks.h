#pragma once

// Port of NFSU2Unity Core/Physics/Engine/EngineLinks.cs.

#include "drive/numerics/X87.h"

namespace bb {

// Small x87 idiom shared by the engine area.
namespace EngineMath {
// "fild dword int ; fmul dword k" under PC24: one rounding of the exact product (int may exceed 2^24).
inline float FildMul(int i, float k) { return F24::MulDouble(k, static_cast<double>(i)).Store(); }
} // namespace EngineMath

} // namespace bb

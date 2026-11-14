#include "drive/physics/World.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/engine/EngineTables.h"

namespace bb {

float World::BRandom(float range, uint32_t& seed) {
    uint32_t s = seed;
    uint32_t r = s % 0x7FFFFFFFu;                       // div 0x7FFFFFFF (remainder < 2^31: the "jge" never adds 2^32)
    uint32_t c = s ^ 0x1D872B41u;
    uint32_t a = (c >> 5) ^ c;
    seed = (a << 27) ^ a ^ c;
    float u = F24::MulDouble(EngineK::RandScale, static_cast<double>(r)).Store();   // fild r ; fmul 2^-31 (r has up to 31 bits)
    return BWareMath::M(u, range);
}

} // namespace bb

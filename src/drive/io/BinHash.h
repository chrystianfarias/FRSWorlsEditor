#pragma once

// Port of NFSU2Unity Core/IO/BinHash.cs.

#include <cstdint>
#include <string>

namespace bb {

/// The engine's bStringHash (U2 SPEED2.EXE 0x43DB50): h = h*33 + byte, seed 0xFFFFFFFF, case-sensitive.
/// Used for solid, texture, light material, position marker, collision and car names.
namespace BinHash {
constexpr uint32_t Seed = 0xFFFFFFFFu;

inline uint32_t Hash(const std::string& name, uint32_t h = Seed)
{
    for (unsigned char c : name) h = h * 33u + c;
    return h;
}
} // namespace BinHash

} // namespace bb

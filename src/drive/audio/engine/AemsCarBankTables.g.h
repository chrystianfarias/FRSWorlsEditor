#pragma once

// Port of NFSU2Unity Core/Audio/Engine/AemsCarBankTables.g.cs (auto-generated data, see the .cpp).

namespace bb {
namespace AemsCarBankTables {

inline constexpr int Count = 41;
// Base64 of u16[2][201][8] (gain at load 0, load 1024) per bank CAR_00..CAR_40.
extern const char* const Gain[Count];
// Base64 of u16[201][8] (pitch 4.12, 0xFFFF = no pitch parameter) per bank.
extern const char* const Pitch[Count];

} // namespace AemsCarBankTables
} // namespace bb

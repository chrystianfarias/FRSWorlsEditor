#pragma once

// Port of NFSU2Unity Core/Physics/Engine/Nitrous.cs.

namespace bb {

class CarState;
class EngineTables;

// Nitrous methods of CarState (fields +0x414..+0x420, 4000 Hz ticks). "now" is the timer the game picks
// on every call: RealTimer when sub_5F00F0(&dword_89CF48) reports an online race, else WorldTimer.
namespace Nitrous {

// sub_592480 (CarState::GetNosSecondsLeft, inferred).
float GetNosSecondsLeft(const CarState& cs, int now);

// CarState::GetNosPercentageLeft 0x5925B0 (can exceed 1 when more than the capacity is left).
float GetNosPercentageLeft(const CarState& cs, int now);

// CarState::StopNitrous 0x592500.
void StopNitrous(CarState& cs, int now, int nitrousContinuous);

// CarState::IsNitrousOn 0x59FA30: NOS level 0..3 (stops the nitrous when it runs out).
int IsNitrousOn(CarState& cs, int now, const EngineTables& k);

// sub_59FAA0 (CanStartNitrous, inferred): level available when the nitrous is off, else 0.
int CanStartNitrous(const CarState& cs, int now, const EngineTables& k);

// CarState::StartNitrous 0x59FAF0.
bool StartNitrous(CarState& cs, int now, const EngineTables& k);

} // namespace Nitrous

} // namespace bb

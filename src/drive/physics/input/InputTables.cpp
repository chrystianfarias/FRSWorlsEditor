#include "drive/physics/input/InputTables.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/timing/SimClock.h"
#include "drive/physics/timing/TimingFpu.h"

#include <algorithm>
#include <utility>

namespace bb {

using namespace TimingFpu;

InputTable::InputTable(const Speed2Image& exe, uint32_t va) {
    Va = va;
    Count = static_cast<int>(exe.U32(va));
    Min = exe.F32(va + 4); Max = exe.F32(va + 8); Mult = exe.F32(va + 0xC);
    uint32_t p = exe.U32(va + 0x10);
    Data.assign(static_cast<size_t>(Count), 0.0f);
    for (int i = 0; i < Count; i++) Data[i] = exe.F32(p + 4 * static_cast<uint32_t>(i));
}

InputTable::InputTable(int count, float min, float max, float mult, std::vector<float> data)
    : Count(count), Min(min), Max(max), Mult(mult), Data(std::move(data)) {}

float InputTable::GetValue(float x) const { return BWareMath::TableGetValue(Count, Min, Mult, Data.data(), x); }

AverageWindow::AverageWindow(float window, float rate) {
    Capacity = static_cast<uint8_t>(Ftol(A(M(window, rate), TimingConstants::K_Half)));
    Window = window;
    Values.assign(static_cast<size_t>(std::max(static_cast<int>(Capacity), 1)), 0.0f);
    Times.assign(static_cast<size_t>(std::max(static_cast<int>(Capacity), 1)), 0.0f);
}

void AverageWindow::Record(float value, float time) {
    int idx = Index;
    if (Jp44(Values[idx], 0.0f) || Jp44(Times[idx], 0.0f)) Sum = S(Sum, Values[idx]);   // slot in use: drop old value
    else Count = static_cast<uint8_t>(Count + 1);
    Sum = A(value, Sum);                                              // fld value ; fadd sum
    Values[idx] = value;
    Times[idx] = time;
    // drop samples older than the window
    if (!Jnz41(S(time, Times[Oldest]), Window)) {
        do {
            if (!Jnz41(Times[Oldest], 0.0f)) {                        // time stamp > 0: slot in use
                Sum = S(Sum, Values[Oldest]);
                Values[Oldest] = 0.0f;
                Times[Oldest] = 0.0f;
                Count = static_cast<uint8_t>(Count - 1);
            }
            Oldest++;
            if (Oldest >= Capacity) Oldest = 0;
        } while (Jz41(S(time, Times[Oldest]), Window));
    }
    Average = D(Sum, static_cast<float>(Count));                      // fild count ; fdivr sum
    Index = static_cast<uint8_t>(Index + 1);
    if (!(Index < Capacity)) Index = 0;
}

InputTables::InputTables(const Speed2Image& exe)
    : SteerResponse(exe, 0x7E9E60),
      SteerResponseSpecial(exe, 0x7E9E9C),
      CounterSteer(exe, 0x7E9EB0),
      CounterSteerDrift(exe, 0x7E9EC4),
      SpeedLimit(exe, 0x7E9ED8),
      CurveAverageGain(exe, 0x7E9F00),
      SpeedFactor(exe, 0x7E9F14),
      RateInput(exe, 0x7E9F28),
      RateCurveAverage(exe, 0x7E9F3C),
      BurnoutThrottle(exe, 0x7EA014),
      DriftYaw(Graph::FromObject(exe, 0x7E9F50)),
      NeutralThrottle(Graph::FromObject(exe, 0x7EA004)),
      NeutralThrottleAlt(Graph::FromObject(exe, 0x7EA00C)),
      CollisionRecovery(Graph::FromPoints(exe, 0x7E9F58, 4)) {}

} // namespace bb

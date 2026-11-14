#include "drive/physics/engine/Nitrous.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarState.h"
#include "drive/physics/engine/EngineLinks.h"
#include "drive/physics/engine/EngineTables.h"

namespace bb {

namespace Nitrous {

namespace {
const int Off = 0x7FFFFFFF;
bool Active(int t) { return t != 0 && t != Off; }
inline float A(float x, float y) { return BWareMath::A(x, y); }
inline float S(float x, float y) { return BWareMath::S(x, y); }
inline float M(float x, float y) { return BWareMath::M(x, y); }
inline float D(float x, float y) { return BWareMath::D(x, y); }
} // namespace

float GetNosSecondsLeft(const CarState& cs, int now) {
    float elapsed = 0.0f;
    if (Active(cs.NosStartTime))
        elapsed = S(EngineMath::FildMul(now, EngineK::TickToSec), EngineMath::FildMul(cs.NosStartTime, EngineK::TickToSec));
    if (Active(cs.NosRemaining))
        return S(EngineMath::FildMul(cs.NosRemaining, EngineK::TickToSec), elapsed);
    return S(0.0f, elapsed);
}

float GetNosPercentageLeft(const CarState& cs, int now) {
    float left = GetNosSecondsLeft(cs, now);
    if (!Active(cs.NosCapacity)) return 0.0f;
    float cap = EngineMath::FildMul(cs.NosCapacity, EngineK::TickToSec);
    if ((X87::Fcom(cap, 0.0f) & 0x41) != 0) return 0.0f;                     // cap <= 0
    if ((X87::Fcom(left, cap) & 0x41) != 0) return A(0.0f, D(left, cap));    // left <= cap
    float over = D(D(S(left, cap), EngineK::NosOverflowDiv), cap);
    return A(over, D(cap, cap));
}

void StopNitrous(CarState& cs, int now, int nitrousContinuous) {
    if (!Active(cs.NosStartTime)) return;
    int elapsed = static_cast<int>(static_cast<uint32_t>(now) - static_cast<uint32_t>(cs.NosStartTime));
    float r = S(EngineMath::FildMul(cs.NosRemaining, EngineK::TickToSec), EngineMath::FildMul(elapsed, EngineK::TickToSec));
    if ((X87::Fcom(0.0f, r) & 0x41) == 0) r = 0.0f;                          // 0 > r
    cs.NosRemaining = X87::FtoL2Lo(A(M(r, EngineK::SecToTick), EngineK::Half));
    if (nitrousContinuous != 0) cs.NosShot = 0;
    cs.NosStartTime = 0;
}

int IsNitrousOn(CarState& cs, int now, const EngineTables& k) {
    if (!Active(cs.NosStartTime)) return 0;
    float p = GetNosPercentageLeft(cs, now);
    for (int i = 2; i >= 0; i--)
        if ((X87::Fcom(p, k.PercentThresholds[i]) & 0x41) == 0) return i + 1;
    StopNitrous(cs, now, k.NitrousContinuous);
    return 0;
}

int CanStartNitrous(const CarState& cs, int now, const EngineTables& k) {
    if (Active(cs.NosStartTime)) return 0;
    float p = GetNosPercentageLeft(cs, now);
    for (int i = 2; i >= 0; i--)
        if ((X87::Fcom(p, k.PercentThresholds[i]) & 0x41) == 0) return i + 1;
    return 0;
}

bool StartNitrous(CarState& cs, int now, const EngineTables& k) {
    if (CanStartNitrous(cs, now, k) == 0) return false;
    cs.NosStartTime = now;
    if (k.NitrousContinuous != 0) cs.NosShot = cs.NosRemaining;
    return true;
}

} // namespace Nitrous

} // namespace bb

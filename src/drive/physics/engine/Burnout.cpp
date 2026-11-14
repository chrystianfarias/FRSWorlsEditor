#include "drive/physics/engine/Burnout.h"

#include "drive/numerics/BWareMath.h"
#include "drive/physics/CarPhysicsInfo.h"
#include "drive/physics/CarState.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/DriveTrain.h"
#include "drive/physics/engine/EngineTables.h"
#include "drive/physics/wheel/Wheel.h"

#include <cmath>
#include <stdexcept>

namespace bb {

namespace Burnout {

namespace {
inline float A(float x, float y) { return BWareMath::A(x, y); }
inline float S(float x, float y) { return BWareMath::S(x, y); }
inline float M(float x, float y) { return BWareMath::M(x, y); }
inline float D(float x, float y) { return BWareMath::D(x, y); }
} // namespace

void CalcBurnout(const EngineTables& k, const BWareMath& math, World& world, Car& car, const CarPhysicsInfo& cpi,
                 Wheel* const* wheels, DriveTrain& dt) {
    CarState& cs = car.State;
    bool active = (car.DriverType == 1 || car.DriverType == 2 || car.DriverType == 3)
                  && !car.IsBlinking(world.WorldTimer)
                  && (X87::Fcom(cs.ForwardSpeed, 0.0f) & 0x41) == 0             // Car+0x42C > 0
                  && !world.Flags.Burnout();
    if (car.RideLevel == -1)
        throw std::runtime_error("RidePhysicsInfo::RebuildPhysicsInfo (lazy rebuild) is not ported");
    float split = cpi.RearTorqueSplit();
    int type = X87::ParityEven(X87::Fcom(split, EngineK::BurnoutDriveMin) & 0x05) ? 1 : 0;   // split >= 0.2 (or NaN)
    if ((X87::Fcom(split, EngineK::BurnoutDriveMax) & 0x41) == 0) type = 2;                // split > 0.8
    float gs = k.MiscCarTunings[type * 81 + car.RideLevel];                               // var_4

    if ((X87::Fcom(dt.ShiftBonus, 0.0f) & 0x41) == 0)                                      // drag shift bonus > 0
    {
        float a = D(1.0f, gs);
        for (int i = 0; i < 4; i++) {
            float x = M(a, dt.ShiftBonus);
            if (!X87::ParityEven(X87::Fcom(1.0f, x) & 0x05)) x = 1.0f;                     // 1 < x
            wheels[i]->GripScale = M(A(x, 1.0f), wheels[i]->GripScale);
        }
    }
    if (!active) return;

    float fwd = cs.ForwardSpeed;
    float mph = M(fwd, EngineK::MsToMph);
    if (cs.BurnoutCount != 0) {
        float slip = X87::ParityEven(X87::Fcom(fwd, EngineK::OneMs) & 0x05) ? cs.GetSlipAngle(math) : 0.0f;
        if ((X87::Fcom(std::fabs(slip), k.BurnoutSlipAngle) & 0x41) == 0) cs.BurnoutCount = 0;
        else {
            bool allBelow = true;
            for (int i = 0; i < 4 && allBelow; i++)
                allBelow = !X87::ParityEven(X87::Fcom(wheels[i]->SlipVel, k.BurnoutSlipLimit) & 0x05);
            if (allBelow) {
                float t = A(world.WorldTimeElapsed, cs.BurnoutStill);
                cs.BurnoutStill = t;
                if ((X87::Fcom(t, k.BurnoutStillTime) & 0x41) == 0) cs.BurnoutCount = 0;
            } else {
                cs.BurnoutStill = 0.0f;
                float t = S(cs.BurnoutTimer, world.WorldTimeElapsed);
                cs.BurnoutTimer = t;
                if (!X87::ParityEven(X87::Fcom(t, 0.0f) & 0x05)) {
                    cs.BurnoutCount--;
                    cs.BurnoutTimer = k.BurnoutPeriod;
                }
            }
        }
    } else if (world.RaceState == 4 && !X87::ParityEven(X87::Fcom(mph, k.BurnoutMaxMph) & 0x05))   // mph < 15
    {
        float inv = D(1.0f, k.BurnoutLoadDiv);                                     // var_8
        float best = 0.0f;                                                          // var_C
        int bestWheel = 0;
        for (int i = 0; i < 4; i++) {
            float s = wheels[i]->SlipVel;
            float e = M(M(wheels[i]->Load, inv), s);
            float m = !X87::ParityEven(X87::Fcom(s, e) & 0x05) ? s : e;            // min(s, e), ties -> e
            if ((X87::Fcom(m, best) & 0x41) == 0) { best = m; bestWheel = i; }
        }
        if ((X87::Fcom(best, EngineK::Half) & 0x41) == 0) {
            float g = inv;                                                          // var_8 is the graph output
            float gv = 0.0f;
            if (BWareMath2::GraphGetValueTo(k.BurnoutGraph.Points.data(), k.BurnoutGraph.Count, best, gv)) g = gv;
            cs.BurnoutIntensity = M(D(1.0f, gs), g);
            float x = M(S(EngineK::OnePointFive, g), static_cast<float>(k.BurnoutCountScale));   // fimul dword_7FC11C
            x = A(x, static_cast<float>(bestWheel & 1));                            // fiadd
            cs.BurnoutCount = X87::FtoL2Lo(x);
            cs.BurnoutTimer = k.BurnoutPeriod;
            cs.BurnoutStill = 0.0f;
        }
    }
    if (cs.BurnoutCount == 0) return;
    for (int i = 0; i < 4; i++) {
        wheels[i]->GripScale = M(k.BurnoutGripBase, wheels[i]->GripScale);
        float p = S(1.0f, cs.BurnoutIntensity);
        int par = cs.BurnoutCount % 2;                                            // C remainder (-1, 0, 1)
        bool diag = (i == 0 || i == 3) ? par == 0 : par == 1;
        p = diag ? M(p, k.BurnoutDiagonal) : M(p, S(1.0f, k.BurnoutDiagonal));
        wheels[i]->GripScale = M(S(1.0f, p), wheels[i]->GripScale);
    }
}

} // namespace Burnout

} // namespace bb

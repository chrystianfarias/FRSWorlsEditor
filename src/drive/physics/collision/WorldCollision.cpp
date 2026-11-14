// Port of NFSU2Unity Core/Physics/Collision/WorldCollision.cs.
#include "drive/physics/collision/WorldCollision.h"

#include "drive/numerics/BWareMath.h"

#include <cstdio>
#include <stdexcept>

namespace bb {
namespace CollisionK {

bool Equal3(const float* a, int ao, const float* b, int bo, float eps)
{
    for (int k = 0; k < 3; k++)
        if ((BWareMath2::Fcom(F24::Abs(F24(a[ao + k]) - b[bo + k]), F24(eps)) & 0x41) == 0) return false;
    return true;
}

F24 SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = (F24(px) - ax).Store(), dy = (F24(py) - ay).Store();
    F24 eyS = F24(by) - ay;
    float ex = (F24(bx) - ax).Store();
    float ey = eyS.Store();
    float len = F24::Sqrt(eyS * eyS + F24(ex) * ex).Store();
    float nx, ny;
    F24 l = F24::Sqrt(F24(ey) * ey + F24(ex) * ex);
    float ls = l.Store();
    if (!BWareMath2::NeOrNaN(F24(ls), F24(Zero))) { nx = 1.0f; ny = 0.0f; }
    else {
        F24 inv = F24(One) / ls;
        nx = (inv * ex).Store();
        ny = (inv * ey).Store();
    }
    float along = (F24(dy) * ny + F24(dx) * nx).Store();
    F24 perp = -F24(nx) * dy + F24(dx) * ny;
    if ((BWareMath2::Fcom(along, Zero) & 0x05) == 0x01)
        return F24::Sqrt(perp * perp + F24(along) * along);
    if ((BWareMath2::Fcom(along, len) & 0x41) != 0) return F24::Abs(perp);
    F24 t = F24(along) - len;
    return F24::Sqrt(t * t + perp * perp);
}

void Load(const Speed2Image& exe)
{
    auto C = [&](uint32_t va, float v) {
        if (exe.U32(va) != X87::Bits(v)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SPEED2.EXE constant at 0x%08X is 0x%08X, expected 0x%08X",
                          va, exe.U32(va), X87::Bits(v));
            throw std::runtime_error(buf);
        }
    };
    C(0x7A192C, GjkZero); C(0x7A1928, GjkContact); C(0x787594, Eps1e6); C(0x7845A0, Eps1e5); C(0x7A1878, PolygonThickness);
    C(0x7A187C, WitnessContactAge); C(0x784268, TickToSec); C(0x7A189C, PushZClamp); C(0x78425C, PushStepGrowth);
    C(0x7A1880, RadiusMargin); C(0x7A1888, AiInertiaScale); C(0x7A18A4, WallFrictionScale); C(0x7A18A8, SmokeableDownScale);
    C(0x7A1884, MaxInvMassDiv); C(0x7A24FC, NoImpulse); C(0x7844D8, FlatNormalZ); C(0x784550, Thousand);
    C(0x784554, InterestVelocityScale); C(0x787ADC, InterestVelocityMax); C(0x78BAFC, InterestVelocityMin);
    C(0x7A206C, SupportSentinel); C(0x784260, Half); C(0x784358, PointOne);
    MinImpulseInvMass = exe.F32(0x7FBF24);
    FrictionSpeed = exe.F32(0x7FBF30);
    CollisionLifetime = exe.F32(0x7FBEE0);
    ContactTimeTolerance = exe.F32(0x7FBF10);
    UseSidedBarrierNormals = static_cast<int>(exe.U32(0x7FBF18));
    RememberBarrierSide = static_cast<int>(exe.U32(0x7FBF14));
    MaxContactResolutions = static_cast<int>(exe.U32(0x802080));
    MaxContactsPerFrame = static_cast<int>(exe.U32(0x802084));
}

} // namespace CollisionK
} // namespace bb

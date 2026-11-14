#include "drive/physics/rigidbody/RigidBodyData.h"

#include "drive/numerics/BWareMath.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace bb {

float RigidBodyData::Table::GetValue(float x) const {
    return BWareMath::TableGetValue(Count, Min, IndexMultiplier, Data.data(), x);
}

RigidBodyData::Table RigidBodyData::Table::Load(const Speed2Image& exe, uint32_t va) {
    Table t;
    t.Count = static_cast<int>(exe.U32(va));
    t.Min = exe.F32(va + 4);
    t.Max = exe.F32(va + 8);
    t.IndexMultiplier = exe.F32(va + 12);
    uint32_t p = exe.U32(va + 16);
    t.Data.assign(static_cast<size_t>(t.Count > 0 ? t.Count : 0), 0.0f);
    for (int i = 0; i < t.Count; i++) t.Data[i] = exe.F32(p + 4 * static_cast<uint32_t>(i));
    return t;
}

RigidBodyData::RigidBodyData(const Speed2Image& exe) {
    auto C = [&exe](uint32_t va, float k) {
        if (exe.U32(va) != X87::Bits(k)) {
            char text[96];
            std::snprintf(text, sizeof text, "constant at 0x%08X differs from the exe", va);
            throw std::runtime_error(text);
        }
    };
    C(0x7D7D3C, Zero); C(0x784250, One); C(0x784260, Half);
    C(0x7A1E80, TopSpeedBias); C(0x7A1B18, RaceEngineTopSpeedMul); C(0x7A1AF4, UphillMaxSlope);
    C(0x7A1AF0, UphillMinFactor); C(0x7A1AEC, DownhillMaxSlope); C(0x7A1AE8, DownhillMaxFactor);
    C(0x7A1E7C, MpsToMph); C(0x7A1AFC, LimiterSlopeGain); C(0x7A1AF8, LimiterMaxForce);
    C(0x7A1B00, RestSpeedDiv); C(0x7A1B04, RestSpinDiv); C(0x7A1B10, RestLevelRise); C(0x7A1B14, RestLevelFall);
    C(0x7A1B0C, RestAngularGain); C(0x7A1B08, RestLinearGain); C(0x7A1E84, InvGravityG); C(0x7A1B1C, MaxAngularSpeed);
    C(0x7A1AD8, MaxForce); C(0x7A1ADC, MaxTorque); C(0x7A1AE0, MaxImpulse); C(0x7A1AE4, MaxAngularImpulse);
    C(0x7A1E4C, AeroMinDownforceFactor); C(0x7A1F18, AeroAirRearShare); C(0x7843EC, AeroMinSpeed);
    C(0x7A1884, CcbMaxInvMassDiv); C(0x78758C, CcbFriction); C(0x7844D8, CcbFlatNormalZ);
    C(0x7A1898, CcbFrictionPerAirWheel); C(0x7A1894, CcbFriction428); C(0x7A1890, CcbWallFrictionMul);
    C(0x7A188C, CcbWallBounceMul); C(0x784358, PointOne);
    std::vector<uint8_t> d = exe.Bytes(0x7A1E88, 8);
    double dv;
    std::memcpy(&dv, d.data(), 8);
    if (dv != VelocityClampMinMass)
        throw std::runtime_error("constant at 0x007A1E88 differs from the exe");
    BWareMath2::CheckConstants(exe);

    VelocityClamp = exe.F32(VA_VelocityClamp);
    Gravity = exe.F32(VA_Gravity);
    AeroThrottleDragMul = exe.F32(VA_AeroThrottleDragMul);
    AeroLiftLever = exe.F32(VA_AeroLiftLever);
    for (int i = 0; i < 3; i++) {
        AeroFrontLever[i] = exe.F32(VA_AeroFrontLever + 4 * static_cast<uint32_t>(i));
        AeroRearLever[i] = exe.F32(VA_AeroRearLever + 4 * static_cast<uint32_t>(i));
    }
    AngularDamping = Table::Load(exe, VA_AngularDampingTable);
    RestLevel = Table::Load(exe, VA_RestLevelTable);
}

} // namespace bb

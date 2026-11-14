#include "drive/physics/wheel/WheelEnvironment.h"

#include "drive/numerics/BWareMath.h"

#include <cstdio>
#include <stdexcept>

namespace bb {

WheelTweaks WheelTweaks::Load(const Speed2Image& exe) {
    WheelTweaks t;
    t.WheelMomentOfInertia = exe.F32(0x7FC6F0);
    t.MaxStaticSlip = exe.F32(0x7FC704);
    t.BrakingTorque = exe.F32(0x7FC708);
    t.EBrakingTorque = exe.F32(0x7FC70C);
    t.StaticToDynamicBrakeForceRatio = exe.F32(0x7FC710);
    t.BrakeLockAngularVelocityFactor = exe.F32(0x7FC714);
    t.ZForceScale = exe.F32(0x7FC718);
    t.PilotFactor = exe.F32(0x7FC71C);
    t.DriveBoost = exe.F32(0x7FC720);
    t.OldCorneringScale = exe.F32(0x7FC740);
    t.NewCorneringScale = exe.F32(0x7FC7F4);
    t.LoadFactor = exe.F32(0x7FC7F8);
    t.UseNewTireForces = static_cast<int>(exe.U32(0x7FC7F0));
    t.WheelForceFrame = static_cast<int>(exe.U32(0x7FC7FC));
    return t;
}

float WheelTable::GetValue(float x) const { return BWareMath::TableGetValue(Count, Min, IndexScale, Data.data(), x); }

WheelTable WheelTable::Read(const Speed2Image& exe, uint32_t va) {
    WheelTable t;
    t.Count = static_cast<int>(exe.U32(va));
    t.Min = exe.F32(va + 4);
    t.Max = exe.F32(va + 8);
    t.IndexScale = exe.F32(va + 12);
    uint32_t data = exe.U32(va + 16);
    t.Data.assign(static_cast<size_t>(t.Count > 0 ? t.Count : 0), 0.0f);
    for (int i = 0; i < t.Count; i++) t.Data[i] = WheelK::F32OrZero(exe, data + static_cast<uint32_t>(4 * i));
    return t;
}

WheelTables::WheelTables(const Speed2Image& exe) {
    for (int i = 0; i < TerrainCount; i++) {
        uint32_t b = VA_TerrainTypeInfoTable + static_cast<uint32_t>(TerrainStride * i);
        Terrain[i].CircleMu = exe.F32(b + 0x24);
        Terrain[i].DynamicMu = exe.F32(b + 0x28);
        Terrain[i].Rolling = exe.F32(b + 0x2C);
    }
    for (int k = 0; k < 7; k++) Curves[k] = WheelTable::Read(exe, exe.U32(VA_NewTireCurves + static_cast<uint32_t>(4 * k)));
    CorneringForce = WheelTable::Read(exe, VA_CorneringForce);
    // Static initialisers (run by the CRT before Direct3D exists, so under PC53):
    // 0x77E120: dword_86B510 = 0x3E64C388; 0x77E130: CorneringForce.max = dword_86B510,
    // CorneringForce.indexScale = (float)(6.0 / max) (fdivr flt_7859D0 = 6.0, PC53, then fstp dword).
    CorneringForce.Max = X87::Bits(0x3E64C388u);
    CorneringForce.IndexScale = static_cast<float>(exe.F32(0x7859D0) / static_cast<double>(CorneringForce.Max));
}

namespace WheelK {

void Verify(const Speed2Image& exe) {
    struct Entry { uint32_t va; float k; };
    const Entry all[] = {
        {0x7D7D3C, Zero}, {0x784250, One}, {0x784260, Half}, {0x784358, PointOne}, {0x784240, AngToRad},
        {0x7A1788, Pi}, {0x784254, HalfPi}, {0x78424C, RadToDeg}, {0x78821C, Twelve}, {0x78435C, Eps1e3},
        {0x7843EC, Eps1e4}, {0x7845A0, RollEps}, {0x7A1E98, MinusRollEps}, {0x7A1EA8, AngleWrap},
        {0x7A1EA4, AngleReset}, {0x7A1EA0, MinusAngleWrap}, {0x7A1E9C, MinusAngleReset}, {0x7A1B20, RollingCoef},
        {0x7A1B24, FlatDragCoef}, {0x7A1B38, FlatWobble}, {0x7A1B30, PenaltyGain}, {0x7A1CA4, NoElevation},
        {0x7A1EAC, NoGround}, {0x7A1EB0, PilotSpeedMin}, {0x7850FC, PilotSpeedScale}, {0x7844F8, LoadTiltMul},
        {0x787ADC, LoadTiltSub}, {0x78758C, LoadTiltMin}, {0x7844F0, LatLongCoupling}, {0x78426C, MpsToMph},
        {0x784554, FwdSplit}, {0x7850EC, RwdSplit},
    };
    for (const Entry& e : all)
        if (exe.U32(e.va) != X87::Bits(e.k)) {
            char text[96];
            std::snprintf(text, sizeof text, "wheel constant at 0x%08X differs from the exe", e.va);
            throw std::runtime_error(text);
        }
}

float F32OrZero(const Speed2Image& exe, uint32_t va) {
    try {
        return exe.F32(va);
    } catch (const std::runtime_error&) {
        return 0.0f;
    }
}

} // namespace WheelK

} // namespace bb

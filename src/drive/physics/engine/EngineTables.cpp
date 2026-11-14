#include "drive/physics/engine/EngineTables.h"

#include <cstdio>
#include <stdexcept>

namespace bb {

EngineTables::EngineTables(const Speed2Image& exe) {
    ClutchSpeed = Floats(exe, VA_ClutchSpeed, 4);
    PerfectShiftMaxRPMDiff = Floats(exe, VA_PerfectShiftMaxRPMDiff, 36);
    PerfectShiftSplit = exe.F32(VA_PerfectShiftSplit);
    LimitedSlip = exe.F32(VA_LimitedSlip);
    TorqueHPMultiplier = exe.F32(VA_TorqueHPMultiplier);
    HeatThreshold = exe.F32(VA_HeatThreshold);
    ClutchSlipClamp = exe.F32(VA_ClutchSlipClamp);
    ClutchSyncTolerance = exe.F32(VA_ClutchSyncTolerance);
    AIShiftRatio = exe.F32(VA_AIShiftRatio);
    ShiftModRangeMin0 = exe.F32(VA_ShiftModRangeMin0);
    NosCurve = Floats(exe, VA_NosCurve, 9);
    NitrousContinuous = static_cast<int>(exe.U32(VA_NitrousContinuous));
    ClutchPlay = ReadGraph(exe, VA_ClutchPlayGraph);
    BurnoutGraph = ReadGraph(exe, VA_BurnoutGraph);
    MiscCarTunings = Floats(exe, VA_MiscCarTunings, 3 * 81);
    BurnoutDiagonal = exe.F32(VA_BurnoutDiagonal);
    BurnoutGripBase = exe.F32(VA_BurnoutGripBase);
    BurnoutSlipLimit = exe.F32(VA_BurnoutSlipLimit);
    BurnoutSlipAngle = exe.F32(VA_BurnoutSlipAngle);
    BurnoutStillTime = exe.F32(VA_BurnoutStillTime);
    BurnoutLoadDiv = exe.F32(VA_BurnoutLoadDiv);
    BurnoutMaxMph = exe.F32(VA_BurnoutMaxMph);
    BurnoutPeriod = exe.F32(VA_BurnoutPeriod);
    BurnoutCountScale = static_cast<int>(exe.U32(VA_BurnoutCountScale));
    NosTorqueMultiplier = Floats(exe, VA_GameBrakerNosTorqueMultiplier, 4);
    PercentThresholds = Floats(exe, VA_GameBrakerPercentThresholds, 3);
    ClutchPlayFactor = Floats(exe, VA_ClutchPlayFactor, 4);
    EngineK::Verify(exe);
}

Graph EngineTables::ReadGraph(const Speed2Image& exe, uint32_t va) {
    uint32_t pts = exe.U32(va);
    int n = static_cast<int>(exe.U32(va + 4));
    return Graph(Floats(exe, pts, 2 * n), n);
}

std::vector<float> EngineTables::Floats(const Speed2Image& exe, uint32_t va, int n) {
    std::vector<float> f(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) f[i] = exe.F32(va + static_cast<uint32_t>(4 * i));
    return f;
}

namespace EngineK {

void Verify(const Speed2Image& exe) {
    struct Entry { uint32_t va; float k; };
    const Entry all[] = {
        {0x7A1D28, RadToRpm}, {0x7A1D38, RpmToRad}, {0x78435C, Split001}, {0x79B158, Split999}, {0x784260, Half},
        {0x7A18D4, ShiftDownRate}, {0x7844D8, EngagingGate}, {0x78A7DC, ClutchEngaged}, {0x7844F4, ClutchReleased}, {0x7A1914, HeatFade},
        {0x7A18D8, Two}, {0x7A1918, NeutralDecel}, {0x7A191C, GearDecel}, {0x7A1920, InertiaMul}, {0x7A1910, HeatBlown},
        {0x7A1904, HeatRise}, {0x7A1908, HeatFall}, {0x7A190C, HeatLimit}, {0x7881F4, Eight}, {0x7844C0, TwoTbl},
        {0x7A18E0, LimiterHyst}, {0x7A18DC, LimiterOn}, {0x7A18E4, TurboMaxDiv}, {0x7A18E8, TurboIdle}, {0x787598, TurboT0},
        {0x7A1900, TurboFreq}, {0x784254, PiOver2}, {0x7A18FC, TurboAmp}, {0x7A18F4, TurboSettle}, {0x7A18F8, TurboMaxT},
        {0x7A18EC, TurboThrottle}, {0x7A18F0, TurboRate}, {0x784D28, Quarter}, {0x7845A0, Eps5}, {0x78A514, RpmDiffBig},
        {0x7A1924, HalfK}, {0x784554, Point2}, {0x78426C, MsToMph}, {0x7864D8, Mph40}, {0x7A1D3C, GoodShift},
        {0x78758C, AutoDisengageThrottle}, {0x7843EC, IdleEps}, {0x784268, TickToSec}, {0x7A18C8, ShiftLockSec},
        {0x7A18CC, BonusSec}, {0x7A18D0, BonusGain}, {0x784258, Hundred}, {0x7A1AC8, Fifty}, {0x795AE8, FiveHundred},
        {0x7A18BC, ModRangeMax}, {0x7A18C4, ModRangeMax}, {0x7A18C0, DefaultShiftUpMin}, {0x786CE8, RandScale},
        {0x7A178C, NosOverflowDiv}, {0x784264, SecToTick}, {0x78AAAC, TwoHundred}, {0x7844F8, SlipLimit},
        {0x784560, TenSec}, {0x7852A8, ThreeHundred}, {0x7D7D3C, Zero}, {0x784250, One},
        {0x784358, Point1}, {0x7842A0, MinusOne}, {0x7A1824, OneMs}, {0x785110, OnePointFive}, {0x7A1788, Pi},
        {0x784240, AngToRad}, {0x784554, BurnoutDriveMin}, {0x7850EC, BurnoutDriveMax},
    };
    for (const Entry& e : all)
        if (exe.U32(e.va) != X87::Bits(e.k)) {
            char text[96];
            std::snprintf(text, sizeof text, "engine constant at 0x%08X differs from the exe", e.va);
            throw std::runtime_error(text);
        }
    if (exe.U32(0x786E60) != 0 || exe.U32(0x786E64) != 0)   // dbl_786E60 (double 0.0, DriveTrain::ComputeAuxiliaries)
        throw std::runtime_error("dbl_786E60 is not 0.0");
}

} // namespace EngineK

} // namespace bb

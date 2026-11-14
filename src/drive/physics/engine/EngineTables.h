#pragma once

// Port of NFSU2Unity Core/Physics/Engine/EngineTables.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"
#include "drive/physics/wheel/Graph.h"

#include <cstdint>
#include <vector>

namespace bb {

// Globals of SPEED2.EXE read by the engine / drivetrain / nitrous code. Values living in .data are
// tunable in the game (they are plain globals) and are loaded from the installed exe at their initial
// value; .rdata constants are kept as bit patterns and verified against the exe.
class EngineTables {
public:
    // ---------------------------------------------------------------- .data (initial values from the exe)
    static constexpr uint32_t VA_ClutchSpeed = 0x7FC298;              // float[4] {-3, 0, 1.5, 0} per clutch state
    static constexpr uint32_t VA_PerfectShiftMaxRPMDiff = 0x7FBF60;   // float[4*9] (perf type 0..3)
    static constexpr uint32_t VA_PerfectShiftSplit = 0x7FBF5C;        // 0.95
    static constexpr uint32_t VA_LimitedSlip = 0x7FBFFC;              // LIMITEDSLIP 0.35
    static constexpr uint32_t VA_TorqueHPMultiplier = 0x7FBFF4;       // 1.05
    static constexpr uint32_t VA_HeatThreshold = 0x7FBFF0;            // flt_7FBFF0 0.6
    static constexpr uint32_t VA_ClutchSlipClamp = 0x7FBF34;          // flt_7FBF34 300 (GuessClutchTorque)
    static constexpr uint32_t VA_ClutchSyncTolerance = 0x7FC028;      // flt_7FC028 50 (IsClutchSynced)
    static constexpr uint32_t VA_AIShiftRatio = 0x7FC02C;             // flt_7FC02C 1.2 (UpdateGearSelection, AI)
    static constexpr uint32_t VA_NosCurve = 0x7FBF38;                 // dword_7FBF38 float[9] NOS torque curve
    static constexpr uint32_t VA_NitrousContinuous = 0x7FBEDC;        // int 1
    static constexpr uint32_t VA_ShiftModRangeMin0 = 0x7FC294;        // flt_7FC294 -100 (DriveTrain ctor, +0xCC)
    static constexpr uint32_t VA_ClutchPlayGraph = 0x7FC8C0;          // off_7FC8C0: Graph {pts*, n}
    static constexpr uint32_t VA_MiscCarTunings = 0x7FC2A8;           // TheMiscCarTunings[3] (FWD, AWD, RWD), 0x144 bytes each
    static constexpr uint32_t VA_BurnoutGraph = 0x7FC924;             // off_7FC924: Graph {pts*, n} (CalcBurnout)
    static constexpr uint32_t VA_BurnoutDiagonal = 0x7FC0FC;          // flt_7FC0FC 0.35 grip share of the hopping diagonal
    static constexpr uint32_t VA_BurnoutGripBase = 0x7FC100;          // flt_7FC100 1.0
    static constexpr uint32_t VA_BurnoutSlipLimit = 0x7FC104;         // flt_7FC104 2.5 (Wheel+0x118)
    static constexpr uint32_t VA_BurnoutSlipAngle = 0x7FC108;         // flt_7FC108 0.3 rad
    static constexpr uint32_t VA_BurnoutStillTime = 0x7FC10C;         // flt_7FC10C 1.0 s
    static constexpr uint32_t VA_BurnoutLoadDiv = 0x7FC110;           // flt_7FC110 3.0
    static constexpr uint32_t VA_BurnoutMaxMph = 0x7FC114;            // flt_7FC114 15 mph
    static constexpr uint32_t VA_BurnoutPeriod = 0x7FC118;            // dword_7FC118 0.5 s
    static constexpr uint32_t VA_BurnoutCountScale = 0x7FC11C;        // dword_7FC11C int 6

    // ---------------------------------------------------------------- .rdata tables
    static constexpr uint32_t VA_GameBrakerNosTorqueMultiplier = 0x7A1798; // float[4], index = NOS level 1..3
    static constexpr uint32_t VA_GameBrakerPercentThresholds = 0x7A1790;   // float[3]
    static constexpr uint32_t VA_ClutchPlayFactor = 0x7A18AC;              // float[4] by perf type (0, -0.2, -0.4, -0.7)

    std::vector<float> ClutchSpeed;
    std::vector<float> PerfectShiftMaxRPMDiff;
    float PerfectShiftSplit = 0, LimitedSlip = 0, TorqueHPMultiplier = 0, HeatThreshold = 0;
    float ClutchSlipClamp = 0, ClutchSyncTolerance = 0, AIShiftRatio = 0, ShiftModRangeMin0 = 0;
    std::vector<float> NosCurve;
    int NitrousContinuous = 0;
    Graph ClutchPlay;                // off_7FC8C0 (sub_40C840)
    std::vector<float> NosTorqueMultiplier, PercentThresholds, ClutchPlayFactor;
    std::vector<float> MiscCarTunings;
    Graph BurnoutGraph;              // off_7FC924 (sub_40C840)
    float BurnoutDiagonal = 0, BurnoutGripBase = 0, BurnoutSlipLimit = 0, BurnoutSlipAngle = 0, BurnoutStillTime = 0;
    float BurnoutLoadDiv = 0, BurnoutMaxMph = 0, BurnoutPeriod = 0;
    int BurnoutCountScale = 0;

    explicit EngineTables(const Speed2Image& exe);

    // Reads a Graph {float (*pts)[2]; int count} from the exe.
    static Graph ReadGraph(const Speed2Image& exe, uint32_t va);

private:
    static std::vector<float> Floats(const Speed2Image& exe, uint32_t va, int n);
};

// .rdata float constants used by this area (bit patterns, checked against the exe).
namespace EngineK {
inline const float RadToRpm = X87::Bits(0x4118C9F3u);      // flt_7A1D28 9.549304
inline const float RpmToRad = X87::Bits(0x3DD67744u);      // flt_7A1D38 0.10471967
inline const float Split001 = X87::Bits(0x3A83126Fu);      // flt_78435C 0.001
inline const float Split999 = X87::Bits(0x3F7FBE77u);      // flt_79B158 0.999
inline const float Half = X87::Bits(0x3F000000u);          // flt_784260 0.5
inline const float One = X87::Bits(0x3F800000u);           // float_1
inline const float Zero = 0.0f;                            // _zero 0x7D7D3C
inline const float EngagingGate = X87::Bits(0x3F333333u);  // flt_7844D8 0.7
inline const float ShiftDownRate = X87::Bits(0x3EB33333u); // flt_7A18D4 0.35
inline const float ClutchEngaged = X87::Bits(0x3F733333u); // flt_78A7DC 0.95
inline const float ClutchReleased = X87::Bits(0x3D4CCCCDu);// flt_7844F4 0.05
inline const float HeatFade = X87::Bits(0x3F400000u);      // flt_7A1914 0.75
inline const float Two = X87::Bits(0x40000000u);           // flt_7A18D8 2.0 (limiter engine brake)
inline const float NeutralDecel = X87::Bits(0x3F266666u);  // flt_7A1918 0.65
inline const float GearDecel = X87::Bits(0x3F59999Au);     // flt_7A191C 0.85
inline const float InertiaMul = X87::Bits(0x3F800000u);    // flt_7A1920 1.0
inline const float HeatBlown = X87::Bits(0x40000000u);     // flt_7A1910 2.0
inline const float HeatRise = X87::Bits(0x3EE66666u);      // flt_7A1904 0.45
inline const float HeatFall = X87::Bits(0xBDCCCCCDu);      // flt_7A1908 -0.1
inline const float HeatLimit = X87::Bits(0x3F99999Au);     // flt_7A190C 1.2
inline const float Eight = X87::Bits(0x41000000u);         // flt_7881F4 8.0
inline const float TwoTbl = X87::Bits(0x40000000u);        // flt_7844C0 2.0
inline const float LimiterHyst = X87::Bits(0x3C4CCCCDu);   // flt_7A18E0 0.0125
inline const float LimiterOn = X87::Bits(0xC1C80000u);     // flt_7A18DC -25.0
inline const float TurboMaxDiv = X87::Bits(0x3E428F5Cu);   // flt_7A18E4 0.19
inline const float TurboIdle = X87::Bits(0x3E800000u);     // flt_7A18E8 0.25
inline const float TurboT0 = X87::Bits(0x3F8CCCCDu);       // flt_787598 1.1
inline const float TurboFreq = X87::Bits(0x40D00000u);     // flt_7A1900 6.5
inline const float PiOver2 = X87::Bits(0x3FC90FDBu);       // flt_784254
inline const float TurboAmp = X87::Bits(0x3C75C28Fu);      // flt_7A18FC 0.015
inline const float TurboSettle = X87::Bits(0x3E19999Au);   // flt_7A18F4 0.15
inline const float TurboMaxT = X87::Bits(0x3F000000u);     // flt_7A18F8 0.5
inline const float TurboThrottle = X87::Bits(0x3E99999Au); // flt_7A18EC 0.3
inline const float TurboRate = X87::Bits(0x40000000u);     // flt_7A18F0 2.0
inline const float Quarter = X87::Bits(0x3E800000u);       // flt_784D28 0.25
inline const float Eps5 = X87::Bits(0x3727C5ACu);          // flt_7845A0 1e-5
inline const float RpmDiffBig = X87::Bits(0x44FA0000u);    // flt_78A514 2000
inline const float HalfK = X87::Bits(0x3F000000u);         // flt_7A1924 0.5
inline const float Point2 = X87::Bits(0x3E4CCCCDu);        // flt_784554 0.2
inline const float Point1 = X87::Bits(0x3DCCCCCDu);        // _zero_point_one 0.1
inline const float MsToMph = X87::Bits(0x400F2B02u);       // flt_78426C 2.237
inline const float Mph40 = X87::Bits(0x42200000u);         // flt_7864D8 40
inline const float GoodShift = X87::Bits(0x3F59999Au);     // flt_7A1D3C 0.85
inline const float MinusOne = X87::Bits(0xBF800000u);      // _float_minus1
inline const float AutoDisengageThrottle = X87::Bits(0x3E99999Au); // flt_78758C 0.3
inline const float IdleEps = X87::Bits(0x38D1B717u);       // flt_7843EC 1e-4
inline const float TickToSec = X87::Bits(0x3983126Fu);     // flt_784268 0.00025
inline const float ShiftLockSec = X87::Bits(0x3EE66666u);  // flt_7A18C8 0.45
inline const float BonusSec = X87::Bits(0x3F000000u);      // flt_7A18CC 0.5
inline const float BonusGain = X87::Bits(0x40000000u);     // flt_7A18D0 2.0
inline const float Hundred = X87::Bits(0x42C80000u);       // flt_784258 100
inline const float Fifty = X87::Bits(0x42480000u);         // flt_7A1AC8 50
inline const float FiveHundred = X87::Bits(0x43FA0000u);   // flt_795AE8 500
inline const float ModRangeMax = 0.0f;                     // flt_7A18BC / flt_7A18C4 0.0
inline const float DefaultShiftUpMin = X87::Bits(0xC2C80000u); // DefaultShiftUpMin 0x7A18C0 -100
inline const float RandScale = X87::Bits(0x30000000u);     // flt_786CE8 2^-31
inline const float NosOverflowDiv = X87::Bits(0x3F800000u);// flt_7A178C 1.0
inline const float SecToTick = X87::Bits(0x457A0000u);     // flt_784264 4000
inline const float TwoHundred = X87::Bits(0x43480000u);    // flt_78AAAC 200
inline const float SlipLimit = X87::Bits(0x40800000u);     // flt_7844F8 4.0
inline const float TenSec = X87::Bits(0x41200000u);        // flt_784560 10
inline const float ThreeHundred = X87::Bits(0x43960000u);  // flt_7852A8 300
inline const float OneMs = X87::Bits(0x3F800000u);         // flt_7A1824 1.0 (CalcBurnout min speed)
inline const float OnePointFive = X87::Bits(0x3FC00000u);  // flt_785110 1.5
inline const float Pi = X87::Bits(0x40490FDBu);            // flt_7A1788
inline const float AngToRad = X87::Bits(0x38C90FDBu);      // flt_784240 2pi/65536
inline const float BurnoutDriveMin = X87::Bits(0x3E4CCCCDu); // flt_784554 0.2
inline const float BurnoutDriveMax = X87::Bits(0x3F4CCCCDu); // flt_7850EC 0.8

void Verify(const Speed2Image& exe);
} // namespace EngineK

} // namespace bb

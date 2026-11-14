#pragma once

// Port of NFSU2Unity Core/Physics/Wheel/WheelEnvironment.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cstdint>
#include <vector>

namespace bb {

// Tweakable globals of the wheel/tyre slice (SPEED2.EXE v1.2 .data), loaded from the installed exe.
// They are variables in the game (initialised .data), so they are read at run time instead of being
// hard-coded; the .rdata constants live in WheelK. See docs/re/physics-overview.md, appendix A section 2.
class WheelTweaks {
public:
    float WheelMomentOfInertia = 0;            // 0x7FC6F0 (0.01)
    float MaxStaticSlip = 0;                   // 0x7FC704 (0.5)
    float BrakingTorque = 0;                   // 0x7FC708 (4.0)
    float EBrakingTorque = 0;                  // 0x7FC70C (10.0)
    float StaticToDynamicBrakeForceRatio = 0;  // 0x7FC710 (1.2)
    float BrakeLockAngularVelocityFactor = 0;  // 0x7FC714 (0.1)
    float ZForceScale = 0;                     // 0x7FC718 (1.0)
    float PilotFactor = 0;                     // 0x7FC71C (0.85)
    float DriveBoost = 0;                      // 0x7FC720 (1.5) longitudinal boost in FinalizeGroundForce
    float OldCorneringScale = 0;               // 0x7FC740 (1.0) old tyre model only
    float NewCorneringScale = 0;               // 0x7FC7F4 (2.5)
    float LoadFactor = 0;                      // 0x7FC7F8 (0.8)

    // Game state that other systems change at run time (defaults = .data / .bss initial values).
    int UseNewTireForces = 0;                  // 0x7FC7F0 (1)
    int WheelForceFrame = 0;                   // 0x7FC7FC (1)
    // UglyTimestepHack 0x7FC800, NoWheels 0x86B36C and the drag flags live in World (shared game globals).

    static WheelTweaks Load(const Speed2Image& exe);
};

// struct Table {int count; float min, max, indexScale; float* data} as read by Table::GetValue 0x5764D0.
class WheelTable {
public:
    int Count = 0;
    float Min = 0, Max = 0, IndexScale = 0;
    std::vector<float> Data;

    float GetValue(float x) const;

    // Reads a Table header from the exe; data in .bss (no file bytes) reads as zeros, like the loaded image.
    static WheelTable Read(const Speed2Image& exe, uint32_t va);
};

// TerrainTypeInfoTable 0x800EE0 (stride 0x98, 29 entries): the three friction fields used by the wheel.
struct TerrainFriction {
    float CircleMu = 0;     // +0x24 friction-circle mu (FinalizeGroundForce)
    float DynamicMu = 0;    // +0x28 dynamic mu (GuessGroundForce)
    float Rolling = 0;      // +0x2C rolling resistance factor (GetDerivative)
};

// Everything the wheel slice reads from the exe: tweakables, tyre curves and the terrain table.
class WheelTables {
public:
    static constexpr uint32_t VA_TerrainTypeInfoTable = 0x800EE0;
    static constexpr int TerrainStride = 0x98, TerrainCount = 29;
    static constexpr uint32_t VA_NewTireCurves = 0x7FC7D4;      // Table* off_7FC7D4[7]; [6] = LoadSensitivityTable (0x7FC7EC)
    static constexpr uint32_t VA_CorneringForce = 0x7FC954;     // Table (old tyre model)
    static constexpr uint32_t VA_CorneringForceMax = 0x86B510;  // .bss, written by the static initialiser 0x77E120

    TerrainFriction Terrain[TerrainCount] = {};
    WheelTable Curves[7];                                    // off_7FC7D4[k], k = slip angle in sixths of the peak
    const WheelTable& LoadSensitivity() const { return Curves[6]; }   // *LoadSensitivityTable (0x7FC7EC)
    WheelTable CorneringForce;

    explicit WheelTables(const Speed2Image& exe);
};

// World queries used by Wheel::CheckTerrain (the real ones walk the track topology).
class IWheelWorld {
public:
    virtual ~IWheelWorld() = default;

    // TopologyCoordinate::GetElevation (thiscall on CarState+0x4): elevation in st0; writes terrain, normal and valid.
    // topology = the C# `object` (TopologyCoordinate of the car, or null).
    virtual float GetElevation(void* topology, const float* point, int& terrain, float* normal, bool& valid) = 0;

    // GetRoadNoise(bVector2*, TerrainType) 0x5D3CA0 (cdecl, st0).
    virtual float GetRoadNoise(const float* pointXY, int terrain) = 0;
};

// .rdata constants used by the wheel slice (exact bit patterns, verified against the exe).
namespace WheelK {
inline const float Zero = X87::Bits(0x00000000u);            // _zero 0x7D7D3C
inline const float One = X87::Bits(0x3F800000u);             // float_1 0x784250
inline const float MinusOne = X87::Bits(0xBF800000u);        // immediate 0BF800000h (GuessGroundForce)
inline const float Half = X87::Bits(0x3F000000u);            // flt_784260 0.5
inline const float PointOne = X87::Bits(0x3DCCCCCDu);        // _zero_point_one 0x784358
inline const float AngToRad = X87::Bits(0x38C90FDBu);        // flt_784240 2pi/65536
inline const float Pi = X87::Bits(0x40490FDBu);              // flt_7A1788 3.1415927
inline const float HalfPi = X87::Bits(0x3FC90FDBu);          // flt_784254 1.5707964
inline const float RadToDeg = X87::Bits(0x42652EE0u);        // flt_78424C 57.295776
inline const float Twelve = X87::Bits(0x41400000u);          // flt_78821C 12.0
inline const float Eps1e3 = X87::Bits(0x3A83126Fu);          // flt_78435C 0.001
inline const float Eps1e4 = X87::Bits(0x38D1B717u);          // flt_7843EC 0.0001
inline const float RollEps = X87::Bits(0x3727C5ACu);         // flt_7845A0 1e-5
inline const float MinusRollEps = X87::Bits(0xB727C5ACu);    // flt_7A1E98 -1e-5
inline const float AngleWrap = X87::Bits(0x441D145Au);       // flt_7A1EA8 628.318
inline const float AngleReset = X87::Bits(0x45C45971u);      // flt_7A1EA4 6283.18
inline const float MinusAngleWrap = X87::Bits(0xC41D145Au);  // flt_7A1EA0 -628.318
inline const float MinusAngleReset = X87::Bits(0xC5C45971u); // flt_7A1E9C -6283.18
inline const float RollingCoef = X87::Bits(0x3B03126Fu);     // flt_7A1B20 0.002
inline const float FlatDragCoef = X87::Bits(0x3DCCCCCDu);    // flt_7A1B24 0.1
inline const float FlatWobble = X87::Bits(0x3DB020C5u);      // flt_7A1B38 0.086
inline const float PenaltyGain = X87::Bits(0x3E666666u);     // flt_7A1B30 0.225
inline const float NoElevation = X87::Bits(0xC61C4000u);     // flt_7A1CA4 -10000.0
inline const float NoGround = X87::Bits(0xC2C80000u);        // flt_7A1EAC -100.0
inline const float PilotSpeedMin = X87::Bits(0x415692B6u);   // flt_7A1EB0 13.410818 m/s (30 mph)
inline const float PilotSpeedScale = X87::Bits(0x3DE5119Du); // flt_7850FC 0.11185 (1/20 mph)
inline const float LoadTiltMul = X87::Bits(0x40800000u);     // flt_7844F8 4.0
inline const float LoadTiltSub = X87::Bits(0x40400000u);     // flt_787ADC 3.0
inline const float LoadTiltMin = X87::Bits(0x3E99999Au);     // flt_78758C 0.3
inline const float LatLongCoupling = X87::Bits(0x3E19999Au); // flt_7844F0 0.15
inline const float MpsToMph = X87::Bits(0x400F2B02u);        // flt_78426C 2.237
inline const float FwdSplit = X87::Bits(0x3E4CCCCDu);        // flt_784554 0.2 (IsDriveWheel)
inline const float RwdSplit = X87::Bits(0x3F4CCCCDu);        // flt_7850EC 0.8 (IsDriveWheel)

void Verify(const Speed2Image& exe);

// Float at a VA; addresses past the file-backed part of a section (.bss) read as 0, like the loaded image.
float F32OrZero(const Speed2Image& exe, uint32_t va);

// fcom st0, x ; fnstsw ax: condition bits for values held on the x87 stack (F24 doubles).
inline int Fcom(double a, double b) {
    if (a < b) return X87::C0;
    if (a > b) return 0;
    if (a == b) return X87::C3;
    return X87::C0 | X87::C2 | X87::C3;
}
} // namespace WheelK

} // namespace bb

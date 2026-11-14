#pragma once

// Port of NFSU2Unity Core/Physics/PhysicsEnvironment.cs (PhysicsEnvironment, FlatGround, MoverData).

#include "drive/numerics/BWareMath.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"
#include "drive/physics/World.h"
#include "drive/physics/engine/EngineTables.h"
#include "drive/physics/rigidbody/RigidBodyData.h"
#include "drive/physics/wheel/Graph.h"
#include "drive/physics/wheel/WheelEnvironment.h"

#include <cstdint>

namespace bb {

// Constants and tables of the PhysicsMover functions (Calibrate, Tune*, BottomOut, ResolveForces and its
// assists, UpdateCarState). Read from the exe at their VAs (.rdata constants and .data tweakables alike;
// .bss values are 0); the graphs built by static initialisers are rebuilt from the same data.
class MoverData {
public:
    // Calibrate / FixCarCenterOfGravity
    int UseNewSuspensionCalibration = 0;   // 0x7FC06C (.data 1)
    float Gravity = 0;                     // Gravity 0x7FBEF0 9.8
    float HydraulicCgMix = 0;              // flt_7842A4 0.33
    float Quarter = 0;                     // flt_784D28 0.25
    float Half = 0;                        // flt_784260 0.5
    // Move
    float MaximumTimeStep = 0;             // 0x7FC070 (.data 0.0044)
    // TuneBoostAndDampers / sub_599170
    float TerrainDrag[WheelTables::TerrainCount] = {};   // TerrainTable 0x800F10 + 0x98*t, +0
    float ModeOneGravity = 0;              // flt_7FC084 11.0
    float ModeOneMaxThrottle = 0;          // flt_7A1E4C 0.4 (also the catch-up engine factor)
    float RestThrottle = 0;                // flt_78758C 0.3
    float RestPitch = 0;                   // flt_7A1A1C 0.15
    int CatchupEnabled = 0;                // dword_7FC0A0 (.data 1)
    float DynoYawClamp = X87::Bits(0x283424DCu);   // immediate (1e-14)
    float OneMs = 0;                       // flt_7A1824 1.0
    float CounterSteerYawClamp = 0;        // dword_7A1A18 20.0
    float PointOne = 0;                    // _zero_point_one 0x784358
    float RwdSplit = 0;                    // flt_7850EC 0.8
    float MpsToMph = 0;                    // flt_78426C 2.237
    float RearSplitMph = 0;                // flt_787590 60.0
    float RearSplitLow = X87::Bits(0x3F266666u);   // immediate 0.65
    // TuneWheelParams / RearGripBoost / RecentHitGrip
    float PilotOffGrip = 0;                // flt_7FC060 0.85
    float EBrakeGripFall = 0;              // flt_7A1A08 20.0
    float EBrakeGripMin = 0;               // flt_7A1A0C 0.5
    float EBrakeGripRise = 0;              // flt_7A1A04 1.0
    float RainGrip = 0;                    // flt_7A19E4 0.975
    float LaunchThrottle = 0;              // flt_7850EC 0.8
    float LaunchMph = 0;                   // flt_784560 10.0
    float LaunchBrakeMul = 0;              // flt_7844F4 0.05
    float EBrakeBoostMin = 0;              // flt_784554 0.2
    float DriftSlip = 0;                   // flt_7A1A10 0.3
    float DriftMph = 0;                    // flt_7A1A14 80.0
    float HandbrakeMask[4] = {};           // flt_7FC074 {0,0,1,1}
    float NosFriction[4] = {};             // GameBrakerNosFrictionMultiplier 0x7A17A4 [level]
    float HitGripMaxSec = 0;               // flt_7A5650 4.0
    Graph RecentHitGrip;                   // unk_89CCE4 = Graph(0x802570, 4) (static init 0x77ED60)
    float RgbEBrake = 0;                   // flt_7FC064 0.5
    float RgbMaxSlipDeg = 0;               // flt_7FC068 20.0
    float DegToRad = 0;                    // flt_784244
    float RgbSpeedMin = 0, RgbValueMin = 0;// flt_86B318, flt_86B314 (.bss 0)
    float RgbSpeedMax = 0;                 // flt_7FC058 30.0
    float RgbValueMax = 0;                 // flt_7FC054 1.0
    float RgbMax = 0;                      // flt_7FC05C 0.35
    // BottomOut / MovementDousingAndRoofReset / EvaluateDampingSuitability
    float DousingDamping = 0;              // flt_7A1A5C 0.95
    float RoofResetSec = 0;                // flt_7A1A78 1.0
    float DampSpeed = 0, DampSpin = 0;     // flt_7A1A60 / flt_7A1A64 0.7
    float DampTilt = 0;                    // flt_7A1A68 0.15
    float RestSpeed = 0, RestSpin = 0;     // flt_7A1A70 / flt_7A1A74 0.25
    float TriggerBounceImpulse = 0;        // dword_7A1A00 25.0
    // ResolveForces assists
    float DynoGate = 0;                    // dword_7A1A20 1.5
    float JumpMinSpin = 0;                 // flt_7A1A50 0.5
    Graph JumpStabilization;               // 0x86B4A0 = Graph(0x7FC8F8, 3) (static init 0x77DF90)
    Graph ForceYaw;                        // unk_89CF0C = Graph(0x802590, 3) (static init 0x77ED80)
    float ForceYawSpeedMin = 0, ForceYawSpeedMax = 0, ForceYawGain = 0;   // dword_7A5668 1, dword_7A566C 30, flt_7A5664 50
    Graph DriftYawAi, DriftYaw;            // dword_86B570 / dword_86B3C8 (static inits 0x77DFB0.. 0x77E0F0)
    WheelTable DriftRearFriction;          // DriftRearFrictionTable 0x7FC910
    float AngToRad = 0;                    // flt_784240
    float Pi = 0;                          // flt_7A1788
    // ResolveHydraulicForces
    float HydHeightScale = 0;              // flt_7FC09C 0.2
    float HydMinHeight = 0;                // flt_7FC08C 0.2
    float HydRate = 0;                     // flt_7FC098 0.5
    float HydMinCompression = 0;           // flt_7FC090 -0.01
    float HydVelGain = 0;                  // flt_7FC088 0.3
    float HydBase = 0;                     // flt_7FC094 0.9
    // ResolveDynoForces
    float DynoSpring = 0;                  // dword_7A1A54 150
    float DynoDamper = 0;                  // flt_7A1A58 15
    double DynoMinSpeed = 0;               // dbl_7A20B0 0.03
    // UpdateCarState
    float SteerToDeg = 0;                  // flt_784248 360/65536
    float SteerVisRate = 0;                // flt_7A1A28 120 deg/s
    float Deg180 = 0;                      // flt_784510
    float Deg360 = 0;                      // WorldAnimFrameCount 0x78450C (360.0)
    float F65536 = 0;                      // flt_784238
    float RadToRpm = 0;                    // flt_7A1D28
    float TestRpmOff = 0;                  // flt_784258 100
    float RpmKeep = 0, RpmNew = 0;         // flt_7A20A8 0.45, flt_7A20A4 0.55
    float SmoothRPMDecel[8] = {};          // 0x7A1A2C
    float SpinDiv = 0;                     // flt_7A1A24 6.0
    float RpmToRad = 0;                    // flt_7A1D38
    // ctor
    float PerfectShiftRangeMultipliers[3] = {};   // 0x7A19E8 [difficulty]

    explicit MoverData(const Speed2Image& e);

private:
    static float F(const Speed2Image& e, uint32_t va) { return WheelK::F32OrZero(e, va); }
    static Graph GraphAt(const Speed2Image& e, uint32_t pts, int n);
    static Graph DriftGraph(float lastY);
    // Checks the immediates of the two static initialisers against the exe (C7 05 addr imm32).
    static void VerifyDriftInit(const Speed2Image& e);
};

// Everything the vehicle simulation reads from SPEED2.EXE (tweakables, tables, constants) plus the shared
// mutable game state (World) and the ground queries (Terrain). One instance per simulation; every component
// of every car keeps a pointer to it (do not move or copy it).
class PhysicsEnvironment {
public:
    BWareMath Math;
    WheelTweaks Tweaks;
    WheelTables Tables;
    EngineTables Engine;
    RigidBodyData Rb;
    MoverData Mover;
    bb::World World;
    IWheelWorld* Terrain = nullptr;   // not owned

    // math: optional already built tables (copied), else built from exe.
    explicit PhysicsEnvironment(const Speed2Image& exe, const BWareMath* math = nullptr);
    PhysicsEnvironment(const PhysicsEnvironment&) = delete;
    PhysicsEnvironment& operator=(const PhysicsEnvironment&) = delete;
};

// Flat ground mock (TopologyCoordinate::GetElevation / GetRoadNoise): plane z = Height, normal
// (0,0,1), one terrain type, no road noise. Used by the goldens and the demo.
class FlatGround : public IWheelWorld {
public:
    float Height = 0;
    int TerrainType = 1;

    float GetElevation(void* topology, const float* point, int& terrain, float* normal, bool& valid) override;
    float GetRoadNoise(const float* pointXY, int terrain) override;
};

} // namespace bb

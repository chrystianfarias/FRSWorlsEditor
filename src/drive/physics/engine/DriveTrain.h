#pragma once

// Port of NFSU2Unity Core/Physics/Engine/DriveTrain.cs.

#include "drive/physics/engine/EngineTables.h"

#include <cstdint>

namespace bb {

class CarState;
class Engine;
class Wheel;

// DriveTrainInfo (CarPhysicsInfo+0x1B0, 0x40 bytes).
struct DriveTrainInfo {
    float NeutralShiftFrac = 0;        // +0x00 shiftUp[N] = idle + (redline - idle) * x
    float Unk04 = 0;                   // +0x04 (no reader found)
    float FinalDriveFront = 0;         // +0x08 wheels 0,1
    float FinalDriveRear = 0;          // +0x0C wheels 2,3
    float RearTorqueSplit = 0;         // +0x10 0 FWD .. 1 RWD
    float ClutchStiffness = 0;         // +0x14 GuessClutchTorque / FinalizeClutchTorque gain
    int NumGears = 0;                  // +0x18 forward gears; valid Gear 2..NumGears+1
    float ClutchRate = 0;              // +0x1C
    float GearRatio[8] = {};           // +0x20 [Gear]: R, N, 1st..6th
};

struct EngineInfo;

// DriveTrain (vtbl 0x7A2508, "Drvtrn", DriveTrainSlotPool, 0xF4 bytes): clutch, gearbox and differentials.
// Single state float = clutch engagement. Port of SPEED2.EXE v1.2 with the x87 operation order.
// Gear enum: 0 = R, 1 = N, 2 = 1st... Clutch states: 0 disengaging, 1 disengaged, 2 engaging, 3 engaged.
class DriveTrain {
public:
    const EngineTables& K;

    float Clutch = 0;                       // +0x14 STATE 0..1
    const DriveTrainInfo* Info = nullptr;   // +0x18
    const float* MiscTunings = nullptr;     // +0x1C MiscCarTunings* (TheMiscCarTunings 0x7FC2A8 + 0x144*type)
    bb::Engine* Engine = nullptr;           // +0x20
    Wheel* Wheels[4] = {};                  // +0x24..+0x30 (PhysicsMover+0x38)
    bb::CarState* CarState = nullptr;       // +0x34
    int TargetGear = 0;                     // +0x38
    int Gear = 0;                           // +0x3C
    int GearAtRequest = 0;                  // +0x40
    int ClutchState = 0;                    // +0x44
    int ShiftPending = 0;                   // +0x48
    float DriveOmega = 0;                   // +0x4C GetDriveWheelSlippage() (drive wheels referred to the crank)
    float AutoTimer = 0;                    // +0x50
    int LastRequest = 0;                    // +0x54
    float LastRpmDiff = 0;                  // +0x58
    float ShiftLockTime = 0;                // +0x5C
    int ClutchMode = 0;                     // +0x60 3 = manual clutch pedal
    float BonusTime = 0;                    // +0x64
    float ShiftBonus = 0;                   // +0x68 RateShift result
    float RpmAtShift = 0;                   // +0x6C
    float ClutchTorque = 0;                 // +0x70 sum of ApplyClutchTorque this step
    bool ClutchPedal = false;               // +0x74 (byte)
    float ShiftModifier[3] = {};            // +0x78 up, down, up-from-2nd
    float ShiftUp[9] = {};                  // +0x84 [Gear]
    float ShiftDown[9] = {};                // +0xA8 [Gear]
    float ShiftModRange[6] = {};            // +0xCC (min, max) pairs
    float PerfectRangeMul = 0;              // +0xE4
    float ClutchRateMul = 0;                // +0xE8
    float RearSplit = 0;                    // +0xEC copy of Info.RearTorqueSplit
    int PerfType = 0;                       // +0xF0

    explicit DriveTrain(const EngineTables& tables) : K(tables) {}

    // DriveTrain::DriveTrain(DriveTrainInfo*, MiscCarTunings*, Engine*, Wheel**, CarState*) 0x5AF9D0.
    // Fields the ctor does not write (GearAtRequest, DriveOmega, AutoTimer, LastRequest, ClutchTorque) keep
    // their previous content. ChooseNewShiftModifiers runs before ShiftModRange is initialised, as in the game.
    // misc must stay alive while the drivetrain is used (MiscTunings points into it).
    void Construct(const DriveTrainInfo* info, const float* misc, bb::Engine* engine, Wheel* const* wheels,
                   bb::CarState* carState, bool shiftPointSearch, uint32_t& worldSeed, bool replayLocked);

    // CalculateShiftPoints(DriveTrainInfo*, EngineInfo*, float* up, float* down) 0x5AC7D0 (cdecl).
    // searchTorque = dword_86B33C (0 in the shipped game): search the crossing of the torque curves.
    // Writes up[1..NumGears+1] and down[3..NumGears+1].
    static void CalculateShiftPoints(const DriveTrainInfo* dti, const EngineInfo* ei, float* up, float* down, bool searchTorque);

    // DriveTrain::ChooseNewShiftModifiers 0x594710 (World::Random -> bRandom on World+0xF8).
    void ChooseNewShiftModifiers(uint32_t& worldSeed, bool replayLocked);

    // DriveTrain::GetDriveWheelSlippage 0x5945F0: drive wheel omega referred to the crankshaft.
    float GetDriveWheelSlippage() const;

    // DriveTrain::GetDifferentialAngularVelocity 0x5947C0: mean of Wheel+0x16C of the drive wheels (fidiv).
    float GetDifferentialAngularVelocity() const;

    // sub_594840 (GetDriveWheelAngularAccel, inferred).
    float GetDriveWheelAngularAccel() const;

    // DriveTrain::SetManualClutchEngaged(float) 0x594910.
    void SetManualClutchEngaged(float c);

    // DriveTrain::GetDerivative(float*) 0x5949B0: clutch engagement rate.
    float GetDerivative() const;

    // sub_594E90 (ShouldAutoDisengage, inferred): anti-stall.
    bool ShouldAutoDisengage() const;

    // sub_594ED0 (IsClutchSynced, inferred): |omega - driveOmega| <= 50.
    bool IsClutchSynced() const;

    // DriveTrain::GetPerfectShiftRange(Gear) 0x594F00.
    float GetPerfectShiftRange(int gear) const;

    // sub_594670 (GetShiftUpRPM(Gear, bool), inferred). isDrag = byte_89E7D8 || g_tweakIsDragRace.
    float GetShiftUpRpm(int gear, bool alt, bool isDrag) const;

    // sub_5946E0 (GetShiftDownRPM(Gear, bool), inferred).
    float GetShiftDownRpm(int gear, bool alt) const;

    // sub_594F20 (RateShift(float rpm, Gear, bool quantize), inferred): drag race shift quality.
    float RateShift(float rpm, int gear, bool quantize) const;

    // sub_5A1700 (RecordShiftQuality, inferred): only in drag races.
    void RecordShiftQuality(float rpm, bool isDrag);

    // DriveTrain::ApplyClutchTorque(float) 0x594A30: splits T through the differentials.
    void ApplyClutchTorque(float t);

    // DriveTrain::GuessClutchTorque 0x594CB0.
    void GuessClutchTorque();

    // DriveTrain::ModifyClutchTorque 0x594DA0 (only with the clutch engaged).
    void ModifyClutchTorque();

    // DriveTrain::FinalizeClutchTorque 0x5AA830.
    void FinalizeClutchTorque();

    // DriveTrain::SetDifferentialAngularVelocity(float) 0x5A12D0: writes Wheel+0x28 of the drive wheels.
    void SetDifferentialAngularVelocity(float w);

    // DriveTrain::ComputeAuxiliaries 0x5A1400: clutch state machine, gear engagement, throttle cut/bonus.
    // worldTimer = WorldTimer (4000 Hz ticks); worldSeed = World+0xF8 (World::Random); replayLocked =
    // pReplayManager->+0xA0 non-null with byte +0x41 set (ChooseNewShiftModifiers keeps zero modifiers).
    void ComputeAuxiliaries(int worldTimer, uint32_t& worldSeed, bool replayLocked);

    // sub_5AA960 (UpdateGearSelection(int request, float rpm), inferred), called by DelegateDriverInput.
    // request: -1 automatic (player), -2 AI, >= 0 manual gear. rpm <= -1 -> engine rpm. Returns the new request.
    int UpdateGearSelection(int request, float rpm, int worldTimer, float worldTimeElapsed, bool isDrag);

private:
    static float Now(int worldTimer);
    static bool RearDriven(float split);
    static bool FrontDriven(float split);
    static float WheelAccel(const Wheel* w);
    // Torque share of the second wheel of the axle (front: w1 / (w0 + w1); rear: w2 / (w3 + w2)).
    float AxleBias(const Wheel* first, const Wheel* second) const;
};

} // namespace bb

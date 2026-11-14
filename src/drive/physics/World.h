#pragma once

// Port of NFSU2Unity Core/Physics/World.cs.

#include "drive/numerics/X87.h"

#include <cstdint>

namespace bb {

// Race-mode globals read by the physics and the input chain (all .bss, 0 by default):
// byte_89E7D8 / g_tweakIsDragRace = drag race, byte_89E7D9 / g_tweakIsDriftRace / g_tweakDriftPhysics = drift,
// byte_89E7E2 / g_tweakIsBurnout / g_tweakBurnoutPhysics = burnout, byte_89E7E3 / g_tweakIsShortTrackRace.
class RaceModeFlags {
public:
    static constexpr uint32_t VA_DragByte = 0x89E7D8, VA_DriftByte = 0x89E7D9, VA_BurnoutByte = 0x89E7E2, VA_ShortTrackByte = 0x89E7E3;
    static constexpr uint32_t VA_TweakIsBurnout = 0x890100, VA_TweakIsDriftRace = 0x890104, VA_TweakIsDragRace = 0x890108;
    static constexpr uint32_t VA_TweakBurnoutPhysics = 0x890110, VA_TweakDriftPhysics = 0x890114;

    uint8_t DragByte = 0;              // byte_89E7D8
    uint8_t DriftByte = 0;             // byte_89E7D9
    uint8_t BurnoutByte = 0;           // byte_89E7E2
    uint8_t ShortTrackByte = 0;        // byte_89E7E3
    int TweakIsBurnout = 0;            // g_tweakIsBurnout 0x890100
    int TweakIsDriftRace = 0;          // g_tweakIsDriftRace 0x890104
    int TweakIsDragRace = 0;           // g_tweakIsDragRace 0x890108
    int TweakBurnoutPhysics = 0;       // g_tweakBurnoutPhysics 0x890110
    int TweakDriftPhysics = 0;         // g_tweakDriftPhysics 0x890114
    int TweakIsShortTrackRace = 0;     // g_tweakIsShortTrackRace

    // byte_89E7D8 || g_tweakIsDragRace.
    bool Drag() const { return DragByte != 0 || TweakIsDragRace != 0; }
    // byte_89E7E2 || g_tweakIsBurnout || g_tweakBurnoutPhysics.
    bool Burnout() const { return BurnoutByte != 0 || TweakIsBurnout != 0 || TweakBurnoutPhysics != 0; }
    // g_tweakDriftPhysics || byte_89E7D9 || g_tweakIsDriftRace.
    bool DriftPhysics() const { return TweakDriftPhysics != 0 || DriftByte != 0 || TweakIsDriftRace != 0; }
    // byte_89E7D9 || g_tweakIsDriftRace (post-collision steering damping only).
    bool DriftRace() const { return DriftByte != 0 || TweakIsDriftRace != 0; }
    // Selector used by the steering code (0x416BB0, 0x4060E0): drift checks are skipped in drag.
    bool SpecialSteer() const { return (!Drag() && DriftPhysics()) || Burnout(); }
    // Test order of Aerodynamics::ResolveForces / TuneWheelParams / ResolveForces: drag wins over drift.
    bool DriftAero() const { return !Drag() && DriftPhysics(); }
};

// The World object and the game globals the vehicle simulation reads (timers, RNG, mode flags, tweakables
// that are .bss/.data variables in the game). One instance is shared by every car of a simulation.
class World {
public:
    int WorldTimer = 0;                  // 0x86518C 4000 Hz ticks (advanced at the end of the frame)
    int RealTimer = 0;                   // 0x8651AC
    int WorldLoopCounter = 0;            // 0x7FB724 (stamped on collision contacts)
    float WorldTimeElapsed = 0.0f;       // 0x865154 dt of the current frame
    uint32_t Seed = 0;                   // World+0xF8 (World::Random)
    float UglyTimestepHack = 1.0f;       // 0x7FC800 (.data 1.0; the substep h inside PhysicsMover::Move, 0 outside)
    int NoWheels = 0;                    // 0x86B36C debug flag
    float TestRPM = X87::Bits(0x42C80000u);   // 0x7FBED8 (.data 100.0 = "use Car+0x440")
    int RaceState = 4;                   // pCurrentRace->state (4 = racing)
    int Difficulty = 0;                  // dword_89E7F4
    int TheRaceParameters = 0;           // TheRaceParameters (+0: race type; Move only skips skids in a range)
    int Field89E7B0 = 0;                 // dword_89E7B0 (sub_5EE250)
    int FEDatabase = 0;                  // FEDatabase 0x850078 (only compared with 1 by the ctor)
    int TriggerBounce = 0;               // 0x86B31C debug impulse
    int ShiftPointSearch = 0;            // dword_86B33C (CalculateShiftPoints torque search)
    bool Online = false;                 // sub_5F00F0(&dword_89CF48): nitrous uses RealTimer
    bool ReplayLocked = false;           // pReplayManager->+0xA0 != 0 && [+0x41] != 0
    float RainIntensity = 0.0f;          // GetDesiredRainIntensity(car x, y) (weather; mocked)
    RaceModeFlags Flags;

    // Timer used by the nitrous code: RealTimer online, WorldTimer offline.
    int NosNow() const { return Online ? RealTimer : WorldTimer; }

    // World::Random(float range) 0x5EA500 = bRandom(range, &World+0xF8).
    float Random(float range) { return BRandom(range, Seed); }

    // bRandom(float range, unsigned int* seed) 0x43C170.
    static float BRandom(float range, uint32_t& seed);
};

} // namespace bb

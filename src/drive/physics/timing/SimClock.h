#pragma once

// Port of NFSU2Unity Core/Physics/Timing/SimClock.cs.

#include "drive/numerics/X87.h"
#include "drive/physics/timing/TimingFpu.h"

#include <cstdint>
#include <functional>

namespace bb {

// Constants of the simulation clock, as bit patterns read from SPEED2.EXE (.rdata unless noted).
namespace TimingConstants {
inline const float K_0_02 = X87::Bits(0x3CA3D70Au);          // flt_7851C0 0.02 (CurrentVideoMode 0 = 50 Hz)
inline const float K_1_60 = X87::Bits(0x3C888889u);          // flt_7875BC 1/60
inline const float K_1800 = X87::Bits(0x44E10000u);          // flt_7A061C 1800.0
inline const float K_4000_1800 = X87::Bits(0x400E38E4u);     // flt_7A0AA8 2.2222223 (4000/1800)
inline const float K_1_1800 = X87::Bits(0x3A11A2B4u);        // flt_7A0618 1/1800
inline const float K_1_4000 = X87::Bits(0x3983126Fu);        // flt_784268 0.00025
inline const float K_Half = X87::Bits(0x3F000000u);          // flt_784260 0.5
inline const float K_60 = X87::Bits(0x42700000u);            // flt_787590 60.0
inline const float K_4000 = X87::Bits(0x457A0000u);          // flt_784264 4000.0
inline const float K_ReplayPaceMax = X87::Bits(0x3CF5C28Fu); // flt_788150 0.03
inline const float K_0_001 = X87::Bits(0x3A83126Fu);         // flt_78435C 0.001 (ms -> s)
inline const float K_1000 = X87::Bits(0x447A0000u);          // _one_thousand 0x784550
inline const float K_One = X87::Bits(0x3F800000u);           // float_1 0x784250
inline const float K_Zero = 0.0f;                            // _zero 0x7D7D3C
inline const float K_2p31 = X87::Bits(0x4F000000u);          // flt_786D84 2147483648.0
inline const float K_60000 = X87::Bits(0x476A6000u);         // flt_786D80 60000.0
inline const float GameSpeed = X87::Bits(0x3F800000u);       // GameSpeed 0x7A5730 = 1.0
inline const float FpsLimiterInGame = X87::Bits(0x41F00000u);// flt_7A572C 30.0 (World::World -> SetFPSLimiter)

// .data defaults (tweakable in the original; the port takes them as instance fields)
inline const float DefaultLimitMinimumVideoTimeElapsed = X87::Bits(0x3C088889u); // 0x7FB710 = 1/120
inline const float DefaultMaxTicksPerTimestep = X87::Bits(0x40400000u);          // 0x7FB714 = 3.0
inline const float DefaultMaximumTimeStep = X87::Bits(0x3B902DE0u);              // 0x7FC070 = 0.0044 (PhysicsMover substep)
constexpr int DefaultCurrentVideoMode = 2;                                        // 0x7FB700 (PC)
} // namespace TimingConstants

// The real/world clock globals of the original (all in .data/.bss). One instance = one game process.
// Field names are the game's symbol names; initial values are the exe's .data values.
class TimeGlobals {
public:
    // ---- frame limiter / tick conversion (sub_57EAD0)
    float LimitMinimumVideoTimeElapsed = TimingConstants::DefaultLimitMinimumVideoTimeElapsed; // 0x865558 (bss; = 1/120 after ResetFPSLimiter, 1/30 with a World)
    float MaxTicksPerTimestep = TimingConstants::DefaultMaxTicksPerTimestep;                     // 0x7FB714
    int CurrentVideoMode = TimingConstants::DefaultCurrentVideoMode;                              // 0x7FB700
    float TimestepRemainder = 0.0f;     // flt_865148: carry, in seconds, after removing whole 1/4000 s units
    int TimestepTicks = 0;              // dword_865140: this frame's step in 1/1800 s ticks
    float RealTimeElapsed = 0.0f;       // 0x865198 = TimestepTicks / 1800
    // ---- AdvanceRealTime (0x57A050)
    float RealTimeElapsedFrame = 0.0f;  // 0x865144
    int RealTimeFrames = 1;             // 0x7FB708
    int RealTimeFramesElapsed = 2;      // 0x7FB70C
    int gCardiogram = 1;                // 0x7FB704
    int RealTimer = 0;                  // 0x8651AC (4000 Hz)
    int RealLoopCounter = 0;            // 0x86514C
    // ---- AdvanceWorldTime (0x57A0E0)
    float WorldTimeElapsed = 0.0f;      // 0x865154 (set by MainLoop to the world step of the frame)
    float WorldTimeElapsedFrame = 0.0f; // 0x865158
    int WorldTimeFrames = 1;            // 0x7FB720
    int WorldTimeFramesElapsed = 0;     // 0x865150
    int WorldTime = 1;                  // 0x7FB71C
    int WorldTimer = 0;                 // 0x86518C (4000 Hz; read by the physics: clutch, nitrous, resets, contacts)
    float WorldTimeSeconds = 1.0f;      // 0x7FB718 = WorldTimer / 4000
    int WorldLoopCounter = 100000;      // 0x7FB724
    int NeedToPrepareWorldTimestep = 1; // 0x7FB728

    // SetFPSLimiter(float fps) 0x576CB0: Limit = 1.0 / fps (World::World passes 30.0).
    void SetFPSLimiter(float fps);
    // ResetFPSLimiter() 0x576CD0: Limit = DefaultLimitMinimumVideoTimeElapsed (0x7FB710, 1/120).
    void ResetFPSLimiter(float defaultLimit) { LimitMinimumVideoTimeElapsed = defaultLimit; }
    // ResetWorldTime() 0x576CF0 (World ctor/dtor).
    void ResetWorldTime();

    // sub_57EAD0 (suggested name PrepareRealTimestep), called by MainLoop with the measured duration of
    // the previous frame in seconds (float). Converts it to whole 1/1800 s ticks with the remainder
    // carried in 1/4000 s units. Returns the busy-wait the frame limiter performs (seconds, 0 if none);
    // the original spins on bGetTicker for that long. The arithmetic never depends on the actual spin.
    // Joylog replay (Joylog::ReplayingFlag): use PrepareRealTimestepReplay.
    // Reads: LimitMinimumVideoTimeElapsed, MaxTicksPerTimestep, CurrentVideoMode, TimestepRemainder.
    // Writes: TimestepRemainder, TimestepTicks, RealTimeElapsed. (Also a 100-entry debug history
    // unk_8647A8 indexed by RealLoopCounter % 100, and the joylog when capturing: not modelled.)
    float PrepareRealTimestep(float realSec);

    // sub_57EAD0 while Joylog::ReplayingFlag: ticks come from the joylog (channel 3); the remainder is untouched.
    void PrepareRealTimestepReplay(int joylogTicks);

    // AdvanceRealTime() 0x57A050 (end of every MainLoop iteration).
    void AdvanceRealTime();

    // AdvanceWorldTime() 0x57A0E0 (right after AdvanceRealTime).
    void AdvanceWorldTime();

    // sub_601A60(dt) (ecx = &dword_89CF48), offline path only: GameSpeed * dt. The online path
    // (byte [obj+0x21], dword_8669F4 > 0 or dword_866A14 > 0) re-synchronises WorldTimer with the
    // server clock and is out of scope.
    static float ApplyGameSpeed(float dt) { return TimingFpu::M(TimingConstants::GameSpeed, dt); }
};

// bGetTicker / bGetTickerDifference: the real-time source (QueryPerformanceCounter >> shift).
// Not part of the simulation (the measured duration is non-deterministic), ported so a host can feed
// the exact float the game would compute from raw counter values.
class Ticker {
public:
    int Shift = 0;            // dword_8284E4
    float MsPerTick = 0.0f;   // flt_828028

    // sub_43BD90 (ticker init): MsPerTick = 1000 / QueryPerformanceFrequency, then the smallest shift
    // with MsPerTick * 2^31 * 2^shift >= 60000 (so 2^31 ticks cover at least a minute).
    // The FPU precision in effect when the game calls it is not known (probably PC53, before
    // CreateDevice): pc53 = true models "fdivr" with 53-bit precision, false with 24-bit.
    static Ticker Init(int64_t qpcFrequency, bool pc53);

    // bGetTickerDifference(uint a, uint b) 0x43BE20, in milliseconds: d = b - a (signed; negative -> 0);
    // fild (1 << Shift) ; fimul d ; fmul MsPerTick. MainLoop multiplies by 0.001 and stores a float.
    float DifferenceMs(uint32_t a, uint32_t b) const;

    // MainLoop 0x5812DC: realSec = bGetTickerDifference(prev, now) * 0.001 (fstp float).
    float RealSeconds(uint32_t prev, uint32_t now) const { return TimingFpu::M(DifferenceMs(prev, now), TimingConstants::K_0_001); }

private:
    static float Div24(double a, int64_t b);
    static double TwoProdErr(double a, double b, double p);
};

// The time-related part of World (0x5FF490 ctor): pause timer, replay pacing, time scale (slow motion).
class WorldTimeState {
public:
    float PauseTime = 0.0f;        // World+0xFC: seconds of pause left (real time)
    int PauseFlag = 0;             // World+0x100
    float ReplayPace = 0.0f;       // World+0x104: replay pacing accumulator (seconds, clamped [0, 0.03])
    float TimeScale = 1.0f;        // World+0x108
    float TimeScaleTimer = 0.0f;   // World+0x10C: seconds of world time until TimeScale returns to 1
    std::function<void(int)> Callback;   // World+0x110 (void(*)(int)), shared by pause and time scale
    int CallbackArg = 0;           // World+0x114

    // sub_5EA280 (World::SetTimeScale, inferred name; called by Player::RequestZone).
    void SetTimeScale(float scale, float duration, std::function<void(int)> cb, int arg);

    // World::UpdateWorldPaused 0x5EA2B0 (MainLoop, in game, before IsWorldPaused).
    void UpdateWorldPaused(float realTimeElapsed);

    // World::IsWorldPaused 0x5EA320. TweakerStepWorld 0x890088, TweakerPauseWorld 0x890084.
    bool IsWorldPaused(bool tweakerStepWorld, bool tweakerPauseWorld) const;

    // World::GetTimestep 0x5EA360. ticks = dword_865140, or ReplayManager::GetData(8,1) when a replay plays
    // (and recorded with AddData(ticks,8,1) when recording). Returns dt * TimeScale (ST0; MainLoop stores
    // it as float before sub_601A60). replayPaceWait receives the busy wait the replay pacing performs.
    // replayPlaying = [ReplayManager+0], replayNoPace = [rm+0x18] || [rm+0x14].
    float GetTimestep(int ticks, float realTimeElapsed, bool replayPlaying, bool replayNoPace, float& replayPaceWait);

private:
    void FireCallback();
};

// PhysicsMover::Move 0x5B72D0 timestep split: n = __ftol2(dt / MaximumTimeStep) + 1 substeps of h = dt / n,
// and the averaging of the force/torque accumulators (mover+0x100/+0x110) by 1/n after the loop.
// UglyTimestepHack (0x7FC800) = h inside the loop.
namespace PhysicsSubsteps {
// 0x5B7344..0x5B736A. nf = (float)n (fild ; fstp var_8). The loop runs only when n > 0.
int Split(float dt, float maximumTimeStep, float& nf, float& h);
// 0x5B73D3..0x5B743F: acc[0..2] += lastForce (RB+0x130), acc[3..5] += lastTorque (RB+0x140).
void Accumulate(float* acc, const float* lastForce, const float* lastTorque);
// 0x5B745F..0x5B74CF: inv = 1.0 / nf ; acc[i] *= inv.
void Average(float* acc, float nf);
} // namespace PhysicsSubsteps

// One MainLoop iteration's timing outputs.
struct SimFrame {
    int Ticks = 0;                  // dword_865140
    float RealTimeElapsed = 0.0f;
    bool Paused = false;
    float Timestep = 0.0f;          // World::GetTimestep result (dt * TimeScale), 0 when paused
    float WorldTimeElapsed = 0.0f;  // sub_601A60 result = the dt passed to World::DoTimestep and every PhysicsMover::Move
    int Substeps = 0;               // PhysicsMover::Move n (0 when no DoTimestep ran)
    float SubstepCount = 0.0f;      // (float)n
    float SubstepDt = 0.0f;         // h = UglyTimestepHack inside the loop
    float BusyWait = 0.0f;          // frame limiter spin (seconds)
    float ReplayPaceWait = 0.0f;    // replay pacing spin (seconds)
    int WorldTimerAtStep = 0;       // WorldTimer seen by the physics during this frame (advanced at the end)
};

// Deterministic replica of the timing part of MainLoop 0x5811C0 (in game, offline, gameflow state 6):
// sub_57EAD0 -> World::UpdateWorldPaused -> World::IsWorldPaused -> World::GetTimestep -> sub_601A60 ->
// World::DoTimestep (PhysicsMover::Move split) -> AdvanceRealTime -> AdvanceWorldTime.
// Feed it the measured float duration of the previous frame (what MainLoop computes with
// bGetTickerDifference * 0.001); it returns the tick / dt / substep sequence of the game.
class SimClock {
public:
    TimeGlobals Globals;
    WorldTimeState World;
    float MaximumTimeStep = TimingConstants::DefaultMaximumTimeStep; // 0x7FC070 (.data)
    bool TweakerStepWorld = false, TweakerPauseWorld = false;         // 0x890088, 0x890084
    int RaceCoordinatorState = 0;                                      // *pRaceCoordinator: 1 = no world step
    bool ReplayFrozen = false;                                         // [ReplayManager+0x1C]

    // World::World: ResetWorldTime(), SetFPSLimiter(30.0), World fields +0xFC..+0x114 zeroed, +0x108 = 1.
    void BeginWorld();

    SimFrame Frame(float realSec);
};

} // namespace bb

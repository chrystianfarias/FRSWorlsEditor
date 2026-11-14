#pragma once

// Port of NFSU2Unity Core/Audio/Engine/EngineAudioController.cs.

#include "drive/audio/engine/EngineAudioMath.h"
#include "drive/audio/engine/EngineSoundData.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bb {

// Car state handed to the engine-audio controller once per audio tick.
struct EngineAudioInput {
    // Smoothed engine rpm (CarState+0x400 = Car+0x440).
    float Rpm = 0;
    // Driver throttle 0..1 (CarDriver+0x20C).
    float Throttle = 0;
    // Gear as the physics stores it: 0 R, 1 N, 2 = 1st ...
    int Gear = 0;
    // |v| in m/s (Car+0x430).
    float Speed = 0;
    // Car::IsShifting (physics gear change in progress).
    bool PhysicsShifting = false;
    // 2D listener distance in metres (3DCarPos out2), drives the distance low-pass.
    float ListenerDistance = 0;
    // Optimal up-shift rpm of the current gear (physics units; AccelTrans "stab" test). 0 = unknown.
    float OptimalShiftUpRpm = 0;
};

// Feature switches. Defaults = game behaviour; ReferenceModel() = audio_engine_ref.py --sim.
struct EngineAudioOptions {
    // false (game, CreateSFXCtrls order): Engine and HybridMotor run before Shifting/AccelTrans/Physics and use
    // the previous tick's telemetry. true: Physics first (no lag), as the Python reference model does.
    bool TelemetryFirst = false;
    bool Shifting = true;
    bool AccelTrans = true;
    bool RandomBump = true;
    // Feed the rpm the GIN player actually reproduces back into CAR d01 (game).
    bool HeardRpmFeedback = true;
    // Human player (redline limiter, RedlineEnable).
    bool Human = true;
    // Race types 6/7 widen the distance low-pass (K = 90 instead of 22).
    bool RaceType67 = false;
    // DMix Q15 output 0 of the player-engine sound (.mxb mixer, not ported): full scale.
    int MixQ15 = 0x7FFF;
    uint32_t RandomSeed = 1;
    // Reproduce audio_engine_ref.py's UpdateDualMixEng, which keeps the DCL-window weights local. The asm
    // (0x470824/0x470830) writes them back into the smoothed weights; false (default) follows the asm.
    bool ReferenceDualMix = false;

    static EngineAudioOptions ReferenceModel()
    {
        EngineAudioOptions o;
        o.TelemetryFirst = true;
        o.Shifting = false;
        o.AccelTrans = false;
        o.RandomBump = false;
        o.HeardRpmFeedback = false;
        o.ReferenceDualMix = true;
        return o;
    }
};

// The GIN side the controller talks to (Grain_SetTarget / heard rpm), see GinVoice.
class IGinTarget {
public:
    virtual ~IGinTarget() = default;
    // GIN header minimum rpm (stored at PlayerEngine+0xDC by InitializeEngine).
    virtual float MinRpm() const = 0;
    // Grain_SetTarget(player, rpm, 60.0) 0x467230.
    virtual void SetTargetRpm(double rpm) = 0;
    // rate / grainLen(tgtGrain + 0.5) * 120 of the last rendered block.
    virtual double HeardRpm() = 0;
};

// Per-tick result: the Csis CAR struct plus the GIN voices' parameters and events.
struct EngineAudioFrame {
    // CAR class data d00..d25 (see audio-engine.md 2.6).
    int D[26] = {};
    // SND voice volume 0..127 of the accel / decel GIN streams.
    int GinVol127 = 0, DclVol127 = 0;
    // SND voice pitch of the GIN streams, 4.12 (4096 = native rate).
    int GinPitch = 4096;
    // SND voice low-pass (Hz) of the GIN streams (HybridMotor +0xBC).
    int GinLpf = 24000;
    // rpm passed to Grain_SetTarget (clamped to the GIN minimum).
    double GinTargetRpm = 0;
    // Shift clunk request this tick: 0 none, 1 up, 2 down (+ volume, PlayShiftSnd 0x45E730).
    int ShiftClunk = 0;
    int ShiftClunkVol = 0;
};

// Player-car engine audio, one instance per car, ticked at a fixed 60 Hz. Transcription of
// SFXCTL_Physics 0x484C50 -> SFXCTL_Shifting 0x483130 -> SFXCTL_AccelTrans 0x4831A0 -> SFXCTL_Engine 0x47C010 ->
// SFXCTL_HybridMotor 0x47C3D0 -> CARSFX_PlayerEngine::SetEngineParams 0x474310 (docs/re/audio-engine.md 2.3/2.4,
// audio-notes/F1-engine.md). All slews/averages are per tick, as in the game.
class EngineAudioController {
public:
    static constexpr double TickSeconds = 1.0 / 60.0;

    // SFXCTL_Physics::UpdateParams 0x484C50 (in-game path, not the FE scripted revs).
    class Telemetry {
    public:
        const float Idle, Max;
        float Thr = 0, ThrRaw = 0, PrevThrRaw = 0, RpmAudio = 1000.0f;
        int Gear = 1, PrevGear = 1;
        bool ThrOn = false;
        float Mph = 0;

        Telemetry(float idle, float max) : Idle(idle), Max(max) {}
        void Update(const EngineAudioInput& i);
    };

    // SFXCTL_Shifting 0x483130 / state machine 0x482D50 (F1-engine.md 4). Transcribed from the static notes;
    // not covered by the Python reference model.
    class ShiftingCtl {
    public:
        int State = 0;
        AudioRamp Rpm, Torque, Vol, LfoCAmp, LfoAAmp, Target;
        float LfoAPeriod = 1, LfoCPeriod = 1;
        // Clunk request (1 up, 2 down), consumed by SetEngineParams.
        int ClunkEvent = 0;
        // Sweetener events (disengage at shift start, engage on 1 -> 2).
        bool StartEvent = false, EngageEvent = false;
        // Engine rpm when the last up-shift started (Shifting+0xA8; CARSFX_Shift engage-sweetener gate).
        float RpmAtShift = 0;

        explicit ShiftingCtl(EngineAudioController* c) : c_(c) {}
        void Update(double dt);

    private:
        float P(int k) const { return c_->Sel.Shift->P[k]; }
        void StartUp();
        void StartDown();
        void StartLfo(int gear);
        void Clear();

        EngineAudioController* c_;
        double eventTime_ = -1;
        int eventType_ = 0;
    };

    // SFXCTL_AccelTrans 0x4831A0 / 0x47C290 (F1-engine.md 5); not covered by the Python reference.
    class AccelTransCtl {
    public:
        int State = 0;
        AudioRamp Rpm, Ramp90, Torque;
        // Throttle lift at high rpm (flag for the bank).
        bool LiftEvent = false;

        explicit AccelTransCtl(EngineAudioController* c) : c_(c) {}
        void Update(double dt, const EngineAudioInput& input);

    private:
        EngineAudioController* c_;
        bool prevOn_ = false;
        double lastStab_ = -10;
        int msRelease_ = 1200, msDecay_ = 800;
    };

    // SFXCTL_Engine::UpdateParams 0x47C010.
    class EngineCtl {
    public:
        float Rpm = 1000.0f, RpmPrev = 1000.0f, Torque = 0, RpmReal = 0;
        int Vol = 0x7FFF;
        AudioAverage Avg3{3}, Avg2{2};
        bool Redlining = false, RedlineEnable = false;
        AudioRamp MainGain{1.0f}, RedGain{0.0f};
        int DistLpf = 24000;
        float LfoA = 0, LfoB = 0, LfoC = 0;
        // RandomBump (0x46F580)
        bool BumpRequest = false;
        float Env = 0;

        EngineCtl(EngineAudioController* c, MsvcRand* rnd) : c_(c), rnd_(rnd) {}
        void Update(double dt, const EngineAudioInput& input);

    private:
        void DistanceLpf(double distM);
        void UpdateLfo(double dt);
        static float Lfo(int& phase, double amp, double periodMs, double dt);
        void RandomBump(double dt);

        EngineAudioController* c_;
        MsvcRand* rnd_;
        bool bounceUp_ = true;
        float bounce_ = 0;
        // LFOs (0x45C7A0)
        int phaseA_ = 0x4097, phaseB_ = 0x4097, phaseC_ = 0x4097;
        AudioRamp bumpUp_, bumpDown_;
        int bumpPhase_ = 0, bumpMs_ = 0;
    };

    // SFXCTL_HybridMotor 0x47C3D0: AEMS x GIN weights, layer volumes, GIN rpm.
    class HybridMotorCtl {
    public:
        AudioAverage Avg4{4};
        float CAems = 0, CGin = 0, CLpf = 0;
        int VolAems = 0, VolGin = 0, VolDcl = 0, VolRed = 0;
        float GinRpm = 0, D = 0;

        HybridMotorCtl(EngineAudioController* c, MsvcRand* rnd);
        void Update(double dt, const EngineAudioInput& input);

    private:
        void SingleMix(bool shifting);
        void DualMix(bool shifting);

        EngineAudioController* c_;
        MsvcRand* rnd_;
        float prevAudio_ = 1000.0f;
        AudioGraph dclWindow_;
        double cruiseStart_ = 0;
        int bumpCountdown_ = 0;
    };

    const EngineSelection Sel;
    const EngineAudioOptions Opt;
    std::vector<int> CosTable;
    const float IdleRpm, MaxRpm;
    Telemetry Tel;
    ShiftingCtl Shift;
    AccelTransCtl Accel;
    EngineCtl Eng;
    HybridMotorCtl Hm;
    EngineAudioFrame Frame;
    IGinTarget* Gin = nullptr;
    IGinTarget* Dcl = nullptr;
    double Time = 0;
    long long Ticks = 0;

    // idleRpm = CarPhysicsInfo+0x1F0, maxRpm = CarPhysicsInfo+0x1F8. cosTable = 513 ints (null: synthetic).
    EngineAudioController(const EngineSelection& sel, float idleRpm, float maxRpm, const int* cosTable = nullptr,
                          const EngineAudioOptions& opt = EngineAudioOptions());
    EngineAudioController(const EngineAudioController&) = delete;
    EngineAudioController& operator=(const EngineAudioController&) = delete;

    // One EAXSound::Update (fixed 1/60 s). Returns the CAR struct and GIN voice parameters.
    const EngineAudioFrame& Tick(const EngineAudioInput& input, double dt = TickSeconds);

    // CARSFX_Shift::PlayShiftSnd 0x45E730 volume (mixer output taken as full scale).
    int ShiftClunkVolume(int baseVol) const;

private:
    // CARSFX_PlayerEngine::SetEngineParams 0x474310 (+ GinsuSingle 0x473E60 / GinsuDual 0x474030).
    void SetEngineParams();

    MsvcRand rnd_;
};

} // namespace bb

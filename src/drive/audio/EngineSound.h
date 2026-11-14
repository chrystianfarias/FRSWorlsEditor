#pragma once

// Player-car engine sound of the test drive: the C++ port of NFSU2Unity's engine audio (EngineAudioController,
// AEMS CAR bank tables, GIN granular player; bit-exact with the C# offline renderer) plus the CARSFX_Shift
// clunk / sweetener one-shots, played through a small XAudio2 stream (AudioOutput). Counterpart of the Unity
// glue Runtime/Audio/EngineSound.cs: the controller runs on a fixed 60 Hz audio clock driven by the real frame
// time, and is fed with the simulation state (CarState+0x400 rpm, CarDriver+0x20C throttle, gear, speed,
// DriveTrain::ShiftPending).
//
// Threading: Update/SimFrame/SetMuted/Start/Stop are called from the game (UI) thread; the mix runs on the
// AudioOutput feeder thread. Everything they share is guarded by one mutex.

#include "drive/audio/AudioOutput.h"
#include "drive/audio/engine/EngineAudioController.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bb {

class Speed2Image;
class CarTypeInfoU2;
class VehicleSim;
class AbkBank;
class EngineSoundData;
class EngineSoundAssets;
class EngineOfflineRenderer;
class CarSfxData;
struct PcmSound;

class EngineSound {
public:
    static constexpr int OutputRate = 44100;          // the game's PC output rate (InitializeSoundDriver 0x4859B0)
    static constexpr int BlockFrames = 441;           // 10 ms per XAudio2 buffer
    static constexpr int QueuedBlocks = 4;            // ~40 ms queued + the XAudio2 quantum
    static constexpr float SfxTrim = 0.35f;           // shift one-shots vs engine (DMix not ported; Unity CarSfxSound.volume)

    EngineSound();
    ~EngineSound();
    EngineSound(const EngineSound&) = delete;
    EngineSound& operator=(const EngineSound&) = delete;

    // Picks the car's engine like the game (GetCarTypeMapping 0x459240 + EAXCar::CarPreSetup 0x4593B0, stock
    // parts: exhaust / drivetrain / turbo level 0), loads SOUND\ENGINE\<bank>_EE.abk, the accel GIN and, for
    // dual engines, the decel GIN, plus the shift-clunk and sweetener banks, and opens the audio device.
    // idle / max rpm = CarPhysicsInfo+0x1F0 / +0x1F8 of the stock physics (CarTypeInfo+0x110).
    // Returns false with a message on failure (missing files, no audio device); nothing plays then.
    bool Start(const std::wstring& gameFolder, const Speed2Image& exe, const CarTypeInfoU2& car, std::wstring& error);

    // Same, with an explicit car type index, exhaust level (0 stock .. 3) and physics rpm range (tests, tools).
    bool StartWith(const std::wstring& gameFolder, const Speed2Image& exe, int carType, int exhaustLevel,
                   float idleRpm, float maxRpm, std::wstring& error, bool openDevice = true);

    // Every rendered frame, after the sim frames: reads the car state and advances the 60 Hz audio clock by
    // realSeconds (at most 8 ticks per call, as the Unity glue).
    void Update(const VehicleSim& sim, float realSeconds);

    // Optional, after each 30 Hz sim frame. The controller needs no per-sim-frame data (the game's EAXSound
    // reads the car once per audio tick), so this only refreshes the sampled car state; Update does the same.
    void SimFrame(const VehicleSim& sim);

    void SetMuted(bool muted);
    // Releases the device voices and the loaded banks. Safe to call twice.
    void Stop();

    // ---- extras (not needed by DriveMode)

    // Car state + clock advance without a VehicleSim (scripted tests).
    void UpdateInput(const EngineAudioInput& input, float realSeconds);
    // 2D camera-to-car distance in metres for the distance low-pass (3DCarPos; default 3 m like the Unity glue).
    void SetListenerDistance(float metres);
    // In-car camera (Player::GetPovType() == 2): full-level shift clunk instead of the exterior x0.48.
    void SetInternalView(bool internalView);
    // Master gain applied to the mix (default 0.8, the Unity EngineSound.masterVolume).
    void SetVolume(float gain);
    bool Started() const { return renderer_ != nullptr; }
    // Shift clunks / sweeteners started so far (tests, HUD).
    int OneShotsPlayed() const { return oneShotsPlayed_; }
    // Bank / GIN names picked for the car ("" before Start).
    std::string Description();
    // One-line state for HUDs (audio rpm, CAR d01/d03, layer volumes, limiter / shift state).
    std::string DebugText();
    // Renders frames mono samples of the current state into dst (what the device thread does; tests).
    void RenderMono(float* dst, int frames);

private:
    struct OneShot {
        const PcmSound* Sound = nullptr;
        double Pos = 0;
        float Gain = 0;
    };

    void Tick();                                       // one 60 Hz tick (lock held)
    void ShiftSfxUpdate();                             // CARSFX_Shift::UpdateParams 0x47C890 (lock held)
    void PlayShiftSnd();                               // 0x45E730
    void PlaySweetener(bool engage);                   // 0x4720D0 / 0x4722B0
    void Play(const PcmSound* s, int vol);
    void RenderDevice(float* interleaved, int frames, int channels);
    static EngineAudioInput InputFrom(const VehicleSim& sim);

    std::mutex mtx_;
    std::unique_ptr<EngineSoundData> data_;
    std::unique_ptr<CarSfxData> sfxData_;
    std::unique_ptr<EngineSoundAssets> assets_;
    std::unique_ptr<EngineOfflineRenderer> renderer_;
    std::unique_ptr<AbkBank> shiftBank_, sweetBank_;
    std::unique_ptr<PcmSound> clunk_[2];               // FX_SHIFTING_01 slot 0 (down) / 1 (up)
    std::unique_ptr<PcmSound> sweet_[2];               // CAR_SWTN slot 0 (disengage) / 1 (engage)
    bool sweetOwnEngine_ = false;
    std::vector<OneShot> oneShots_;
    int oneShotsPlayed_ = 0;
    std::vector<float> mono_;
    AudioOutput out_;

    EngineAudioInput input_;
    double accumulator_ = 0;
    float listenerDistance_ = 3.0f;
    bool internalView_ = false;
    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{0.8f};
    float curGain_ = 0;
};

} // namespace bb

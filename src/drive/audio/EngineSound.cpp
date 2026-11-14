#include "drive/audio/EngineSound.h"

#include "drive/audio/AbkBank.h"
#include "drive/audio/carsfx/CarSfxData.h"
#include "drive/audio/engine/AemsCarBank.h"
#include "drive/audio/engine/EngineSoundAssets.h"
#include "drive/audio/engine/EngineSoundData.h"
#include "drive/cars/CarTypeInfoU2.h"
#include "drive/numerics/Speed2Image.h"
#include "drive/physics/VehicleSim.h"
#include "drive/physics/engine/DriveTrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace bb {

using namespace EngineAudioMath;

namespace {

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::vector<uint8_t> ReadGameFile(const std::wstring& folder, const std::string& rel)
{
    std::wstring path = folder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += Widen(rel);
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + rel);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string AbkName(const std::string& n)
{
    if (n.size() >= 4) {
        std::string e = n.substr(n.size() - 4);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == ".abk") return n;
    }
    return n + ".abk";
}

// Decodes sample-table slot `slot` of the first player of the module exporting Csis class `cls`
// (the slot the FX_SHIFTING_01 / CAR_SWTN patches play for data d0 = type). Null when absent.
std::unique_ptr<PcmSound> SlotSound(const AbkBank& b, const char* cls, int slot)
{
    const AbkModule* mod = nullptr;
    for (const AbkInterface& it : b.Interfaces)
        if (it.Name == cls)
            for (const AbkModule& m : b.Modules)
                if (static_cast<uint32_t>(m.Offset) + 4 == it.ModuleOffset || static_cast<uint32_t>(m.Offset) == it.ModuleOffset)
                    mod = &m;
    if (mod == nullptr && !b.Modules.empty()) mod = &b.Modules[0];
    if (mod == nullptr || mod->PlayerSampleTables.empty() || b.Bnk == nullptr) return nullptr;
    auto t = b.SampleTableEntries.find(mod->PlayerSampleTables[0]);
    if (t == b.SampleTableEntries.end() || slot >= static_cast<int>(t->second.size())) return nullptr;
    const AbkSampleRef& e = t->second[static_cast<size_t>(slot)];
    if (e.Type != 0 || e.Index == 0 || e.Index >= b.Bnk->Sounds.size()) return nullptr;
    return b.Bnk->DecodeGame(static_cast<int>(e.Index));
}

} // namespace

EngineSound::EngineSound() = default;

EngineSound::~EngineSound() { Stop(); }

bool EngineSound::Start(const std::wstring& gameFolder, const Speed2Image& exe, const CarTypeInfoU2& car,
                        std::wstring& error)
{
    // Stock CarPhysicsInfo = CarTypeInfo+0x110; EngineInfo at +0x1F0: idle +0x1F0, max +0x1F8.
    float idle = 0, max = 0;
    const size_t off = 0x110 + 0x1F0;
    if (car.Raw.size() >= off + 12) {
        std::memcpy(&idle, car.Raw.data() + off, 4);
        std::memcpy(&max, car.Raw.data() + off + 8, 4);
    }
    if (!(max > idle)) {
        error = L"engine sound: the car has no engine rpm range";
        return false;
    }
    return StartWith(gameFolder, exe, car.Index, 0, idle, max, error, true);
}

bool EngineSound::StartWith(const std::wstring& gameFolder, const Speed2Image& exe, int carType, int exhaustLevel,
                            float idleRpm, float maxRpm, std::wstring& error, bool openDevice)
{
    Stop();
    try {
        std::lock_guard<std::mutex> lock(mtx_);
        data_ = std::make_unique<EngineSoundData>(exe);
        EngineSelection sel = data_->Select(carType, exhaustLevel);
        auto read = [&](const std::string& rel) { return ReadGameFile(gameFolder, rel); };
        assets_ = std::make_unique<EngineSoundAssets>(sel, read);
        renderer_ = std::make_unique<EngineOfflineRenderer>(*assets_, idleRpm, maxRpm, data_->CosTable,
                                                            EngineAudioOptions(), OutputRate);
        sfxData_ = std::make_unique<CarSfxData>(exe, data_->CosTable);

        // CARSFX_Shift banks (CarSfxBanks): clunk SOUND\SHIFTING\<pattern>, sweeteners SOUND\ENGINE\<SWTN bank>.
        // A missing bank only silences that effect, as in the C# port.
        try {
            if (sel.Shift != nullptr && !sel.Shift->Bank.empty()) {
                shiftBank_ = std::make_unique<AbkBank>(read("SOUND\\SHIFTING\\" + AbkName(sel.Shift->Bank)));
                clunk_[0] = SlotSound(*shiftBank_, "FX_SHIFTING_01", 0);
                clunk_[1] = SlotSound(*shiftBank_, "FX_SHIFTING_01", 1);
            }
        } catch (const std::exception&) {
            shiftBank_.reset();
        }
        try {
            int swi = sel.Engine->Index < CarSfxData::SweetenerCount ? sel.Engine->Index : 0;
            const SweetenerRow& sw = sfxData_->Sweeteners[swi];
            if (!sw.Bank.empty()) {
                sweetBank_ = std::make_unique<AbkBank>(read("SOUND\\ENGINE\\" + AbkName(sw.Bank)));
                sweet_[0] = SlotSound(*sweetBank_, "CAR_SWTN", 0);
                sweet_[1] = SlotSound(*sweetBank_, "CAR_SWTN", 1);
                // Every CAR_SWTN patch releases the instances whose engine id (d1) is not its own bank's.
                sweetOwnEngine_ = AemsCarBank::IndexFromName(sw.Bank) == ClampI(sel.Engine->AemsId, 0, 60);
            }
        } catch (const std::exception&) {
            sweetBank_.reset();
        }

        input_ = EngineAudioInput();
        input_.Rpm = idleRpm;
        input_.Gear = 1;
        input_.ListenerDistance = listenerDistance_;
        accumulator_ = 0;
        curGain_ = 0;
        oneShots_.clear();
    } catch (const std::exception& e) {
        error = L"engine sound: " + Widen(e.what());
        std::lock_guard<std::mutex> lock(mtx_);
        renderer_.reset();
        assets_.reset();
        return false;
    }
    if (openDevice &&
        !out_.Start(OutputRate, 2, BlockFrames, QueuedBlocks,
                    [this](float* buf, int frames, int ch) { RenderDevice(buf, frames, ch); }, error)) {
        Stop();
        return false;
    }
    return true;
}

void EngineSound::Stop()
{
    out_.Stop();
    std::lock_guard<std::mutex> lock(mtx_);
    oneShots_.clear();
    renderer_.reset();
    assets_.reset();
    for (auto& s : clunk_) s.reset();
    for (auto& s : sweet_) s.reset();
    shiftBank_.reset();
    sweetBank_.reset();
    sweetOwnEngine_ = false;
    sfxData_.reset();
    data_.reset();
}

EngineAudioInput EngineSound::InputFrom(const VehicleSim& sim)
{
    EngineAudioInput in;
    in.Rpm = sim.Rpm();                       // CarState+0x400
    in.Throttle = sim.Driver.Throttle;        // CarDriver+0x20C
    in.Gear = sim.Gear();                     // 0 R, 1 N, 2 = 1st
    in.Speed = sim.Speed();                   // |v| m/s
    in.PhysicsShifting = sim.Mover && sim.Mover->DriveTrain && sim.Mover->DriveTrain->ShiftPending != 0;
    return in;
}

void EngineSound::SimFrame(const VehicleSim& sim)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!renderer_) return;
    input_ = InputFrom(sim);
}

void EngineSound::Update(const VehicleSim& sim, float realSeconds)
{
    UpdateInput(InputFrom(sim), realSeconds);
}

void EngineSound::UpdateInput(const EngineAudioInput& input, float realSeconds)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!renderer_) return;
    input_ = input;
    // EngineSound.Step (Unity glue): fixed 60 Hz ticks, at most 8 per call.
    accumulator_ += realSeconds > 0 ? realSeconds : 0.0f;
    int guard = 0;
    while (accumulator_ >= EngineAudioController::TickSeconds && guard++ < 8) {
        accumulator_ -= EngineAudioController::TickSeconds;
        Tick();
    }
    if (guard >= 8) accumulator_ = 0;
}

void EngineSound::Tick()
{
    input_.ListenerDistance = listenerDistance_;
    renderer_->Control(input_);
    ShiftSfxUpdate();
}

void EngineSound::ShiftSfxUpdate()
{
    // CARSFX_Shift::UpdateParams 0x47C890, right after the engine controller's tick (CarSfx.Tick order).
    const EngineAudioController& c = *renderer_->Ctl;
    if (c.Frame.ShiftClunk != 0) PlayShiftSnd();
    if (c.Shift.StartEvent) PlaySweetener(false);
    if (c.Shift.EngageEvent) PlaySweetener(true);
}

void EngineSound::PlayShiftSnd()
{
    // CarSfx.ShiftSfx.PlayShiftSnd (0x45E730), DMix output 0x7FFF.
    if (!shiftBank_) return;
    const EngineAudioController& c = *renderer_->Ctl;
    int st = c.Shift.State;
    bool up = st == 1 || st == 2 || st == 3;
    const ShiftPatternRow& pat = *c.Sel.Shift;
    int baseVol = static_cast<int>(up ? pat.P[1] : pat.P[15]);
    double x = (static_cast<int>(c.Tel.RpmAudio) - 1500) / (8000.0 - 1500.0);
    x = x <= 0 ? 0 : x > 1 ? 1 : x;
    double y = (1.0 - 0.1) * x + 0.1;
    int idx = Ftol(y * -512.0 + 512.0);
    int v = (sfxData_->CosTable[static_cast<size_t>(ClampI(idx, 0, 512))] * baseVol) >> 15;
    v = (v * 0x7FFF) >> 15;                                        // GetQ15Output(up ? 0 : 1)
    int d1 = ClampI(v, 0, 0x7FFF);
    // FX_SHIFTING_01 patch (learned from the bank code by the C# AEMS VM): sample slot = type (0 down, 1 up),
    // unpitched, voice volume = d1 * MAP(inCar) / 32767 with MAP {15720 exterior, 32767 in-car}.
    int map = internalView_ ? 32767 : 15720;
    int vol = static_cast<int>(std::lround(d1 * static_cast<double>(map) / 32767.0));
    Play(clunk_[up ? 1 : 0].get(), vol);
}

void EngineSound::PlaySweetener(bool engage)
{
    // CarSfx.ShiftSfx.PlaySweetener: PlayDisengageSnd 0x4720D0 / PlayEngageSnd 0x4722B0 (gated at 7000 audio rpm).
    if (!sweetBank_) return;
    const EngineAudioController& c = *renderer_->Ctl;
    float gateRpm = engage ? c.Shift.RpmAtShift : c.Tel.RpmAudio;
    if (!(gateRpm >= 7000.0f)) return;
    // LinMap 0x7F095C {26000 -> 32767 over 4000..7000} of the gate rpm
    double t = (gateRpm - 4000.0) / (7000.0 - 4000.0);
    t = t <= 0 ? 0 : t > 1 ? 1 : t;
    int gain = Ftol(static_cast<float>(26000.0 + (32767.0 - 26000.0) * t));
    int si = c.Sel.Engine->Index < CarSfxData::SweetenerCount ? c.Sel.Engine->Index : 0;
    int v = ((gain * sfxData_->Sweeteners[si].Vol1) >> 15) * 0x7FFF >> 15;   // GetQ15Output(3 | 2)
    int d3 = ClampI(v, 0, 0x8013);
    // CAR_SWTN patch: slot = type (0 disengage, 1 engage), vol = d3, pitch 1.0; released by the banks of other
    // engines.
    if (!sweetOwnEngine_) return;
    Play(sweet_[engage ? 1 : 0].get(), d3);
}

void EngineSound::Play(const PcmSound* s, int vol)
{
    if (s == nullptr || s->Float.empty() || s->Channels != 1) return;
    OneShot o;
    o.Sound = s;
    o.Pos = 0;
    // AemsVoiceMixer level: volume (type 2, clamped to 0x7FFF) / 32767, int16 scale -> 1.0, times the SFX trim
    // (the .mxb DMix that attenuates the SFX in the game is not ported; 0.35 = CarSfxSound.volume in Unity).
    o.Gain = ClampI(vol, 0, 0x7FFF) / 32767.0f / 32768.0f * SfxTrim;
    oneShots_.push_back(o);
    oneShotsPlayed_++;
}

void EngineSound::RenderMono(float* dst, int frames)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!renderer_) {
        std::fill(dst, dst + frames, 0.0f);
        return;
    }
    renderer_->Mix(dst, frames);
    // One-shots (SND voices at their own rate, pitch 4096, linear interpolation, played to the end).
    for (OneShot& o : oneShots_) {
        const std::vector<float>& d = o.Sound->Float;
        int n = static_cast<int>(d.size());
        double step = o.Sound->SampleRate / static_cast<double>(OutputRate);
        for (int i = 0; i < frames; i++) {
            int i0 = static_cast<int>(o.Pos);
            if (i0 >= n) break;
            double f = o.Pos - i0;
            float a = d[static_cast<size_t>(i0)], b = i0 + 1 < n ? d[static_cast<size_t>(i0) + 1] : a;
            dst[i] += static_cast<float>(a + (b - a) * f) * o.Gain;
            o.Pos += step;
        }
    }
    oneShots_.erase(std::remove_if(oneShots_.begin(), oneShots_.end(),
                                   [](const OneShot& o) { return o.Pos >= static_cast<double>(o.Sound->Float.size()); }),
                    oneShots_.end());
}

void EngineSound::RenderDevice(float* interleaved, int frames, int channels)
{
    if (static_cast<int>(mono_.size()) < frames) mono_.resize(static_cast<size_t>(frames));
    RenderMono(mono_.data(), frames);
    float target = muted_.load() ? 0.0f : volume_.load();
    float g0 = curGain_, dg = (target - g0) / std::max(1, frames);
    for (int i = 0; i < frames; i++) {
        float s = mono_[static_cast<size_t>(i)] * (g0 + dg * i);
        s = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
        for (int c = 0; c < channels; c++) interleaved[static_cast<size_t>(i) * channels + c] = s;
    }
    curGain_ = target;
}

void EngineSound::SetMuted(bool muted) { muted_ = muted; }

void EngineSound::SetVolume(float gain) { volume_ = gain < 0 ? 0 : gain; }

void EngineSound::SetListenerDistance(float metres)
{
    std::lock_guard<std::mutex> lock(mtx_);
    listenerDistance_ = metres;
}

void EngineSound::SetInternalView(bool internalView)
{
    std::lock_guard<std::mutex> lock(mtx_);
    internalView_ = internalView;
}

std::string EngineSound::Description()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!assets_) return "";
    return assets_->Selection.ToString();
}

std::string EngineSound::DebugText()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!renderer_) return "";
    const EngineAudioController& c = *renderer_->Ctl;
    const EngineAudioFrame& f = c.Frame;
    char buf[256];
    std::snprintf(buf, sizeof buf, "audio rpm %.0f -> bank %d load %d AEMS %d GIN %d/%d%s%s", c.Tel.RpmAudio, f.D[1],
                  f.D[3], c.Hm.VolAems, f.GinVol127, f.DclVol127, c.Eng.Redlining ? " LIMITER" : "",
                  c.Shift.State != 0 ? (" shift " + std::to_string(c.Shift.State)).c_str() : "");
    return buf;
}

} // namespace bb

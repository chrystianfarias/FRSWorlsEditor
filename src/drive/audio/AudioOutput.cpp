#include "drive/audio/AudioOutput.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include <future>
#include <vector>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace bb {

namespace {

struct StartResult {
    bool ok = false;
    std::wstring error;
};

std::wstring HrMessage(const wchar_t* what, HRESULT hr)
{
    wchar_t buf[160];
    swprintf_s(buf, L"%s failed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
    return buf;
}

class VoiceCallback : public IXAudio2VoiceCallback {
public:
    explicit VoiceCallback(HANDLE wake) : wake_(wake) {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(wake_); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override { SetEvent(wake_); }

private:
    HANDLE wake_;
};

} // namespace

AudioOutput::~AudioOutput() { Stop(); }

bool AudioOutput::Start(int sampleRate, int channels, int blockFrames, int queuedBlocks, RenderCallback callback,
                        std::wstring& error)
{
    Stop();
    if (sampleRate <= 0 || channels <= 0 || blockFrames <= 0 || queuedBlocks <= 0 || !callback) {
        error = L"AudioOutput: bad parameters";
        return false;
    }
    callback_ = std::move(callback);
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wakeEvent_ == nullptr) {
        error = L"AudioOutput: CreateEvent failed";
        return false;
    }
    quit_ = false;
    std::promise<StartResult> started;
    std::future<StartResult> result = started.get_future();
    thread_ = std::thread(&AudioOutput::ThreadMain, this, sampleRate, channels, blockFrames, queuedBlocks,
                          static_cast<void*>(&started));
    StartResult r = result.get();
    if (!r.ok) {
        error = r.error;
        if (thread_.joinable()) thread_.join();
        CloseHandle(static_cast<HANDLE>(wakeEvent_));
        wakeEvent_ = nullptr;
        callback_ = nullptr;
        return false;
    }
    running_ = true;
    return true;
}

void AudioOutput::Stop()
{
    if (thread_.joinable()) {
        quit_ = true;
        if (wakeEvent_) SetEvent(static_cast<HANDLE>(wakeEvent_));
        thread_.join();
    }
    running_ = false;
    if (wakeEvent_) {
        CloseHandle(static_cast<HANDLE>(wakeEvent_));
        wakeEvent_ = nullptr;
    }
    callback_ = nullptr;
}

void AudioOutput::ThreadMain(int sampleRate, int channels, int blockFrames, int queuedBlocks, void* startSignal)
{
    auto* started = static_cast<std::promise<StartResult>*>(startSignal);
    StartResult res;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    VoiceCallback cb(static_cast<HANDLE>(wakeEvent_));

    HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) res.error = HrMessage(L"XAudio2Create", hr);
    if (SUCCEEDED(hr)) {
        hr = xa->CreateMasteringVoice(&master);
        if (FAILED(hr)) res.error = HrMessage(L"XAudio2 CreateMasteringVoice (no audio device?)", hr);
    }
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = static_cast<WORD>(channels);
        wf.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = static_cast<WORD>(channels * 4);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        hr = xa->CreateSourceVoice(&source, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &cb);
        if (FAILED(hr)) res.error = HrMessage(L"XAudio2 CreateSourceVoice", hr);
    }

    const int slots = queuedBlocks + 1;
    std::vector<float> ring(static_cast<size_t>(slots) * blockFrames * channels, 0.0f);
    int next = 0;
    auto submit = [&](bool silent) -> bool {
        float* buf = ring.data() + static_cast<size_t>(next) * blockFrames * channels;
        if (silent) std::fill(buf, buf + static_cast<size_t>(blockFrames) * channels, 0.0f);
        else callback_(buf, blockFrames, channels);
        XAUDIO2_BUFFER b = {};
        b.AudioBytes = static_cast<UINT32>(blockFrames * channels * sizeof(float));
        b.pAudioData = reinterpret_cast<const BYTE*>(buf);
        next = (next + 1) % slots;
        return SUCCEEDED(source->SubmitSourceBuffer(&b));
    };

    if (SUCCEEDED(hr)) {
        // Prime with silence so the first real blocks do not underrun, then start.
        for (int i = 0; i < queuedBlocks; i++) submit(true);
        hr = source->Start(0);
        if (FAILED(hr)) res.error = HrMessage(L"XAudio2 Start", hr);
    }
    res.ok = SUCCEEDED(hr);
    bool ok = res.ok;
    started->set_value(std::move(res));   // `started` is dead after this line

    while (ok && !quit_.load()) {
        XAUDIO2_VOICE_STATE st = {};
        source->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        int guard = 0;
        while (st.BuffersQueued < static_cast<UINT32>(queuedBlocks) && guard++ < slots && !quit_.load()) {
            if (!submit(false)) break;
            source->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        }
        WaitForSingleObject(static_cast<HANDLE>(wakeEvent_), 20);
    }

    if (source) {
        source->Stop(0);
        source->FlushSourceBuffers();
        source->DestroyVoice();
    }
    if (master) master->DestroyVoice();
    if (xa) xa->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

} // namespace bb

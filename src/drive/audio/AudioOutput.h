#pragma once

// Small streaming output on XAudio2 (Windows SDK xaudio2.lib / XAudio2_9.dll): one float source voice fed by a
// render callback on a private feeder thread. The thread owns the whole XAudio2 graph (COM is initialised
// there, multithreaded), keeps `queuedBlocks` buffers of `blockFrames` frames in flight and refills one each
// time the voice finishes a buffer, so the latency is about queuedBlocks * blockFrames / sampleRate plus the
// XAudio2 quantum (~10 ms).

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace bb {

class AudioOutput {
public:
    // Fills `frames` interleaved frames of `channels` floats (-1..1). Runs on the feeder thread.
    using RenderCallback = std::function<void(float* interleaved, int frames, int channels)>;

    AudioOutput() = default;
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Opens the default device and starts streaming. Returns false (with a message) when XAudio2 or the
    // device cannot be opened; the callback is then never called.
    bool Start(int sampleRate, int channels, int blockFrames, int queuedBlocks, RenderCallback callback,
               std::wstring& error);

    // Stops the voice, releases the XAudio2 objects and joins the feeder thread. Safe to call twice.
    void Stop();

    bool Running() const { return running_.load(); }

private:
    void ThreadMain(int sampleRate, int channels, int blockFrames, int queuedBlocks, void* startSignal);

    RenderCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    void* wakeEvent_ = nullptr;   // HANDLE, signalled by OnBufferEnd and by Stop
};

} // namespace bb

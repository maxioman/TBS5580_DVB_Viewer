#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif

namespace TBS {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool init(uint32_t sampleRate = 48000, uint32_t channels = 2);
    void shutdown();

    void setVolume(float volume); // 0.0 to 1.0
    float getVolume() const { return volume_.load(); }

    void setMute(bool mute);
    bool isMuted() const { return isMuted_.load(); }

    // Push 16-bit interleaved PCM samples
    void pushPcm(const int16_t* samples, size_t numSamples);
    void flush();

private:
#if defined(__APPLE__)
    static void audioQueueCallback(void* userData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);
    void fillBuffer(AudioQueueBufferRef buffer);

    AudioQueueRef audioQueue_ = nullptr;
    static constexpr int NUM_BUFFERS = 3;
    static constexpr int BUFFER_SIZE_BYTES = 4096 * 4;
    AudioQueueBufferRef buffers_[NUM_BUFFERS] = {nullptr};
#elif defined(HAVE_ALSA)
    void alsaPlaybackLoop();
    snd_pcm_t* alsaPcm_ = nullptr;
    std::thread alsaThread_;
#endif

    std::atomic<float> volume_{0.8f};
    std::atomic<bool> isMuted_{false};
    std::atomic<bool> isRunning_{false};

    // PCM Ring Buffer
    static constexpr size_t PCM_RING_SIZE = 192000 * 2; // 2 seconds of stereo 48kHz audio
    std::vector<int16_t> pcmRing_;
    size_t ringReadHead_ = 0;
    size_t ringWriteHead_ = 0;
    size_t samplesAvailable_ = 0;
    mutable std::mutex ringMutex_;
};

} // namespace TBS

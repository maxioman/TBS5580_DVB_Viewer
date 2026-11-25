#include "AudioPlayer.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace TBS {

AudioPlayer::AudioPlayer() {
    pcmRing_.resize(PCM_RING_SIZE, 0);
}

AudioPlayer::~AudioPlayer() {
    shutdown();
}

#if defined(__APPLE__)

bool AudioPlayer::init(uint32_t sampleRate, uint32_t channels) {
    if (audioQueue_) {
        shutdown();
    }

    AudioStreamBasicDescription desc{};
    desc.mSampleRate = static_cast<Float64>(sampleRate);
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    desc.mBytesPerPacket = channels * sizeof(int16_t);
    desc.mFramesPerPacket = 1;
    desc.mBytesPerFrame = channels * sizeof(int16_t);
    desc.mChannelsPerFrame = channels;
    desc.mBitsPerChannel = 16;

    OSStatus status = AudioQueueNewOutput(
        &desc,
        audioQueueCallback,
        this,
        CFRunLoopGetCurrent(),
        kCFRunLoopCommonModes,
        0,
        &audioQueue_
    );

    if (status != noErr || !audioQueue_) {
        std::cerr << "[AudioPlayer] AudioQueueNewOutput failed with error " << status << std::endl;
        return false;
    }

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        status = AudioQueueAllocateBuffer(audioQueue_, BUFFER_SIZE_BYTES, &buffers_[i]);
        if (status != noErr) {
            std::cerr << "[AudioPlayer] AudioQueueAllocateBuffer failed with error " << status << std::endl;
            shutdown();
            return false;
        }
        std::memset(buffers_[i]->mAudioData, 0, BUFFER_SIZE_BYTES);
        buffers_[i]->mAudioDataByteSize = BUFFER_SIZE_BYTES;
        AudioQueueEnqueueBuffer(audioQueue_, buffers_[i], 0, nullptr);
    }

    setVolume(volume_.load());

    status = AudioQueueStart(audioQueue_, nullptr);
    if (status != noErr) {
        std::cerr << "[AudioPlayer] AudioQueueStart failed with error " << status << std::endl;
        shutdown();
        return false;
    }

    isRunning_ = true;
    std::cout << "[AudioPlayer] macOS AudioQueue started (" << sampleRate << " Hz, " << channels << " ch)" << std::endl;
    return true;
}

void AudioPlayer::shutdown() {
    isRunning_ = false;
    if (audioQueue_) {
        AudioQueueStop(audioQueue_, true);
        AudioQueueDispose(audioQueue_, true);
        audioQueue_ = nullptr;
    }
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        buffers_[i] = nullptr;
    }
}

void AudioPlayer::setVolume(float volume) {
    volume_ = std::max(0.0f, std::min(1.0f, volume));
    if (audioQueue_) {
        float effectiveVol = isMuted_.load() ? 0.0f : volume_.load();
        AudioQueueSetParameter(audioQueue_, kAudioQueueParam_Volume, effectiveVol);
    }
}

void AudioPlayer::audioQueueCallback(void* userData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    auto* self = static_cast<AudioPlayer*>(userData);
    if (!self || !self->isRunning_.load()) return;
    self->fillBuffer(inBuffer);
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
}

void AudioPlayer::fillBuffer(AudioQueueBufferRef buffer) {
    size_t maxSamples = buffer->mAudioDataBytesCapacity / sizeof(int16_t);
    int16_t* dest = static_cast<int16_t*>(buffer->mAudioData);

    std::lock_guard<std::mutex> lock(ringMutex_);
    size_t toCopy = std::min(maxSamples, samplesAvailable_);

    if (toCopy > 0) {
        size_t firstChunk = std::min(toCopy, PCM_RING_SIZE - ringReadHead_);
        std::memcpy(dest, &pcmRing_[ringReadHead_], firstChunk * sizeof(int16_t));
        if (toCopy > firstChunk) {
            std::memcpy(dest + firstChunk, &pcmRing_[0], (toCopy - firstChunk) * sizeof(int16_t));
            ringReadHead_ = toCopy - firstChunk;
        } else {
            ringReadHead_ = (ringReadHead_ + firstChunk) % PCM_RING_SIZE;
        }
        samplesAvailable_ -= toCopy;
    }

    if (toCopy < maxSamples) {
        std::memset(dest + toCopy, 0, (maxSamples - toCopy) * sizeof(int16_t));
    }

    buffer->mAudioDataByteSize = static_cast<UInt32>(maxSamples * sizeof(int16_t));
}

#elif defined(HAVE_ALSA)

bool AudioPlayer::init(uint32_t sampleRate, uint32_t channels) {
    if (alsaPcm_) shutdown();

    int err = snd_pcm_open(&alsaPcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "[AudioPlayer] Cannot open ALSA default audio device: " << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_hw_params_t* hwParams = nullptr;
    snd_pcm_hw_params_alloca(&hwParams);
    snd_pcm_hw_params_any(alsaPcm_, hwParams);
    snd_pcm_hw_params_set_access(alsaPcm_, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(alsaPcm_, hwParams, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(alsaPcm_, hwParams, channels);
    unsigned int rate = sampleRate;
    snd_pcm_hw_params_set_rate_near(alsaPcm_, hwParams, &rate, 0);

    snd_pcm_uframes_t periodSize = 1024;
    snd_pcm_hw_params_set_period_size_near(alsaPcm_, hwParams, &periodSize, 0);
    snd_pcm_hw_params(alsaPcm_, hwParams);

    isRunning_ = true;
    alsaThread_ = std::thread(&AudioPlayer::alsaPlaybackLoop, this);
    std::cout << "[AudioPlayer] Linux ALSA audio output started (" << sampleRate << " Hz, " << channels << " ch)" << std::endl;
    return true;
}

void AudioPlayer::shutdown() {
    isRunning_ = false;
    if (alsaThread_.joinable()) {
        alsaThread_.join();
    }
    if (alsaPcm_) {
        snd_pcm_close(alsaPcm_);
        alsaPcm_ = nullptr;
    }
}

void AudioPlayer::setVolume(float volume) {
    volume_ = std::max(0.0f, std::min(1.0f, volume));
}

void AudioPlayer::alsaPlaybackLoop() {
    int16_t buffer[2048];
    while (isRunning_.load()) {
        size_t samplesToRead = 0;
        {
            std::lock_guard<std::mutex> lock(ringMutex_);
            samplesToRead = std::min(samplesAvailable_, static_cast<size_t>(2048));
            if (samplesToRead > 0) {
                size_t firstChunk = std::min(samplesToRead, PCM_RING_SIZE - ringReadHead_);
                std::memcpy(buffer, &pcmRing_[ringReadHead_], firstChunk * sizeof(int16_t));
                if (samplesToRead > firstChunk) {
                    std::memcpy(buffer + firstChunk, &pcmRing_[0], (samplesToRead - firstChunk) * sizeof(int16_t));
                    ringReadHead_ = samplesToRead - firstChunk;
                } else {
                    ringReadHead_ = (ringReadHead_ + firstChunk) % PCM_RING_SIZE;
                }
                samplesAvailable_ -= samplesToRead;
            }
        }

        if (samplesToRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        float vol = isMuted_.load() ? 0.0f : volume_.load();
        if (vol < 0.99f) {
            for (size_t i = 0; i < samplesToRead; ++i) {
                buffer[i] = static_cast<int16_t>(buffer[i] * vol);
            }
        }

        snd_pcm_uframes_t frames = samplesToRead / 2;
        snd_pcm_sframes_t written = snd_pcm_writei(alsaPcm_, buffer, frames);
        if (written < 0) {
            snd_pcm_recover(alsaPcm_, written, 0);
        }
    }
}

#else // Fallback stub

bool AudioPlayer::init(uint32_t sampleRate, uint32_t channels) {
    std::cout << "[AudioPlayer] Running in dummy audio mode (audio device disabled)\n";
    isRunning_ = true;
    return true;
}

void AudioPlayer::shutdown() {
    isRunning_ = false;
}

void AudioPlayer::setVolume(float volume) {
    volume_ = std::max(0.0f, std::min(1.0f, volume));
}

#endif

void AudioPlayer::setMute(bool mute) {
    isMuted_ = mute;
    setVolume(volume_.load());
}

void AudioPlayer::flush() {
    std::lock_guard<std::mutex> lock(ringMutex_);
    ringReadHead_ = 0;
    ringWriteHead_ = 0;
    samplesAvailable_ = 0;
}

void AudioPlayer::pushPcm(const int16_t* samples, size_t numSamples) {
    if (!samples || numSamples == 0) return;

    std::lock_guard<std::mutex> lock(ringMutex_);
    if (samplesAvailable_ + numSamples > PCM_RING_SIZE) {
        size_t overflow = (samplesAvailable_ + numSamples) - PCM_RING_SIZE;
        ringReadHead_ = (ringReadHead_ + overflow) % PCM_RING_SIZE;
        samplesAvailable_ -= overflow;
    }

    size_t firstChunk = std::min(numSamples, PCM_RING_SIZE - ringWriteHead_);
    std::memcpy(&pcmRing_[ringWriteHead_], samples, firstChunk * sizeof(int16_t));
    if (numSamples > firstChunk) {
        std::memcpy(&pcmRing_[0], samples + firstChunk, (numSamples - firstChunk) * sizeof(int16_t));
        ringWriteHead_ = numSamples - firstChunk;
    } else {
        ringWriteHead_ = (ringWriteHead_ + firstChunk) % PCM_RING_SIZE;
    }
    samplesAvailable_ += numSamples;
}

} // namespace TBS

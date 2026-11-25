#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;
struct SwsContext;
struct SwrContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace TBS {

class TsParser;
class AudioPlayer;

class VideoDecoder {
public:
    VideoDecoder(TsParser* tsParser, AudioPlayer* audioPlayer);
    ~VideoDecoder();

    bool start();
    void stop();
    void restart();

    bool isRunning() const { return isRunning_.load(); }
    bool isHardwareAccelerated() const { return isHwAccelerated_.load(); }

    // Call on main/render thread to bind/update OpenGL texture
    // Returns OpenGL texture ID (or 0 if no frame ready yet)
    uint32_t getTexture(int* outWidth, int* outHeight);

    std::string getVideoCodecName() const;
    std::string getAudioCodecName() const;
    void getVideoResolution(int* width, int* height, double* fps) const;

    void setTargetProgram(uint16_t programId);
    uint16_t getTargetProgram() const { return targetProgramId_.load(); }

    void setEnableHardwareAccel(bool enable);
    bool isHardwareAccelEnabled() const { return enableHwAccel_.load(); }

    bool shouldStop() const { return shouldStop_.load(); }
    TsParser* getTsParser() const { return tsParser_; }

private:
    static int readPacketCallback(void* opaque, uint8_t* buf, int buf_size);
    static int interruptCallback(void* opaque);

    void decoderThreadLoop();
    void cleanupPipeline();
    bool setupPipeline();

    TsParser* tsParser_ = nullptr;
    AudioPlayer* audioPlayer_ = nullptr;

    std::atomic<bool> isRunning_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> isHwAccelerated_{false};
    std::atomic<bool> enableHwAccel_{false}; // Default software decode for 1080i broadcast TV
    std::atomic<uint16_t> targetProgramId_{0};
    std::thread decodeThread_;

    // FFmpeg state
    AVFormatContext* formatCtx_ = nullptr;
    AVIOContext* avioCtx_ = nullptr;
    uint8_t* avioBuffer_ = nullptr;
    static constexpr int AVIO_BUF_SIZE = 64 * 1024;

    int videoStreamIdx_ = -1;
    int audioStreamIdx_ = -1;
    AVCodecContext* videoCodecCtx_ = nullptr;
    AVCodecContext* audioCodecCtx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;

    SwsContext* swsCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    int swrInSampleRate_ = 0;
    int swrInFmt_ = -1;

    // Decoded video frame buffer (RGBA)
    std::vector<uint8_t> rgbaBuffer_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    double frameFps_ = 0.0;
    std::atomic<bool> newFrameReady_{false};
    mutable std::mutex frameMutex_;

    // OpenGL texture
    GLuint textureId_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;

    // Codec names
    mutable std::mutex codecNamesMutex_;
    std::string videoCodecName_ = "None";
    std::string audioCodecName_ = "None";
};

} // namespace TBS

#include "VideoDecoder.hpp"
#include "../ts/TsParser.hpp"
#include "../audio/AudioPlayer.hpp"

#include <iostream>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

namespace TBS {

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    (void)ctx;
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
#if defined(__APPLE__)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
#elif defined(__linux__)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
#endif
    }
    return AV_PIX_FMT_NONE;
}

VideoDecoder::VideoDecoder(TsParser* tsParser, AudioPlayer* audioPlayer)
    : tsParser_(tsParser), audioPlayer_(audioPlayer) {
}

void VideoDecoder::setTargetProgram(uint16_t programId) {
    targetProgramId_.store(programId);
}

void VideoDecoder::setEnableHardwareAccel(bool enable) {
    enableHwAccel_.store(enable);
}

VideoDecoder::~VideoDecoder() {
    stop();
    if (textureId_ != 0) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
}

int VideoDecoder::readPacketCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<VideoDecoder*>(opaque);
    if (!self || self->shouldStop() || self->restartRequested_.load()) {
        return AVERROR_EOF;
    }

    size_t n = self->getTsParser()->readSelectedTs(buf, buf_size);
    if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return AVERROR(EAGAIN);
    }
    return static_cast<int>(n);
}

int VideoDecoder::interruptCallback(void* opaque) {
    auto* self = static_cast<VideoDecoder*>(opaque);
    if (!self) return 1;
    return (self->shouldStop() || self->restartRequested_.load()) ? 1 : 0;
}

bool VideoDecoder::start() {
    if (isRunning_.load()) return true;

    shouldStop_ = false;
    restartRequested_ = false;
    isRunning_ = true;
    decodeThread_ = std::thread(&VideoDecoder::decoderThreadLoop, this);
    return true;
}

void VideoDecoder::stop() {
    shouldStop_ = true;
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }
    cleanupPipeline();
    isRunning_ = false;
}

void VideoDecoder::restart() {
    restartRequested_ = true;
}

void VideoDecoder::cleanupPipeline() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
    swrInSampleRate_ = 0;
    swrInFmt_ = -1;
    if (videoCodecCtx_) {
        avcodec_free_context(&videoCodecCtx_);
        videoCodecCtx_ = nullptr;
    }
    if (audioCodecCtx_) {
        avcodec_free_context(&audioCodecCtx_);
        audioCodecCtx_ = nullptr;
    }
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    if (avioCtx_) {
        if (avioCtx_->buffer) {
            av_freep(&avioCtx_->buffer);
        }
        avio_context_free(&avioCtx_);
        avioCtx_ = nullptr;
    }

    videoStreamIdx_ = -1;
    audioStreamIdx_ = -1;
    isHwAccelerated_ = false;

    {
        std::lock_guard<std::mutex> lock(codecNamesMutex_);
        videoCodecName_ = "None";
        audioCodecName_ = "None";
    }
}

bool VideoDecoder::setupPipeline() {
    cleanupPipeline();

    // Allocate AVIO buffer and context
    avioBuffer_ = static_cast<uint8_t*>(av_malloc(AVIO_BUF_SIZE));
    if (!avioBuffer_) return false;

    avioCtx_ = avio_alloc_context(
        avioBuffer_,
        AVIO_BUF_SIZE,
        0, // read-only
        this,
        readPacketCallback,
        nullptr,
        nullptr
    );
    if (!avioCtx_) {
        av_freep(&avioBuffer_);
        return false;
    }

    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) return false;

    formatCtx_->pb = avioCtx_;
    formatCtx_->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NONBLOCK;

    AVIOInterruptCB interruptCb;
    interruptCb.callback = interruptCallback;
    interruptCb.opaque = this;
    formatCtx_->interrupt_callback = interruptCb;

    // Adequate probe options for live broadcast TS (finds SPS/PPS and IDR frames across GOP)
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "5000000", 0);
    av_dict_set(&opts, "analyzeduration", "4000000", 0);
    av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);

    const AVInputFormat* tsFmt = av_find_input_format("mpegts");
    int ret = avformat_open_input(&formatCtx_, nullptr, tsFmt, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        return false;
    }

    ret = avformat_find_stream_info(formatCtx_, nullptr);
    if (ret < 0) {
        return false;
    }

    // Program selection
    uint16_t targetProg = targetProgramId_.load();
    videoStreamIdx_ = -1;
    audioStreamIdx_ = -1;
    const AVCodec* videoDecoder = nullptr;
    const AVCodec* audioDecoder = nullptr;

    if (targetProg != 0) {
        for (unsigned int p = 0; p < formatCtx_->nb_programs; ++p) {
            AVProgram* prog = formatCtx_->programs[p];
            if (prog && prog->id == targetProg) {
                std::cout << "[VideoDecoder] Selecting Program " << targetProg 
                          << " with " << prog->nb_stream_indexes << " elementary streams\n";
                for (unsigned int s = 0; s < prog->nb_stream_indexes; ++s) {
                    int sIdx = prog->stream_index[s];
                    if (sIdx >= 0 && sIdx < static_cast<int>(formatCtx_->nb_streams)) {
                        auto type = formatCtx_->streams[sIdx]->codecpar->codec_type;
                        if (type == AVMEDIA_TYPE_VIDEO && videoStreamIdx_ < 0) {
                            videoStreamIdx_ = sIdx;
                        } else if (type == AVMEDIA_TYPE_AUDIO && audioStreamIdx_ < 0) {
                            audioStreamIdx_ = sIdx;
                        }
                    }
                }
                break;
            }
        }
    }

    if (videoStreamIdx_ < 0) {
        videoStreamIdx_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &videoDecoder, 0);
    } else {
        videoDecoder = avcodec_find_decoder(formatCtx_->streams[videoStreamIdx_]->codecpar->codec_id);
    }

    if (audioStreamIdx_ < 0) {
        audioStreamIdx_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
    } else {
        audioDecoder = avcodec_find_decoder(formatCtx_->streams[audioStreamIdx_]->codecpar->codec_id);
    }

    if (videoStreamIdx_ >= 0 && videoDecoder) {
        videoCodecCtx_ = avcodec_alloc_context3(videoDecoder);
        if (videoCodecCtx_) {
            avcodec_parameters_to_context(videoCodecCtx_, formatCtx_->streams[videoStreamIdx_]->codecpar);
            videoCodecCtx_->thread_count = 0; // Auto threads

            // Hardware Acceleration Negotiation
            isHwAccelerated_ = false;
            if (enableHwAccel_.load()) {
                videoCodecCtx_->get_format = get_hw_format;
#if defined(__APPLE__)
                int hwRet = av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
                if (hwRet >= 0 && hwDeviceCtx_) {
                    videoCodecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
                    isHwAccelerated_ = true;
                    std::cout << "[VideoDecoder] Apple VideoToolbox HW acceleration enabled for " << videoDecoder->name << std::endl;
                }
#elif defined(__linux__)
                int hwRet = av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
                if (hwRet >= 0 && hwDeviceCtx_) {
                    videoCodecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
                    isHwAccelerated_ = true;
                    std::cout << "[VideoDecoder] VAAPI HW acceleration enabled for " << videoDecoder->name << std::endl;
                }
#endif
            }

            if (!isHwAccelerated_) {
                std::cout << "[VideoDecoder] Using FFmpeg software decoder (MBAFF/Interlaced broadcast ready) for " << videoDecoder->name << std::endl;
            }

            if (avcodec_open2(videoCodecCtx_, videoDecoder, nullptr) < 0) {
                avcodec_free_context(&videoCodecCtx_);
                videoCodecCtx_ = nullptr;
                videoStreamIdx_ = -1;
            } else {
                std::lock_guard<std::mutex> lock(codecNamesMutex_);
                videoCodecName_ = videoDecoder->name;
                if (isHwAccelerated_) videoCodecName_ += " (HW)";
                else videoCodecName_ += " (Software)";
            }
        }
    }

    if (audioStreamIdx_ >= 0 && audioDecoder) {
        audioCodecCtx_ = avcodec_alloc_context3(audioDecoder);
        if (audioCodecCtx_) {
            avcodec_parameters_to_context(audioCodecCtx_, formatCtx_->streams[audioStreamIdx_]->codecpar);

            if (avcodec_open2(audioCodecCtx_, audioDecoder, nullptr) < 0) {
                avcodec_free_context(&audioCodecCtx_);
                audioCodecCtx_ = nullptr;
                audioStreamIdx_ = -1;
            } else {
                std::lock_guard<std::mutex> lock(codecNamesMutex_);
                audioCodecName_ = audioDecoder->name;
            }
        }
    }

    return (videoCodecCtx_ != nullptr || audioCodecCtx_ != nullptr);
}

void VideoDecoder::decoderThreadLoop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* swFrame = av_frame_alloc();

    while (!shouldStop_.load()) {
        if (restartRequested_.load() || !formatCtx_) {
            restartRequested_ = false;
            // Wait for enough TS packets to arrive before probing (at least 256 KB)
            while (!shouldStop_.load() && !restartRequested_.load() && tsParser_->getRingBufferFill() < 256 * 1024) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (shouldStop_.load()) break;

            if (!setupPipeline()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        int ret = av_read_frame(formatCtx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (ret == AVERROR_EOF || restartRequested_.load() || shouldStop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // Transient error in live broadcast TS, continue
            av_packet_unref(pkt);
            continue;
        }

        // Process Video
        if (pkt->stream_index == videoStreamIdx_ && videoCodecCtx_) {
            if (avcodec_send_packet(videoCodecCtx_, pkt) >= 0) {
                while (avcodec_receive_frame(videoCodecCtx_, frame) >= 0) {
                    AVFrame* targetFrame = frame;

                    // If hardware surface frame, transfer to host memory
                    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX || frame->format == AV_PIX_FMT_VAAPI) {
                        if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                            targetFrame = swFrame;
                        }
                    }

                    int width = targetFrame->width;
                    int height = targetFrame->height;

                    if (width > 0 && height > 0) {
                        // Check or recreate swsCtx_
                        swsCtx_ = sws_getCachedContext(
                            swsCtx_,
                            width, height, static_cast<AVPixelFormat>(targetFrame->format),
                            width, height, AV_PIX_FMT_RGBA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                        );

                        if (swsCtx_) {
                            std::lock_guard<std::mutex> lock(frameMutex_);
                            size_t neededSize = static_cast<size_t>(width * height * 4);
                            if (rgbaBuffer_.size() != neededSize) {
                                rgbaBuffer_.resize(neededSize);
                            }

                            uint8_t* dstData[4] = { rgbaBuffer_.data(), nullptr, nullptr, nullptr };
                            int dstLinesize[4] = { width * 4, 0, 0, 0 };

                            sws_scale(swsCtx_, targetFrame->data, targetFrame->linesize,
                                      0, height, dstData, dstLinesize);

                            frameWidth_ = width;
                            frameHeight_ = height;

                            // Estimate frame rate
                            AVRational avgFr = formatCtx_->streams[videoStreamIdx_]->avg_frame_rate;
                            if (avgFr.den > 0 && avgFr.num > 0) {
                                frameFps_ = av_q2d(avgFr);
                            } else {
                                frameFps_ = 25.0; // Default PAL
                            }

                            newFrameReady_ = true;
                        }
                    }
                    av_frame_unref(swFrame);
                    av_frame_unref(frame);
                }
            }
        }
        // Process Audio
        else if (pkt->stream_index == audioStreamIdx_ && audioCodecCtx_ && audioPlayer_) {
            if (avcodec_send_packet(audioCodecCtx_, pkt) >= 0) {
                while (avcodec_receive_frame(audioCodecCtx_, frame) >= 0) {
                    if (frame->nb_samples > 0) {
                        int inRate = frame->sample_rate > 0 ? frame->sample_rate : (audioCodecCtx_->sample_rate > 0 ? audioCodecCtx_->sample_rate : 48000);
                        int inFmt = frame->format != AV_SAMPLE_FMT_NONE ? frame->format : (audioCodecCtx_->sample_fmt != AV_SAMPLE_FMT_NONE ? audioCodecCtx_->sample_fmt : AV_SAMPLE_FMT_FLTP);

                        if (!swrCtx_ || swrInSampleRate_ != inRate || swrInFmt_ != inFmt) {
                            if (swrCtx_) {
                                swr_free(&swrCtx_);
                                swrCtx_ = nullptr;
                            }
                            AVChannelLayout inLayout;
                            std::memset(&inLayout, 0, sizeof(inLayout));
                            if (frame->ch_layout.nb_channels > 0) {
                                av_channel_layout_copy(&inLayout, &frame->ch_layout);
                            } else if (audioCodecCtx_->ch_layout.nb_channels > 0) {
                                av_channel_layout_copy(&inLayout, &audioCodecCtx_->ch_layout);
                            } else {
                                av_channel_layout_default(&inLayout, 2);
                            }

                            AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                            int swrRet = swr_alloc_set_opts2(
                                &swrCtx_,
                                &outLayout,
                                AV_SAMPLE_FMT_S16,
                                48000,
                                &inLayout,
                                static_cast<AVSampleFormat>(inFmt),
                                inRate,
                                0,
                                nullptr
                            );
                            av_channel_layout_uninit(&inLayout);

                            if (swrRet == 0 && swrCtx_) {
                                if (swr_init(swrCtx_) == 0) {
                                    swrInSampleRate_ = inRate;
                                    swrInFmt_ = inFmt;
                                } else {
                                    swr_free(&swrCtx_);
                                    swrCtx_ = nullptr;
                                }
                            }
                        }

                        if (swrCtx_) {
                            int maxOutSamples = swr_get_out_samples(swrCtx_, frame->nb_samples);
                            std::vector<int16_t> pcmOut(maxOutSamples * 2);
                            uint8_t* outData[2] = { reinterpret_cast<uint8_t*>(pcmOut.data()), nullptr };

                            int converted = swr_convert(
                                swrCtx_,
                                outData,
                                maxOutSamples,
                                const_cast<const uint8_t**>(frame->data),
                                frame->nb_samples
                            );

                            if (converted > 0) {
                                audioPlayer_->pushPcm(pcmOut.data(), converted * 2);
                            }
                        }
                    }
                    av_frame_unref(frame);
                }
            }
        }

        av_packet_unref(pkt);
    }

    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    cleanupPipeline();
}

uint32_t VideoDecoder::getTexture(int* outWidth, int* outHeight) {
    if (textureId_ == 0) {
        glGenTextures(1, &textureId_);
        glBindTexture(GL_TEXTURE_2D, textureId_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (newFrameReady_.load()) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!rgbaBuffer_.empty() && frameWidth_ > 0 && frameHeight_ > 0) {
            glBindTexture(GL_TEXTURE_2D, textureId_);
            if (texWidth_ != frameWidth_ || texHeight_ != frameHeight_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameWidth_, frameHeight_, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBuffer_.data());
                texWidth_ = frameWidth_;
                texHeight_ = frameHeight_;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameWidth_, frameHeight_, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBuffer_.data());
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            newFrameReady_ = false;
        }
    }

    if (outWidth) *outWidth = texWidth_;
    if (outHeight) *outHeight = texHeight_;
    return textureId_;
}

std::string VideoDecoder::getVideoCodecName() const {
    std::lock_guard<std::mutex> lock(codecNamesMutex_);
    return videoCodecName_;
}

std::string VideoDecoder::getAudioCodecName() const {
    std::lock_guard<std::mutex> lock(codecNamesMutex_);
    return audioCodecName_;
}

void VideoDecoder::getVideoResolution(int* width, int* height, double* fps) const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (width) *width = frameWidth_;
    if (height) *height = frameHeight_;
    if (fps) *fps = frameFps_;
}

} // namespace TBS

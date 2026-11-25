#pragma once

#include <string>
#include <memory>
#include <vector>
#include "../net/TbsClient.hpp"
#include "../ts/TsParser.hpp"
#include "../video/VideoDecoder.hpp"
#include "../audio/AudioPlayer.hpp"

struct GLFWwindow;

namespace TBS {

struct ChannelPreset {
    const char* label;
    float freqMhz;
    int bwMhz;
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool init(int width = 1280, int height = 800, const char* title = "TBS-5580 Multi-Standard DVB Receiver");
    void run();
    void renderGui();
    void shutdown();

private:
    void renderConnectionBar();
    void renderTuningControls();
    void renderSpectrumPanel();
    void renderTelemetryPanel();
    void renderServiceList();
    void renderTvViewport();

    void triggerSpectrumScan(bool uhfOnly);
    void tuneToFrequency(float freqMhz, int bwMhz = 8);

    // GLFW & Window state
    GLFWwindow* window_ = nullptr;
    int winWidth_ = 1280;
    int winHeight_ = 800;

    // Subsystems
    std::unique_ptr<TbsClient> client_;
    std::unique_ptr<TsParser> tsParser_;
    std::unique_ptr<AudioPlayer> audioPlayer_;
    std::unique_ptr<VideoDecoder> videoDecoder_;

    // Connection settings
    char hostBuf_[64] = "192.168.1.27";
    int port_ = DEFAULT_PORT;

    // Terrestrial Tuning State
    int terrStandardIdx_ = 0; // 0: DVB-T, 1: DVB-T2, 2: ISDB-T
    float terrFreqMhz_ = 482.0f; // e.g. 482 MHz (UHF Ch 22)
    int terrBwMhz_ = 8; // 6, 7, 8
    int terrPlpId_ = -1;
    int selectedPresetIdx_ = 12; // Index in Channel Presets table

    // Satellite Tuning State
    int satStandardIdx_ = 1; // 0: DVB-S, 1: DVB-S2
    float satFreqMhz_ = 11766.0f;
    int satSymRateKs_ = 27500;
    int satPolIdx_ = 0; // 0: H (18V), 1: V (13V)
    bool satTone22k_ = false;
    int satDiseqcPort_ = 0; // 0: Port A, 1: Port B, 2: Port C, 3: Port D

    // Cable Tuning State
    float cableFreqMhz_ = 306.0f;
    int cableSymRateKs_ = 6900;
    int cableModIdx_ = 0; // 0: QAM64, 1: QAM256

    // Spectrum Scan Data
    std::vector<double> specFreqsMhz_;
    std::vector<double> specDbm_;
    std::vector<double> peakFreqsMhz_;
    std::vector<double> peakDbm_;
    std::string scanStatus_ = "Idle (Click 'Scan' to sweep band)";

    // Audio & Video controls
    float volume_ = 0.8f;
    bool isMuted_ = false;
    bool fitToWindow_ = true;

    // Telemetry history for plots
    static constexpr int HIST_LEN = 100;
    float snrHistory_[HIST_LEN] = {0};
    float sigHistory_[HIST_LEN] = {0};
    int histIdx_ = 0;
};

} // namespace TBS

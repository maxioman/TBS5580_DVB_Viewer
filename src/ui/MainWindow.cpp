#include "MainWindow.hpp"

#include <imgui.h>
#include <implot.h>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace TBS {

static const ChannelPreset CHANNEL_PRESETS[] = {
    // VHF Band I
    { "VHF Ch 2 (48.25 MHz)", 48.25f, 7 },
    { "VHF Ch 3 (55.25 MHz)", 55.25f, 7 },
    { "VHF Ch 4 (62.25 MHz)", 62.25f, 7 },
    // VHF Band III (DVB-T / DAB 7 MHz)
    { "VHF Ch 5 (177.5 MHz)", 177.5f, 7 },
    { "VHF Ch 6 (184.5 MHz)", 184.5f, 7 },
    { "VHF Ch 7 (191.5 MHz)", 191.5f, 7 },
    { "VHF Ch 8 (198.5 MHz)", 198.5f, 7 },
    { "VHF Ch 9 (205.5 MHz)", 205.5f, 7 },
    { "VHF Ch 10 (212.5 MHz)", 212.5f, 7 },
    { "VHF Ch 11 (219.5 MHz)", 219.5f, 7 },
    { "VHF Ch 12 (226.5 MHz)", 226.5f, 7 },
    // UHF Band IV/V (Ch 21 - 69, 8 MHz raster)
    { "UHF Ch 21 (474.0 MHz)", 474.0f, 8 },
    { "UHF Ch 22 (482.0 MHz)", 482.0f, 8 },
    { "UHF Ch 23 (490.0 MHz)", 490.0f, 8 },
    { "UHF Ch 24 (498.0 MHz)", 498.0f, 8 },
    { "UHF Ch 25 (506.0 MHz)", 506.0f, 8 },
    { "UHF Ch 26 (514.0 MHz)", 514.0f, 8 },
    { "UHF Ch 27 (522.0 MHz)", 522.0f, 8 },
    { "UHF Ch 28 (530.0 MHz)", 530.0f, 8 },
    { "UHF Ch 29 (538.0 MHz)", 538.0f, 8 },
    { "UHF Ch 30 (546.0 MHz)", 546.0f, 8 },
    { "UHF Ch 31 (554.0 MHz)", 554.0f, 8 },
    { "UHF Ch 32 (562.0 MHz)", 562.0f, 8 },
    { "UHF Ch 33 (570.0 MHz)", 570.0f, 8 },
    { "UHF Ch 34 (578.0 MHz)", 578.0f, 8 },
    { "UHF Ch 35 (586.0 MHz)", 586.0f, 8 },
    { "UHF Ch 36 (594.0 MHz)", 594.0f, 8 },
    { "UHF Ch 37 (602.0 MHz)", 602.0f, 8 },
    { "UHF Ch 38 (610.0 MHz)", 610.0f, 8 },
    { "UHF Ch 39 (618.0 MHz)", 618.0f, 8 },
    { "UHF Ch 40 (626.0 MHz)", 626.0f, 8 },
    { "UHF Ch 41 (634.0 MHz)", 634.0f, 8 },
    { "UHF Ch 42 (642.0 MHz)", 642.0f, 8 },
    { "UHF Ch 43 (650.0 MHz)", 650.0f, 8 },
    { "UHF Ch 44 (658.0 MHz)", 658.0f, 8 },
    { "UHF Ch 45 (666.0 MHz)", 666.0f, 8 },
    { "UHF Ch 46 (674.0 MHz)", 674.0f, 8 },
    { "UHF Ch 47 (682.0 MHz)", 682.0f, 8 },
    { "UHF Ch 48 (690.0 MHz)", 690.0f, 8 },
    { "UHF Ch 49 (698.0 MHz)", 698.0f, 8 },
    { "UHF Ch 50 (706.0 MHz)", 706.0f, 8 },
    { "UHF Ch 51 (714.0 MHz)", 714.0f, 8 },
    { "UHF Ch 52 (722.0 MHz)", 722.0f, 8 },
    { "UHF Ch 53 (730.0 MHz)", 730.0f, 8 },
    { "UHF Ch 54 (738.0 MHz)", 738.0f, 8 },
    { "UHF Ch 55 (746.0 MHz)", 746.0f, 8 },
    { "UHF Ch 56 (754.0 MHz)", 754.0f, 8 },
    { "UHF Ch 57 (762.0 MHz)", 762.0f, 8 },
    { "UHF Ch 58 (770.0 MHz)", 770.0f, 8 },
    { "UHF Ch 59 (778.0 MHz)", 778.0f, 8 },
    { "UHF Ch 60 (786.0 MHz)", 786.0f, 8 },
    { "UHF Ch 61 (794.0 MHz)", 794.0f, 8 },
    { "UHF Ch 62 (802.0 MHz)", 802.0f, 8 },
    { "UHF Ch 63 (810.0 MHz)", 810.0f, 8 },
    { "UHF Ch 64 (818.0 MHz)", 818.0f, 8 },
    { "UHF Ch 65 (826.0 MHz)", 826.0f, 8 },
    { "UHF Ch 66 (834.0 MHz)", 834.0f, 8 },
    { "UHF Ch 67 (842.0 MHz)", 842.0f, 8 },
    { "UHF Ch 68 (850.0 MHz)", 850.0f, 8 },
    { "UHF Ch 69 (858.0 MHz)", 858.0f, 8 }
};
static constexpr int NUM_PRESETS = sizeof(CHANNEL_PRESETS) / sizeof(CHANNEL_PRESETS[0]);

MainWindow::MainWindow() {
    client_ = std::make_unique<TbsClient>();
    tsParser_ = std::make_unique<TsParser>();
    audioPlayer_ = std::make_unique<AudioPlayer>();
    videoDecoder_ = std::make_unique<VideoDecoder>(tsParser_.get(), audioPlayer_.get());

    // Connect TS stream from network client to TS Parser
    client_->setTsCallback([this](const uint8_t* data, size_t bytes) {
        tsParser_->ingest(data, bytes);
    });

    // Spectrum scan response callback
    client_->setSpectrumCallback([this](const std::vector<SpectrumPoint>& pts) {
        specFreqsMhz_.clear();
        specDbm_.clear();
        peakFreqsMhz_.clear();
        peakDbm_.clear();

        for (const auto& pt : pts) {
            double fMhz = pt.frequencyHz / 1e6;
            double pDbm = pt.signalStrengthDbm;
            specFreqsMhz_.push_back(fMhz);
            specDbm_.push_back(pDbm);
        }

        // Detect peaks > -75 dBm with prominent prominence
        for (size_t i = 1; i + 1 < specDbm_.size(); ++i) {
            if (specDbm_[i] > -75.0 && specDbm_[i] > specDbm_[i - 1] && specDbm_[i] >= specDbm_[i + 1]) {
                peakFreqsMhz_.push_back(specFreqsMhz_[i]);
                peakDbm_.push_back(specDbm_[i]);
            }
        }
        scanStatus_ = "Found " + std::to_string(peakFreqsMhz_.size()) + " signal peak(s). Click any peak to tune!";
    });

    // Initialize audio queue
    audioPlayer_->init(48000, 2);
    audioPlayer_->setVolume(volume_);

    // Start video decoder background thread
    videoDecoder_->start();
}

MainWindow::~MainWindow() {
    shutdown();
}

void MainWindow::shutdown() {
    if (client_) {
        client_->disconnect();
    }
    if (videoDecoder_) {
        videoDecoder_->stop();
    }
    if (audioPlayer_) {
        audioPlayer_->shutdown();
    }
}

void MainWindow::tuneToFrequency(float freqMhz, int bwMhz) {
    terrFreqMhz_ = freqMhz;
    terrBwMhz_ = bwMhz;

    // Match preset index
    selectedPresetIdx_ = -1;
    for (int i = 0; i < NUM_PRESETS; ++i) {
        if (std::abs(CHANNEL_PRESETS[i].freqMhz - freqMhz) < 0.5f) {
            selectedPresetIdx_ = i;
            break;
        }
    }

    DeliverySystem sys = (terrStandardIdx_ == 0) ? SYS_DVB_T :
                         ((terrStandardIdx_ == 1) ? SYS_DVB_T2 : SYS_ISDB_T);
    uint32_t freqHz = static_cast<uint32_t>(std::round(terrFreqMhz_ * 1e6f));
    uint32_t bwHz = static_cast<uint32_t>(terrBwMhz_ * 1000000);
    tsParser_->reset();
    client_->tuneTerrestrial(sys, freqHz, bwHz, terrPlpId_);
    videoDecoder_->restart();
    audioPlayer_->flush();
}

void MainWindow::triggerSpectrumScan(bool uhfOnly) {
    if (!client_->isConnected()) return;

    DeliverySystem sys = (terrStandardIdx_ == 0) ? SYS_DVB_T :
                         ((terrStandardIdx_ == 1) ? SYS_DVB_T2 : SYS_ISDB_T);
    uint32_t startHz = uhfOnly ? 470000000 : 174000000;
    uint32_t stopHz = 862000000;
    uint32_t stepHz = 4000000; // 4 MHz step for fast high-resolution sweep

    scanStatus_ = "Scanning RF Band (" + std::to_string(startHz / 1000000) + " - " + std::to_string(stopHz / 1000000) + " MHz)...";
    client_->requestSpectrumScan(sys, startHz, stopHz, stepHz);
}

void MainWindow::renderGui() {
    // Collect telemetry for history plots
    RespTelemetry telem = client_->getLatestTelemetry();
    float snrDb = static_cast<float>(telem.snrTenthsDb) / 10.0f;
    float sigPct = static_cast<float>(telem.signalStrengthPct);

    snrHistory_[histIdx_] = snrDb;
    sigHistory_[histIdx_] = sigPct;
    histIdx_ = (histIdx_ + 1) % HIST_LEN;

    // Fullscreen dock/layout window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::Begin("TBS-5580 Master Console", nullptr, windowFlags);
    ImGui::PopStyleVar();

    // Top Header: Connection & Status Bar
    renderConnectionBar();
    ImGui::Separator();

    // 2-column layout: Left = Controls & Spectrum (460px); Right = TV Viewport
    float leftColWidth = 460.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    ImGui::BeginChild("ControlPanel", ImVec2(leftColWidth, contentSize.y), true);
    renderTuningControls();
    ImGui::Spacing();
    renderSpectrumPanel();
    ImGui::Spacing();
    renderTelemetryPanel();
    ImGui::Spacing();
    renderServiceList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("TvViewportContainer", ImVec2(0, contentSize.y), true);
    renderTvViewport();
    ImGui::EndChild();

    ImGui::End();
}

void MainWindow::renderConnectionBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "TBS-5580 DVB-S2/S/T2/T/C/ISDB-T Network Viewer");

    ImGui::SameLine(380);
    ImGui::Text("Bridge IP:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##Host", hostBuf_, sizeof(hostBuf_));

    ImGui::SameLine();
    ImGui::Text("Port:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65);
    ImGui::InputInt("##Port", &port_, 0, 0);

    ImGui::SameLine();
    bool connected = client_->isConnected();
    if (!connected) {
        if (ImGui::Button(" Connect ", ImVec2(80, 0))) {
            client_->connectToServer(hostBuf_, static_cast<uint16_t>(port_));
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Disconnect", ImVec2(80, 0))) {
            client_->disconnect();
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    if (!connected) {
        ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "[ OFFLINE ]");
    } else {
        RespTelemetry telem = client_->getLatestTelemetry();
        if (telem.lockFlags & FE_IS_SIMULATED_FLAG) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[ CONNECTED: SIMULATION MODE ]");
        } else {
            ImGui::TextColored(ImVec4(0.1f, 0.9f, 0.2f, 1.0f), "[ CONNECTED: TBS HARDWARE ]");
        }
    }
}

void MainWindow::renderTuningControls() {
    if (ImGui::CollapsingHeader("1. RF Tuning & Front-End Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTabBar("DeliverySystems")) {
            // TERRESTRIAL TAB
            if (ImGui::BeginTabItem("Terrestrial")) {
                const char* terrStandards[] = { "DVB-T", "DVB-T2", "ISDB-T" };
                ImGui::Combo("Standard", &terrStandardIdx_, terrStandards, IM_ARRAYSIZE(terrStandards));

                // Channel Preset Dropdown
                std::vector<const char*> presetLabels;
                presetLabels.reserve(NUM_PRESETS);
                for (int i = 0; i < NUM_PRESETS; ++i) {
                    presetLabels.push_back(CHANNEL_PRESETS[i].label);
                }

                int displayPresetIdx = selectedPresetIdx_;
                if (ImGui::Combo("Channel Preset (VHF/UHF)", &displayPresetIdx, presetLabels.data(), static_cast<int>(presetLabels.size()))) {
                    selectedPresetIdx_ = displayPresetIdx;
                    if (selectedPresetIdx_ >= 0 && selectedPresetIdx_ < NUM_PRESETS) {
                        terrFreqMhz_ = CHANNEL_PRESETS[selectedPresetIdx_].freqMhz;
                        terrBwMhz_ = CHANNEL_PRESETS[selectedPresetIdx_].bwMhz;
                    }
                }

                // Frequency input (MHz)
                if (ImGui::DragFloat("Frequency (MHz)", &terrFreqMhz_, 0.5f, 48.0f, 862.0f, "%.3f MHz")) {
                    selectedPresetIdx_ = -1;
                    for (int i = 0; i < NUM_PRESETS; ++i) {
                        if (std::abs(CHANNEL_PRESETS[i].freqMhz - terrFreqMhz_) < 0.2f) {
                            selectedPresetIdx_ = i;
                            break;
                        }
                    }
                }

                const char* bwList[] = { "6 MHz", "7 MHz", "8 MHz" };
                int bwIdx = (terrBwMhz_ == 6) ? 0 : ((terrBwMhz_ == 7) ? 1 : 2);
                if (ImGui::Combo("Bandwidth", &bwIdx, bwList, IM_ARRAYSIZE(bwList))) {
                    terrBwMhz_ = (bwIdx == 0) ? 6 : ((bwIdx == 1) ? 7 : 8);
                }

                if (terrStandardIdx_ == 1) { // DVB-T2 PLP
                    ImGui::InputInt("PLP ID (-1 Auto)", &terrPlpId_);
                }

                if (ImGui::Button("Tune & Lock Terrestrial", ImVec2(-1, 28))) {
                    tuneToFrequency(terrFreqMhz_, terrBwMhz_);
                }
                ImGui::EndTabItem();
            }

            // SATELLITE TAB
            if (ImGui::BeginTabItem("Satellite")) {
                const char* satStandards[] = { "DVB-S (QPSK)", "DVB-S2 (8PSK/16APSK)" };
                ImGui::Combo("Standard", &satStandardIdx_, satStandards, IM_ARRAYSIZE(satStandards));

                ImGui::DragFloat("Transponder (MHz)", &satFreqMhz_, 1.0f, 950.0f, 14500.0f, "%.1f MHz");
                ImGui::InputInt("Symbol Rate (kS/s)", &satSymRateKs_, 1000, 5000);

                const char* pols[] = { "Horizontal (18V)", "Vertical (13V)" };
                ImGui::Combo("Polarization", &satPolIdx_, pols, IM_ARRAYSIZE(pols));

                ImGui::Checkbox("22 kHz Tone (High Band)", &satTone22k_);

                const char* diseqcPorts[] = { "DiSEqC A (1)", "DiSEqC B (2)", "DiSEqC C (3)", "DiSEqC D (4)" };
                ImGui::Combo("DiSEqC Switch", &satDiseqcPort_, diseqcPorts, IM_ARRAYSIZE(diseqcPorts));

                if (ImGui::Button("Tune & Lock Satellite", ImVec2(-1, 28))) {
                    DeliverySystem sys = (satStandardIdx_ == 0) ? SYS_DVB_S : SYS_DVB_S2;
                    uint32_t freqKhz = static_cast<uint32_t>(std::round(satFreqMhz_ * 1000.0f));
                    uint32_t symRate = static_cast<uint32_t>(satSymRateKs_ * 1000);
                    tsParser_->reset();
                    client_->tuneSatellite(sys, freqKhz, symRate, static_cast<uint8_t>(satPolIdx_),
                                           satTone22k_ ? 1 : 0, static_cast<uint8_t>(satDiseqcPort_));
                    videoDecoder_->restart();
                    audioPlayer_->flush();
                }
                ImGui::EndTabItem();
            }

            // CABLE TAB
            if (ImGui::BeginTabItem("Cable (DVB-C)")) {
                ImGui::DragFloat("Frequency (MHz)", &cableFreqMhz_, 1.0f, 100.0f, 862.0f, "%.2f MHz");
                ImGui::InputInt("Symbol Rate (kS/s)", &cableSymRateKs_, 100, 1000);
                const char* mods[] = { "64-QAM", "256-QAM" };
                ImGui::Combo("Modulation", &cableModIdx_, mods, IM_ARRAYSIZE(mods));

                if (ImGui::Button("Tune & Lock Cable", ImVec2(-1, 28))) {
                    uint32_t freqHz = static_cast<uint32_t>(std::round(cableFreqMhz_ * 1e6f));
                    uint32_t symRate = static_cast<uint32_t>(cableSymRateKs_ * 1000);
                    tsParser_->reset();
                    client_->tuneCable(freqHz, symRate, static_cast<uint8_t>(cableModIdx_));
                    videoDecoder_->restart();
                    audioPlayer_->flush();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
}

void MainWindow::renderSpectrumPanel() {
    if (ImGui::CollapsingHeader("2. RF Spectrum & AGC Peak Scanner", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Sweep RF band to detect broadcast carriers & AGC peaks:");

        // Scan control buttons
        bool scanning = client_->isScanning();
        if (scanning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.1f, 1.0f));
            ImGui::Button("Scanning in Progress...", ImVec2(-1, 26));
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("Scan UHF Band (470-862 MHz)", ImVec2(220, 26))) {
                triggerSpectrumScan(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Full Scan (174-862 MHz)", ImVec2(-1, 26))) {
                triggerSpectrumScan(false);
            }
        }

        ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "%s", scanStatus_.c_str());

        // Quick Peak Buttons
        if (!peakFreqsMhz_.empty()) {
            ImGui::Text("Detected Carrier Peaks (Click to Tune):");
            for (size_t i = 0; i < peakFreqsMhz_.size(); ++i) {
                char btnLabel[64];
                std::snprintf(btnLabel, sizeof(btnLabel), "%.1f MHz (%.0f dBm)", peakFreqsMhz_[i], peakDbm_[i]);
                if (ImGui::SmallButton(btnLabel)) {
                    tuneToFrequency(static_cast<float>(peakFreqsMhz_[i]), 8);
                }
                if ((i + 1) % 3 != 0 && i + 1 < peakFreqsMhz_.size()) {
                    ImGui::SameLine();
                }
            }
        }

        // ImPlot RF Spectrum Graph
        if (!specFreqsMhz_.empty()) {
            if (ImPlot::BeginPlot("##RfSpectrumPlot", ImVec2(-1, 160), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
                ImPlot::SetupAxes("Frequency (MHz)", "Power (dBm)", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
                ImPlot::SetupAxesLimits(specFreqsMhz_.front(), specFreqsMhz_.back(), -100, -10, ImPlotCond_Always);

                // Live RF Spectrum Curve
                ImPlot::PlotLine("Spectrum", specFreqsMhz_.data(), specDbm_.data(), static_cast<int>(specFreqsMhz_.size()));

                // Highlight detected peaks
                if (!peakFreqsMhz_.empty()) {
                    ImPlot::PlotScatter("Peaks", peakFreqsMhz_.data(), peakDbm_.data(), static_cast<int>(peakFreqsMhz_.size()));
                }

                // Currently tuned frequency vertical line
                double curF = static_cast<double>(terrFreqMhz_);
                double vX[2] = { curF, curF };
                double vY[2] = { -100.0, -10.0 };
                ImPlot::PlotLine("Tuned", vX, vY, 2);

                // Interactive Click-to-Tune on Plot
                if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(0)) {
                    ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                    float clickedFreq = static_cast<float>(mousePos.x);

                    // Snap to nearest detected peak if within 6 MHz
                    float targetFreq = clickedFreq;
                    float minPeakDist = 999.0f;
                    for (double pf : peakFreqsMhz_) {
                        float dist = std::abs(static_cast<float>(pf) - clickedFreq);
                        if (dist < minPeakDist && dist < 6.0f) {
                            minPeakDist = dist;
                            targetFreq = static_cast<float>(pf);
                        }
                    }

                    tuneToFrequency(targetFreq, terrBwMhz_);
                }

                ImPlot::EndPlot();
            }
        }
    }
}

void MainWindow::renderTelemetryPanel() {
    if (ImGui::CollapsingHeader("3. Live RF Telemetry & Signal Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        RespTelemetry telem = client_->getLatestTelemetry();
        uint8_t flags = telem.lockFlags;

        if (flags & FE_IS_SIMULATED_FLAG) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.2f, 1.0f));
            ImGui::TextWrapped("[!] SERVER IN SIMULATION MODE (Dummy MPEG-TS):");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("TBS tuner not open. Tvheadend may be holding /dev/dvb/adapter0.\nRun 'sudo systemctl stop tvheadend' on 192.168.1.27, then restart tbs_bridge.");
            ImGui::Separator();
        }

        // Status LEDs
        auto renderLed = [](const char* name, bool on) {
            ImVec4 col = on ? ImVec4(0.0f, 1.0f, 0.3f, 1.0f) : ImVec4(0.3f, 0.3f, 0.35f, 1.0f);
            ImGui::TextColored(col, on ? "[%s]" : "[ %s ]", name);
        };

        ImGui::Text("Front-End Status:");
        ImGui::SameLine();
        renderLed("SIGNAL", (flags & FE_HAS_SIGNAL_FLAG) != 0);
        ImGui::SameLine();
        renderLed("CARRIER", (flags & FE_HAS_CARRIER_FLAG) != 0);
        ImGui::SameLine();
        renderLed("VITERBI", (flags & FE_HAS_VITERBI_FLAG) != 0);
        ImGui::SameLine();
        renderLed("SYNC", (flags & FE_HAS_SYNC_FLAG) != 0);
        ImGui::SameLine();
        renderLed("LOCK", (flags & FE_HAS_LOCK_FLAG) != 0);

        // Signal Quality Bars
        float sigPct = telem.signalStrengthPct / 100.0f;
        char sigOverlay[32];
        std::snprintf(sigOverlay, sizeof(sigOverlay), "%u%% (%d dBm)", telem.signalStrengthPct, telem.signalStrengthDbm);
        ImGui::Text("Signal Strength:");
        ImGui::ProgressBar(sigPct, ImVec2(-1, 16), sigOverlay);

        float snrDb = static_cast<float>(telem.snrTenthsDb) / 10.0f;
        float snrNorm = std::min(1.0f, snrDb / 35.0f); // 0 - 35 dB scale
        char snrOverlay[32];
        std::snprintf(snrOverlay, sizeof(snrOverlay), "%.1f dB", snrDb);
        ImGui::Text("SNR (MER):");
        ImGui::ProgressBar(snrNorm, ImVec2(-1, 16), snrOverlay);

        // Bitrates & Error counters
        float mbps = static_cast<float>(telem.tsBytesPerSec * 8) / 1000000.0f;
        ImGui::Columns(2, "TelemCols", false);
        ImGui::Text("TS Bitrate: %.2f Mbps", mbps);
        ImGui::Text("Total Packets: %llu", static_cast<unsigned long long>(tsParser_->getTotalPackets()));
        ImGui::NextColumn();
        ImGui::Text("CC Errors: %llu", static_cast<unsigned long long>(tsParser_->getTotalCcErrors()));
        ImGui::Text("UNC Blocks: %u", telem.uncorrectedBlocks);
        ImGui::Columns(1);

        // Real-time mini SNR plot
        if (ImPlot::BeginPlot("##SnrHistory", ImVec2(-1, 80), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_None);
            ImPlot::SetupAxesLimits(0, HIST_LEN, 0, 35, ImPlotCond_Always);
            ImPlot::PlotLine("SNR", snrHistory_, HIST_LEN);
            ImPlot::EndPlot();
        }
    }
}

void MainWindow::renderServiceList() {
    if (ImGui::CollapsingHeader("4. DVB Multiplex Service Explorer", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto services = tsParser_->getServices();
        uint16_t currentSel = tsParser_->getSelectedService();

        if (services.empty()) {
            ImGui::TextDisabled("No services discovered yet.\nWaiting for PAT / SDT tables from TS...");
            return;
        }

        // Auto-select first channel with video if none currently selected
        if (currentSel == 0 && !services.empty()) {
            for (const auto& s : services) {
                if (s.videoPid != 0) {
                    tsParser_->selectService(s.serviceId);
                    videoDecoder_->setTargetProgram(s.serviceId);
                    videoDecoder_->restart();
                    currentSel = s.serviceId;
                    break;
                }
            }
        }

        ImGui::Text("Discovered %zu Channel(s) in Multiplex:", services.size());
        ImGui::Separator();

        if (ImGui::BeginTable("ServicesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(-1, 180))) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Channel Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 65.0f);
            ImGui::TableHeadersRow();

            for (const auto& s : services) {
                ImGui::TableNextRow();
                bool isSelected = (currentSel == s.serviceId);

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", s.serviceId);

                ImGui::TableSetColumnIndex(1);
                if (isSelected) {
                    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "%s *", s.name.c_str());
                } else {
                    ImGui::Text("%s", s.name.c_str());
                }
                if (!s.provider.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", s.provider.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", TsParser::serviceTypeToString(s.serviceType).c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::PushID(s.serviceId);
                if (isSelected) {
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "Playing");
                } else {
                    if (ImGui::SmallButton("Watch")) {
                        tsParser_->selectService(s.serviceId);
                        videoDecoder_->setTargetProgram(s.serviceId);
                        videoDecoder_->restart();
                        audioPlayer_->flush();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Decoder Controls
        ImGui::Spacing();
        bool hwAccel = videoDecoder_->isHardwareAccelEnabled();
        if (ImGui::Checkbox("Apple VideoToolbox HW Acceleration", &hwAccel)) {
            videoDecoder_->setEnableHardwareAccel(hwAccel);
            videoDecoder_->restart();
        }
        if (!hwAccel) {
            ImGui::SameLine();
            ImGui::TextDisabled("(FFmpeg Software Decode: optimal for 1080i MBAFF)");
        }
    }
}

void MainWindow::renderTvViewport() {
    int texW = 0, texH = 0;
    double fps = 0.0;
    uint32_t texId = videoDecoder_->getTexture(&texW, &texH);
    videoDecoder_->getVideoResolution(nullptr, nullptr, &fps);

    // Audio / Video Toolbar at Top of Viewport
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Volume:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##Volume", &volume_, 0.0f, 1.0f, "%.0f%%")) {
        audioPlayer_->setVolume(volume_);
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &isMuted_)) {
        audioPlayer_->setMute(isMuted_);
    }

    ImGui::SameLine(320);
    std::string vCodec = videoDecoder_->getVideoCodecName();
    std::string aCodec = videoDecoder_->getAudioCodecName();

    if (videoDecoder_->isHardwareAccelerated()) {
#if defined(__APPLE__)
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "[Apple VideoToolbox HW Decode]");
#else
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "[Hardware Decode Active]");
#endif
        ImGui::SameLine();
    }
    ImGui::TextDisabled("| %dx%d @ %.1f fps | %s | %s", texW, texH, fps, vCodec.c_str(), aCodec.c_str());
    ImGui::EndGroup();

    ImGui::Separator();

    // Available viewport rendering size
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 100 || avail.y < 100) return;

    if (texId != 0 && texW > 0 && texH > 0) {
        // Calculate 16:9 or native aspect ratio display box
        float aspect = static_cast<float>(texW) / static_cast<float>(texH);
        float renderW = avail.x;
        float renderH = renderW / aspect;

        if (renderH > avail.y) {
            renderH = avail.y;
            renderW = renderH * aspect;
        }

        // Center the video in the viewport area
        float offsetX = (avail.x - renderW) * 0.5f;
        float offsetY = (avail.y - renderH) * 0.5f;

        if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        if (offsetY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

        ImGui::Image(
            static_cast<ImTextureID>(texId),
            ImVec2(renderW, renderH),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
    } else {
        // Standby screen when waiting for stream
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + avail.y);

        // Draw dark gradient background
        drawList->AddRectFilled(p0, p1, IM_COL32(12, 16, 22, 255));
        drawList->AddRect(p0, p1, IM_COL32(35, 50, 70, 255));

        const char* text1 = "TBS-5580 Digital Broadcast Receiver";
        const char* text2 = client_->isConnected() ?
            "Awaiting Carrier Lock & MPEG-TS Stream (Use Channel Preset or Click Peak on Spectrum)..." :
            "Connect to Raspberry Pi Bridge (Default: 192.168.1.27:1235) to Begin Live TV Reception";

        ImVec2 textSize1 = ImGui::CalcTextSize(text1);
        ImVec2 textSize2 = ImGui::CalcTextSize(text2);

        float cx = p0.x + avail.x * 0.5f;
        float cy = p0.y + avail.y * 0.5f;

        drawList->AddText(ImVec2(cx - textSize1.x * 0.5f, cy - 20), IM_COL32(0, 215, 255, 255), text1);
        drawList->AddText(ImVec2(cx - textSize2.x * 0.5f, cy + 10), IM_COL32(180, 190, 205, 255), text2);

        ImGui::Dummy(avail);
    }
}

} // namespace TBS

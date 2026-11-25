#pragma once

#include "TbsProtocol.hpp"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace TBS {

using TsDataCallback = std::function<void(const uint8_t* tsData, size_t bytes)>;
using TelemetryCallback = std::function<void(const RespTelemetry& telem)>;
using SpectrumCallback = std::function<void(const std::vector<SpectrumPoint>& points)>;

class TbsClient {
public:
    TbsClient();
    ~TbsClient();

    bool connectToServer(const std::string& host, uint16_t port = DEFAULT_PORT);
    void disconnect();
    bool isConnected() const { return isConnected_.load(); }

    void setTsCallback(TsDataCallback cb) { tsCallback_ = std::move(cb); }
    void setTelemetryCallback(TelemetryCallback cb) { telemCallback_ = std::move(cb); }
    void setSpectrumCallback(SpectrumCallback cb) { spectrumCallback_ = std::move(cb); }

    bool tuneTerrestrial(DeliverySystem sys, uint32_t frequencyHz, uint32_t bandwidthHz = 8000000, int32_t plpId = -1);
    bool tuneSatellite(DeliverySystem sys, uint32_t frequencyKhz, uint32_t symbolRateBps, uint8_t pol, uint8_t tone22k = 0, uint8_t diseqcPort = 0);
    bool tuneCable(uint32_t frequencyHz, uint32_t symbolRateBps, uint8_t modulation = 0);

    bool requestSpectrumScan(DeliverySystem sys, uint32_t startFreqHz, uint32_t stopFreqHz, uint32_t stepHz = 8000000);

    RespTelemetry getLatestTelemetry() const;
    std::vector<SpectrumPoint> getLatestSpectrum() const;
    bool isScanning() const { return isScanning_.load(); }

private:
    void receiverThreadLoop();
    bool sendCommand(uint16_t msgType, const void* payload, size_t len);

    std::string host_ = "192.168.1.27";
    uint16_t port_ = DEFAULT_PORT;
    int socketFd_ = -1;

    std::atomic<bool> isConnected_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> isScanning_{false};
    std::thread rxThread_;

    TsDataCallback tsCallback_;
    TelemetryCallback telemCallback_;
    SpectrumCallback spectrumCallback_;

    mutable std::mutex telemMutex_;
    RespTelemetry latestTelemetry_{};

    mutable std::mutex spectrumMutex_;
    std::vector<SpectrumPoint> latestSpectrum_;
};

} // namespace TBS

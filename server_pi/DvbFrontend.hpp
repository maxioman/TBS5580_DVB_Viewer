#pragma once

#include "TbsProtocol.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace TBS {

class DvbFrontend {
public:
    DvbFrontend();
    ~DvbFrontend();

    bool openDevice(int adapterIndex = 0);
    void closeDevice();

    bool tuneTerrestrial(const CmdTuneTerrestrial& cmd);
    bool tuneSatellite(const CmdTuneSatellite& cmd);
    bool tuneCable(const CmdTuneCable& cmd);

    RespTelemetry getTelemetry();

    // Fast RF Spectrum & Signal Strength Scan across a frequency range
    std::vector<SpectrumPoint> scanSpectrum(const CmdScanSpectrum& cmd);

    // Read TS packets (returns number of bytes read into buffer, must be multiple of 188)
    ssize_t readTsData(uint8_t* buffer, size_t maxBytes);

    bool isSimulated() const { return isSimulated_; }
    void setSimulate(bool sim) { isSimulated_ = sim; }

private:
    void initSimulation();
    void generateSimulatedTs(uint8_t* buffer, size_t bytes);

    int adapterIdx_ = 0;
    int feFd_ = -1;
    int dvrFd_ = -1;
    int dmxFd_ = -1;

    bool isSimulated_ = false;
    uint32_t currentFreqHz_ = 546000000;
    uint8_t  currentDelSys_ = SYS_DVB_T2;
    std::atomic<bool> isStreaming_{false};

    // Simulated TS state
    uint8_t simPatCc_ = 0;
    uint8_t simPmtCc_ = 0;
    uint8_t simSdtCc_ = 0;
    uint8_t simVidCc_ = 0;
    uint8_t simAudCc_ = 0;
    uint64_t simSampleIndex_ = 0;

    std::vector<uint8_t> tsRemainder_;
    std::mutex mutex_;
};

} // namespace TBS

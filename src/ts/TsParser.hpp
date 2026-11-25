#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>

namespace TBS {

struct EsStreamInfo {
    uint8_t  streamType = 0;
    uint16_t pid = 0;
    std::string codecName;
    bool isVideo = false;
    bool isAudio = false;
};

struct DvbService {
    uint16_t serviceId = 0; // Matches program_number
    uint16_t pmtPid = 0;
    uint16_t pcrPid = 0;
    std::string name;
    std::string provider;
    uint8_t  serviceType = 0; // 0x01=SD TV, 0x19=HD TV, 0x1F=UHD TV, 0x02=Radio
    std::vector<EsStreamInfo> streams;
    uint16_t videoPid = 0;
    uint16_t audioPid = 0;
    uint8_t  videoStreamType = 0;
    uint8_t  audioStreamType = 0;
    uint64_t packetCount = 0;
};

struct PidStats {
    uint16_t pid = 0;
    uint64_t packetCount = 0;
    uint64_t ccErrors = 0;
    int8_t   lastCc = -1;
    uint32_t bitrateBps = 0;
};

class TsParser {
public:
    TsParser();
    ~TsParser();

    // Reset all tables and PID stats (e.g. upon retuning)
    void reset();

    // Ingest raw bytes from network (may contain multiple 188-byte packets)
    void ingest(const uint8_t* data, size_t len);

    // Get discovered services (thread-safe copy)
    std::vector<DvbService> getServices() const;

    // Set selected service ID to filter TS for playback
    void selectService(uint16_t serviceId);
    uint16_t getSelectedService() const;

    // Read filtered TS packets for FFmpeg AVIOContext
    // Returns bytes read (up to maxLen). Non-blocking.
    size_t readSelectedTs(uint8_t* outBuf, size_t maxLen);

    // Get current total TS stats
    uint64_t getTotalPackets() const { return totalPackets_.load(); }
    uint64_t getTotalCcErrors() const { return totalCcErrors_.load(); }
    size_t getRingBufferFill() const;

    // Helpers
    static std::string streamTypeToString(uint8_t streamType);
    static std::string serviceTypeToString(uint8_t serviceType);

private:
    void processPacket(const uint8_t* pkt);
    void parsePat(const uint8_t* payload, size_t len);
    void parsePmt(uint16_t pmtPid, const uint8_t* payload, size_t len);
    void parseSdt(const uint8_t* payload, size_t len);

    // Thread safety for discovered services
    mutable std::mutex servicesMutex_;
    std::map<uint16_t, DvbService> services_; // keyed by serviceId (program_number)
    std::map<uint16_t, uint16_t> pmtPidToServiceId_; // pmtPid -> serviceId

    // PID tracking
    mutable std::mutex statsMutex_;
    std::map<uint16_t, PidStats> pidStats_;
    std::atomic<uint64_t> totalPackets_{0};
    std::atomic<uint64_t> totalCcErrors_{0};

    // Active service selection
    std::atomic<uint16_t> selectedServiceId_{0};

    // TS Ring Buffer for Video/Audio Decoders
    static constexpr size_t RING_BUFFER_SIZE = 32 * 1024 * 1024; // 32 MB ring buffer
    std::vector<uint8_t> ringBuffer_;
    size_t ringWriteHead_ = 0;
    size_t ringReadHead_ = 0;
    size_t ringBytesAvailable_ = 0;
    mutable std::mutex ringMutex_;

    // Remainder buffer for TCP chunk boundaries
    std::vector<uint8_t> ingestRemainder_;

    // Section reassembly buffers for PAT, PMT, SDT
    struct SectionAccumulator {
        std::vector<uint8_t> buffer;
        size_t expectedLen = 0;
        bool inProgress = false;
    };
    std::map<uint16_t, SectionAccumulator> sectionAccumulators_;
};

} // namespace TBS

#include "TsParser.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace TBS {

TsParser::TsParser() {
    ringBuffer_.resize(RING_BUFFER_SIZE, 0);
}

TsParser::~TsParser() = default;

void TsParser::reset() {
    {
        std::lock_guard<std::mutex> lock(servicesMutex_);
        services_.clear();
        pmtPidToServiceId_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        pidStats_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        ringWriteHead_ = 0;
        ringReadHead_ = 0;
        ringBytesAvailable_ = 0;
    }
    sectionAccumulators_.clear();
    totalPackets_ = 0;
    totalCcErrors_ = 0;
}

void TsParser::selectService(uint16_t serviceId) {
    selectedServiceId_.store(serviceId);
    // Flush ring buffer when switching channels
    std::lock_guard<std::mutex> lock(ringMutex_);
    ringWriteHead_ = 0;
    ringReadHead_ = 0;
    ringBytesAvailable_ = 0;
}

uint16_t TsParser::getSelectedService() const {
    return selectedServiceId_.load();
}

std::vector<DvbService> TsParser::getServices() const {
    std::lock_guard<std::mutex> lock(servicesMutex_);
    std::vector<DvbService> result;
    result.reserve(services_.size());
    for (const auto& pair : services_) {
        result.push_back(pair.second);
    }
    return result;
}

size_t TsParser::getRingBufferFill() const {
    std::lock_guard<std::mutex> lock(ringMutex_);
    return ringBytesAvailable_;
}

size_t TsParser::readSelectedTs(uint8_t* outBuf, size_t maxLen) {
    std::lock_guard<std::mutex> lock(ringMutex_);
    if (ringBytesAvailable_ == 0 || maxLen == 0) {
        return 0;
    }

    size_t toRead = std::min(maxLen, ringBytesAvailable_);
    size_t firstChunk = std::min(toRead, RING_BUFFER_SIZE - ringReadHead_);
    std::memcpy(outBuf, &ringBuffer_[ringReadHead_], firstChunk);

    if (toRead > firstChunk) {
        size_t secondChunk = toRead - firstChunk;
        std::memcpy(outBuf + firstChunk, &ringBuffer_[0], secondChunk);
        ringReadHead_ = secondChunk;
    } else {
        ringReadHead_ = (ringReadHead_ + firstChunk) % RING_BUFFER_SIZE;
    }

    ringBytesAvailable_ -= toRead;
    return toRead;
}

void TsParser::ingest(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    std::vector<uint8_t> buffer;
    const uint8_t* src = data;
    size_t totalLen = len;

    if (!ingestRemainder_.empty()) {
        buffer = std::move(ingestRemainder_);
        buffer.insert(buffer.end(), data, data + len);
        src = buffer.data();
        totalLen = buffer.size();
    }

    size_t offset = 0;
    while (offset + 188 <= totalLen) {
        if (src[offset] != 0x47) {
            const uint8_t* p = static_cast<const uint8_t*>(std::memchr(src + offset, 0x47, totalLen - offset));
            if (!p) {
                offset = totalLen;
                break;
            }
            offset = p - src;
            if (offset + 188 > totalLen) break;
        }

        processPacket(src + offset);
        offset += 188;
    }

    if (offset < totalLen) {
        ingestRemainder_.assign(src + offset, src + totalLen);
    } else {
        ingestRemainder_.clear();
    }
}

void TsParser::processPacket(const uint8_t* pkt) {
    totalPackets_++;

    uint16_t pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    bool tei = (pkt[1] & 0x80) != 0;
    bool pusi = (pkt[1] & 0x40) != 0;
    uint8_t adaptCtrl = (pkt[3] >> 4) & 0x03;
    uint8_t cc = pkt[3] & 0x0F;

    if (tei) {
        totalCcErrors_++;
        return;
    }

    // Continuity counter verification
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        auto& stat = pidStats_[pid];
        stat.pid = pid;
        stat.packetCount++;

        // Only check CC if packet has payload
        if (adaptCtrl == 0x01 || adaptCtrl == 0x03) {
            if (stat.lastCc >= 0) {
                uint8_t expectedCc = (stat.lastCc + 1) & 0x0F;
                if (cc != expectedCc && cc != stat.lastCc) {
                    stat.ccErrors++;
                    totalCcErrors_++;
                }
            }
            stat.lastCc = cc;
        }
    }

    // Selective TS filtering for playback: only push PAT, SDT, and PIDs belonging to selected channel
    uint16_t selProg = selectedServiceId_.load();
    bool pushToRing = false;

    if (selProg == 0) {
        pushToRing = true; // Stream all until user or auto-select chooses a channel
    } else {
        if (pid == 0 || pid == 0x0011) {
            pushToRing = true; // Always pass PAT and SDT
        } else {
            std::lock_guard<std::mutex> lock(servicesMutex_);
            auto it = services_.find(selProg);
            if (it != services_.end()) {
                const auto& s = it->second;
                if (pid == s.pmtPid || pid == s.pcrPid || pid == s.videoPid || pid == s.audioPid) {
                    pushToRing = true;
                } else {
                    for (const auto& es : s.streams) {
                        if (pid == es.pid) {
                            pushToRing = true;
                            break;
                        }
                    }
                }
            } else {
                pushToRing = true;
            }
        }
    }

    if (pushToRing) {
        std::lock_guard<std::mutex> lock(ringMutex_);
        if (ringBytesAvailable_ + 188 > RING_BUFFER_SIZE) {
            // Drop oldest 4 MB in one clean leap to prevent sporadic missing packets
            size_t dropBytes = 4 * 1024 * 1024;
            if (dropBytes > ringBytesAvailable_) dropBytes = ringBytesAvailable_;
            ringReadHead_ = (ringReadHead_ + dropBytes) % RING_BUFFER_SIZE;
            ringBytesAvailable_ -= dropBytes;

            // Re-align ringReadHead_ to next 0x47 sync byte
            for (size_t i = 0; i < 188 && ringBytesAvailable_ > 0; ++i) {
                if (ringBuffer_[ringReadHead_] == 0x47) break;
                ringReadHead_ = (ringReadHead_ + 1) % RING_BUFFER_SIZE;
                ringBytesAvailable_--;
            }
        }

        size_t firstChunk = std::min((size_t)188, RING_BUFFER_SIZE - ringWriteHead_);
        std::memcpy(&ringBuffer_[ringWriteHead_], pkt, firstChunk);
        if (188 > firstChunk) {
            std::memcpy(&ringBuffer_[0], pkt + firstChunk, 188 - firstChunk);
            ringWriteHead_ = 188 - firstChunk;
        } else {
            ringWriteHead_ = (ringWriteHead_ + 188) % RING_BUFFER_SIZE;
        }
        ringBytesAvailable_ += 188;
    }

    // Parse PSI/SI tables (PAT = 0, SDT = 0x11, or PMT)
    bool isPat = (pid == 0);
    bool isSdt = (pid == 0x0011);
    bool isPmt = false;
    {
        std::lock_guard<std::mutex> lock(servicesMutex_);
        if (pmtPidToServiceId_.find(pid) != pmtPidToServiceId_.end()) {
            isPmt = true;
        }
    }

    if (!isPat && !isSdt && !isPmt) {
        return; // Ordinary video/audio/null packet
    }

    // Payload extraction
    size_t payloadOffset = 4;
    if (adaptCtrl == 0x02) {
        return; // Adaptation field only, no payload
    }
    if (adaptCtrl == 0x03) {
        uint8_t adaptLen = pkt[4];
        payloadOffset = 5 + adaptLen;
        if (payloadOffset >= 188) return;
    }

    const uint8_t* payload = pkt + payloadOffset;
    size_t payloadLen = 188 - payloadOffset;

    // PSI section accumulator
    auto& acc = sectionAccumulators_[pid];
    if (pusi) {
        // Pointer field
        uint8_t pointerField = payload[0];
        if (1 + pointerField >= payloadLen) return;

        payload += 1 + pointerField;
        payloadLen -= (1 + pointerField);

        acc.buffer.clear();
        acc.inProgress = true;
        if (payloadLen >= 3) {
            acc.expectedLen = (((payload[1] & 0x0F) << 8) | payload[2]) + 3;
        } else {
            acc.expectedLen = 0;
        }
    }

    if (!acc.inProgress) return;

    acc.buffer.insert(acc.buffer.end(), payload, payload + payloadLen);

    if (acc.expectedLen == 0 && acc.buffer.size() >= 3) {
        acc.expectedLen = (((acc.buffer[1] & 0x0F) << 8) | acc.buffer[2]) + 3;
    }

    if (acc.expectedLen > 0 && acc.buffer.size() >= acc.expectedLen) {
        // Complete section assembled!
        if (isPat) {
            parsePat(acc.buffer.data(), acc.expectedLen);
        } else if (isPmt) {
            parsePmt(pid, acc.buffer.data(), acc.expectedLen);
        } else if (isSdt) {
            parseSdt(acc.buffer.data(), acc.expectedLen);
        }
        acc.inProgress = false;
        acc.buffer.clear();
    }
}

void TsParser::parsePat(const uint8_t* payload, size_t len) {
    if (len < 8 || payload[0] != 0x00) return; // table_id 0x00 = PAT

    size_t sectionLen = (((payload[1] & 0x0F) << 8) | payload[2]);
    if (sectionLen + 3 > len) return;

    size_t idx = 8;
    size_t endIdx = sectionLen - 1; // Exclude 4-byte CRC

    std::lock_guard<std::mutex> lock(servicesMutex_);
    while (idx + 4 <= endIdx) {
        uint16_t progNum = (payload[idx] << 8) | payload[idx + 1];
        uint16_t pid = ((payload[idx + 2] & 0x1F) << 8) | payload[idx + 3];

        if (progNum != 0) {
            auto& s = services_[progNum];
            s.serviceId = progNum;
            s.pmtPid = pid;
            pmtPidToServiceId_[pid] = progNum;
            if (s.name.empty()) {
                s.name = "Program " + std::to_string(progNum);
            }
        }
        idx += 4;
    }
}

void TsParser::parsePmt(uint16_t pmtPid, const uint8_t* payload, size_t len) {
    if (len < 12 || payload[0] != 0x02) return; // table_id 0x02 = PMT

    size_t sectionLen = (((payload[1] & 0x0F) << 8) | payload[2]);
    if (sectionLen + 3 > len) return;

    uint16_t progNum = (payload[3] << 8) | payload[4];
    uint16_t pcrPid = ((payload[8] & 0x1F) << 8) | payload[9];
    uint16_t progInfoLen = ((payload[10] & 0x0F) << 8) | payload[11];

    size_t idx = 12 + progInfoLen;
    size_t endIdx = sectionLen - 1; // Exclude 4-byte CRC

    std::lock_guard<std::mutex> lock(servicesMutex_);
    auto& s = services_[progNum];
    s.serviceId = progNum;
    s.pmtPid = pmtPid;
    s.pcrPid = pcrPid;
    s.streams.clear();

    while (idx + 5 <= endIdx) {
        uint8_t stype = payload[idx];
        uint16_t epid = ((payload[idx + 1] & 0x1F) << 8) | payload[idx + 2];
        uint16_t esInfoLen = ((payload[idx + 3] & 0x0F) << 8) | payload[idx + 4];

        EsStreamInfo info;
        info.streamType = stype;
        info.pid = epid;
        info.codecName = streamTypeToString(stype);

        // Check if Video
        if (stype == 0x01 || stype == 0x02 || stype == 0x1B || stype == 0x24) {
            info.isVideo = true;
            if (s.videoPid == 0) {
                s.videoPid = epid;
                s.videoStreamType = stype;
            }
        }
        // Check if Audio
        else if (stype == 0x03 || stype == 0x04 || stype == 0x0F || stype == 0x11 ||
                 stype == 0x06 || stype == 0x81 || stype == 0x87) {
            info.isAudio = true;
            if (s.audioPid == 0) {
                s.audioPid = epid;
                s.audioStreamType = stype;
            }
        }

        s.streams.push_back(info);
        idx += 5 + esInfoLen;
    }
}

void TsParser::parseSdt(const uint8_t* payload, size_t len) {
    if (len < 11) return;
    uint8_t tableId = payload[0];
    if (tableId != 0x42 && tableId != 0x46) return; // SDT Actual or Other TS

    size_t sectionLen = (((payload[1] & 0x0F) << 8) | payload[2]);
    if (sectionLen + 3 > len) return;

    size_t idx = 11;
    size_t endIdx = sectionLen - 1; // Exclude CRC

    std::lock_guard<std::mutex> lock(servicesMutex_);
    while (idx + 5 <= endIdx) {
        uint16_t srvId = (payload[idx] << 8) | payload[idx + 1];
        uint16_t dloopLen = ((payload[idx + 3] & 0x0F) << 8) | payload[idx + 4];
        idx += 5;

        size_t descEnd = idx + dloopLen;
        if (descEnd > endIdx) break;

        while (idx + 2 <= descEnd) {
            uint8_t tag = payload[idx];
            uint8_t dlen = payload[idx + 1];
            idx += 2;

            if (idx + dlen > descEnd) break;

            if (tag == 0x48 && dlen >= 3) { // Service Descriptor
                uint8_t stype = payload[idx];
                uint8_t provLen = payload[idx + 1];
                std::string provider;
                if (idx + 2 + provLen <= descEnd) {
                    provider = std::string(reinterpret_cast<const char*>(&payload[idx + 2]), provLen);
                }

                size_t srvNameOffset = idx + 2 + provLen;
                std::string srvName;
                if (srvNameOffset < descEnd) {
                    uint8_t nameLen = payload[srvNameOffset];
                    if (srvNameOffset + 1 + nameLen <= descEnd) {
                        srvName = std::string(reinterpret_cast<const char*>(&payload[srvNameOffset + 1]), nameLen);
                    }
                }

                auto& s = services_[srvId];
                s.serviceId = srvId;
                s.serviceType = stype;
                if (!srvName.empty()) s.name = srvName;
                if (!provider.empty()) s.provider = provider;
            }

            idx += dlen;
        }
    }
}

std::string TsParser::streamTypeToString(uint8_t stype) {
    switch (stype) {
        case 0x01: return "MPEG-1 Video";
        case 0x02: return "MPEG-2 Video (SD/HD)";
        case 0x1B: return "H.264 / AVC HD";
        case 0x24: return "H.265 / HEVC UHD";
        case 0x03: return "MPEG-1 Audio (MP1)";
        case 0x04: return "MPEG-2 Audio (MP2)";
        case 0x0F: return "AAC (ADTS)";
        case 0x11: return "AAC (LATM/LOAS)";
        case 0x06: return "AC-3 / Teletext / DVB Subs";
        case 0x81: return "AC-3 (Dolby Digital)";
        case 0x87: return "E-AC-3 (Dolby Digital Plus)";
        default:   return "Stream 0x" + std::to_string(stype);
    }
}

std::string TsParser::serviceTypeToString(uint8_t stype) {
    switch (stype) {
        case 0x01: return "Digital TV (SD)";
        case 0x02: return "Digital Radio";
        case 0x16: return "DVB MHP";
        case 0x19: return "Digital TV (HD)";
        case 0x1F: return "Digital TV (HEVC UHD)";
        default:   return "Service";
    }
}

} // namespace TBS

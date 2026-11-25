#include "TbsClient.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace TBS {

TbsClient::TbsClient() {
}

TbsClient::~TbsClient() {
    disconnect();
}

bool TbsClient::connectToServer(const std::string& host, uint16_t port) {
    disconnect();
    host_ = host;
    port_ = port;

    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd_ < 0) {
        return false;
    }

    // Resolve hostname or IP
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    std::memcpy(&servAddr.sin_addr.s_addr, server->h_addr, server->h_length);
    servAddr.sin_port = htons(port);

    // Timeout on connect (2.5 seconds)
    timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 500000;
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socketFd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(socketFd_, (sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    // Clear socket timeouts after connection to allow continuous streaming
    timeval tvZero{};
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &tvZero, sizeof(tvZero));
    setsockopt(socketFd_, SOL_SOCKET, SO_SNDTIMEO, &tvZero, sizeof(tvZero));

    int nodelay = 1;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int rcvBuf = 4 * 1024 * 1024;
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    shouldStop_ = false;
    isConnected_ = true;
    rxThread_ = std::thread(&TbsClient::receiverThreadLoop, this);
    return true;
}

void TbsClient::disconnect() {
    shouldStop_ = true;
    if (socketFd_ >= 0) {
        shutdown(socketFd_, SHUT_RDWR);
        close(socketFd_);
        socketFd_ = -1;
    }
    if (rxThread_.joinable()) {
        rxThread_.join();
    }
    isConnected_ = false;

    std::lock_guard<std::mutex> lock(telemMutex_);
    std::memset(&latestTelemetry_, 0, sizeof(latestTelemetry_));
}

bool TbsClient::sendCommand(uint16_t msgType, const void* payload, size_t len) {
    if (!isConnected_ || socketFd_ < 0) return false;

    MsgHeader hdr;
    hdr.magic = TBS_MAGIC;
    hdr.msgType = msgType;
    hdr.payloadLen = static_cast<uint32_t>(len);

    std::vector<uint8_t> buf(sizeof(hdr) + len);
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    if (len > 0 && payload) {
        std::memcpy(buf.data() + sizeof(hdr), payload, len);
    }

    ssize_t sent = send(socketFd_, buf.data(), buf.size(), 0);
    return sent == static_cast<ssize_t>(buf.size());
}

bool TbsClient::tuneTerrestrial(DeliverySystem sys, uint32_t frequencyHz, uint32_t bandwidthHz, int32_t plpId) {
    CmdTuneTerrestrial cmd;
    cmd.deliverySystem = static_cast<uint8_t>(sys);
    cmd.frequencyHz = frequencyHz;
    cmd.bandwidthHz = bandwidthHz;
    cmd.plpId = plpId;
    return sendCommand(CMD_TUNE_TERRESTRIAL, &cmd, sizeof(cmd));
}

bool TbsClient::tuneSatellite(DeliverySystem sys, uint32_t frequencyKhz, uint32_t symbolRateBps, uint8_t pol, uint8_t tone22k, uint8_t diseqcPort) {
    CmdTuneSatellite cmd;
    cmd.deliverySystem = static_cast<uint8_t>(sys);
    cmd.frequencyKhz = frequencyKhz;
    cmd.symbolRateBps = symbolRateBps;
    cmd.polarization = pol;
    cmd.tone22k = tone22k;
    cmd.diseqcPort = diseqcPort;
    return sendCommand(CMD_TUNE_SATELLITE, &cmd, sizeof(cmd));
}

bool TbsClient::tuneCable(uint32_t frequencyHz, uint32_t symbolRateBps, uint8_t modulation) {
    CmdTuneCable cmd;
    cmd.frequencyHz = frequencyHz;
    cmd.symbolRateBps = symbolRateBps;
    cmd.modulation = modulation;
    return sendCommand(CMD_TUNE_CABLE, &cmd, sizeof(cmd));
}

RespTelemetry TbsClient::getLatestTelemetry() const {
    std::lock_guard<std::mutex> lock(telemMutex_);
    return latestTelemetry_;
}

bool TbsClient::requestSpectrumScan(DeliverySystem sys, uint32_t startFreqHz, uint32_t stopFreqHz, uint32_t stepHz) {
    if (!isConnected_ || socketFd_ < 0) return false;

    CmdScanSpectrum cmd;
    cmd.deliverySystem = static_cast<uint8_t>(sys);
    cmd.startFreqHz = startFreqHz;
    cmd.stopFreqHz = stopFreqHz;
    cmd.stepHz = stepHz;

    isScanning_ = true;
    return sendCommand(CMD_SCAN_SPECTRUM, &cmd, sizeof(cmd));
}

std::vector<SpectrumPoint> TbsClient::getLatestSpectrum() const {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    return latestSpectrum_;
}

void TbsClient::receiverThreadLoop() {
    std::vector<uint8_t> payloadBuf(65536);

    while (!shouldStop_ && isConnected_) {
        MsgHeader hdr;
        size_t bytesRead = 0;
        while (bytesRead < sizeof(hdr) && !shouldStop_) {
            ssize_t r = recv(socketFd_, reinterpret_cast<char*>(&hdr) + bytesRead, sizeof(hdr) - bytesRead, 0);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                isConnected_ = false;
                return;
            } else if (r == 0) {
                isConnected_ = false;
                return;
            }
            bytesRead += r;
        }

        if (hdr.magic != TBS_MAGIC) {
            std::cerr << "[TbsClient] Framing sync lost. Re-synchronizing to TBS_MAGIC...\n";
            uint32_t window = hdr.magic;
            bool synced = false;
            while (!shouldStop_ && isConnected_) {
                uint8_t byte = 0;
                ssize_t r = recv(socketFd_, &byte, 1, 0);
                if (r <= 0) {
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    isConnected_ = false;
                    return;
                }
                window = (window >> 8) | (static_cast<uint32_t>(byte) << 24);
                if (window == TBS_MAGIC) {
                    // Synced! Read remaining 6 bytes of header (msgType, payloadLen)
                    uint8_t rest[6];
                    size_t rBytes = 0;
                    while (rBytes < 6 && !shouldStop_) {
                        ssize_t s = recv(socketFd_, rest + rBytes, 6 - rBytes, 0);
                        if (s <= 0) {
                            if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                continue;
                            }
                            isConnected_ = false;
                            return;
                        }
                        rBytes += s;
                    }
                    hdr.magic = TBS_MAGIC;
                    hdr.msgType = static_cast<uint16_t>(rest[0] | (rest[1] << 8));
                    hdr.payloadLen = static_cast<uint32_t>(rest[2] | (rest[3] << 8) | (rest[4] << 16) | (rest[5] << 24));
                    synced = true;
                    break;
                }
            }
            if (!synced) {
                isConnected_ = false;
                return;
            }
        }

        if (payloadBuf.size() < hdr.payloadLen) {
            payloadBuf.resize(hdr.payloadLen);
        }

        bytesRead = 0;
        while (bytesRead < hdr.payloadLen && !shouldStop_) {
            ssize_t r = recv(socketFd_, payloadBuf.data() + bytesRead, hdr.payloadLen - bytesRead, 0);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                isConnected_ = false;
                return;
            } else if (r == 0) {
                isConnected_ = false;
                return;
            }
            bytesRead += r;
        }

        if (hdr.msgType == RESP_TELEMETRY && hdr.payloadLen >= sizeof(RespTelemetry)) {
            RespTelemetry telem;
            std::memcpy(&telem, payloadBuf.data(), sizeof(telem));
            {
                std::lock_guard<std::mutex> lock(telemMutex_);
                latestTelemetry_ = telem;
            }
            if (telemCallback_) {
                telemCallback_(telem);
            }
        } else if (hdr.msgType == DATA_MPEG_TS && hdr.payloadLen > 0) {
            if (tsCallback_) {
                tsCallback_(payloadBuf.data(), hdr.payloadLen);
            }
        } else if (hdr.msgType == RESP_SPECTRUM_SCAN && hdr.payloadLen >= sizeof(RespSpectrumScanHeader)) {
            RespSpectrumScanHeader sHdr;
            std::memcpy(&sHdr, payloadBuf.data(), sizeof(sHdr));

            size_t ptsBytes = hdr.payloadLen - sizeof(sHdr);
            size_t numPts = ptsBytes / sizeof(SpectrumPoint);

            std::vector<SpectrumPoint> pts(numPts);
            if (numPts > 0) {
                std::memcpy(pts.data(), payloadBuf.data() + sizeof(sHdr), numPts * sizeof(SpectrumPoint));
            }

            {
                std::lock_guard<std::mutex> lock(spectrumMutex_);
                latestSpectrum_ = pts;
            }
            isScanning_ = false;

            if (spectrumCallback_) {
                spectrumCallback_(pts);
            }
        }
    }
}

} // namespace TBS

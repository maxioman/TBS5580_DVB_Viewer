#include "DvbFrontend.hpp"
#include "TbsProtocol.hpp"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

std::atomic<bool> gRunning{true};

void sigHandler(int) {
    gRunning = false;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe on client disconnect

    int port = TBS::DEFAULT_PORT;
    int adapterIdx = 0;
    bool forceSim = false;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (std::string(argv[i]) == "--adapter" && i + 1 < argc) {
            adapterIdx = std::stoi(argv[++i]);
        } else if (std::string(argv[i]) == "--simulate") {
            forceSim = true;
        }
    }

    std::cout << "=========================================================\n";
    std::cout << "  TBS-5580 Raspberry Pi Multi-Standard DVB Bridge Server  \n";
    std::cout << "  DVB-T/T2, ISDB-T, DVB-S/S2, DVB-C & MPEG-TS Streamer    \n";
    std::cout << "=========================================================\n";
    std::cout << "[Server] Port: " << port << ", DVB Adapter: " << adapterIdx << "\n";

    TBS::DvbFrontend frontend;
    if (forceSim) {
        frontend.setSimulate(true);
    }
    frontend.openDevice(adapterIdx);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("[Server] Socket creation failed");
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = INADDR_ANY;
    servAddr.sin_port = htons(port);

    if (bind(serverFd, (sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        perror("[Server] Bind failed");
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 2) < 0) {
        perror("[Server] Listen failed");
        close(serverFd);
        return 1;
    }

    std::cout << "[Server] Listening on 0.0.0.0:" << port << " for macOS client...\n";

    while (gRunning) {
        sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (!gRunning) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr, INET_ADDRSTRLEN);
        std::cout << "[Server] Client connected from " << ipStr << "\n";

        // Optimize TCP for streaming
        int nodelay = 1;
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int sndBuf = 4 * 1024 * 1024;
        setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));

        std::atomic<bool> clientRunning{true};
        std::mutex sendMutex;

        auto sendAll = [&](const void* data, size_t len) -> bool {
            std::lock_guard<std::mutex> lock(sendMutex);
            const uint8_t* ptr = static_cast<const uint8_t*>(data);
            size_t remaining = len;
            while (remaining > 0 && clientRunning && gRunning) {
                ssize_t sent = send(clientFd, ptr, remaining, MSG_NOSIGNAL);
                if (sent <= 0) return false;
                ptr += sent;
                remaining -= sent;
            }
            return remaining == 0;
        };

        // Telemetry thread: sends RF metrics to macOS at 5 Hz
        std::thread telemThread([&]() {
            while (clientRunning && gRunning) {
                TBS::RespTelemetry telem = frontend.getTelemetry();
                TBS::MsgHeader hdr;
                hdr.magic = TBS::TBS_MAGIC;
                hdr.msgType = TBS::RESP_TELEMETRY;
                hdr.payloadLen = sizeof(telem);

                std::vector<uint8_t> pkt(sizeof(hdr) + sizeof(telem));
                std::memcpy(pkt.data(), &hdr, sizeof(hdr));
                std::memcpy(pkt.data() + sizeof(hdr), &telem, sizeof(telem));

                if (!sendAll(pkt.data(), pkt.size())) {
                    clientRunning = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        // TS Streaming thread: reads /dev/dvb/.../dvr0 and streams 188-byte chunks
        std::thread streamThread([&]() {
            constexpr size_t TS_BUF_SIZE = 7 * 188 * 32; // ~42 KB (224 TS packets per network write)
            std::vector<uint8_t> tsBuffer(TS_BUF_SIZE);
            std::vector<uint8_t> netPkt(sizeof(TBS::MsgHeader) + TS_BUF_SIZE);

            while (clientRunning && gRunning) {
                ssize_t bytes = frontend.readTsData(tsBuffer.data(), TS_BUF_SIZE);
                if (bytes > 0) {
                    TBS::MsgHeader hdr;
                    hdr.magic = TBS::TBS_MAGIC;
                    hdr.msgType = TBS::DATA_MPEG_TS;
                    hdr.payloadLen = static_cast<uint32_t>(bytes);

                    std::memcpy(netPkt.data(), &hdr, sizeof(hdr));
                    std::memcpy(netPkt.data() + sizeof(hdr), tsBuffer.data(), bytes);

                    if (!sendAll(netPkt.data(), sizeof(hdr) + bytes)) {
                        clientRunning = false;
                        break;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });

        // Main thread: reads incoming control commands from macOS client
        while (clientRunning && gRunning) {
            TBS::MsgHeader hdr;
            ssize_t n = recv(clientFd, &hdr, sizeof(hdr), MSG_WAITALL);
            if (n != sizeof(hdr) || hdr.magic != TBS::TBS_MAGIC) {
                break;
            }

            std::vector<uint8_t> payload(hdr.payloadLen);
            if (hdr.payloadLen > 0) {
                ssize_t p = recv(clientFd, payload.data(), hdr.payloadLen, MSG_WAITALL);
                if (p != static_cast<ssize_t>(hdr.payloadLen)) {
                    break;
                }
            }

            TBS::RespStatus resp;
            std::memset(&resp, 0, sizeof(resp));

            if (hdr.msgType == TBS::CMD_TUNE_TERRESTRIAL && payload.size() >= sizeof(TBS::CmdTuneTerrestrial)) {
                TBS::CmdTuneTerrestrial cmd;
                std::memcpy(&cmd, payload.data(), sizeof(cmd));
                bool ok = frontend.tuneTerrestrial(cmd);
                resp.errorCode = ok ? 0 : -1;
                std::strncpy(resp.message, ok ? "Tuned Terrestrial" : "Tune Failed", sizeof(resp.message) - 1);
            } else if (hdr.msgType == TBS::CMD_TUNE_SATELLITE && payload.size() >= sizeof(TBS::CmdTuneSatellite)) {
                TBS::CmdTuneSatellite cmd;
                std::memcpy(&cmd, payload.data(), sizeof(cmd));
                bool ok = frontend.tuneSatellite(cmd);
                resp.errorCode = ok ? 0 : -1;
                std::strncpy(resp.message, ok ? "Tuned Satellite" : "Tune Failed", sizeof(resp.message) - 1);
            } else if (hdr.msgType == TBS::CMD_TUNE_CABLE && payload.size() >= sizeof(TBS::CmdTuneCable)) {
                TBS::CmdTuneCable cmd;
                std::memcpy(&cmd, payload.data(), sizeof(cmd));
                bool ok = frontend.tuneCable(cmd);
                resp.errorCode = ok ? 0 : -1;
                std::strncpy(resp.message, ok ? "Tuned Cable" : "Tune Failed", sizeof(resp.message) - 1);
            } else if (hdr.msgType == TBS::CMD_SCAN_SPECTRUM && payload.size() >= sizeof(TBS::CmdScanSpectrum)) {
                TBS::CmdScanSpectrum cmd;
                std::memcpy(&cmd, payload.data(), sizeof(cmd));
                std::vector<TBS::SpectrumPoint> pts = frontend.scanSpectrum(cmd);

                TBS::RespSpectrumScanHeader sHdr;
                sHdr.startFreqHz = cmd.startFreqHz;
                sHdr.stopFreqHz = cmd.stopFreqHz;
                sHdr.stepHz = cmd.stepHz;
                sHdr.numPoints = static_cast<uint32_t>(pts.size());

                size_t totalPayload = sizeof(sHdr) + pts.size() * sizeof(TBS::SpectrumPoint);
                TBS::MsgHeader scanHdr;
                scanHdr.magic = TBS::TBS_MAGIC;
                scanHdr.msgType = TBS::RESP_SPECTRUM_SCAN;
                scanHdr.payloadLen = static_cast<uint32_t>(totalPayload);

                std::vector<uint8_t> scanBuf(sizeof(scanHdr) + totalPayload);
                std::memcpy(scanBuf.data(), &scanHdr, sizeof(scanHdr));
                std::memcpy(scanBuf.data() + sizeof(scanHdr), &sHdr, sizeof(sHdr));
                if (!pts.empty()) {
                    std::memcpy(scanBuf.data() + sizeof(scanHdr) + sizeof(sHdr), pts.data(), pts.size() * sizeof(TBS::SpectrumPoint));
                }
                sendAll(scanBuf.data(), scanBuf.size());
                continue;
            }

            // Send back command acknowledgement
            TBS::MsgHeader respHdr;
            respHdr.magic = TBS::TBS_MAGIC;
            respHdr.msgType = TBS::RESP_STATUS;
            respHdr.payloadLen = sizeof(resp);

            std::vector<uint8_t> outBuf(sizeof(respHdr) + sizeof(resp));
            std::memcpy(outBuf.data(), &respHdr, sizeof(respHdr));
            std::memcpy(outBuf.data() + sizeof(respHdr), &resp, sizeof(resp));
            sendAll(outBuf.data(), outBuf.size());
        }

        clientRunning = false;
        if (telemThread.joinable()) telemThread.join();
        if (streamThread.joinable()) streamThread.join();
        close(clientFd);
        std::cout << "[Server] Client disconnected.\n";
    }

    close(serverFd);
    std::cout << "[Server] Shutdown complete.\n";
    return 0;
}

#include "DvbFrontend.hpp"

#include <iostream>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#endif

namespace TBS {

DvbFrontend::DvbFrontend() {
}

DvbFrontend::~DvbFrontend() {
    closeDevice();
}

bool DvbFrontend::openDevice(int adapterIndex) {
    closeDevice();
    adapterIdx_ = adapterIndex;

#ifdef __linux__
    std::string fePath = "/dev/dvb/adapter" + std::to_string(adapterIndex) + "/frontend0";
    std::string dvrPath = "/dev/dvb/adapter" + std::to_string(adapterIndex) + "/dvr0";
    std::string dmxPath = "/dev/dvb/adapter" + std::to_string(adapterIndex) + "/demux0";

    feFd_ = open(fePath.c_str(), O_RDWR | O_NONBLOCK);
    if (feFd_ < 0) {
        int err = errno;
        std::cerr << "\n===================================================================\n";
        std::cerr << "[TBS Server] ERROR: Cannot open " << fePath << ": " << strerror(err) << " (errno " << err << ")\n";
        if (err == EBUSY) {
            std::cerr << "[TBS Server] >> REASON: Device is BUSY! Another process (such as Tvheadend) holds an exclusive lock.\n";
            std::cerr << "[TBS Server] >> FIX: Stop Tvheadend by running: sudo systemctl stop tvheadend\n";
            std::cerr << "[TBS Server] >> Or identify locking processes: sudo fuser -v " << fePath << "\n";
        } else if (err == EACCES) {
            std::cerr << "[TBS Server] >> REASON: Permission denied! Please run tbs_bridge with sudo.\n";
        } else if (err == ENOENT) {
            std::cerr << "[TBS Server] >> REASON: " << fePath << " not found! Check 'ls /dev/dvb' - adapter index may be different.\n";
        }
        std::cerr << "[TBS Server] >> FALLING BACK TO SIMULATED MODE (Dummy TS with no real video)\n";
        std::cerr << "===================================================================\n\n";
        isSimulated_ = true;
        initSimulation();
        return true;
    }

    dmxFd_ = open(dmxPath.c_str(), O_RDWR);
    if (dmxFd_ >= 0) {
        // Expand kernel DMX ring buffer to 8 MB BEFORE setting filter
        int bufSize = 8 * 1024 * 1024;
        if (ioctl(dmxFd_, DMX_SET_BUFFER_SIZE, bufSize) < 0) {
            std::cerr << "[TBS Server] Warning: DMX_SET_BUFFER_SIZE on dmx0 failed: " << strerror(errno) << "\n";
        }

        // Set demux to DVR mode (pass full transport stream to /dev/dvb/adapterX/dvr0)
        struct dmx_pes_filter_params pesFilter;
        std::memset(&pesFilter, 0, sizeof(pesFilter));
        pesFilter.pid = 0x2000; // Special wildcard PID: pass all PIDs to DVR
        pesFilter.input = DMX_IN_FRONTEND;
        pesFilter.output = DMX_OUT_TS_TAP;
        pesFilter.pes_type = DMX_PES_OTHER;
        pesFilter.flags = DMX_IMMEDIATE_START;
        if (ioctl(dmxFd_, DMX_SET_PES_FILTER, &pesFilter) < 0) {
            std::cerr << "[TBS Server] Warning: DMX_SET_PES_FILTER failed.\n";
        }
    }

    dvrFd_ = open(dvrPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (dvrFd_ < 0) {
        std::cerr << "[TBS Server] Cannot open " << dvrPath << "\n";
    }

    std::cout << "[TBS Server] Opened hardware DVB frontend on adapter " << adapterIndex << "\n";
    isSimulated_ = false;
    return true;
#else
    isSimulated_ = true;
    initSimulation();
    std::cout << "[TBS Server] Non-Linux host detected. Running in TBS5580 Simulation Mode.\n";
    return true;
#endif
}

void DvbFrontend::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feFd_ >= 0) { close(feFd_); feFd_ = -1; }
    if (dmxFd_ >= 0) { close(dmxFd_); dmxFd_ = -1; }
    if (dvrFd_ >= 0) { close(dvrFd_); dvrFd_ = -1; }
}

bool DvbFrontend::tuneTerrestrial(const CmdTuneTerrestrial& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentFreqHz_ = cmd.frequencyHz;
    currentDelSys_ = cmd.deliverySystem;

    std::cout << "[TBS Server] Tuning Terrestrial: Freq=" << (cmd.frequencyHz / 1e6) 
              << " MHz, BW=" << (cmd.bandwidthHz / 1e6) 
              << " MHz, Sys=" << (cmd.deliverySystem == SYS_DVB_T2 ? "DVB-T2" : cmd.deliverySystem == SYS_ISDB_T ? "ISDB-T" : "DVB-T")
              << "\n";

    if (isSimulated_) {
        return true;
    }

#ifdef __linux__
    if (feFd_ < 0) return false;

    struct dtv_property props[6];
    struct dtv_properties dtvProp;
    std::memset(props, 0, sizeof(props));
    std::memset(&dtvProp, 0, sizeof(dtvProp));

    int n = 0;
    props[n].cmd = DTV_CLEAR; n++;
    
    props[n].cmd = DTV_DELIVERY_SYSTEM;
    if (cmd.deliverySystem == SYS_DVB_T2) props[n].u.data = SYS_DVBT2;
    else if (cmd.deliverySystem == SYS_ISDB_T) props[n].u.data = SYS_ISDBT;
    else props[n].u.data = SYS_DVBT;
    n++;

    props[n].cmd = DTV_FREQUENCY;
    props[n].u.data = cmd.frequencyHz;
    n++;

    props[n].cmd = DTV_BANDWIDTH_HZ;
    props[n].u.data = cmd.bandwidthHz;
    n++;

    if (cmd.deliverySystem == SYS_DVB_T2 && cmd.plpId >= 0) {
        props[n].cmd = DTV_STREAM_ID;
        props[n].u.data = static_cast<uint32_t>(cmd.plpId);
        n++;
    }

    props[n].cmd = DTV_TUNE; n++;

    dtvProp.num = n;
    dtvProp.props = props;

    if (ioctl(feFd_, FE_SET_PROPERTY, &dtvProp) < 0) {
        perror("[TBS Server] FE_SET_PROPERTY tune failed");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool DvbFrontend::tuneSatellite(const CmdTuneSatellite& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentFreqHz_ = cmd.frequencyKhz * 1000;
    currentDelSys_ = cmd.deliverySystem;

    std::cout << "[TBS Server] Tuning Satellite: Freq=" << (cmd.frequencyKhz / 1000.0) 
              << " MHz, SR=" << (cmd.symbolRateBps / 1000) 
              << " kSps, Pol=" << (cmd.polarization == 0 ? "H (18V)" : "V (13V)")
              << ", 22k=" << (cmd.tone22k ? "ON" : "OFF")
              << ", DiSEqC Port=" << static_cast<int>(cmd.diseqcPort)
              << "\n";

    if (isSimulated_) return true;

#ifdef __linux__
    if (feFd_ < 0) return false;

    // Set LNB Voltage (13V vs 18V)
    fe_sec_voltage_t volt = (cmd.polarization == 0) ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
    ioctl(feFd_, FE_SET_VOLTAGE, volt);

    // Set 22kHz Tone
    fe_sec_tone_mode_t tone = cmd.tone22k ? SEC_TONE_ON : SEC_TONE_OFF;
    ioctl(feFd_, FE_SET_TONE, tone);

    struct dtv_property props[6];
    struct dtv_properties dtvProp;
    std::memset(props, 0, sizeof(props));
    std::memset(&dtvProp, 0, sizeof(dtvProp));

    int n = 0;
    props[n].cmd = DTV_CLEAR; n++;
    props[n].cmd = DTV_DELIVERY_SYSTEM;
    props[n].u.data = (cmd.deliverySystem == SYS_DVB_S2) ? SYS_DVBS2 : SYS_DVBS; n++;
    props[n].cmd = DTV_FREQUENCY;
    props[n].u.data = cmd.frequencyKhz; n++;
    props[n].cmd = DTV_SYMBOL_RATE;
    props[n].u.data = cmd.symbolRateBps; n++;
    props[n].cmd = DTV_INVERSION;
    props[n].u.data = INVERSION_AUTO; n++;
    props[n].cmd = DTV_TUNE; n++;

    dtvProp.num = n;
    dtvProp.props = props;
    if (ioctl(feFd_, FE_SET_PROPERTY, &dtvProp) < 0) {
        perror("[TBS Server] FE_SET_PROPERTY Satellite tune failed");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool DvbFrontend::tuneCable(const CmdTuneCable& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentFreqHz_ = cmd.frequencyHz;
    currentDelSys_ = SYS_DVB_C;

    std::cout << "[TBS Server] Tuning Cable: Freq=" << (cmd.frequencyHz / 1e6) << " MHz\n";
    if (isSimulated_) return true;

#ifdef __linux__
    if (feFd_ < 0) return false;
    struct dtv_property props[6];
    struct dtv_properties dtvProp;
    std::memset(props, 0, sizeof(props));
    std::memset(&dtvProp, 0, sizeof(dtvProp));

    int n = 0;
    props[n].cmd = DTV_CLEAR; n++;
    props[n].cmd = DTV_DELIVERY_SYSTEM; props[n].u.data = SYS_DVBC_ANNEX_A; n++;
    props[n].cmd = DTV_FREQUENCY; props[n].u.data = cmd.frequencyHz; n++;
    props[n].cmd = DTV_MODULATION; props[n].u.data = (cmd.modulation == 1) ? QAM_256 : QAM_64; n++;
    props[n].cmd = DTV_SYMBOL_RATE; props[n].u.data = cmd.symbolRateBps; n++;
    props[n].cmd = DTV_TUNE; n++;

    dtvProp.num = n;
    dtvProp.props = props;
    return ioctl(feFd_, FE_SET_PROPERTY, &dtvProp) >= 0;
#else
    return false;
#endif
}

RespTelemetry DvbFrontend::getTelemetry() {
    std::lock_guard<std::mutex> lock(mutex_);
    RespTelemetry telem;
    std::memset(&telem, 0, sizeof(telem));

    telem.deliverySystem = currentDelSys_;
    telem.frequencyHz = currentFreqHz_;

    if (isSimulated_) {
        // High quality simulated DVB-T2 / ISDB-T signal metrics
        telem.lockFlags = FE_HAS_SIGNAL_FLAG | FE_HAS_CARRIER_FLAG | FE_HAS_VITERBI_FLAG | FE_HAS_SYNC_FLAG | FE_HAS_LOCK_FLAG | FE_IS_SIMULATED_FLAG;
        telem.signalStrengthPct = 88;
        telem.signalStrengthDbm = -42;
        telem.snrTenthsDb = 278; // 27.8 dB
        telem.berNumerator = 0;
        telem.berDenominator = 1000000;
        telem.uncorrectedBlocks = 0;
        telem.tsBytesPerSec = 2450000; // ~2.45 MB/s transport stream
        return telem;
    }

#ifdef __linux__
    if (feFd_ >= 0) {
        fe_status_t status = (fe_status_t)0;
        if (ioctl(feFd_, FE_READ_STATUS, &status) == 0) {
            if (status & FE_HAS_SIGNAL)  telem.lockFlags |= FE_HAS_SIGNAL_FLAG;
            if (status & FE_HAS_CARRIER) telem.lockFlags |= FE_HAS_CARRIER_FLAG;
            if (status & FE_HAS_VITERBI) telem.lockFlags |= FE_HAS_VITERBI_FLAG;
            if (status & FE_HAS_SYNC)    telem.lockFlags |= FE_HAS_SYNC_FLAG;
            if (status & FE_HAS_LOCK)    telem.lockFlags |= FE_HAS_LOCK_FLAG;
        }

        uint16_t rawStrength = 0;
        if (ioctl(feFd_, FE_READ_SIGNAL_STRENGTH, &rawStrength) == 0) {
            telem.signalStrengthPct = static_cast<uint16_t>((rawStrength * 100) / 65535);
            telem.signalStrengthDbm = static_cast<int16_t>(-95 + (telem.signalStrengthPct * 65 / 100));
        }

        uint16_t rawSnr = 0;
        if (ioctl(feFd_, FE_READ_SNR, &rawSnr) == 0) {
            // Standard mapping: 65535 is ~32 dB SNR
            float snrDb = (static_cast<float>(rawSnr) / 65535.0f) * 32.0f;
            telem.snrTenthsDb = static_cast<uint16_t>(snrDb * 10.0f);
        }

        uint32_t rawBer = 0;
        if (ioctl(feFd_, FE_READ_BER, &rawBer) == 0) {
            telem.berNumerator = rawBer;
            telem.berDenominator = 1000000;
        }

        uint32_t rawUnc = 0;
        if (ioctl(feFd_, FE_READ_UNCORRECTED_BLOCKS, &rawUnc) == 0) {
            telem.uncorrectedBlocks = rawUnc;
        }
    }
#endif
    return telem;
}

ssize_t DvbFrontend::readTsData(uint8_t* buffer, size_t maxBytes) {
    if (isSimulated_) {
        generateSimulatedTs(buffer, maxBytes);
        return static_cast<ssize_t>(maxBytes);
    }

#ifdef __linux__
    if (dvrFd_ < 0 || maxBytes < 188) return 0;

    // Read available raw data from DVR into persistent accumulation buffer
    constexpr size_t MAX_ACCUM = 512 * 1024;
    if (tsRemainder_.size() < MAX_ACCUM) {
        uint8_t temp[32768];
        ssize_t n = read(dvrFd_, temp, sizeof(temp));
        if (n > 0) {
            tsRemainder_.insert(tsRemainder_.end(), temp, temp + n);
        }
    }

    if (tsRemainder_.empty()) {
        return 0;
    }

    // Synchronize to 0x47 TS packet cadence:
    // Search for sync lock where tsRemainder_[i] == 0x47 AND tsRemainder_[i + 188] == 0x47
    size_t syncIdx = 0;
    bool synced = false;
    while (syncIdx + 188 < tsRemainder_.size()) {
        if (tsRemainder_[syncIdx] == 0x47 && tsRemainder_[syncIdx + 188] == 0x47) {
            synced = true;
            break;
        }
        syncIdx++;
    }

    if (syncIdx > 0) {
        // Discard unaligned prefix bytes before sync lock
        tsRemainder_.erase(tsRemainder_.begin(), tsRemainder_.begin() + syncIdx);
    }

    if (!synced || tsRemainder_.size() < 188) {
        if (tsRemainder_.size() > MAX_ACCUM / 2) {
            tsRemainder_.clear(); // Reset if prolonged invalid data
        }
        return 0;
    }

    // Now tsRemainder_[0] is GUARANTEED to be 0x47!
    size_t completeBytes = (tsRemainder_.size() / 188) * 188;
    if (completeBytes > maxBytes) {
        completeBytes = (maxBytes / 188) * 188;
    }

    if (completeBytes > 0) {
        std::memcpy(buffer, tsRemainder_.data(), completeBytes);
        tsRemainder_.erase(tsRemainder_.begin(), tsRemainder_.begin() + completeBytes);
    }

    return static_cast<ssize_t>(completeBytes);
#endif
    return 0;
}

void DvbFrontend::initSimulation() {
    simPatCc_ = 0;
    simPmtCc_ = 0;
    simSdtCc_ = 0;
    simVidCc_ = 0;
    simAudCc_ = 0;
    simSampleIndex_ = 0;
}

// Generates valid 188-byte DVB MPEG-TS packets (PAT, PMT, SDT and dummy Video PES)
void DvbFrontend::generateSimulatedTs(uint8_t* buffer, size_t bytes) {
    size_t numPackets = bytes / 188;
    for (size_t p = 0; p < numPackets; ++p) {
        uint8_t* pkt = buffer + (p * 188);
        std::memset(pkt, 0xFF, 188); // Fill with stuffing by default
        pkt[0] = 0x47;               // DVB TS Sync Byte

        size_t packetIdx = (simSampleIndex_ + p) % 100;

        if (packetIdx == 0) {
            // PAT (Program Association Table) - PID 0x0000
            pkt[1] = 0x40; // Payload unit start + PID 0
            pkt[2] = 0x00;
            pkt[3] = 0x10 | (simPatCc_++ & 0x0F); // Payload only + continuity counter
            pkt[4] = 0x00; // Pointer field

            // Table ID: 0x00 (PAT), Section syntax indicator: 1, Section len: 13
            uint8_t patData[] = {
                0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00,
                0x00, 0x01, 0xE0, 0x64, // Prog 1 -> PMT PID 0x0064 (100)
                0x28, 0x5D, 0x7E, 0x1A  // CRC32
            };
            std::memcpy(pkt + 5, patData, sizeof(patData));
        } else if (packetIdx == 1) {
            // SDT (Service Description Table) - PID 0x0011
            pkt[1] = 0x40; // Payload start
            pkt[2] = 0x11; // PID 0x0011
            pkt[3] = 0x10 | (simSdtCc_++ & 0x0F);
            pkt[4] = 0x00;

            // SDT Table ID: 0x42, Program 1: Name "TBS-5580 UHD Live", Provider "TBS-Networks"
            const char provName[] = "TBS-Networks";
            const char sName[] = "TBS-5580 UHD Live";
            uint8_t provLen = sizeof(provName) - 1;
            uint8_t sLen = sizeof(sName) - 1;
            uint8_t descLen = 3 + provLen + sLen;
            uint8_t sdtHdr[] = {
                0x42, 0xF0, static_cast<uint8_t>(17 + descLen), 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xFF,
                0x00, 0x01, 0xFC, 0x80, static_cast<uint8_t>(descLen + 5),
                0x48, static_cast<uint8_t>(descLen), 0x19, // Service type 0x19 = Advanced Codec HD TV
                provLen
            };
            size_t off = 5;
            std::memcpy(pkt + off, sdtHdr, sizeof(sdtHdr)); off += sizeof(sdtHdr);
            std::memcpy(pkt + off, provName, provLen); off += provLen;
            pkt[off++] = sLen;
            std::memcpy(pkt + off, sName, sLen); off += sLen;
        } else if (packetIdx == 2) {
            // PMT (Program Map Table) - PID 0x0064 (100)
            pkt[1] = 0x40;
            pkt[2] = 0x64; // PID 0x0064
            pkt[3] = 0x10 | (simPmtCc_++ & 0x0F);
            pkt[4] = 0x00;

            // Stream types: 0x1B (H.264 Video) on PID 0x0100 (256), 0x0F (AAC Audio) on PID 0x0101 (257)
            uint8_t pmtData[] = {
                0x02, 0xB0, 0x17, 0x00, 0x01, 0xC1, 0x00, 0x00,
                0xE1, 0x00, 0xF0, 0x00, // PCR PID 0x0100
                0x1B, 0xE1, 0x00, 0xF0, 0x00, // Stream 1: H.264 Video (PID 0x0100)
                0x0F, 0xE1, 0x01, 0xF0, 0x00, // Stream 2: AAC Audio (PID 0x0101)
                0x3C, 0x56, 0x81, 0x24  // CRC32
            };
            std::memcpy(pkt + 5, pmtData, sizeof(pmtData));
        } else if (packetIdx >= 3 && packetIdx < 85) {
            // Video PES Packets (PID 0x0100)
            pkt[1] = (packetIdx == 3) ? 0x41 : 0x01; // Payload start on first packet
            pkt[2] = 0x00; // PID 0x0100
            pkt[3] = 0x10 | (simVidCc_++ & 0x0F);

            if (packetIdx == 3) {
                // PES Header: 0x000001E0 (Video Stream 0)
                uint8_t pesHdr[] = { 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x80, 0x05, 0x21, 0x00, 0x01, 0x00, 0x01 };
                std::memcpy(pkt + 4, pesHdr, sizeof(pesHdr));
            }
        } else {
            // Audio PES Packets (PID 0x0101)
            pkt[1] = (packetIdx == 85) ? 0x41 : 0x01;
            pkt[2] = 0x01; // PID 0x0101
            pkt[3] = 0x10 | (simAudCc_++ & 0x0F);
        }
    }
    simSampleIndex_ += numPackets;
}

std::vector<SpectrumPoint> DvbFrontend::scanSpectrum(const CmdScanSpectrum& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SpectrumPoint> results;

    uint32_t start = cmd.startFreqHz;
    uint32_t stop = cmd.stopFreqHz;
    uint32_t step = cmd.stepHz;
    if (step == 0) step = 8000000;
    if (start >= stop) return results;

    size_t numPoints = (stop - start) / step + 1;
    if (numPoints > 1000) numPoints = 1000;
    results.reserve(numPoints);

    if (isSimulated_ || feFd_ < 0) {
        // Simulated spectrum with realistic broadcast peaks
        for (uint32_t f = start; f <= stop; f += step) {
            SpectrumPoint pt{};
            pt.frequencyHz = f;
            float freqMhz = f / 1e6f;

            // Base noise floor around -88 to -92 dBm
            float noise = -90.0f + static_cast<float>((f / 100000) % 7) * 0.5f;

            // Synthetic broadcast peaks on standard channels
            float peakSig = 0.0f;
            float peaks[] = { 184.5f, 212.5f, 482.0f, 530.0f, 578.0f, 650.0f, 722.0f };
            for (float p : peaks) {
                float dist = std::abs(freqMhz - p);
                if (dist < 4.0f) { // 8 MHz channel
                    float gain = (1.0f - (dist / 4.0f) * (dist / 4.0f)) * 48.0f;
                    if (gain > peakSig) peakSig = gain;
                }
            }

            float finalDbm = noise + peakSig;
            pt.signalStrengthDbm = static_cast<int16_t>(finalDbm);
            pt.signalStrengthPct = static_cast<uint16_t>(std::max(0.0f, std::min(100.0f, (finalDbm + 100.0f) * 1.5f)));

            if (peakSig > 30.0f) {
                pt.lockFlags = FE_HAS_SIGNAL_FLAG | FE_HAS_CARRIER_FLAG | FE_HAS_VITERBI_FLAG | FE_HAS_SYNC_FLAG | FE_HAS_LOCK_FLAG;
            } else if (peakSig > 10.0f) {
                pt.lockFlags = FE_HAS_SIGNAL_FLAG | FE_HAS_CARRIER_FLAG;
            } else {
                pt.lockFlags = 0;
            }

            results.push_back(pt);
        }
        return results;
    }

#ifdef __linux__
    for (uint32_t f = start; f <= stop; f += step) {
        struct dtv_property props[3];
        std::memset(props, 0, sizeof(props));

        props[0].cmd = DTV_DELIVERY_SYSTEM;
        props[0].u.data = cmd.deliverySystem;
        props[1].cmd = DTV_FREQUENCY;
        props[1].u.data = f;
        props[2].cmd = DTV_TUNE;

        struct dtv_properties cmdseq;
        cmdseq.num = 3;
        cmdseq.props = props;

        ioctl(feFd_, FE_SET_PROPERTY, &cmdseq);
        usleep(8000); // 8ms settle

        uint16_t strength = 0;
        ioctl(feFd_, FE_READ_SIGNAL_STRENGTH, &strength);

        fe_status_t status = (fe_status_t)0;
        ioctl(feFd_, FE_READ_STATUS, &status);

        SpectrumPoint pt{};
        pt.frequencyHz = f;
        pt.signalStrengthPct = (strength * 100) / 65535;
        pt.signalStrengthDbm = static_cast<int16_t>(-100.0f + (static_cast<float>(strength) / 65535.0f) * 90.0f);
        pt.lockFlags = static_cast<uint8_t>(status & 0x1F);

        results.push_back(pt);
    }

    // Restore original tuned frequency
    struct dtv_property rProps[3];
    std::memset(rProps, 0, sizeof(rProps));
    rProps[0].cmd = DTV_DELIVERY_SYSTEM;
    rProps[0].u.data = currentDelSys_;
    rProps[1].cmd = DTV_FREQUENCY;
    rProps[1].u.data = currentFreqHz_;
    rProps[2].cmd = DTV_TUNE;
    struct dtv_properties rCmdseq;
    rCmdseq.num = 3;
    rCmdseq.props = rProps;
    ioctl(feFd_, FE_SET_PROPERTY, &rCmdseq);
#endif

    return results;
}

} // namespace TBS

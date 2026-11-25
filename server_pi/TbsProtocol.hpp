#pragma once

#include <cstdint>

namespace TBS {

constexpr uint32_t TBS_MAGIC = 0x54425331; // "TBS1"
constexpr uint16_t DEFAULT_PORT = 1235;

// Packet command and response types
enum MessageType : uint16_t {
    // Client -> Server Commands
    CMD_TUNE_TERRESTRIAL   = 0x0001,
    CMD_TUNE_SATELLITE     = 0x0002,
    CMD_TUNE_CABLE         = 0x0003,
    CMD_START_STREAM       = 0x0010,
    CMD_STOP_STREAM        = 0x0011,
    CMD_GET_TELEMETRY      = 0x0012,
    CMD_SCAN_SPECTRUM      = 0x0020,

    // Server -> Client Responses
    RESP_STATUS            = 0x8001,
    RESP_TELEMETRY         = 0x8002,
    DATA_MPEG_TS           = 0x8003,
    RESP_SPECTRUM_SCAN     = 0x8020
};

// Delivery systems matching Linux DVB API v5
enum DeliverySystem : uint8_t {
    SYS_DVB_T    = 0,
    SYS_DVB_T2   = 1,
    SYS_ISDB_T   = 2,
    SYS_DVB_S    = 3,
    SYS_DVB_S2   = 4,
    SYS_DVB_C    = 5
};

#pragma pack(push, 1)

struct MsgHeader {
    uint32_t magic = TBS_MAGIC;
    uint16_t msgType = 0;
    uint32_t payloadLen = 0;
};

// Command payload: Terrestrial (DVB-T / DVB-T2 / ISDB-T)
struct CmdTuneTerrestrial {
    uint8_t  deliverySystem; // SYS_DVB_T, SYS_DVB_T2, or SYS_ISDB_T
    uint32_t frequencyHz;    // e.g. 546000000 (546.0 MHz)
    uint32_t bandwidthHz;    // e.g. 8000000 (8 MHz), 6000000, 7000000
    int32_t  plpId;          // DVB-T2 Physical Layer Pipe ID (-1 for auto/any)
};

// Command payload: Satellite (DVB-S / DVB-S2 / S2X)
struct CmdTuneSatellite {
    uint8_t  deliverySystem;     // SYS_DVB_S or SYS_DVB_S2
    uint32_t frequencyKhz;        // e.g. 11766000 kHz or IF 1166000 kHz
    uint32_t symbolRateBps;      // e.g. 27500000 (27.5 MSps)
    uint8_t  polarization;       // 0: Horizontal (18V), 1: Vertical (13V)
    uint8_t  tone22k;            // 0: Off, 1: On (High band)
    uint8_t  diseqcPort;         // 0: Port A, 1: Port B, 2: Port C, 3: Port D
};

// Command payload: Cable (DVB-C)
struct CmdTuneCable {
    uint32_t frequencyHz;        // e.g. 306000000
    uint32_t symbolRateBps;      // e.g. 6900000
    uint8_t  modulation;         // 0: QAM64, 1: QAM256
};

// Command payload: Spectrum Scan
struct CmdScanSpectrum {
    uint8_t  deliverySystem;     // SYS_DVB_T, SYS_DVB_T2, etc.
    uint32_t startFreqHz;        // e.g. 470000000 (470 MHz)
    uint32_t stopFreqHz;         // e.g. 862000000 (862 MHz)
    uint32_t stepHz;             // e.g. 8000000 (8 MHz) or 1000000 (1 MHz)
};

// Hardware Lock Flags (from Linux DVB FE_READ_STATUS)
enum FeStatusFlags : uint8_t {
    FE_HAS_SIGNAL_FLAG   = 0x01, // Found something above noise floor
    FE_HAS_CARRIER_FLAG  = 0x02, // Found a DVB carrier
    FE_HAS_VITERBI_FLAG  = 0x04, // FEC is stable
    FE_HAS_SYNC_FLAG     = 0x08, // Synchronized with packet sync bytes
    FE_HAS_LOCK_FLAG     = 0x10, // Fully locked and producing valid MPEG-TS!
    FE_IS_SIMULATED_FLAG = 0x80  // Server running in simulated fallback mode (hardware not opened)
};

// Response payload: RF Telemetry & Signal Quality
struct RespTelemetry {
    uint8_t  deliverySystem;
    uint32_t frequencyHz;
    uint8_t  lockFlags;          // Bitmask of FeStatusFlags
    uint16_t signalStrengthPct;  // 0 - 100 %
    int16_t  signalStrengthDbm;  // e.g. -45 dBm
    uint16_t snrTenthsDb;        // SNR in tenths of dB (e.g. 245 = 24.5 dB)
    uint32_t berNumerator;       // Bit error rate
    uint32_t berDenominator;
    uint32_t uncorrectedBlocks;  // UNC error packet count
    uint32_t tsBytesPerSec;      // Current MPEG-TS stream bitrate
};

// Spectrum scan point
struct SpectrumPoint {
    uint32_t frequencyHz;
    int16_t  signalStrengthDbm;
    uint16_t signalStrengthPct;
    uint8_t  lockFlags;
};

// Header before array of SpectrumPoint
struct RespSpectrumScanHeader {
    uint32_t startFreqHz;
    uint32_t stopFreqHz;
    uint32_t stepHz;
    uint32_t numPoints;
};

// Generic response status
struct RespStatus {
    int32_t  errorCode;          // 0 = Success, < 0 = Error
    char     message[128];       // Human-readable status/error description
};

#pragma pack(pop)

} // namespace TBS

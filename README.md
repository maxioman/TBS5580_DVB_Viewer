# TBS-5580 Multi-Standard DVB Receiver & macOS Network Viewer

A native, high-performance C++17 client and Raspberry Pi bridge daemon for the **TBS-5580 multi-standard USB TV Tuner box** (DVB-S2/S, DVB-T2/T, DVB-C, ISDB-T).

Designed to decouple RF hardware from the display workstation: the **Raspberry Pi** connects to the tuner hardware, while the **macOS client** provides hardware-accelerated (Apple VideoToolbox) broadcast video decoding, native CoreAudio sound, and professional RF telemetry instrumentation.

---

## Architecture Overview

```
                      +---------------------------------------+
                      |       Raspberry Pi 4 / 5              |
                      |   [TBS-5580 USB Multi-Standard Tuner] |
                      |    /dev/dvb/adapter0/frontend0        |
                      |    /dev/dvb/adapter0/dvr0             |
                      +-------------------+-------------------+
                                          |
                         tbs_bridge (TCP Port 1235)
                         • DVB API v5 Tuning Control
                         • 5 Hz RF Telemetry (SNR, BER, UNC)
                         • Non-blocking 188-byte TS Stream
                                          |  (Wi-Fi / Ethernet)
                                          v
                      +---------------------------------------+
                      |         macOS Workstation             |
                      |       TBS5580_DVB_Viewer              |
                      +-------------------+-------------------+
                                          |
        +---------------------------------+---------------------------------+
        |                                 |                                 |
        v                                 v                                 v
 [TsParser]                       [VideoDecoder]                    [AudioPlayer]
 • PAT / PMT / SDT Parsing        • Apple VideoToolbox HW Decode   • macOS AudioQueue
 • Channel Name & Provider        • H.264 / HEVC / MPEG-2          • 48 kHz 16-bit PCM
 • Stream Demuxing & Filtering    • OpenGL RGBA Texture Viewport   • Volume & Mute
```

---

## Key Features

1. **Multi-Standard Front-End Tuning:**
   - **Terrestrial:** DVB-T, DVB-T2 (with Physical Layer Pipe / PLP ID selection), ISDB-T (6, 7, 8 MHz bandwidth).
   - **Satellite:** DVB-S, DVB-S2 (QPSK, 8PSK, 16APSK), DiSEqC 1.0 (Ports A/B/C/D), 22 kHz tone (High/Low band), Horizontal (18V) / Vertical (13V) LNB polarizations.
   - **Cable:** DVB-C (64-QAM, 256-QAM).
2. **Real-Time RF Telemetry & Signal Quality Instrumentation:**
   - Linux DVB API v5 hardware lock indicators: `[SIGNAL]`, `[CARRIER]`, `[VITERBI]`, `[SYNC]`, `[LOCK]`.
   - Continuous Signal Strength metering (0–100% and calibrated dBm).
   - Modulation Error Ratio / SNR in dB with live ImPlot timeline history.
   - Bit Error Rate (BER), Uncorrected packet counters (UNC), and TS stream bitrate (Mbps).
3. **MPEG-TS Demuxer & Service Explorer:**
   - Live discovery of multiplex channels via DVB SI/PSI tables:
     - **PAT** (Program Association Table): Program numbers and PMT PIDs.
     - **PMT** (Program Map Table): Video/Audio elementary stream identification (H.264, HEVC, MPEG-2, AAC, AC-3, MP2).
     - **SDT** (Service Description Table): Channel names, providers, and service types (HD TV, UHD TV, SD TV, Radio).
   - One-click channel switching from the service table.
4. **Hardware-Accelerated Video Pipeline:**
   - macOS **VideoToolbox** hardware decoding for H.264 and HEVC (H.265) up to 4K 60 FPS with near-zero CPU usage.
   - OpenGL 3.2 Core texture viewport with automatic 16:9 / native aspect ratio scaling.
5. **Native macOS CoreAudio Output:**
   - Real-time low-latency audio via `AudioToolbox` / `AudioQueue`.
   - Automatic resampling to 48 kHz stereo 16-bit PCM with master volume and mute controls.

---

## Directory Structure

```
15_TBS5580_DVB_Viewer/
├── CMakeLists.txt            # Build configuration for macOS client & Pi daemon
├── server_pi/                # Lightweight Raspberry Pi daemon
│   ├── DvbFrontend.hpp/.cpp  # Linux DVB API v5 ioctl controller & simulator
│   ├── TbsProtocol.hpp       # Binary framing protocol (Magic: TBS1, Port 1235)
│   └── main.cpp              # Multi-threaded TCP streaming server
├── src/                      # macOS Client source code
│   ├── audio/AudioPlayer.*   # CoreAudio AudioQueue PCM player
│   ├── net/TbsClient.*       # TCP client with non-blocking reception
│   ├── net/TbsProtocol.hpp   # Protocol definitions
│   ├── ts/TsParser.*         # 188-byte TS packet parser, PAT/PMT/SDT explorer
│   ├── ui/MainWindow.*       # Dear ImGui / ImPlot user interface
│   ├── video/VideoDecoder.*  # VideoToolbox HW video decoder & OpenGL texture
│   └── main.cpp              # Application entry point & GLFW event loop
└── vendor/                   # Vendored GUI libraries
    ├── imgui/                # Dear ImGui v1.91
    └── implot/               # ImPlot v0.17
```

---

## Setup & Running on Raspberry Pi

### 1. Requirements
- Raspberry Pi 4B or 5 running Raspberry Pi OS (64-bit recommended).
- TBS-5580 connected to a USB 3.0 / 2.0 port with external 12V power supply connected.
- Linux DVB drivers:
  ```bash
  sudo apt update
  sudo apt install -y build-essential cmake git dvb-tools dvb-apps
  # Verify TBS5580 is recognized:
  ls -l /dev/dvb/adapter*
  ```

### 2. Build the Bridge Daemon
Copy the `server_pi` folder or the entire repository to your Pi:
```bash
cd 15_TBS5580_DVB_Viewer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target tbs_bridge -j$(nproc)
```

### 3. Run the Daemon
```bash
./tbs_bridge 1235 0
```
*(Arguments: `<port>` `<dvb_adapter_index>`, defaults to 1235 and adapter 0).*

---

## Setup & Running on macOS

### 1. Prerequisites
Install dependencies via Homebrew:
```bash
brew install cmake pkg-config glfw ffmpeg
```

### 2. Build
```bash
cd 15_TBS5580_DVB_Viewer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target TBS5580_DVB_Viewer -j8
```

### 3. Run
```bash
./build/TBS5580_DVB_Viewer
```

1. Enter your Raspberry Pi's IP address (e.g. `192.168.1.27`) and port `1235` in the top bar.
2. Click **Connect**.
3. Select your desired delivery system tab (**Terrestrial**, **Satellite**, or **Cable**).
4. Enter frequency and modulation parameters, then click **Tune & Lock**.
5. Once locked, the service explorer will automatically populate with all discovered TV and radio channels. Click **Watch** on any channel to begin live playback!

# ESP32 Drone Detector — Installation Guide

Hardware setup and flashing instructions for the Friend or Foe ESP32 edition.

## Hardware Required

| Component | Part | ~Cost | Notes |
|-----------|------|-------|-------|
| Scanner MCU | ESP32-S3-DevKitC-1 (8MB PSRAM) | $8-10 | Must be S3 variant for BLE+WiFi coexistence |
| Uplink MCU | ESP32-C3-DevKitM-1 | $3-5 | Any C3 dev board works |
| GPS module | u-blox NEO-6M (or NEO-M8N) | $8-12 | 3.3V TTL UART output |
| OLED display | SSD1306 0.96" 128x64 I2C | $3-5 | I2C address 0x3C |
| Status LED | Any standard LED + 220 ohm resistor | $0.50 | Or a WS2812B NeoPixel |
| Interconnect | 3 jumper wires (TX, RX, GND) | $0.50 | Between the two boards |
| Power | USB-C cable (one per board during dev) | — | Or shared 5V supply |
| Optional | 18650 battery + TP4056 charger | $5-8 | For portable operation |

**Total: ~$25-40**

## Wiring

```
Scanner ESP32-S3                  Uplink ESP32-C3
┌──────────────┐                  ┌──────────────┐
│         TX ──┼── GPIO 17 ──────►│ GPIO 20 ── RX│
│         RX ──┼── GPIO 18 ◄──────┤ GPIO 21 ── TX│
│        GND ──┼─────────────────┼── GND        │
└──────────────┘                  └──────────────┘

Uplink ESP32-C3 Peripherals:
  GPIO 4 (SDA) ───► SSD1306 OLED SDA
  GPIO 5 (SCL) ───► SSD1306 OLED SCL
  GPIO 6 (TX)  ───► GPS module RX
  GPIO 7 (RX)  ◄─── GPS module TX
  GPIO 8       ───► Status LED (+ 220Ω → GND)
  GPIO 3 (ADC) ◄─── Battery voltage divider midpoint
  3.3V         ───► OLED VCC, GPS VCC
  GND          ───► OLED GND, GPS GND, LED GND
```

### Battery Voltage Divider (optional)
If monitoring battery voltage, connect a voltage divider (two equal resistors, e.g., 100K + 100K) between battery positive and GND, with the midpoint wired to Uplink GPIO 3.

## Web Flasher (No PlatformIO Required)

The easiest way to flash is with the **browser-based web flasher** — no toolchain installation needed.

1. Open [`web-flasher/index.html`](web-flasher/index.html) in **Google Chrome** or **Microsoft Edge**
2. Connect your ESP32-S3, click **"Flash Scanner Firmware"**
3. Disconnect, connect your ESP32-C3, click **"Flash Uplink Firmware"**

Requirements:
- Chrome 89+ or Edge 89+ (Firefox/Safari not supported — Web Serial API)
- A data-capable USB cable (not charge-only)
- Pre-built firmware binaries in `web-flasher/firmware/` (run `web-flasher/build.sh` or download from GitHub Releases)

For local testing, serve the page with: `cd esp32/web-flasher && python3 -m http.server 8080`, then open `http://localhost:8080`.

If you prefer building from source with full control, continue with the PlatformIO instructions below.

## Software Prerequisites (PlatformIO)

1. **PlatformIO** — install via VS Code extension or CLI:
   ```bash
   pip install platformio
   ```

2. **ESP-IDF toolchain** — PlatformIO downloads this automatically on first build.

3. **USB drivers** — your OS needs drivers for the CP2102 or CH340 USB-to-serial chip on your dev boards. Most modern OSes include these.

## Configuration

Before flashing, edit the uplink configuration:

```bash
nano esp32/uplink/main/core/config.h
```

Set your WiFi network and backend server:
```c
#define CONFIG_WIFI_SSID        "YourWiFiName"
#define CONFIG_WIFI_PASSWORD    "YourWiFiPassword"
#define CONFIG_BACKEND_URL      "http://192.168.1.100:8000"
#define CONFIG_DEVICE_ID        "fof_esp32_001"
```

These can also be changed at runtime via NVS (serial console commands — future feature).

## Building & Flashing

### Scanner (ESP32-S3)

```bash
cd esp32/scanner

# Build
pio run

# Flash (connect S3 board via USB)
pio run -t upload

# Monitor serial output
pio device monitor
```

### Uplink (ESP32-C3)

```bash
cd esp32/uplink

# Build
pio run

# Flash (connect C3 board via USB)
pio run -t upload

# Monitor serial output (console is on USB-JTAG, not UART0)
pio device monitor
```

Note: The C3's serial console uses the built-in USB-JTAG interface, not UART0. UART0 is dedicated to the GPS module. Connect via the USB-C port on the C3 dev board.

### Unit Tests (no hardware needed)

```bash
cd esp32

# Run all tests on your computer (native build)
pio test -e test
```

## Backend Setup

The ESP32 uplink sends detections to your existing Friend or Foe backend. Make sure it's running:

```bash
cd backend
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Or with Docker:
```bash
cd backend
docker compose up
```

The new endpoints are:
- `POST /detections/drones` — receives batched detections from ESP32 nodes
- `GET /detections/drones/recent?limit=100` — view recent detections

Verify it's working:
```bash
curl http://localhost:8000/health
curl http://localhost:8000/detections/drones/recent
```

## First Power-On Checklist

1. Flash both boards (scanner and uplink)
2. Wire TX/RX/GND between boards
3. Connect GPS module to uplink
4. Connect OLED to uplink
5. Power on both boards (USB or shared 5V)
6. Watch uplink serial monitor for:
   ```
   Friend or Foe -- Uplink v0.7.0-beta
   WiFi connected: 192.168.1.42
   SNTP synchronized
   GPS fix acquired (sats=7)
   Scanner status: BLE=0 WiFi=0 ch=1 uptime=5s
   ```
7. Watch scanner serial monitor for:
   ```
   Friend or Foe -- Scanner v0.7.0-beta
   WiFi scanner started on core 0
   BLE Remote ID scanner started on core 0
   UART1 -> Uplink @ 921600 baud
   ```

## Verification

### Test with a real drone
Power on a DJI drone nearby. Within seconds you should see:
- Scanner log: detection with DJI IE or BLE RID data
- Uplink OLED: drone count increments
- Backend: `GET /detections/drones/recent` returns the detection

### Test with a phone app
Install an OpenDroneID test app (e.g., "OpenDroneID Transmitter" on Android) to broadcast test BLE Remote ID beacons. The scanner should detect these immediately.

### Test WiFi SSID detection
Create a mobile hotspot named `DJI-TEST-12345`. The scanner should detect the SSID pattern match within one channel hop cycle (~1.3 seconds).

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Scanner builds but no WiFi detections | Channel hopping not starting | Check scanner serial log for WiFi init errors |
| Uplink can't connect to WiFi | Wrong credentials | Edit `config.h` and reflash, or set via NVS |
| OLED blank | Wrong I2C address | Most SSD1306 boards are 0x3C; some are 0x3D — check with I2C scanner |
| GPS no fix | Indoor or cold start | Move outdoors, wait 1-2 minutes for first fix |
| No detections reaching backend | WiFi disconnect or wrong URL | Check uplink log for HTTP errors, verify backend URL |
| Scanner log shows BLE errors | BLE init failed | Ensure you're using ESP32-S3 (not plain ESP32 or S2) |
| Upload failures | Backend not running | Start backend with `uvicorn app.main:app` |
| `uart_rx: JSON parse error` | Baud rate mismatch | Both boards must be at 921600 — check wiring |

## Architecture Overview

```
┌──────────────────────────┐   UART 921600   ┌──────────────────────────┐
│   ESP32-S3 "SCANNER"     │ ──────────────► │   ESP32-C3 "UPLINK"      │
│                          │  JSON messages   │                          │
│  Core 0: WiFi promisc    │                  │  WiFi STA (always on)    │
│          BLE scan        │                  │  HTTP POST → backend     │
│  Core 1: Bayesian fusion │                  │  GPS, OLED, LED, Battery │
│          UART TX         │                  │  UART RX                 │
│                          │                  │                          │
│  Never leaves scan mode  │                  │  Uploads every 5 seconds │
└──────────────────────────┘                  └──────────────────────────┘
                                                       │
                                                       ▼
                                              ┌────────────────┐
                                              │  FastAPI Backend │
                                              │  /detections/   │
                                              │  drones         │
                                              └────────────────┘
```

## Directory Layout

```
esp32/
├── shared/                 # Headers shared between both boards
│   ├── constants.h         # Bayesian, ODID, DJI/ASTM constants
│   ├── detection_types.h   # drone_detection_t struct
│   └── uart_protocol.h     # JSON keys, UART config
│
├── scanner/                # ESP32-S3 scanner firmware
│   ├── platformio.ini      # PlatformIO build config
│   ├── CMakeLists.txt      # ESP-IDF project root
│   ├── sdkconfig.defaults  # NimBLE, WiFi coexist, PSRAM
│   ├── partitions.csv
│   └── main/
│       ├── main.c
│       ├── detection/      # All detection modules
│       │   ├── ble_remote_id.c/h
│       │   ├── open_drone_id_parser.c/h
│       │   ├── dji_drone_id_parser.c/h
│       │   ├── wifi_beacon_rid_parser.c/h
│       │   ├── wifi_scanner.c/h
│       │   ├── wifi_ssid_patterns.c/h
│       │   ├── wifi_oui_database.c/h
│       │   └── bayesian_fusion.c/h
│       ├── comms/
│       │   └── uart_tx.c/h
│       └── core/
│           └── task_priorities.h
│
├── uplink/                 # ESP32-C3 uplink firmware
│   ├── platformio.ini
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults  # Console on USB-JTAG
│   ├── partitions.csv
│   └── main/
│       ├── main.c
│       ├── comms/          # UART RX, WiFi, HTTP upload
│       ├── hw/             # GPS, OLED, battery, LED
│       ├── network/        # SNTP time sync
│       └── core/           # Config, NVS, ring buffer
│
└── test/                   # Native unit tests (no hardware)
    ├── test_open_drone_id_parser.c
    ├── test_dji_drone_id_parser.c
    ├── test_bayesian_fusion.c
    └── test_ssid_patterns.c
```

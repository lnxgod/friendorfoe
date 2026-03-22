# ESP32 Drone Detector — Installation Guide

Hardware setup and flashing instructions for the Friend or Foe ESP32 edition.

## Hardware Required

| Component | Part | ~Cost | Notes |
|-----------|------|-------|-------|
| Scanner MCU | ESP32-S3-DevKitC-1 (8MB PSRAM) | $8-10 | Must be S3 variant for BLE+WiFi coexistence |
| Scanner MCU (alt) | ESP32-C5 dev board | $8-12 | Alternative scanner — adds 5 GHz WiFi scanning |
| Uplink MCU | ESP32-C3-DevKitM-1 | $3-5 | Any C3 dev board works |
| GPS module | u-blox NEO-6M (or NEO-M8N) | $8-12 | 3.3V TTL UART output |
| OLED display (x2) | SSD1306 0.96" 128x64 I2C | $3-5 each | I2C address 0x3C; one for scanner, one for uplink |
| Scanner LED | Built-in on DevKitC-1 (GPIO 48) | — | Or external LED + 220 ohm resistor |
| Uplink LED | Any standard LED + 220 ohm resistor | $0.50 | Or a WS2812B NeoPixel |
| Interconnect | 3 jumper wires (TX, RX, GND) | $0.50 | Between the two boards |
| Power | USB-C cable (one per board during dev) | — | Or shared 5V supply |
| Optional | 18650 battery + TP4056 charger | $5-8 | For portable operation |

**Total: ~$30-50**

## Wiring

### Scanner ↔ Uplink UART

```
Scanner ESP32-S3                  Uplink ESP32-C3
┌──────────────┐                  ┌──────────────┐
│         TX ──┼── GPIO 17 ──────►│ GPIO 20 ── RX│
│         RX ──┼── GPIO 18 ◄──────┤ GPIO 21 ── TX│
│        GND ──┼─────────────────┼── GND        │
└──────────────┘                  └──────────────┘
```

### Scanner ESP32-S3 Peripherals

```
Scanner ESP32-S3 Peripherals:
  GPIO 4 (SDA) ───► SSD1306 OLED SDA
  GPIO 5 (SCL) ───► SSD1306 OLED SCL
  GPIO 48      ───► Status LED (built-in on DevKitC-1)
  3.3V         ───► OLED VCC
  GND          ───► OLED GND
```

The S3 DevKitC-1 has a built-in addressable RGB LED on GPIO 48 — no external LED or resistor needed. The firmware drives it as a simple on/off indicator.

### Uplink ESP32-C3 Peripherals

```
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

### Scanner ESP32-C5 Variant (alternative, adds 5 GHz)

If using an ESP32-C5 as the scanner instead of an S3, the pin assignments differ because GPIO 4/5 are used for UART on the C5:

```
Scanner ESP32-C5:
  UART:  TX = GPIO 4,  RX = GPIO 5   (to Uplink GPIO 20/21)
  OLED:  SDA = GPIO 6, SCL = GPIO 7
  LED:   GPIO 27
```

The C5 is a RISC-V single-core chip with 2.4 + 5 GHz WiFi. It can detect drones operating on 5 GHz channels that the S3 cannot see. BLE scanning still works on the C5.

### Battery Voltage Divider (optional)
If monitoring battery voltage, connect a voltage divider (two equal resistors, e.g., 100K + 100K) between battery positive and GND, with the midpoint wired to Uplink GPIO 3.

## Boards with Built-in OLED (Heltec, etc.)

Some ESP32 dev boards have a built-in SSD1306 OLED that uses different I2C pins than the defaults. Override the pins via `platformio.ini` build flags:

```ini
# Example: Heltec WiFi Kit 32 V3 (ESP32-S3, built-in 0.96" OLED)
build_flags =
  -DCONFIG_FOF_OLED_SDA_PIN=17
  -DCONFIG_FOF_OLED_SCL_PIN=18
  -DCONFIG_FOF_OLED_RST_PIN=21
```

Add the `build_flags` to the appropriate `[env]` section in `esp32/scanner/platformio.ini` or `esp32/uplink/platformio.ini`.

### Known Board Pin Mappings

| Board | SDA | SCL | RST | Notes |
|-------|-----|-----|-----|-------|
| Generic SSD1306 breakout | 4 | 5 | -1 | Default, no reset pin |
| Heltec WiFi Kit 32 V3 (S3) | 17 | 18 | 21 | Built-in 0.96" OLED |
| Heltec WiFi LoRa 32 V3 (S3) | 17 | 18 | 21 | Same OLED as WiFi Kit |
| LILYGO T-Display-S3 | 43 | 44 | -1 | I2C on Qwiic connector |

Set `RST` to `-1` if the board has no hardware reset line for the OLED (most breakout modules).

The KConfig variables are defined in `scanner/main/Kconfig.projbuild` and `uplink/main/Kconfig.projbuild`. You can also override them via `idf.py menuconfig` under "FoF Scanner OLED" / "FoF Uplink OLED".

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

To run in **standalone mode** (no backend needed), leave `CONFIG_WIFI_SSID` as `"YourSSID"` or set it to `""`. See the [Standalone Mode](#standalone-mode-no-backend) section below.

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

## Standalone Mode (No Backend)

The uplink can run without a WiFi network or backend server. This is useful for field testing scanner hardware, demos, or portable operation.

**How to activate:** Leave `CONFIG_WIFI_SSID` as `"YourSSID"` (the factory default) or set it to an empty string `""` in `config.h`, then flash.

**What happens:**
- The uplink skips STA WiFi connection — no retry noise or upload failures
- The built-in AP starts as usual: SSID `FoF-XXYYZZ` (last 3 bytes of MAC), password `friendorfoe`
- The HTTP status page serves at `http://192.168.4.1`
- The web page shows a "Standalone Mode" banner and live scanner connection status
- Detections from the scanner are still received, decoded, and displayed — they just aren't uploaded

**To use:**
1. Flash both boards with `CONFIG_WIFI_SSID` left as `"YourSSID"`
2. Power on both boards and wire them together as usual
3. On your phone or laptop, connect to the `FoF-XXYYZZ` WiFi network (password: `friendorfoe`)
4. Open `http://192.168.4.1` in a browser
5. You'll see the live detection dashboard with drone counts, scanner status, and signal info

To switch back to normal mode, set a real SSID/password in `config.h` and reflash.

## Scanner-Only Mode (No Uplink)

The scanner board works as a standalone device — no uplink board, WiFi, or backend needed. UART writes silently drop when nothing is connected on the other end.

**What you get:**
- **OLED display** — firmware version, active drone count, channel, BLE/WiFi detection counts, total + uptime, and a persistent last-detection panel showing drone ID, manufacturer, RSSI with signal bars, and confidence percentage
- **LED patterns** — fast blink while scanning, solid 2s flash on each new detection, even without an OLED attached
- **Full detection pipeline** — WiFi promiscuous scanning, BLE Remote ID, Bayesian fusion all run normally

**Setup:**
1. Flash the scanner firmware onto an ESP32-S3 (or C5)
2. Connect an SSD1306 OLED (SDA→GPIO4, SCL→GPIO5; or GPIO6/7 on C5)
3. Power on via USB — the OLED shows stats immediately, LED blinks to confirm scanning
4. No wiring to an uplink board required

**Use cases:**
- Portable field testing with a battery pack
- Quick drone detection demo without full system setup
- Verifying scanner hardware and antenna placement before deploying with an uplink

## First Power-On Checklist

1. Flash both boards (scanner and uplink)
2. Wire TX/RX/GND between boards
3. Connect GPS module to uplink
4. Connect OLED to uplink (SDA→GPIO4, SCL→GPIO5)
5. Connect OLED to scanner (SDA→GPIO4, SCL→GPIO5; or GPIO6/7 on C5)
6. Power on both boards (USB or shared 5V)
7. **Scanner OLED** should show: `FOF Scanner`, drone count, channel, uptime
8. Watch uplink serial monitor for:
   ```
   Friend or Foe -- Uplink v0.10.0-beta
   WiFi connected: 192.168.1.42
   SNTP synchronized
   GPS fix acquired (sats=7)
   Scanner status: BLE=0 WiFi=0 ch=1 uptime=5s
   ```
9. Watch scanner serial monitor for:
   ```
   Friend or Foe -- Scanner v0.10.0-beta
   WiFi scanner started on core 0
   BLE Remote ID scanner started on core 0
   OLED initialized (SSD1306 128x64, SDA=4 SCL=5)
   LED initialized (GPIO48)
   UART1 -> Uplink @ 921600 baud
   ```

**Standalone mode alternative:** If running without a backend, skip WiFi config. The uplink log will show `Mode: STANDALONE (AP-only, no backend)` and `AP URL: http://192.168.4.1`. Connect your phone to the AP to view the status page.

## Verification

### Test with a real drone
Power on a DJI drone nearby. Within seconds you should see:
- Scanner log: detection with DJI IE or BLE RID data
- Scanner OLED: detection overlay with drone ID, manufacturer, RSSI
- Scanner LED: fast blink pattern on new detection
- Uplink OLED: drone count increments
- Backend: `GET /detections/drones/recent` returns the detection (or check `http://192.168.4.1` in standalone mode)

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
| OLED blank on board with built-in display | Wrong I2C pins | Override via build flags: `-DCONFIG_FOF_OLED_SDA_PIN=X -DCONFIG_FOF_OLED_SCL_PIN=Y` (see [Known Board Pin Mappings](#known-board-pin-mappings)) |
| GPS no fix | Indoor or cold start | Move outdoors, wait 1-2 minutes for first fix |
| No detections reaching backend | WiFi disconnect or wrong URL | Check uplink log for HTTP errors, verify backend URL |
| Scanner log shows BLE errors | BLE init failed | Ensure you're using ESP32-S3 or C5 (not plain ESP32 or S2) |
| Upload failures | Backend not running | Start backend with `uvicorn app.main:app`, or use standalone mode |
| `uart_rx: JSON parse error` | Baud rate mismatch | Both boards must be at 921600 — check wiring |
| Standalone mode not activating | SSID is set to a real network | Set `CONFIG_WIFI_SSID` to `"YourSSID"` or `""` in `config.h` and reflash |
| Standalone mode — can't reach status page | Not connected to AP | Connect to the `FoF-XXYYZZ` WiFi network, then open `http://192.168.4.1` |

## Architecture Overview

```
┌──────────────────────────┐   UART 921600   ┌──────────────────────────┐
│   ESP32-S3 "SCANNER"     │ ──────────────► │   ESP32-C3 "UPLINK"      │
│                          │  JSON messages   │                          │
│  Core 0: WiFi promisc    │                  │  WiFi APSTA              │
│          BLE scan        │                  │  HTTP POST → backend     │
│  Core 1: Bayesian fusion │                  │  HTTP status page (AP)   │
│          UART TX         │                  │  GPS, OLED, LED, Battery │
│  OLED + LED status       │                  │  UART RX                 │
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
│   └── uart_protocol.h     # JSON keys, UART config, pin assignments
│
├── scanner/                # ESP32-S3 (or C5) scanner firmware
│   ├── platformio.ini      # PlatformIO build config
│   ├── CMakeLists.txt      # ESP-IDF project root
│   ├── sdkconfig.defaults  # NimBLE, WiFi coexist, PSRAM
│   ├── partitions.csv
│   └── main/
│       ├── main.c
│       ├── Kconfig.projbuild   # OLED pin configuration (menuconfig)
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
│       ├── hw/             # Hardware drivers
│       │   ├── oled_display.c/h
│       │   └── led_status.c/h
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
│       ├── Kconfig.projbuild   # OLED pin configuration (menuconfig)
│       ├── comms/          # UART RX, WiFi STA, HTTP upload
│       ├── hw/             # GPS, OLED, battery, LED
│       ├── network/        # WiFi AP, HTTP status page, SNTP
│       └── core/           # Config, NVS, ring buffer
│
└── test/                   # Native unit tests (no hardware)
    ├── test_open_drone_id_parser.c
    ├── test_dji_drone_id_parser.c
    ├── test_bayesian_fusion.c
    └── test_ssid_patterns.c
```

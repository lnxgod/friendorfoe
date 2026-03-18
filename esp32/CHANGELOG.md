# ESP32 Drone Detector Changelog

All notable changes to the ESP32 hardware edition of Friend or Foe.

## [0.2.0-alpha] - 2026-03-18

### Added
- **Web Flasher** — browser-based firmware flashing via ESP Web Tools (Chrome/Edge). No PlatformIO installation needed. Separate install buttons for Scanner (S3) and Uplink (C3) with auto chip detection.
- **Detection Parity documentation** (`PARITY.md`) — detailed comparison of Android vs ESP32 detection capabilities with actual line counts and feature analysis.
- `build.sh` script to compile both firmwares and stage binaries for the web flasher.

## [0.1.0-alpha] - 2026-03-18

### Added

#### Scanner Firmware (ESP32-S3)
- BLE Remote ID scanner (ASTM F3411) via NimBLE — scans for UUID 0xFFFA, parses all 6 OpenDroneID message types (Basic ID, Location, Auth, Self-ID, System, Operator ID, Message Pack)
- WiFi promiscuous mode scanner — continuous raw 802.11 frame capture, no OS throttling
- DJI DroneID vendor IE parser (OUI 26:37:12) — extracts GPS, altitude, speed, heading, operator home point from beacon frames
- ASTM F3411 WiFi Beacon Remote ID parser (OUI FA:0B:BC, type 0x0D)
- SSID pattern matching database — 104 drone SSID prefixes covering DJI, Skydio, Parrot, Autel, HOVERAir, Holy Stone, Yuneec, and 30+ other manufacturers
- OUI lookup database — 29 known drone hardware OUIs (DJI, Parrot, Autel, Skydio, Yuneec, HOVERAir, Xiaomi, Hubsan, etc.)
- Bayesian sensor fusion engine — log-odds evidence combination with time decay (30s half-life), ported from Android `BayesianFusionEngine.kt`
- Channel hopping state machine — cycles 2.4 GHz channels 1-13 with 100ms dwell time
- RSSI-based distance estimation — log-distance path loss model (ref -40 dBm, exponent 2.5)
- FreeRTOS dual-core task architecture — radio tasks on Core 0, processing on Core 1

#### Uplink Firmware (ESP32-C3)
- WiFi STA with auto-reconnect and exponential backoff (1s to 60s)
- HTTP batch upload to backend — groups detections every 5s or 10 items, POST to `/detections/drones`
- Offline ring buffer — stores up to 100 failed batches, drains on reconnect
- GPS NMEA parser — $GPGGA and $GPRMC sentence parsing, checksum validation
- SSD1306 OLED display — 128x64 status screen showing drone count, GPS fix, WiFi status, battery, upload count
- Battery ADC monitoring — voltage divider on GPIO3, Li-Ion percentage curve
- Status LED — 6 blink patterns (idle, scanning, detection, uploading, error, no GPS)
- SNTP time synchronization via pool.ntp.org and time.google.com
- NVS persistent configuration — WiFi credentials, backend URL, device ID stored in flash

#### Inter-board Communication
- UART link at 921,600 baud — Scanner GPIO17/18 to Uplink GPIO20/21
- Newline-delimited JSON protocol with 30+ field types
- Detection and status message types

#### Backend
- `POST /detections/drones` — batch ingestion endpoint for ESP32 sensor nodes
- `GET /detections/drones/recent` — query last N detections from in-memory ring buffer (max 1000)
- Pydantic v2 schemas: `DroneDetectionBatch`, `DroneDetectionItem`, `StoredDetection`

#### Tests
- 27 Unity test cases across 4 test files (OpenDroneID parser, DJI parser, Bayesian fusion, SSID patterns)
- Native test environment — compiles and runs on host without ESP-IDF

### Detection Parity with Android App
| Feature | Android | ESP32 | Notes |
|---------|---------|-------|-------|
| BLE Remote ID (all 6 msg types) | Yes | Yes | Byte-for-byte port |
| DJI DroneID IE | Yes | Yes | Same OUI + payload parsing |
| ASTM WiFi Beacon RID | Yes | Yes | Same OUI + ODID delegation |
| SSID pattern matching | 99 patterns | 104 patterns | Added 5 more |
| OUI database | 25 entries | 29 entries | Added 4 more |
| Bayesian fusion | Yes | Yes | Same constants + algorithm |
| WiFi NaN | Yes | No | Android-specific API |

### Advantages over Android
- **Continuous promiscuous capture** — no OS throttle, raw 802.11 frames
- **Unlimited scanning** — no Android 4-scan/2-min limit
- **24/7 unattended operation** — deploy as permanent sensor node
- **Probe request detection** — can detect drone controller phones (future)
- **Multi-node sensor network** — multiple ESP32s report to one backend

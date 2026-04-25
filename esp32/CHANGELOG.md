# ESP32 Firmware Changelog

All notable changes to the ESP32 hardware edition of Friend or Foe.

## [Unreleased]

## [0.63.0-svc139] - 2026-04-25

### Added
- **RF evidence fields on the scanner wire.** Scanner JSON now preserves full
  targeted probe SSID arrays, BLE local names, and classification reasons so
  uplink/backend enrichment can explain brand/device-class claims.
- **Diagnostic AP inventory.** Combo/seed scanners emit rate-limited
  `wifi_ap_inventory` observations for generic APs when UART pressure is low,
  with calibration mode and queue-pressure gates keeping the release safe.

### Changed
- **Hidden-camera heuristics are cautious.** Ambiguous BLE LED/IoT names are
  demoted to suspect IoT fingerprints unless explicit camera evidence exists.
- **Uplink dual-slot dedupe preserves evidence.** Upload dedupe now collapses
  same-packet captures from both scanner slots while unioning slot metadata and
  probed SSIDs.

### Fixed
- **Probe serialization no longer drops extra SSIDs.** Multi-SSID targeted
  probes are emitted and uploaded as arrays while legacy string parsing remains
  accepted for mixed-fleet compatibility.

## [0.63.0-svc138] - 2026-04-24

### Added
- **Native unit tests are back in the live path.** CI now runs `pio test -e test`
  before firmware builds, and local preflight does the same.
- **Continuous time-sync health telemetry** now surfaces on both layers:
  uplinks report fetch/broadcast source and freshness, and scanners report
  valid time counts plus `waiting|fresh|stale` state.
- **Android-led calibration mode** is now a first-class firmware feature.
  Uplinks expose `/api/calibration/mode*`, scanners support `cal_mode_start`
  / `cal_mode_stop`, and active sessions can switch the fleet into an
  exact-UUID BLE-only calibration scan lane.

### Changed
- **Live fleet targets are first-class in CI.** `scanner-s3-combo-seed` and
  `uplink-s3` are now built in the firmware workflow instead of only the older
  legacy/web-flasher targets.
- **Release artifacts now include seed scanner + S3 uplink binaries.** This
  keeps GitHub release assets aligned with what the platform and fleet actually
  deploy.
- **Time polling/broadcast now runs on all uplink targets.** Backend `/detections/time`
  polling is no longer S3-only, reconnect paths re-arm SNTP until it syncs,
  and backend time can re-steer the wall clock until SNTP becomes authoritative.
- **Calibration-mode safety nets now exist on both layers.** While a node is in
  calibration mode, uplinks reject lock-on style scan commands and hard-drop any
  non-calibration detections; scanners pause WiFi scanning and only forward BLE
  advertisements matching the exact active session UUID.

### Fixed
- **Uplink backend time fetch is resilient to stale endpoint state.** The
  `/detections/time` poll now tries primary and fallback URLs, attempts all
  resolved IPv4 addresses, reports the endpoint/source/IP it actually used,
  and clears the cached upload socket address when switching URLs. The compiled
  fallback IP is updated to `192.168.42.162`.
- **Root PlatformIO native-test config** now sets `src_dir = .` and `test_dir = test`
  at the correct scope, fixing the broken host-side link step.
- **ODID state-to-detection conversion** now copies the originating device
  address into `drone_detection_t.bssid`, and the native parser test asserts it.
- **Scanner stale time offsets now expire after 30 seconds.** Old firmware could
  keep using drifted epoch offsets forever after losing valid uplink time.
- **Dashboard/node status no longer treats `tcnt > 0` as healthy sync.** Legacy
  nodes stay `unknown`, and fresh vs stale sync is now reported honestly.
- **Calibration beacons no longer disappear in noise filtering.** Exact-session
  `cafe...` BLE UUID advertisements are preserved through scanner filtering,
  UART transport, and uplink upload generation so Android walk sessions can be
  heard by the fleet.

## [0.59.2] - 2026-04-18

### Phase-2 firmware — offline queue, OTA rollback, URI headroom

Finishes the Phase-2 items flagged deferred in v0.59.1. S3 targets only; legacy envs are frozen at 0.59.0 per the S3-only fleet direction.

### Added — Uplink (uplink-s3)
- **Offline detection queue in PSRAM** — `ring_buffer_create_psram()` in `esp32/uplink/main/core/ring_buffer.{c,h}`. Storage goes to PSRAM (`psram_alloc_strict`), header stays in internal SRAM, auto-falls-back to calloc on boards without PSRAM. `CONFIG_MAX_OFFLINE_BATCHES` bumped from `1` to `512` on `UPLINK_ESP32S3`, giving ≈ 2 MB / ~10 min of steady traffic buffered through WiFi outages.
- **OTA rollback** — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in `sdkconfig.esp32s3.defaults`. New `rollback_check_at_boot()` / `rollback_mark_valid()` / `rollback_and_reboot_or_restart()` in `main.c`. Images that boot `ESP_OTA_IMG_PENDING_VERIFY` are marked valid on first successful HTTP upload; the connectivity watchdog rolls back to the previous slot (instead of `esp_restart()`) on no-upload / no-WiFi / low-heap if verification never happened.

### Changed — Uplink
- `config.max_uri_handlers` in `http_status.c` 12 → 20 (15 handlers active today + headroom).
- Heap-low watchdog in `main.c` now goes through the rollback path — a memory leak in the first 20 s of uptime is a strong bad-OTA signal, so PENDING_VERIFY reverts instead of restarting in place.
- **OTA upload staging buffer (P4)** — `/api/fw/upload` in `fw_store.c` now stages through a lazily-allocated 64 KB PSRAM buffer instead of a 4 KB static. 16× fewer `httpd_req_recv`/`esp_ota_write` iterations per 1.1 MB scanner firmware. 4 KB internal fallback preserved for non-PSRAM boards.
- **`/api/status` exposes `offline_queue`** — new `{"depth", "capacity"}` object so the dashboard can see how full the 512-batch PSRAM queue is getting (adds `http_upload_get_offline_count` / `_capacity` accessors).

### Changed — Scanner (scanner-s3-combo)
- `BEACON_CACHE_SLOTS` 128 → 1024 on S3 (internal SRAM, policy rule 7). Reduces LRU wrap in dense beacon environments.
- `PROBE_CACHE_SLOTS` 16 → 128 on S3. Previous 16-slot cache overflowed in minutes in any multi-device household.
- Drone-protocol sources (BLE RID, DJI IE, Beacon RID) remain exempt from rate-limit caches per `feedback_rid_top_priority`.

### Deferred — Phase 3 (not implemented in 0.59.2)
- On-scanner BLE-JA3 entity table emission (needs uplink schema).
- LittleFS-backed offline queue persistence across reboots (PSRAM is volatile; a reboot still drops the queue).
- Android-to-backend detection forwarding.

### Notes
- **Rollback activation requires a new bootloader.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` lives in the bootloader binary. An app-only OTA (including the v0.59.1 → v0.59.2 path on existing Pool) does NOT replace the bootloader, so rollback stays latent until the node is USB-flashed with the v0.59.2 bootloader. The app-side code (`rollback_check_at_boot`) safely no-ops when the current bootloader doesn't mark images PENDING_VERIFY, so v0.59.2 runs correctly on both old and new bootloaders — rollback simply doesn't fire on pre-flag bootloaders. This means Pool gets rollback only after its next USB session; new fleet S3 hardware has it from day 1.
- Legacy (`uplink-esp32`, `scanner-esp32`, `scanner-c5`, `scanner-s3-wifi`) intentionally left at 0.59.0. They are retiring, not upgrading — see `project_s3_only_direction`.
- Scanner rollback NOT enabled. Scanners don't have a clear "boot good" signal (no outbound HTTP), and bad scanner OTAs are already recoverable via `scripts/fof_flash.py` from the healthy uplink. Cheaper to keep scanner boot deterministic than to design a proof-of-boot protocol.

## [0.59.1] - 2026-04-18

### Remote UART flash reliability — staged handshake + store-then-flash scanner OTA

Closes the PRD failure mode that made `/api/fw/relay?uart=ble` hang at "chunk 0 failed" in 11 s. The relay now completes end-to-end on first try, returns structured JSON on success AND every failure path, and survives a wedged scanner cleanly.

### Added — Scanner (scanner-s3-combo, scanner-s3-legacy, scanner-s3-wifi)
- **Store-then-flash OTA** — Incoming image is staged in PSRAM (`psram_alloc_strict`) before any flash write. Explicit state machine `IDLE → STAGING → VALIDATING → FLASHING → REBOOTING` with a single `cleanup_and_idle()` recovery path so any failure returns to IDLE.
- **Scan halt during OTA** — `halt_scans()` stops BLE + WiFi radios at `ota_begin`; always paired with `resume_scans()` on exit so a failed flash no longer wedges the radios.
- **Watchdogs** — 30 s idle watchdog during STAGING, 15 min overall ceiling. Resolves the v0.55 "poisoned `s_ota.active=true` forever" bug.
- **`{"type":"stop_ack"}`** — Scanner confirms TX halted after receiving `{"type":"stop"}`, so the uplink can proceed deterministically instead of sleeping 500 ms and hoping.
- **`{"type":"ota_nack","seq":N}`** — Bad-chunk-CRC frames now request retransmit by seq, capped at 3 retries per chunk.
- **`uart_wait_tx_done(1000)` after `ota_done`** — Guarantees the final ACK escapes the TX FIFO before the scanner reboots into the new image.

### Added — Uplink (uplink-s3)
- **Staged-handshake relay** (`fw_relay_handler` rewrite) with explicit stages: Stop → Begin → Chunks (with NACK-poll) → End. Every stage has a single success path and a named failure: `stop_ack_timeout`, `ota_ack_timeout`, `chunk_N_crc_retries_exhausted`, `finalize_timeout`. Structured JSON response: `{"ok":…, "size":…, "chunks":…, "nacks":…, "retries":…, "elapsed_s":…, "stage":…, "error":…}`.
- **`relay_read_line` drain-overflow** — Long scanner detection JSON (>160 chars with BLE enrichment) no longer returns -1 mid-line and corrupts the relay handshake; lines are drained past the buffer cap.
- **Stage 3 resilience** — Accepts either `{"type":"ota_done"}` OR a fresh scanner identity line as the implicit "new firmware booted" signal (slow legacy scanners sometimes don't flush the TX FIFO before rebooting).
- **Legacy scanner leniency** — Missing `stop_ack` falls back to a 1 s delay and continues. Missing `ota_ack` is still a hard abort — v0.55 and earlier scanners must be USB-flashed once to reach v0.59+.
- **Self-OTA** (`/api/ota`) proven: 1.1 MB uplink image over WiFi → ota_N → reboot → new partition active. ~18 s in quiet conditions, ~140 s under load.

### Added — Shared
- **`MSG_TYPE_STOP_ACK`**, **`MSG_TYPE_OTA_NACK`**, **`JSON_KEY_OTA_SEQ`**, **`JSON_KEY_OTA_REASON`** constants in `esp32/shared/uart_protocol.h`.
- **`esp32/shared/psram_alloc.{h,c}`** — opt-in PSRAM helpers (`psram_alloc`, `psram_alloc_strict`, `psram_calloc`, `psram_free`, `psram_available`, `psram_free_size`, `psram_total_size`) with safe fallback on non-PSRAM boards.

### Changed — Partition / PSRAM fleet coverage
- All three S3 scanner envs (`scanner-s3-combo`, `scanner-s3-legacy`, `scanner-s3-wifi`) now use `partitions_s3_scanner_16mb.csv` (ota_0 @ 0x20000 size 0x300000, ota_1 @ 0x320000 size 0x300000) and `sdkconfig.scanner-s3.defaults` (PSRAM octal @ 80 MHz, `SPIRAM_USE_CAPS_ALLOC=y`, `SPIRAM_BOOT_INIT=y`, `SPIRAM_IGNORE_NOTFOUND=y`).
- Uplink-s3 partition table (`partitions_s3_16mb.csv`) has named firmware-cache partitions (`fw_scanner_s3`, `fw_scanner_esp32`, `fw_scanner_c5`, `fw_self`) so the uplink can hold every scanner variant binary plus its own next-boot image at once. Relay selects by query param.
- `CONFIG_ESP32S3_SPIRAM_SUPPORT=y`, `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y` active on both scanner-s3 and uplink-s3 binaries.

### Fleet status — Pool (uplink_CC59FC)
- **Uplink**: `fof_uplink v0.59.1`, PSRAM 7.9 MB / 8 MB free, internal heap 189 KB free, `uploads_fail=0`.
- **BLE-slot scanner**: `scanner-s3-combo v0.59.1` (USB-flashed once, then remote-flashed end-to-end as PRD proof: 2112 chunks, 0 NACKs, 0 retries, 115 s).
- **WiFi-slot scanner**: `scanner-s3-combo v0.59.1` (remote-flashed: 2112 chunks, 0 NACKs, 0 retries, 136 s).

### Notes
- `/api/fw/relay?uart={ble,wifi}&ack=1` — the `ack=1` per-chunk mode is now ignored (never implemented on scanner side). Fire-and-forget + NACK retransmit is the only protocol. Param retained for dashboard backward-compat, will be removed in 0.60.
- Max URI handler count is currently 12 on uplink — `/api/calibrate/{measure,power,stop}` registrations silently drop. Follow-on bumps this to 16.

## [0.58.0] - 2026-04-16

### Firmware cleanup v2 — honest Continuity + probe-IE grouping

### Changed — Scanner (scanner-s3-combo)
- **Honest Apple classification** — New `BLE_DEV_APPLE_GENERIC` ("Apple Device") replaces `APPLE_IPHONE` as the default for Continuity types 0x10 (Nearby Info), 0x0F (Nearby Action), 0x05 (AirDrop). Apple doesn't broadcast iPhone-vs-iPad-vs-Mac in those — scanner stops guessing. Handoff/AirPlay still classify as MacBook; AirPods/AirTag unchanged.
- **`ble_apple_flags` always emitted** — Cold rename from `ble_apple_info` / `ble_ainfo`. Now emitted on every 0x10 / 0x0F advertisement, even when zero, so the backend can distinguish "all flags false" from "field absent". Flag byte also extracted from type 0x0F (was 0x10 only).
- **Enriched `probe_ie_hash`** — FNV1a now folds in payload bytes of HT Capabilities (tag 45), VHT Capabilities (tag 191), Extended Capabilities (tag 127), and Supported Rates (tag 1), plus first 4 body bytes of each vendor IE. Per PETS-2017, these survive random-MAC rotation.
- **`SCANNER_S3_COMBO=1` build flag** — Added to the combo env for future S3-only gating.

### Changed — Uplink (uplink-s3, uplink-esp32)
- **`ie_hash` forwarded to backend** — Was parsed off UART but silently dropped in the HTTP payload; now forwarded per detection.
- **`ble_apple_flags` cold switch** — Uplink reads the new UART key and always forwards the byte.

### Notes
- All new fields additive on the wire. Legacy uplinks not-yet-reflashed continue to work; they drop `ble_apple_flags` and `ie_hash` until updated.
- `probe_ie_hash` values change — devices observed across v0.57 → v0.58 appear as new identities in the correlator for a few minutes.

## [0.57.0] - 2026-04-16

### False-positive cleanup — Scanner

### Changed — Scanner
- **Broad SSID prefixes removed** — `HOLY` and `UFO-` removed (caught too much non-drone traffic). Specific Holy Stone / UFO drone prefixes still match.
- **Soft SSID matches gated on motion** — `WIFI_xxx` / `FPV_xxx` / `CAMERA_xxx` only emitted when transmitter RSSI has moved recently. Static APs with drone-like names are dropped. Minimum confidence 0.25.
- **Probe-request soft matches dropped** — Probe requests reflect what a client is searching for, not what a drone is broadcasting. Classified as generic wifi_device now.
- **Per-BSSID beacon rate limit** — 128-slot LRU suppresses re-reports of the same BSSID within 30s unless RSSI shifted ≥5 dB. Drone-protocol sources intentionally exempt.

## [0.56.0] - 2026-04-15

### Added
- **Inter-node RF calibration** — Backend-orchestrated calibration sequence: each node broadcasts AP at max and min power (20dBm + 11dBm) for 15s each, all other nodes measure RSSI, linear regression computes path loss model. Calibration results persisted to disk and applied on startup.
- **Calibration HTTP handlers** — `/api/calibrate/start` (enable AP, set TX power), `/api/calibrate/measure` (passive scan, average RSSI), `/api/calibrate/power` (change TX power), `/api/calibrate/stop` (reboot to normal mode)
- **CRC32 firmware verification** — Full-image CRC32 checksum replaces additive checksum for UART OTA relay. Stored in NVS as `fw_crc32`, included in `ota_begin` JSON.
- **Raw LWIP socket HTTP client** — Zero-alloc replacement for esp_http_client on uplink. Static 4KB payload buffer, cached DNS resolution, keep-alive with response body drain. Eliminates heap fragmentation that caused crash loops on legacy ESP32 nodes (320KB RAM).
- **Static WiFi TX buffers** — `CONFIG_ESP32_WIFI_STATIC_TX_BUFFER=y` with 6 static buffers prevents LWIP malloc storms.
- **UART backpressure** — Uplink sends stop command to scanner at 80% queue, resume at 40%. Prevents queue overflow without dropping detections.
- **Confidence filtering** — BLE unknowns below 0.04 confidence dropped at scanner (uart_tx.c) and uplink (uart_rx.c) before queuing.
- **BLE rate limiting** — Unknown devices: 10s, known: 5s, low-interest: 10s. BLE Remote ID drones (UUID 0xFFFA) bypass all rate limits — every packet processed immediately.
- **Deferred AP mode switch** — WiFi AP disabled via `esp_timer` callback when STA connects, preventing task watchdog crash from calling `esp_wifi_set_mode` in event handler.
- **JSON SSID escaping** — WiFi scan handler now escapes `"` and `\` in SSIDs before JSON embedding.
- **UART pause during OTA** — All OTA and firmware operations pause UART RX tasks to prevent contention.

### Fixed
- **OTA abort `\n` never sent** — `uart_write_bytes` was called with length 5 on a 9-byte array; the trailing newline that flushes the scanner's line buffer was never transmitted. Fixed to use `sizeof(abort_seq)`. This caused a 30-second detection delay after every reboot.
- **Atomic backpressure flag** — `s_backpressure_active` changed from `volatile bool` to `_Atomic bool` for correct behavior on dual-core S3 with two RX tasks.
- **Broadcast probe drop** — Empty-SSID probe requests returned immediately at scanner, eliminating UART noise.
- **Passive WiFi scanning** — Changed from active to passive to avoid disrupting STA connection.
- **Full scan interval** — Increased from 1.5s to 30s to reduce radio contention.
- **Scanner silence during OTA** — Scanner suppresses all UART TX during OTA to prevent bidirectional contention.

### Changed
- Firmware version: 0.55.0 → 0.56.0
- Queue size: 20 slots, batch size: 6, batch interval: 80ms, idle flush: 25ms
- HTTP stack: 16KB, UART RX: 5KB
- Heap guard reboot threshold: 4KB
- Watchdog interval: 30s with ready signal re-send
- FOF-Drone- SSID pattern (was FOF-)
- BLE rate limits increased (unknown 3s→10s, known 2s→5s)
- `sdkconfig.esp32.defaults`: static WiFi TX buffers, TCP_SND_BUF 2880, MAX_SOCKETS 3

### Build Targets (all pass)
| Target | Board | Description |
|--------|-------|-------------|
| scanner-s3-combo | ESP32-S3 | WiFi + BLE dual scanner |
| scanner-s3-legacy | ESP32-S3 | Legacy S3 scanner |
| scanner-s3-wifi | ESP32-S3 | WiFi-only scanner |
| scanner-esp32 | ESP32 | WiFi-only scanner |
| scanner-c5 | ESP32-C5 | WiFi 6 + BLE 5 scanner |
| uplink | ESP32-C3 | Standard C3 uplink |
| uplink-esp32 | ESP32 | Legacy OLED uplink |
| uplink-s3 | ESP32-S3 | S3 uplink variant |

## [0.4.0-alpha] - 2026-03-21

### Added
- **Scanner LED status patterns** — 6 blink patterns on Scanner board: boot (3 fast flashes), idle (slow pulse), scanning (double blink), detection (rapid triple), UART heartbeat (single short), error (SOS)
- **Scanner LED GPIO selection** — GPIO48 on ESP32-S3, GPIO27 on ESP32-C5 (compile-time via sdkconfig)
- **Uplink `LED_NO_SCANNER` pattern** — alternating long/short blink when Scanner UART link lost (5s timeout)
- **Uplink scanner connection tracking** — `uart_rx_is_scanner_connected()` with 5-second heartbeat timeout
- **Uplink first-connect handshake** — solid 2s LED flash when Scanner's first status message arrives
- LED pattern quick reference:

| Pattern | Scanner | Uplink |
|---------|---------|--------|
| Boot | 3 fast flashes | (existing) |
| Idle | Slow pulse (1s on/1s off) | — |
| Scanning | Double blink (0.1s) | — |
| Detection | Rapid triple (0.08s) | — |
| UART OK | Single short (0.05s) | — |
| Error | SOS pattern | — |
| No Scanner | — | Alternating long/short |
| Connected | — | Solid 2s flash |

## [0.3.0-alpha] - 2026-03-19

### Added
- **WiFi AP mode** — uplink now runs AP + STA concurrently, creating `FoF-XXXX` hotspot for field configuration without needing the main WiFi network
- **HTTP status page** — embedded web server at `http://192.168.4.1` showing live device status: drone count, GPS fix, WiFi strength, battery level, upload stats
- **Post-flash Web Serial configuration** — configure WiFi credentials and backend URL directly from Chrome after flashing, no serial terminal needed
- Improved UART RX buffering with larger message parsing
- Extended NVS configuration for AP settings

### Fixed
- Format-truncation warning in serial_config.c (msg buffer 64→96)
- Uplink build: use stdin/stdout for serial config instead of usb_serial_jtag
- Shared include path in CMakeLists.txt

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

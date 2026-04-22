# Changelog

All notable changes to Friend or Foe will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- **Android calibrate operator console** — The existing `Calibrate` route is
  now a three-tab operator workflow: `Walk`, `Nodes`, and `Probes`. Operators
  can keep the phone BLE advertiser running during a walk, see calibration
  model truth and phone-vs-fleet position drift, inspect node/scanner pressure,
  and review probe-first intelligence without leaving the app.
- **Probe-first event contract** — Backend events now support stable
  `new_probe_identity` alerts, `probe_activity_spike` warnings, repeated or
  comma-separated event-type filtering, and `unack_by_type` counts for
  in-app badging.
- **Probe diagnostics API expansion** — `/detections/probes` now returns
  first/last seen timing, 24-hour activity counts, sensor spread, and recent
  probe event types keyed by stable identity (`PROBE:<ie_hash>` first, MAC
  fallback second).
- **TDD + live-code playbook** — `docs/tdd-live-playbook.md` now defines the
  default workflow for backend, firmware, and Android changes: failing test
  first, live-target verification, changelog updates in the same change, and
  canary proof before fleet rollout.
- **Local preflight runner** — `python3 scripts/preflight.py` runs the backend
  pytest suite, ESP32 native unit tests, and the live fleet firmware builds
  (`scanner-s3-combo`, `scanner-s3-combo-seed`, `uplink-s3`) so risky changes
  can be checked the same way every time.
- **Backend CI** — new GitHub Actions workflow runs `pytest tests -q` on backend
  pushes and pull requests instead of relying on manual verification.

### Changed
- **Calibration debugging is now app-native.** Android consumes
  `/detections/calibrate/model`, `/detections/nodes/status`, `/detections/probes`,
  and `/detections/events` directly so queue pressure, UART drops, probe-drop
  reasons, recent source fixups, and model trust state are visible during a
  live walk instead of hidden in the dashboard.
- **ESP32 CI now follows the live fleet.** The firmware workflow now runs on
  pull requests, executes the native `pio test -e test` suite, and builds the
  actual deployed targets including `scanner-s3-combo-seed` and `uplink-s3`.
- **Release artifacts now include live S3 binaries.** GitHub release packaging
  now exports the seed scanner and S3 uplink firmware so the firmware catalog
  can track the same targets the fleet is using.

### Fixed
- **Native ESP32 test harness actually compiles firmware logic again.** The root
  PlatformIO test config now sets `src_dir`/`test_dir` at the right scope, so
  the parser/fusion unit tests link the production sources instead of silently
  skipping them.
- **OpenDroneID parser regression coverage now protects advertiser MAC
  propagation.** The native test suite now asserts that parsed BLE Remote ID
  detections keep the originating advertiser MAC in `bssid`.
- **Backend test suite matches the current API contract.** Health-version and
  validation-status expectations were updated to reflect the current backend
  behavior, turning the suite back into a useful guardrail.

## [0.60.0] - 2026-04-18

### Cross-node time sync + ambient triangulation + high-risk tracking

Three-part release. The fleet now shares a sub-50 ms epoch timeline, ambient
WiFi APs / probes / BLE devices get triangulated alongside drones, and
surveillance / hostile-tool device classes get extended retention + a "threat"
auto-category.

### Added — Time sync (v0.60+)
- **Uplink → backend epoch fetch.** Every 10 s the uplink polls
  `GET /detections/time` (new endpoint returning `{"ms": epoch_ms}`) and
  applies via `time_sync_set_from_backend()` if SNTP hasn't synced. Works
  on walled-garden networks that block outbound NTP.
- **Uplink → scanner epoch broadcast.** Same 10 s cadence sends
  `{"type":"time","ms":N}` over UART to both scanner slots. Scanners
  store an offset against `esp_timer_get_time()` and tag every outbound
  detection with `ts = local_ms + g_epoch_offset_ms`.
- **Backend uses scanner-tagged timestamps in EKF + trilateration.**
  `_sensor_tracker.ingest()` now accepts `timestamp` (epoch seconds) →
  passed through to `PositionFilterManager.update()` so EKF dt reflects
  real elapsed scan time, not HTTP arrival jitter. Trilateration window
  tightened to 2.0 s against newest observation per drone.
- **Diagnostic surfacing.** `/api/status.time_sync` now exposes
  `{last_epoch_ms, perf, status, clen, nread, bcasts}` so time-sync
  health is debuggable via HTTP. Scanner `scanner_info` JSON gains
  `toff` (current offset) + `tcnt` (received broadcasts), forwarded to
  the backend's `/detections/nodes/status` per-scanner block.
- **Per-detection `timestamp` field** added to `DroneDetectionItem`
  schema (`backend/app/models/schemas.py`) and `http_upload.c` payload.

### Added — Ambient device triangulation
- **Stable tracking IDs for ambient sources** in
  `triangulation.py:ingest()`: WiFi APs (wifi_ssid / wifi_oui /
  wifi_beacon_rid / wifi_dji_ie) now group on `AP:<bssid>`; WiFi probe
  requests group on the scanner-supplied stable ID. Result: AirTags,
  Tile Trackers, WiFi APs, and other stable-identity ambient devices
  now accumulate observations across nodes and reach the EKF — visible
  via `/detections/drones/map` with `position_source=kalman` once 3+
  observations land.

### Added — High-risk device tracking
- **Per-class TTL** in `entity_tracker.py`. Default still 300 s
  gone / 1800 s stale; high-risk classes get **900 s gone / 7200 s stale**
  so a Meta-glasses / Flipper / AirTag visit doesn't silently drop off
  mid-session.
- **Auto "threat" category** for `Meta Glasses`, `Ray-Ban Meta`,
  `Oakley Meta`, `Meta Quest`, `Meta VR`, `Flipper`, `Pwnagotchi`,
  `Marauder`, `AirTag`, `FindMy`, `Tile Tracker`, `SmartTag`, `Chipolo`,
  `Pebblebee`. Doesn't overwrite manual category assignment.
- **Lingering-tracker alerts cover surveillance hardware.**
  `anomaly_detector._TRACKER_KEYWORDS` extended from 8 entries to 16 —
  Meta glasses + headsets and pentest tools now hit the same 30 min /
  2 hr / 8 hr dwell thresholds as Bluetooth trackers.

### Fixed
- **`esp_http_client_perform()` auto-consumes response body** — the
  default-mode perform reads the body internally, leaving nothing for
  `esp_http_client_read()`. Time-sync HTTP fetch was returning 200 OK
  with `nread=0` for hours. Fixed by switching to `open` +
  `fetch_headers` + `read_response` pattern in `http_upload.c`.
- **EKF `_record_emit` referenced undefined `now`** introduced when
  the per-detection-timestamp plumbing landed. Fixed in
  `triangulation.py:422` (use `time.time()` directly).
- **OTA upload begin failure under PENDING_VERIFY rollback state.** The
  uplink's rollback feature was protecting the unused OTA slot from
  being repurposed for scanner firmware staging. Fix: call
  `rollback_mark_valid()` on WiFi-up rather than waiting for first
  upload, and widen the no-upload watchdog from 300 s to 900 s so
  scanner-flash sequences don't trip a false rollback.

### Production pinout (committed alongside)
- ProductionFullSize S3 uplink↔scanner pinout codified in
  `esp32/uplink/main/core/config.h`: BLE slot uses uplink GPIO 18 (RX) +
  17 (TX); WiFi slot uses 16 (RX) + 15 (TX); scanner side fixed at
  17/18. Every wire crosses to a visibly different pin number on both
  sides — no more "straight-through by label" cable mistakes that bit
  Pool / FrontYard's BLE-slot builds.
- Seed scanner variant — `esp32/scanner/platformio.ini` adds
  `scanner-s3-combo-seed` env (`SEED_SCANNER_PINS`) targeting an alt
  carrier with GPIO 1 (TX) / GPIO 2 (RX) on the scanner side and 8 MB
  flash via `partitions_s3_scanner_8mb.csv`. Same firmware code as
  full-size combo, just different pin map + smaller partition table.
  In production at gate.

### Deferred — Phase 3
- LittleFS persistence for the offline queue across reboots.
- On-scanner BLE-JA3 entity table emission (needs uplink schema).
- Android-to-backend detection forwarding.

### Notes
- All 6 fleet nodes (Pool, FrontYard, area51, Chomper, gate, patio)
  rolled to v0.60.0 — uplinks via OTA, scanners via UART relay.
  Verified `toff != 0` and `tcnt > 0` on all 12 scanners.
- Calibration R² didn't improve from time sync (0.079 → 0.037 — noise
  band) and that's expected: calibration measures static distance↔RSSI,
  which doesn't benefit from timestamp alignment. The win shows up in
  multi-node drone tracking.

## [0.59.2] - 2026-04-18

### Phase-2 firmware — offline queue, rollback, URI headroom

Closes out the remaining PRD Phase-2 items from v0.59.1 so S3 nodes are ready for fleet rollout. S3-only per `project_s3_only_direction`; legacy variants are frozen at 0.59.0.

### Added — Uplink (uplink-s3)
- **PSRAM-backed offline detection queue** — `CONFIG_MAX_OFFLINE_BATCHES` bumped from 1 to 512 on S3 (≈ 2 MB in PSRAM, ~10 min of steady detection traffic buffered through a WiFi outage). `ring_buffer_create_psram()` allocates storage from external RAM with SRAM fallback.
- **Self-OTA rollback watchdog** — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. On boot from a `PENDING_VERIFY` OTA slot the image is marked valid only after the first successful backend upload. If the connectivity watchdog (no-upload / no-WiFi / low-heap) fires before verification, the uplink rolls back to the previous slot instead of restarting in place — removing the USB-rescue requirement for a bad self-OTA.

### Changed — Uplink
- URI handler table 12 → 20 slots. Prior cap silently dropped three registrations (two of the three `/api/fw/*` + one of the four `/api/calibrate/*`). All 15 handlers now register cleanly with headroom for Phase-3 additions.
- **PSRAM-backed OTA upload staging** (P4) — 4 KB → 64 KB buffer for `/api/fw/upload`. Visibly faster scanner firmware staging with 16× fewer recv/write iterations.
- **`/api/status` exposes `offline_queue.depth` / `.capacity`** for dashboard visibility into the new 512-batch PSRAM queue.

### Changed — Scanner (scanner-s3-combo)
- Beacon rate-limit LRU enlarged: 128 → 1024 slots on S3 (16 KB internal SRAM; stays out of PSRAM per policy rule 7 — cache is iterated every packet). Reduces wrap in dense RF environments so legitimate state changes don't get suppressed behind unrelated BSSID churn.
- Probe-request rate-limit LRU enlarged: 16 → 128 slots on S3. Crowded venues easily exceed 16 concurrent probers.

### Deferred — Phase 3
- On-scanner BLE-JA3 entity table (PSRAM). Depends on uplink-side schema work.
- LittleFS persistence for the offline queue across reboots (PSRAM is volatile — a reboot still drops the queue).
- Android-to-backend detection forwarding.

## [0.59.1] - 2026-04-18

### Remote UART flash reliability — scanner OTA finally works first-try

The PRD goal that had been chasing us all week: `POST /api/fw/relay?uart=ble` flashes a scanner end-to-end on first try, no retries, no manual intervention. Proven on Pool today: both the WiFi-slot scanner (`0.59.0 → 0.59.1`, 2112 chunks, 0 NACKs, 136 s) AND the BLE-slot scanner (after one bench-flash to get it onto v0.59+, then a remote proof flash: 2112 chunks, 0 NACKs, 115 s).

See `esp32/CHANGELOG.md` for the full firmware-side rewrite. Summary at this layer:

### Added — ESP32 firmware (scanner-s3-combo + uplink-s3, v0.59.1)
- Staged-handshake relay protocol: `stop_ack` → `ota_begin` → `ota_ack` → chunks + NACK-retransmit → `ota_end` → `ota_done`. Every failure path returns structured JSON with `stage` and `error`.
- Scanner store-then-flash OTA — image staged in PSRAM before flash writes, radios halted, single `cleanup_and_idle()` recovery path.
- Watchdogs on scanner OTA state machine — 30 s idle, 15 min overall. Closes the v0.55 "stuck in OTA forever" bug.
- Uplink self-OTA (`/api/ota`) proven end-to-end on Pool (1.1 MB image, ota_0/ota_1 flip, reboot, new partition active).

### Added — Tools / CLI
- **`scripts/fof_flash.py`** — headless remote scanner flash. `python3 scripts/fof_flash.py <node> --scanner {s3-combo,esp32,c5} --uart {ble,wifi}`. Stages firmware to node's uplink, triggers relay, reports structured chunks/NACKs/retries/elapsed. Used for Pool's end-to-end proofs today.

### Changed — Partition / PSRAM fleet
- 16 MB partition table (`partitions_s3_16mb.csv`, `partitions_s3_scanner_16mb.csv`) with 2 MB OTA slots (uplink) and 3 MB OTA slots (scanner, headroom for larger binaries).
- Octal PSRAM @ 80 MHz enabled on all S3 targets: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`. ~8 MB of PSRAM available per node via `psram_alloc()` / `psram_alloc_strict()` (shared `esp32/shared/psram_alloc.{h,c}`).
- Pool's live fleet at end-of-day: uplink + 2× scanner-s3-combo all on v0.59.1. Internal heap 189 KB free, PSRAM 7.9 MB / 8 MB free, `uploads_fail=0` at the uplink.

### Deferred — follow-on pass (non-blocking for rollout)
- Backend `fw_manager.stage_and_flash()` + `FirmwareOperation` DB model (R7).
- Backend route `POST /nodes/{device_id}/firmware/flash-scanner` (R8).
- Dashboard per-node "Flash scanner" button + progress toast (R9).
  The CLI covers every fleet-rollout workflow today; these three ship in a follow-on PR for dashboard ergonomics.
- PSRAM-backed offline detection queue (Phase 2).

## [0.59.0-android-alpha] - 2026-04-17

### Android detection parity — Marauder + furiousMAC port

**Alpha release. Backend and ESP32 firmware v0.58.0 remain UNTESTED and NOT YET FLASHED on the fleet — this release only adds Android-side BLE detection improvements. The Privacy screen enrichments are phone-local and don't depend on backend-forwarding yet.**

### Added — Android
- **`BleSignatures` central registry** — single source of truth for 30+ BLE company IDs, 12 service UUIDs, all Apple Continuity sub-types, data-flags bit semantics, Nearby Action sub-types, Microsoft Swift Pair scenarios, FNV-1a constants, and Marauder Pwnagotchi BSSID. Mirrors the ESP32 scanner firmware byte-for-byte.
- **`AppleContinuityDecoder`** — full Kotlin port of `classify_apple()` + furiousMAC deep-field extraction: auth tag (3 bytes), activity code, data-flags byte, iOS major version nibble, Nearby Action sub-type. Preserves the v0.58 honest-Apple stance (only type 0x0D Tether Source can honestly say "iPhone"; 0x0F/0x10/0x05 stay "Apple Device").
- **`MicrosoftSwiftPairDecoder`** — Surface Pen / Xbox Controller / Precision Mouse / Surface Headphones / Surface Earbuds / Surface Laptop classifier (CID 0x0006 was silently `UNKNOWN` before).
- **BLE-JA3 structural hash** — `BleFeatureExtractor.computeJa3Hash()` — same FNV-1a seeds, same field ordering as the ESP32 firmware. Output is a MAC-rotation-surviving device-model identifier. The Android-to-backend forwarding PRD (out of scope this release) will correlate phone observations with fleet observations via this hash.
- **`BlePacketParser.parseAdvertisement()`** — new full-AD walk that captures AD types, service UUIDs, advertising flags byte (bit 0x08 = dual-mode host), appearance, local name, raw manufacturer data.
- **Pwnagotchi detection** — BSSID `DE:AD:BE:EF:DE:AD` now fires the WiFi anomaly detector as an ATTACK_TOOL threat with 0.95 confidence. Matches Marauder's signature.
- **Privacy screen WiFi anomaly banner** — new red banner for Pwnagotchi / Evil Twin / Karma Attack / Rogue AP detections. 15-second ticker polls `WifiAnomalyDetector.analyze()` and decays stale alerts after 60 s.
- **Drone / GlassesDetection enrichment fields** — 11+ optional BLE fields (`bleCompanyId`, `bleAppleType`, `bleAppleFlags`, `bleAppleAction`, `bleAppleIosVersion`, `bleAdvFlags`, `bleDualModeHost`, `bleJa3Hash`, `bleServiceUuids`, `bleAppearance`, `bleLocalName`) propagate through to the Privacy screen card details.

### Fixed — Android
- **Meta glasses detection parity** — `GlassesDetector` now matches Luxottica CID `0x0D53` (the frame-manufacturer ID on every Ray-Ban Meta / Oakley Meta unit) with 0.95 confidence → SMART_GLASSES. Marauder matches this CID; firmware v0.58 matches this CID; the Android app did not until now. This was the headline user-reported gap.
- **Flipper Zero detection** — added Flipper Devices CID `0x0E29` → ATTACK_TOOL classification (previously silent).

### Tests
- 4 new unit test classes: `BleSignaturesTest` (constant drift guards), `AppleContinuityDecoderTest` (fixture-driven Continuity decode — AirTag, AirPods, Handoff, Tether Source, Nearby Info + data-flags, Nearby Action sub-type), `MicrosoftSwiftPairDecoderTest`, `BleJa3HashTest` (deterministic hash, CID / address-type / props sensitivity).

### Not in this release (queued follow-ons)
- Android-to-backend forwarding (phone as fleet sensor node).
- iBeacon UUID + Major + Minor retail-network database.
- Eddystone TLM battery / temperature decode.
- Pwnagotchi vendor-IE JSON decode (`pwnd_tot`, `name`).

### Firmware status
v0.58.0 firmware binaries (scanner-s3-combo, uplink-s3, uplink-esp32) are built and committed (commit `341cab6`) but **NOT yet flashed on the fleet and NOT yet validated on-device**. Android v0.59.0 enrichment is Android-local only and does not require the new firmware.

## [0.58.0] - 2026-04-16

### Firmware cleanup v2 — Honest Continuity + probe-IE grouping

v0.57 fixed phantom-iPhone counts at ingest (22 → 0) and disappearance dedup (185 → 0). This release moves the fixes up into the firmware so the wire data is honest end-to-end, and plumbs `probe_ie_hash` through the uplink HTTP payload so identity grouping actually activates.

### Changed — ESP32 scanner firmware (v0.58.0)
- **Honest Apple Continuity classification** — New `BLE_DEV_APPLE_GENERIC` ("Apple Device") replaces `BLE_DEV_APPLE_IPHONE` as the default for Nearby Info (0x10), Nearby Action (0x0F), and AirDrop (0x05). Apple doesn't broadcast iPhone-vs-iPad-vs-Mac in those — scanner stops guessing. Handoff/AirPlay still classify as MacBook; AirPods and AirTag unchanged.
- **Apple data-flags always emitted** — `ble_apple_flags` (was `ble_apple_info` / `ble_ainfo`) is emitted on every Nearby Info / Nearby Action advertisement, even when zero, so the backend can distinguish "flags all false" from "field absent". Cold rename — legacy uplinks drop the byte until reflashed. Nearby Action (0x0F) now extracts the flag byte too (previously only 0x10).
- **Enriched `probe_ie_hash`** — FNV1a hash now folds in payload bytes of identity-stable IEs (Supported Rates, HT Cap, Ext Cap, VHT Cap) plus first 4 bytes of each vendor IE body. Per PETS-2017, these survive random-MAC rotation. One-time reset of the probe-IE identity space; groupings rebuild within minutes of observation.
- **`SCANNER_S3_COMBO=1` build flag** — Added to the `scanner-s3-combo` platformio env to enable S3-combo-only features in future passes.

### Changed — ESP32 uplink firmware (v0.58.0)
- **`ie_hash` forwarded to backend** — Uplink was parsing `probe_ie_hash` off UART but silently dropping it in the HTTP payload. Now emitted as `ie_hash` per detection, so `identity_correlator.find_identity()` finally has data to work with.
- **`ble_apple_flags` cold switch** — Uplink now reads the new UART key and forwards it as `ble_apple_flags` in the HTTP payload (always emitted, matches scanner contract).

### Changed — Backend
- **`ble_apple_flags` as first-class field** — `DroneDetectionItem` accepts the new key; legacy `ble_apple_info` retained as an alias for one release.
- **Skip iPhone→Apple-Device translation when scanner is already honest** — If the incoming manufacturer string is `"Apple Device"`, ingest stops rewriting it.
- **EntityTracker probe-IE grouping** — `_correlate_by_probes` now consults `identity_correlator.find_identity(ie_hash)` before falling back to SSID-Jaccard; known entities are linked directly without a Jaccard threshold dance.

### Notes
- `probe_ie_hash` values change under the new scheme. Devices observed across the v0.57 → v0.58 scanner upgrade appear as new identities in the correlator until re-seen a few times.
- Firmware artifacts built but not flashed. User flashes locally tomorrow; legacy uplinks not-yet-reflashed continue to work, they simply drop `ble_apple_flags` and `ie_hash` until updated.

## [0.57.0] - 2026-04-16

### False-positive & grouping cleanup — Backend + Scanner

Pipeline audit surfaced three classes of noise: stationary devices triggering motion anomalies, low-confidence BLE frames classified as confirmed drones, and one physical device appearing as many separate entities. This release addresses all three layers.

### Fixed — Backend
- **Duplicate anomaly ingest** — `_anomaly_detector.ingest()` was being called twice per detection in the router, doubling every RSSI sample and inflating velocity/spike deltas. Single call now.
- **Per-sensor velocity false positives** — Velocity alerts now require ≥2 sensors to independently agree on a monotonic RSSI slope; a stationary transmitter seen at different distances by different sensors no longer registers as motion.
- **RF anomaly monotonic check** — The 3-point monotonic-rise trip (`rssi[0]<=rssi[1]<=rssi[2]`) was vulnerable to normal multipath noise. Replaced with full-history monotonicity over 5+ samples spanning ≥15 s, total delta ≥18 dB.
- **Strong-signal spam** — `STRONG_SIGNAL_DB` tightened from -15 to -10 dBm and gated to first 30 s after first-seen so close legitimate devices don't keep re-alerting.
- **Per-alert-type cooldown** — Added 600 s cooldown per `(track, alert_type)` in anomaly_detector so the same device can't re-alert every batch.
- **Low-confidence BLE RID → confirmed_drone** — Any `ble_rid` source was classified `confirmed_drone` with no confidence floor. Now requires ≥0.30; weaker frames downgrade to `likely_drone`.
- **Fuzzy WiFi SSID floor** — Soft-match classification floor raised 0.10 → 0.20 so household hotspots stop clearing the bar.
- **EKF init weighting** — Position filter now inverse-variance weights sensor observations at cold start instead of inverse-distance, so weak-signal distant sensors don't pull the initial position off.
- **`_last_update_time` bug** — Position filter's history timestamps always fell back to wall time because the guard attribute was never set. Now uses the filter clock consistently.

### Added — Backend
- **Ingest dedup** — 30 s LRU on `(drone_id, source, bssid, 10 s bucket)` in the detections router drops duplicates from scanner re-emission and uplink offline-replay before they hit the classifier / anomaly detectors / DB.
- **Apple auth-tag entity merge** — EntityTracker now consults the BLEEnricher's `_auth_tag_links` table (built from Apple Nearby auth tags) and merges entities when MAC rotation is detected. One iPhone no longer fragments into many entities over its rotation cycle.
- **Cross-source / single-SSID correlation** — EntityTracker probe correlation now admits strong co-location + manufacturer match + any shared SSID as a link, fixing the over-splitting case where a phone with one dominant home network never re-associated across MAC rotations.
- **Persistent entity store** — New `tracked_entities` table; EntityTracker checkpoints dirty entities every ~30 s and rehydrates from the last 24 h on backend startup, so the dashboard entity count survives a restart.
- **`/detections/grouped` by entity** — The grouped view now keys by EntityTracker entity_id when known and falls back to drone_id otherwise; the best classification across all of an entity's detections is surfaced.

### Changed — Backend
- **Triangulation sensor TTL** — Lowered from 24 h to 30 min so offline sensors drop in sync with EntityTracker.

### Changed — ESP32 scanner firmware (v0.57.0)
- **Broad SSID prefixes removed** — `HOLY` (caught "HolyCow", etc.) and `UFO-` (caught nightclubs and consumer gear) removed. Specific Holy Stone / UFO drone prefixes still match.
- **Soft SSID matches gated on motion** — `WIFI_xxx` / `FPV_xxx` / `CAMERA_xxx` soft matches are now only emitted when the transmitter's RSSI has moved recently. Static APs with drone-like names are dropped. Minimum confidence 0.25.
- **Probe-request soft matches dropped** — Probe requests reflect what a client is *searching for*, not what a drone is broadcasting. Soft-match probes no longer escalate to "Drone Likely"; classified as generic wifi_device.
- **Per-BSSID beacon rate limit** — 128-slot LRU suppresses re-reports of the same BSSID within 30 s unless RSSI shifted ≥5 dB. Applied to hard SSID match, OUI match, and scan-result paths (drone-protocol sources intentionally exempt).

### Notes
- Scanner firmware changes only; uplink firmware untouched.
- Version bumped across `esp32/shared/version.h` and `backend/app/main.py`.

## [0.56.0] - 2026-04-15

### Platform Release — ESP32 + Backend + Dashboard

This release unifies the ESP32 firmware (v0.56.0) and backend (v0.56.0) version numbers, reflecting a major evolution from the sensor node prototype into a full detection platform with triangulation, calibration, entity tracking, and a comprehensive real-time dashboard.

### Added — Backend
- **Inter-node RF calibration system** — Orchestrates sequential AP broadcasting across sensor nodes at two power levels (20dBm + 11dBm), measures RSSI between all node pairs, computes path loss model via linear regression, persists results to disk
- **Calibration API** — `POST /detections/calibrate`, `GET /detections/calibrate/status`, `GET /detections/calibrate/model`, `GET /detections/calibrate/history`, `GET /detections/calibrate/matrix`
- **Entity correlation engine** — Groups random-MAC BLE devices and WiFi probes into tracked entities using Jaccard SSID overlap scoring, BLE fingerprint clustering, and timeline events for sensor handoffs
- **Entity API** — `GET /detections/entities` with locate buttons per entity
- **Anomaly detection** — RF anomaly engine with configurable thresholds for RSSI spikes, disappearances, velocity anomalies, channel hopping, and rogue AP detection
- **Device classifier** — 25+ known device types, BLE drone type filtering, mobile hotspot detection by SSID keywords, locally-administered MAC detection
- **BLE manufacturer database** — Expanded from ~200 to ~500 OUI entries covering networking, phones, IoT, vehicles, drones, wearables, cameras, gaming
- **Triangulation engine** — Multilateration from 2+ sensor observations with Extended Kalman Filter position smoothing, mutable RSSI model updated by calibration
- **Dashboard overhaul** — 12 tabs: Overview, WiFi APs, BLE, Mobile Devices, Probes, Map, Alerts, Entities, Whitelist, Anomalies, All Detections, Sensors
- **Map features** — Satellite Leaflet map with draggable sensor markers, device tracking (magenta pulsing markers), triangulation lines, range circles, trail filtering, per-sensor RSSI display in popups
- **Sensor management** — GPS position editor with placement map, OTA firmware update dialog, scanner diagnostics, firmware stack display
- **Probe tracking** — WiFi probe requests grouped by source MAC with SSID lists, probe history, mobile device profiles
- **Alert system** — Drone alerts, tracker alerts, BLE fingerprint alerts, probe activity monitoring with alert history

### Added — ESP32 Firmware (v0.56.0)
- **Calibration handlers** — AP enable/disable, TX power control, passive RSSI measurement, reboot-to-free-RAM on calibration stop
- **CRC32 firmware verification** — Full-image CRC32 replaces additive checksum for OTA relay integrity
- **Raw socket HTTP client** — Replaced esp_http_client with zero-alloc LWIP sockets to eliminate heap fragmentation on legacy ESP32 nodes
- **Static WiFi TX buffers** — Prevents LWIP malloc storms that caused heap exhaustion crash loops
- **UART backpressure** — Stop/start flow control between scanner and uplink at 80%/40% queue thresholds
- **Confidence filtering** — BLE unknowns below 0.04 confidence dropped at scanner before UART transmission
- **BLE rate limiting** — Unknown devices: 10s, known: 5s, low-interest: 10s (Remote ID drones bypass all limits)
- **Passive WiFi scanning** — Reduced from active to passive scanning to avoid interfering with AP connections
- **Broadcast probe drop** — Empty-SSID probe requests (broadcast) dropped immediately at scanner
- **Deferred AP mode switch** — WiFi AP disabled via esp_timer callback when STA connects, preventing watchdog crashes

### Fixed — ESP32 Firmware
- **OTA abort sequence truncated** — Was sending 5 bytes instead of 9; the `\n` flush never reached the scanner, causing line buffer pollution and 30s detection delay after every reboot
- **SSID JSON injection** — WiFi scan results now properly escape `"` and `\` in SSIDs for the setup UI
- **Atomic backpressure flag** — `s_backpressure_active` changed from `volatile bool` to `_Atomic bool` for thread safety on dual-core S3
- **Heap exhaustion crash loop** — Legacy ESP32 uplinks (320KB RAM) crashed from cJSON + esp_http_client fragmentation; replaced with static buffers and raw sockets
- **Scanner UART flood** — BLE unknowns at 0.02 confidence caused JSON parse errors and heap exhaustion on uplink

### Changed
- Backend version bumped from 0.32.0 to 0.56.0 (unified with firmware)
- ESP32 firmware version bumped from 0.55.0 to 0.56.0
- RSSI reference changed from -40 to -50 dBm (better match for ESP32 PCB antennas)
- Triangulation distance clamp reduced from 5000m to 200m
- EKF measurement noise increased to `(30 + d*2)^2` for more stable positioning
- Anomaly detection thresholds raised to eliminate residential WiFi noise

## [0.51.0] - 2026-04-06

### ESP32 Firmware
- Fix legacy node crash from cJSON/HTTP heap fragmentation
- Scanner start/stop protocol improvements
- Delete cached sdkconfigs from repository

## [0.50.0] - 2026-04-04

### Added
- **AI detection engine** — ML Kit object labeling for visual drone detection, training data collector, camera optimization
- **Complete BLE privacy scanner overhaul** — Restructured BLE detection pipeline on ESP32

## [0.47.0] - 2026-03-31

### Fixed
- Meta glasses detection — connected device scanner with refresh cycle

## [0.46.0] - 2026-03-30

### Added
- 303 BLE detection rules (up from 288)
- Dash cam detection via WiFi SSID patterns
- `isBonded` flag for BLE device classification

## [0.45.0] - 2026-03-30

### Added
- 287 BLE device detection rules
- Bonded device scanner for Android
- Meta Quest detection fix

## [0.44.0] - 2026-03-29

### Added
- **Ocean search** — Long-press map to scan 250 NM radius anywhere in the world

## [0.43.0] - 2026-03-29

### Fixed
- Meta Quest BLE detection
- Unit normalization to miles/feet/mph
- 40 new aircraft photos added

## [0.42.0] - 2026-03-28

### Added
- All-S3 production sensor node architecture
- Android AR viewfinder fixes
- 22 new aircraft reference photos
- UART relay OTA with CRC32 verification

## [0.37.0] - 2026-03-28

### Added
- **OTA firmware updates** — Over-the-air updates via UART relay from uplink to scanner
- **Apple Continuity deep parsing** — Decode AirDrop, Handoff, and other Apple BLE messages
- **Entity resolution** — Group related BLE advertisements into logical devices
- **Dashboard overhaul** — Probes tab, WiFi probe tracking, firmware version display, OTA update buttons
- **Lock-on system** — Dashboard to backend to uplink to scanner directed scanning chain
- **BLE-JA3 fingerprinting** — Fingerprint BLE devices by advertisement structure
- **Multi-SSID WiFi** — Uplink tries multiple saved WiFi networks
- **Setup portal** — Web-based WiFi configuration at 192.168.4.1

### Fixed
- OLED scanner version display
- mDNS default backend URL
- OTA relay crash (smaller chunks + RTOS yield)
- Uplink heap stability (stack overflow fixes)
- Dashboard flashing empty sensors

## [0.35.0] - 2026-03-27

### Added
- 3-board sensor node architecture (S3 BLE + C5 WiFi + ESP32 OLED Uplink)
- Plain ESP32 WiFi scanner build
- Android sensor backend settings with connection test
- Uplink connectivity watchdog with auto-reboot

## [0.33.0-beta] - 2026-03-27

### Added
- **PostgreSQL persistent storage** — All drone detections now logged to PostgreSQL (was in-memory only, lost on restart). 3 tables: sensor_nodes, drone_detections, triangulated_positions
- **Node management API** — Register ESP32 sensor nodes at fixed GPS positions:
  - `POST /nodes` — Register node with fixed lat/lon/alt
  - `GET /nodes` — List all registered nodes
  - `PUT /nodes/{device_id}` — Update node position/name
  - `DELETE /nodes/{device_id}` — Remove node
  - `GET /nodes/{device_id}/detections` — Get detections from this node
- **Fixed node position override** — When a registered fixed node sends a detection batch, its registered GPS position is used instead of the GPS from the payload. Dynamic nodes auto-update position from GPS.
- **Detection history API** — `GET /detections/drones/history?hours=1&source=wifi_ssid` — Paginated query with time range and source filter
- **Drone track API** — `GET /detections/drones/{drone_id}/track?hours=1` — Position history for a specific drone over time (triangulated positions)
- **Triangulated position logging** — Every triangulation result stored in PostgreSQL with drone_id, position, accuracy, source, sensor observations JSON
- **Auto-create tables on startup** — Tables created via SQLAlchemy IF NOT EXISTS on each server start

### Changed
- Backend version bumped to 0.32.0
- Detection ingestion endpoint now writes to PostgreSQL + ring buffer + triangulation engine
- Sensor position resolved from DB before triangulation (fixed nodes override GPS payload)

## [0.32.0-beta] - 2026-03-27

### Added
- **Ultrasonic beacon detection wired into Privacy tab** — UltrasonicDetector (FFT at 48kHz, 17.5-22kHz range) was implemented but never connected. Now collects alerts and displays red banner with frequency, SNR, and persistence data when tracking beacons detected
- **Retail tracking beacons** (threat 3) — Estimote, Kontakt.io, Gimbal, Radius Networks, RetailNext, FootfallCam, V-Count, Density, VergeSense, XY Sense
- **Conference cameras** (threat 2) — Owl Labs Meeting Owl/Owl Pro/Owl 3, Poly Studio, Logitech Rally/MeetUp/RoomMate, Neat Bar/Board, Cisco Webex
- **Video intercoms** (threat 2) — DoorBird, ButterflyMX, Akuvox, 2N
- **Fleet AI dashcams** (threat 1) — Samsara, Motive/KeepTruckin, Geotab, Verizon Hum, Zubie, Lytx
- **Insurance telematics** — Progressive Snapshot, Allstate Drivewise, State Farm Drive Safe
- 7 new privacy categories (29 total): Ultrasonic Beacons, Retail Trackers, Conference Cameras, Video Intercoms, Fleet Dashcams + expanded existing
- 288 total detection signatures (161 BLE names + 101 WiFi SSIDs + 14 MFR CIDs + 12 UUIDs)

### Fixed
- BLE scan not detecting already-on Meta glasses — added retry (3 attempts, 5s backoff) + periodic 30s scan restart to catch slow advertisers

## [0.31.0-beta] - 2026-03-26

### Added
- **BLE direction-finding UI** — Tap "Track" on any privacy device to open a full-screen guided direction scan
- **Phase 1: Guided 360° rotation** — Compass rose with 12 sector arcs that fill green as you spin. Rotation speed guide (too slow / good / too fast). Auto-finishes at 16+ samples with 10/12 sectors covered, or manual finish at 8+
- **Phase 2: Active tracking** — Large green arrow rotates with phone compass, pointing toward device. 10-bar signal strength meter (red→yellow→green). Proximity label (Very Close / Close / Medium / Far / Very Far). Live RSSI updates every 200ms. Rescan button to improve accuracy
- **254 total detection signatures** (up from 88):
  - Baby monitors: Owlet, Miku, CuboAi, Lollipop, iBaby, VTech, Hubble
  - Thermal cameras: FLIR One/Edge, Seek Thermal, InfiRay
  - Trail cameras: Spypoint, Bushnell, Moultrie, Reconyx, Stealth Cam
  - OBD/car trackers: ELM327, VEEPEAK, BlueDriver, FIXD, OBDLink, Carly
  - GPS trackers: Tracki, Bouncie, Invoxia, LandAirSea, Spytec
  - Pet trackers: Whistle, Fi, Tractive, Jiobit, PitPat
  - Smart TVs: Samsung [TV], LG webOS, Sony BRAVIA, Vizio, Roku
  - Drone controllers: DJI-RC-, DJI-RC Pro
  - Smart speakers: Sonos Move/Roam
  - Smart home hubs: Google Home/Nest Mini/Audio/Hub, Apple HomePod
  - Smart locks: August, Yale, Schlage, Kwikset, Level Lock
  - More cameras: EZVIZ, Lorex, ZOSI, Swann, Annke, RemoBell
  - Chinese IP cams: IPCAM-, Care-AP, Danale-
  - Attack tools: ESP8266 deauther ("pwned", "Advanced-Deauther")
- **14 new privacy categories** (22 total): Baby Monitors, Thermal Cameras, Trail Cameras, OBD/Car Trackers, GPS Trackers, Smart Speakers, Smart Home Hubs, Smart Locks, Smart TVs, Drone Controllers, E-Scooters, Doorbell Cameras, Surveillance Cameras, ALPR/Plate Readers
- **Sectioned Privacy UI** — 4 color-coded sections: THREATS (red), AWARENESS (orange), NEARBY (yellow), INFO (gray). Section headers with aggregate counts, tap to collapse/expand all categories in group

### Fixed
- **Compass bearing was hardcoded to 0** — Direction scanning was completely non-functional. Now wired to SensorFusionEngine for real compass data
- **Stalker alerts never cleared** when threat moved out of range (stale alerts persisted forever)
- **UUID false positives** — Added Bluetooth SIG base UUID validation before 16-bit extraction
- **Ignored MAC CSV re-parsed on every BLE advertisement** — Now cached in memory
- **BleTracker.getOrPut race condition** — Changed to atomic computeIfAbsent (prevents firstSeen reset)
- **BleTracker TrackedDevice fields** — Marked @Volatile for cross-thread visibility

## [0.29.0-beta] - 2026-03-25

### Added
- **Surveillance camera detection** — Nest Cam, Arlo, Wyze Cam, eufy (Indoor/Doorbell/Floodlight), SimpliSafe, Verkada, Rhombus, Reolink detected via BLE setup advertisements
- **ALPR / license plate reader detection** — Flock Safety, Leonardo ELSAG, Genetec AutoVu, Vigilant/Motorola detected via BLE name + WiFi SSID patterns
- **Doorbell camera detection** — Ring, Nest Hello/Doorbell, eufy Doorbell, Blink via WiFi AP and BLE
- **Police / fleet camera detection** — Axon Fleet, WatchGuard/Motorola via BLE name patterns
- **New privacy categories** — Surveillance Cameras (threat 2), ALPR/Plate Readers (threat 2), Doorbell Cameras (threat 1)
- **ESP32-C5 dual-mode scanning** — BLE 5 + WiFi 6 running simultaneously with NimBLE coexistence
- **ESP32 BOOT button privacy view** — Double-tap GPIO9 switches OLED between drone view and privacy scan view. Single tap scrolls pages
- **ESP32 privacy OLED view** — Shows device type, manufacturer, name, RSSI, confidence, camera flag with paged display
- **Meta product name updates** — Added "Ray-Ban Meta" (current official name), "Oakley Meta" (June 2025), "Meta Neural Band" (Sep 2025) to both Android and ESP32
- **8 new WiFi SSID patterns** — Ring-, BLINK-, SimpliSafe-, Nest-, Verkada-, Rhombus-, Flock-, ELSAG-

### Fixed
- **12 Android privacy detection bugs** — SortedMap category collision (TreeMap collapsed same-threatLevel categories into one), scanner not starting from Privacy tab, backgrounding killing scanner permanently, neverForLocation filtering BLE packets, toggle leaking coroutines, debounce dropping UI updates, stop() not cleaning privacy state, missing device.name fallback
- **BLE permission fix** — Removed neverForLocation from BLUETOOTH_SCAN (was silently filtering tracker/glasses BLE packets), added legacy BLUETOOTH/BLUETOOTH_ADMIN for API ≤30
- **App lifecycle fix** — Added ProcessLifecycleOwner.onStart() to restart scanning when app returns from background
- **Privacy stale timeout** — Increased from 60s to 180s (BLE advertisers are intermittent)
- **FindMy/AirTag noise** — Separated into own collapsed category (threat level 0), won't trigger threat banner
- **BLE Trackers decluttered** — Dropped from threat level 3 to 1, collapsed by default in tree view
- **ESP32 tracker entries removed** — Removed AirTag/FindMy/Tile/Samsung/DULT from ESP32 glasses detector to focus on glasses + drones + cameras
- **Dead code cleanup** — Deleted unused GlassesDetectionPrefs.kt
- **BleTracker thread safety** — directionSamples synchronized, finishDirectionScan takes snapshot under lock

## [0.28.0-beta] - 2026-03-25

### Added
- **ESP32 OLED tracking lock** — Double-tap BOOT button to lock onto a privacy device. Signal strength bar shows RSSI + percentage + CLOSER/AWAY/SAME direction relative to normalized baseline. LED indicates proximity (red=close, cyan=medium, green=far).
- **ESP32 ACK confirmation** — Long-press shows "ACKNOWLEDGED / Marked as friendly" overlay for 2 seconds with "[OK] device whitelisted" status bar
- **ESP32 double-tap detection** — 2 taps within 800ms = lock tracking, 3 taps = cycle scan mode
- **Hybrid WiFi+BLE scanning** — Time-sliced: 25s BLE + 5s WiFi every 30s. Detects hidden camera SSIDs + Meta WiFi transfer hotspots alongside BLE privacy devices
- **WiFi privacy scanner** — 40+ hidden camera SSID patterns, Meta/Luxottica OUI hotspot detection
- **Triple-tap scan mode cycling** — [B+W] Hybrid → [BLE] BLE-only → [WiFi] WiFi-only
- **Dedicated Privacy tab** — 5th nav tab (Shield icon) with categorized tree view, 10 privacy categories
- **136 detection signatures** — 39 BLE names, 15 CIDs, 12 UUIDs, 70 WiFi SSIDs

### Fixed
- **OLED flicker eliminated** — Single atomic flush per frame instead of double-flush
- **Status bar stability** — Shows "LIVE" when <2s, no more rapid counter updates
- **FindMy iPhone noise** — Only actual AirTag accessories (length 0x19), not phone relays
- **Samsung CID noise** — Removed 0x0075 (matched all Samsung devices)
- **iBeacon/Eddystone/FastPair** — Dropped below threshold, filtered out
- **ESP32 stack overflow** — Display task 4096→6144 bytes

## [0.27.0-beta] - 2026-03-25

### Fixed
- **ESP32 display stack overflow** — Increased display task stack from 4096 to 6144 bytes to accommodate glasses_detection_t array
- **Samsung CID 0x0075 false positives** — Removed Samsung general CID (matched ALL Samsung devices: phones, watches, earbuds). SmartTags now detected by specific UUID 0xFD5A/0xFD59 only (no false positives)
- **ESP32 glasses code forward declaration** — Fixed C compilation order issue with static variables

### Added
- **ESP32 BLE scanner glasses detection verified** — Confirmed working: AirTag/FindMy, Apple iBeacons, Google Eddystone beacons all detected correctly. Samsung spam eliminated. Meta glasses detection ready (CID 0x01AB/0x058E).

## [0.26.0-beta] - 2026-03-25

### Added
- **25 new detection signatures** — Total now **136 signatures** (39 BLE names, 15 CIDs, 12 UUIDs, 70 WiFi SSIDs)
- **New BLE trackers**: Chipolo, Pebblebee, Eufy SmartTrack, Nutale name patterns
- **New WiFi cameras**: Wyze, Reolink, TP-Link Tapo, Hikvision, Dahua, Amcrest, Arlo, Blink, Eufy, Furbo pet camera, Petcube, Nanit baby monitor
- **New WiFi action/dash cams**: Akaso, SJCAM, Rexing, Apeman, YI dashcam
- **ESP32 parity**: Same false positive fixes (Glass→Glass EE, removed Google CID, raised threshold) and new tracker names applied to ESP32 glasses_detector.c

## [0.25.0-beta] - 2026-03-25

### Added
- **Dedicated Privacy tab** — New 5th bottom nav tab (Shield icon) with full categorized tree view of all privacy detections. Categories: Smart Glasses, BLE Trackers, Hidden Cameras, Body Cameras, Vehicle Cameras, Attack Tools, Action Cameras, Dash Cameras, IoT Devices, Informational.
- **Category tree view** — Expandable/collapsible sections grouped by threat level. High-threat categories (glasses, trackers, cameras, attack tools) expanded by default. Informational (beacons, Fast Pair) collapsed.
- **PrivacyCategory enum** — 10 categories with icons, labels, and threat levels (0-3)
- **Status bar** — Shows scanning status, total device count, and threat count
- **Per-device cards** — Tap to expand details. Shows RSSI with color coding (red=close, orange=near, gray=far), parsed BLE details, match reason, confidence %, and action buttons
- **Device detail dialog** — Full packet breakdown with all parsed fields

### Fixed
- **FindMy iPhone noise eliminated** — Only flags actual AirTag/FindMy accessories (length byte 0x19), not iPhones/iPads/Macs relaying FindMy. Further filters: separated AirTags flagged as threats, near-owner AirTags shown as informational
- **Google device noise eliminated** — Removed Google CID 0x00E0 from manufacturer database (matched all Nest/Chromecast/Pixel devices). Glass EE detected by name only.
- **Eddystone/Fast Pair noise reduced** — Moved to informational category at lower confidence

### Changed
- **Privacy alerts moved from List to own tab** — List screen is now clean for aircraft/drones only
- **5-tab navigation** — AR View, Map, List, Privacy, History

## [0.24.0-beta] - 2026-03-25

### Fixed
- **False positives eliminated** — Removed overly broad "Glass" name pattern (matched everything), Amazon CID 0x0171 (matched all Amazon devices), Xiaomi UUID 0xFD2E, generic "Camera-" and "Cam-" patterns. Raised confidence threshold from 0.50 to 0.60.
- **Flickering stopped** — Privacy scanner now debounces UI updates to every 2 seconds instead of every BLE scan result

### Added
- **Expandable privacy scanner** — Collapsed "Privacy Scanner — N devices" header, tap to expand into full device cards with all details
- **Per-device actions** — Each detected device has Ignore, Track, and Details buttons
- **Ignore/dismiss devices** — Tap "Ignore" to permanently suppress a false positive. Persists across app restarts via SharedPreferences
- **Device detail dialog** — Tap any device for full info: MAC, RSSI, confidence, match reason, camera status, and all parsed BLE packet details
- **Direction scan trigger** — "Track" button starts BLE direction finder for that specific device

## [0.23.0-beta] - 2026-03-25

### Added
- **Deep BLE packet parsing** — New `BlePacketParser` extracts rich details from every BLE advertisement:
  - Apple AirTag/FindMy: battery level (Full/Medium/Low/Critical), separated-from-owner flag
  - Apple AirPods/Beats: device family, left/right/case battery percentages
  - Samsung SmartTag: state (offline/connected/overmature), aging counter (how long separated), battery
  - iBeacon: proximity UUID, major/minor IDs, TX power → distance
  - Eddystone-URL: decoded broadcast URL
  - Eddystone-TLM: beacon battery voltage, temperature, uptime
  - Google Fast Pair: 24-bit model ID
  - TX Power + RSSI → estimated distance in meters
  - BLE address type (Public vs Random, API 33+ native, heuristic fallback)
- **Details in privacy alerts** — Detected devices now show parsed info inline (battery, separated status, distance, beacon data)

## [0.22.0-beta] - 2026-03-25

### Added
- **WiFi evil twin / rogue AP detection** — Detects same SSID on multiple BSSIDs with mixed security (classic evil twin), different vendor OUIs (rogue AP), and single BSSID broadcasting many SSIDs (karma/Pineapple attack)
- **Ultrasonic beacon detection** — FFT analysis of microphone input at 48 kHz to detect inaudible 18-22 kHz tracking beacons (SilverPush, Lisnr, Shopkick, Signal360). Off by default — requires microphone permission
- **EMF sweep detector** — Magnetometer-based electromagnetic field detector for finding hidden electronics at close range (1-5cm). Real-time magnitude gauge with color-coded threat levels
- **IR camera detector** — Front camera analysis for detecting night-vision IR LEDs from hidden cameras in dark rooms. Detects bright saturated clusters persisting across frames
- **30+ new WiFi SSID patterns** — V380 variants, Tuya/SmartLife, iCam365, XMeye, CareCam, BVCAM, P2PCam, ThroughTek, TinyCam/SpyGear, AI-Thinker, DEPSTECH/Jetion endoscopes, Tactacam trail cameras, ez Share WiFi SD cards, DDPai/Garmin dashcams
- **8 new BLE signatures** — Tuya IoT (CID 0x07D0), Xiaomi (UUID 0xFD2E), AB Shutter3 camera remotes, DEPSTECH endoscopes, Roborock/iRobot/Ecovacs robot vacuums
- **Settings toggles** for WiFi evil twin detection and ultrasonic beacon detection

## [0.21.0-beta] - 2026-03-25

### Added
- **Per-source detection toggles** — Settings screen with on/off switches for: ADS-B Aircraft, BLE Remote ID (Drones), WiFi Detection (Drones), Privacy Scanner, and Stalker/Follower Detection. All default to ON. Changes take effect on next app launch.
- **BLE stalker/follower detection** — Tracks all detected BLE devices over time. Alerts when a device follows you across multiple locations (2+ min, 50+ meters movement) or lingers near you while stationary. Threat levels: Low (tracking), Medium (persistent), High (camera device following 5+ min). Red "STALKER ALERT" banner in list screen.
- **BLE direction finder engine** — Lock onto any detected BLE device and rotate 360° to map RSSI to compass bearing. Estimates the direction to the device with confidence scoring. Uses top 20% of RSSI samples with circular mean for accuracy.
- **DetectionPrefs** — Centralized settings class replacing GlassesDetectionPrefs. Controls all detection source toggles via SharedPreferences.
- **BleTracker** — New tracking engine that records BLE sightings with location context, calculates user movement, detects followers, and provides direction-finding scan API.

## [0.20.0-beta] - 2026-03-24

### Changed
- **Privacy detection ON by default** — Smart glasses, trackers, hidden cameras, and all privacy device scanning now enabled out of the box. Toggle in About > Settings.
- **README rebrand** — Friend or Foe is now positioned as a "Privacy Awareness & Airspace Detection" platform. README leads with privacy detection capabilities, documents all 60+ device signatures.

### Added
- **Expanded privacy database** — Added Apple AirTag/FindMy, Samsung SmartTag, Tile, DULT unwanted tracker protocol, Google Find My Device, iBeacon, Eddystone, Tesla Sentry Mode, Flipper Zero, Motorola body cameras, hidden camera BLE names, and 25 WiFi SSID patterns (hidden cameras, action cams, dash cams, attack tools, doorbell cameras)
- **WiFi privacy scanning** — WiFi scan results now checked against 25 suspicious SSID patterns for hidden cameras, spy cams, dash cams, action cameras, body cameras, and attack tools

## [0.19.0-beta] - 2026-03-24

### Added
- **Smart glasses / privacy device BLE detection** — New detection module identifies Meta Ray-Ban, Snap Spectacles, Xreal, Vuzix, Google Glass, Bose Frames, Amazon Echo Frames, Brilliant Labs, TCL RayNeo, Rokid, INMO, Even Realities, Solos AirGo, and Axon body cameras via BLE manufacturer Company IDs, service UUIDs, and device name patterns
- **Privacy alert OLED display** — When smart glasses are detected nearby, OLED shows alert with device type, manufacturer, signal strength, camera indicator, and estimated distance
- **Glasses detection JSON output** — New `"type":"glasses"` messages in serial JSON output with device info, confidence, match reason, and camera flag
- **KConfig toggle** — `CONFIG_FOF_GLASSES_DETECTION` (default: ON) enables/disables at build time; NVS runtime toggle via web flasher serial config
- **BLE Scanner web flasher card** — New "BLE Scanner" option on the web flasher page for standalone BLE drone + glasses detection firmware
- **BLE Scanner in CI** — GitHub Actions now builds and deploys BLE Scanner firmware alongside Scanner and Uplink
- **Web flasher manifest** — `manifest-ble-scanner.json` for ESP32-S3 BLE scanner firmware
- **README update** — BLE Scanner with smart glasses detection listed in ESP32 Hardware Edition section

### Detection Database
- 12 manufacturer Company IDs (Meta, Snap, Google, Vuzix, Bose, Axon, Brilliant Labs, TCL, Rokid, Amazon)
- 4 service UUID signatures (Meta 0xFD5F, Bose 0xFDD2, Snap 0xFE45, Amazon 0xFE15)
- 21 device name patterns covering all major smart glasses brands
- GAP Appearance 0x01C0 (eyeglasses) detection as fallback
- Confidence scoring from 0.50 to 0.95 based on match specificity

## [0.18.0-beta] - 2026-03-24

### Changed
- **Material Design 3 UI polish** — Comprehensive UI overhaul based on M3 guidelines audit (Gemini + Codex analysis)
- **Bottom navigation** — Cleaned up from 4 tabs + 2 stray IconButtons to proper 4-tab M3 NavigationBar. Reference Guide and About moved to overflow menu (⋮) in filter bar
- **Back arrows** — Replaced Unicode text arrows with proper `Icons.AutoMirrored.Filled.ArrowBack` + content descriptions across 5 screens
- **Theme system** — Added custom Typography (12 styles with weights, line heights, letter spacing) and Shapes (8/12/16/24dp corners) to MaterialTheme
- **Bottom sheets light theme** — Fixed ~20 hardcoded white/cyan text colors in ZoomViewSheet and SnapPhotoSheet — now readable in both light and dark themes
- **Category colors** — Added dark mode variants with lighter tones for readability
- **Detail screen** — Dynamic title shows callsign/serial instead of generic "Object Detail"
- **Screen transitions** — Added fade+slide animations to all navigation transitions
- **Dark mode map** — Map tiles invert colors in dark theme for consistent dark UI
- **Welcome screen** — Upgraded to ElevatedCard with tonal surface, fixed deprecated Divider
- **Haptic feedback** — Added haptics on AR label tap and long-press interactions
- **Spacing system** — Created Dimens.kt with named constants for consistent spacing

### Fixed
- **ConcurrentModificationException** — SkyObjectRepository.rebuildMergedList no longer modifies list during iteration (Critical bug C2)
- **Database query performance** — Added indices on object_id, object_type, last_seen, timestamp across HistoryEntity and TrackingEntity (DB v3→v4 migration)
- **WiFi permission check** — Added runtime NEARBY_WIFI_DEVICES permission check (Android 13+) before WiFi scanning
- **AutoCaptureEngine cleanup** — Removed pointless screenWidthPx/screenHeightPx normalization (coordinates already 0-1)
- **AR ground banner flicker** — Added pitch hysteresis (show <-10°, hide >-5°) to prevent banner flickering

## [0.17.0-beta] - 2026-03-24

### Fixed
- **Drone stale timeout** — Increased from 60s to 120s, matching ESP32 scanner. Drones now stay visible for 2 full minutes after last BLE update instead of disappearing too quickly.
- **MAC address in drone ID** — Fixed multi-drone-on-same-MAC issue where simulator's 2 drones shared one BLE address. Now keys drone state by serial number when available, preventing MAC-based IDs from appearing.
- **Map auto-centering** — Map no longer snaps back to user position while panning. Detects touch gestures and disables auto-center for 10 seconds after last interaction. Allows free exploration of the map.

## [0.16.0-beta] - 2026-03-24

### Added
- **Visual engines wired to UI** — ROI confirmation, motion detector, and SORT tracker now produce visible effects instead of being orphaned dead code
- **SORT label persistence** — Labels stay on screen during ML Kit dropouts via velocity-predicted synthetic detections
- **ROI confidence boost** — Triple-confirmed aircraft (radio + visual + ROI) get 20% brighter labels
- **Motion blob alerting** — Radio-silent moving sky objects trigger DarkTargetScorer "UNKNOWN FLYING" alerts
- **Multi-drone RID simulator** — 2 simulated drones: FOF-SIM-001 (80m/15m/s CW) + FOF-SIM-002 (120m/10m/s CCW)

### Fixed
- **SORT duplicate prevention** — Coasted tracks no longer duplicate ML Kit detections in same frame
- **ROI recycled bitmap guard** — Prevents crash when bitmap is recycled during ROI analysis
- **Removed USB OTG detector** — Not a contactless detection method

## [0.15.0-beta] - 2026-03-24

### Added
- **ROI-based visual confirmation** — Crops regions around ADS-B predicted positions, analyzes brightness/edge patterns at 5Hz to confirm aircraft visually present
- **Motion detection for radio-silent objects** — IMU-stabilized temporal differencing detects moving sky objects without radio signals (catches drones with no transponder)
- **SORT visual tracker** — Keeps AR labels alive between ML Kit frames using velocity prediction (up to 10 frame coast)
- **ESP32 probe request sniffing** — WiFi scanner now catches drone controllers probing for their drone's SSID, detecting operators even when the drone isn't broadcasting
- **193 aircraft reference entries** (+35 new: Su-57, FC-31, Y-20, H-6, Z-10, MQ-1, RQ-4, PC-24, SF50, AW169, Bell 505, CH-53, NH90, Mi-26, B-1B, B-52H, F-4, F-5, Kfir, and more)
- **135 airline callsigns** (+105 new: IndiGo, Korean Air, LATAM, Qatar Airways, Swiss, Iberia, Wizz Air, FedEx, UPS, and 95 more)
- **62 ICAO country hex ranges** (+43 new: Turkey, Saudi Arabia, UAE, Indonesia, Thailand, Poland, Sweden, Argentina, and 35 more)
- **195 drone WiFi SSID patterns** (+91 from baseline: DJI newer models, FPV systems, underwater drones, military datalinks, budget brands, counter-UAS)
- **52 drone OUI entries** (+23 from baseline: DJI variants, Autel, Skydio, Shield AI, AeroVironment, PowerVision, Silvus Technologies)

### Fixed
- **BLE drone GPS coordinates** — 2-byte offset in OpenDroneID service data parsing
- **BLE drone duplicates** — Wait for Basic ID serial before emitting drone
- **Autel OUI misattribution** — 78:8C:B5 was TP-Link, not Autel (removed)

### Improved
- **Chinese/Russian military classification** — Added Su-57, FC-31, Y-20, TU16 (H-6), WZ10 (Z-10), SU25, AN72 type codes
- **ESP32 scanner patterns synced** — 191 patterns matching Android's 195

## [0.14.0-beta] - 2026-03-23

### Added
- **ROI-based visual confirmation** — Crops small regions around ADS-B predicted positions and analyzes brightness/edge patterns to confirm aircraft are visually present. Runs at 5Hz on background thread, independent of ML Kit. Reduces false positives by focusing on where planes should be.
- **Motion detection for radio-silent objects** — IMU-stabilized temporal differencing detects moving objects in the sky even without ADS-B or Remote ID. Finds drones without transponders by detecting pixel changes between camera frames, compensating for phone movement using gyroscope data.
- **SORT visual tracker** — Simple Online Realtime Tracker keeps AR labels alive between ML Kit detection frames using velocity prediction. A plane detected in frame 1 stays tracked through frames 2-10 even if ML Kit temporarily misses it. Assigns stable tracking IDs across frames.

## [0.13.0-beta] - 2026-03-23

### Added
- **Trajectory prediction** — Dead-reckoning extrapolates aircraft positions between 5s ADS-B polls using heading, speed, and vertical rate. Labels now track smoothly instead of jumping.
- **Compass bias auto-correction** — Visual-radio match residuals used to estimate and correct compass drift in real-time
- **Triple photo capture** — Each capture saves clean photo + annotated (AR overlay + info panel) + AI-aimed zoomed shot
- **One-tap smart spotter** — Tap AR label to auto-lock, track 2s, then capture all 3 photos automatically
- **Trajectory direction arrows** — Cyan arrows on AR labels showing aircraft heading
- **Confidence-based label styling** — Labels fade from solid (fresh) to yellow (coasting) to gray (stale)
- **ESP32 BLE Scanner** — Standalone BLE Remote ID scanner with OLED drone list display (ESP32 original support)
- **OLED drone list** — Clean display showing drone ID, lat/lon, altitude, speed with yellow/blue split layout

### Fixed
- **BLE Remote ID scan filter** — Android was filtering by Service UUID (AD type 0x03) instead of Service Data (AD type 0x16), preventing detection of BLE Remote ID drones
- **BLE drone GPS coordinates** — 2-byte offset in service data parsing caused wrong lat/lon
- **BLE drone duplicates** — Location messages arriving before Basic ID created MAC-based duplicate entries
- **Camera FOV swap** — Horizontal/vertical FOV defaults were swapped (45/60 instead of 60/45), causing incorrect AR label placement. All 51 tests now pass.
- **ADS-B timestamp merge bug** — All aircraft got `Instant.now()` instead of actual API timestamps, making merge logic pick random winners
- **Scanner raw AD walker** — BLE parser now always tries raw AD structure walking even when NimBLE structured parse fails
- **MapViewModel thread safety** — Location listener uses AtomicBoolean to prevent double registration

### Improved
- **Adaptive visual-radio match threshold** — Tighter when data is fresh, looser when stale/extrapolated; match candidates increased from 2 to 5
- **DJI DroneID GPS validation** — Rejects near-null-island coordinates and physically implausible altitude/speed values
- **Faster compass convergence** — Adaptive P-controller gain (5s convergence vs 20s)
- **Aircraft images** — Replaced 10 wrong photos (H60, UH60, U2, R44, E135, SF34, BE76, C5, C5M, R22)
- **Type code mappings** — Added H47 (Chinook) + 12 new codes (SF50, PC24, RQ4, MQ1, etc.)
- **Simulator orbit** — 150m radius at 15 m/s for better map tracking visibility

## [0.11.0-beta] - 2026-03-22

### Added
- Scanner OLED display improvements
- Uplink enhancements and KConfig pin configuration

## [0.10.0-beta] - 2026-03-21

### Added
- **Scanner status LED** — 6 blink patterns on Scanner board (boot, idle, scanning, detection, UART heartbeat, error) using GPIO48 (S3) / GPIO27 (C5)
- **Uplink scanner-disconnect LED** — alternating long/short blink pattern when Scanner UART link is lost (5s timeout)
- **UART connection handshake** — Uplink flashes solid 2s "connected!" when Scanner's first status message arrives
- Version sync: all ESP32 firmware and web flasher versions now match project version

## [0.9.0-beta] - 2026-03-20

### Added
- **ESP32-C5 dual-band scanner** — 2.4 + 5 GHz WiFi 6 channel hopping (38 channels interleaved), RISC-V single-core support
- **French DRI parser** — "Signalement Electronique a Distance" beacon parsing (Android + ESP32)
- **Acoustic drone detector** — microphone-based drone detection using frequency analysis (Android)
- **Dark target scorer** — visual detection scoring for non-transmitting objects (Android)
- **Trajectory classifier** — flight path analysis for drone vs bird/aircraft discrimination (Android)
- **Sensor map API service** — crowd-sourced detection map endpoint (Android)
- **Backend triangulation service** — multi-sensor position triangulation
- **ESP32 web flasher C5 variant** — flash ESP32-C5 boards from browser
- ESP32-C5 sdkconfig with WiFi 6 (802.11ac/ax), BLE 5.0, single-core FreeRTOS

### Changed
- Scanner firmware builds for both ESP32-S3 and ESP32-C5 from shared source tree
- CI builds and packages both scanner variants; C5 firmware attached to releases
- Web flasher updated with C5 flash card and wiring diagram

## [0.8.0-beta] - 2026-03-19

### Added

#### Android — New ADS-B & Enrichment Sources
- **ADSB One** — 5th ADS-B source added to parallel query (ADSBx v2 format, no API key)
- **hexdb.io enrichment** — real aircraft database lookup replaces heuristic registration/type guessing. Returns actual registration, ICAO type code, manufacturer, type, and registered owner
- hexdb.io route lookup — resolves callsigns to origin/destination airport pairs
- Aircraft detail now populated from hexdb.io instead of returning failure when backend is unavailable

#### Android — Filtering & Search
- **Filter bar** on List and History screens — search by callsign/ICAO/registration, filter by category, detection source, and object type
- Advanced filter options: max distance, altitude range
- `FilterEngine` — stateless filter engine supporting all filter dimensions
- `FilterState` domain model for reactive filter UI state

#### Android — Auto-Capture & Threat Assessment
- **Auto-capture engine** — correlates ML Kit visual detections with radio-tracked sky objects to automatically photograph nearby aircraft/drones
- Auto-capture toggle button on AR screen with visual indicator
- **Drone threat assessment** on detail cards — risk level (Benign→Restricted) and threat classification (Civilian, Military ISR, Loitering Munition, FPV Combat, etc.) from OSINT drone database
- Autonomy level display (Manual, Semi-Autonomous, Fully Autonomous)

#### Android — Drone Database Expansion
- Drone database expanded with risk levels, threat classifications, and autonomy levels for all entries
- New enum types: `RiskLevel`, `ThreatClassification`, `AutonomyLevel`
- ~1,100 new lines of drone reference data

#### Backend — New ADS-B Sources (fixes bug B8: missing adsb.lol)
- **ADSB One** added to backend fallback chain
- **adsb.lol** added to backend fallback chain (was in Android but missing from backend)
- Backend ADS-B chain now: adsb.fi → airplanes.live → ADSB One → adsb.lol → OpenSky

#### Backend — Enhanced Enrichment
- **hexdb.io** — primary aircraft data source for detail endpoint (real registration, type, manufacturer, owner)
- **hexdb.io route API** — resolves callsigns to origin→destination routes
- **airport-data.com** — added as photo fallback source
- **hexdb.io thumbnails** — added as tertiary photo fallback
- Photo chain now: planespotters.net → airport-data.com → hexdb thumbnail → placeholder
- Concurrent fetching of hexdb.io data and photos for faster detail responses

#### ESP32 — Uplink Enhancements
- **WiFi AP mode** — uplink now runs AP + STA concurrently, creates `FoF-XXXX` hotspot for field configuration
- **HTTP status page** — embedded web server on `http://192.168.4.1` shows live device status, drone count, GPS, WiFi, battery
- Post-flash Web Serial configuration for WiFi credentials and backend URL
- Improved UART RX buffering and NVS configuration persistence

### Changed
- ADS-B fallback chain expanded from 4 to 5 sources (Android) and 3 to 5 sources (backend)
- `AircraftRepository.getAircraftDetail()` now queries hexdb.io instead of returning failure
- `DataSource` enum extended with `ADSB_ONE`

## [0.8.0-alpha] - 2026-03-18

### Added
- **ESP32 Hardware Edition** — dual-board drone detector (ESP32-S3 scanner + ESP32-C3 uplink)
- BLE Remote ID + WiFi promiscuous scanning ported from Android to ESP-IDF C firmware
- 104 SSID patterns, 29 OUI entries, Bayesian fusion engine — full detection parity with Android app
- UART inter-board protocol at 921,600 baud with newline-delimited JSON
- SSD1306 OLED display, GPS NMEA parser, battery monitor, status LED on uplink board
- HTTP batch upload to backend with offline ring buffer (100 batches)
- `POST /detections/drones` and `GET /detections/drones/recent` backend endpoints
- 27 native unit tests for parsers and fusion engine
- See `esp32/INSTALL.md` for hardware setup and `esp32/CHANGELOG.md` for full details

## [0.7.0-beta] - 2026-03-17

### Added
- Always-visible floating zoom +/- buttons on AR screen right edge
- Take Photo button captures full-resolution image to gallery
- Share confirmation bar with Android share sheet integration
- capturePhotoToGallery now returns saved URI for sharing
- ~75 new drone reference photos

### Fixed
- BLE scanner not stopped in RemoteIdScanner.stopScanning()
- Bounds checks in OpenDroneIdParser and DjiDroneIdParser
- ImageProxy not closed on exception in VisualDetectionAnalyzer
- Thread-unsafe LinkedList replaced with ConcurrentLinkedDeque in WifiDroneScanner
- Coroutine scope leaks in AdsbPoller and SkyObjectRepository
- Missing synchronized blocks for DronePartialState mutations
- ListViewModel.locationStarted race condition (now AtomicBoolean)
- DAO providers missing @Singleton in DatabaseModule
- BayesianFusionEngine beliefStates unbounded growth (capped at 500)

## [0.6.0-beta] - 2026-03-16

### Added
- 49 new drones across consumer 2024-25, budget/toy, enterprise, and military/defense categories
- 12 new WiFi SSID patterns for SIMREX, Neheme, AOVO, TENSSENX, Freefly, senseFly, Wingcopter, Flyability, DJI Flip/Neo
- Visual shape classification using bounding box aspect ratio and motion history (quad vs fixed-wing hints)
- WiFi beacon Remote ID parser
- Airline lookup and ICAO country code display
- WiFi channel utility for drone frequency detection

### Changed
- Visual detection tags now show shape hints ("DRONE? quad", "DRONE? fw")
- Mark CI releases as full releases instead of prerelease

## [0.5.1-beta] - 2026-03-15

### Fixed
- ADS-B silent failure in release builds: ProGuard was stripping Gson/Retrofit response classes, causing null deserialization and 0 aircraft displayed

### Added
- Combined Reference Guide screen with tabbed Aircraft (138) and Drones (52) view, replacing drone-only entry point

## [0.5.0-beta] - 2026-03-15

### Added
- Snap-to photo capture: tap any AR label to lock on, zoom, and capture full-resolution photos
- Aircraft Reference Guide with 138 searchable entries covering all bundled aircraft types
- 20 new drone database entries (DJI Neo, Mini 3, Agras T40, Bayraktar TB3, MQ-25 Stingray, XQ-58 Valkyrie, and more)
- 20 new drone photos sourced from Wikimedia Commons
- Zoom slider spans ultrawide to telephoto (all forward-facing lenses via CameraX)
- Zoom preset buttons (0.5x, 1x, 2x, 5x, Max) in snap photo sheet
- "View in Aircraft Guide" button on aircraft detail cards
- AircraftGuide navigation route with type code deep linking
- ImageCapture use case added to CameraX pipeline for full-resolution photo capture

### Changed
- AR label tap behavior: now opens snap-to photo sheet instead of detail bottom sheet
- Camera zoom range now tracks minZoomRatio for ultrawide lens access

## [0.4.0-beta] - 2026-03-15

### Added
- Drone reference guide with searchable database of common drone types
- 68 new bundled aircraft photos (AC-130, AH-64, SR-71, U-2, UH-1, and more)
- 9 government/law enforcement callsign patterns (Secret Service, US Marshals, DEA, ICE, State Police, and more)
- 8 additional military type codes (E-4B, C-17A, C-130J, AC-130, F-117, AV-8B, EA-18G, C-12)
- 12 README translations (Hebrew, Ukrainian, Arabic, Turkish, Azerbaijani, Turkmen, Pashto, Urdu, Kurdish, Armenian, Georgian, Persian)
- Drone photo download script (`scripts/download_drone_photos.py`)

### Changed
- Government aircraft silhouette fallback changed from BIZJET to NARROWBODY for better accuracy when type code is unavailable

### Fixed
- Government aircraft without type codes no longer show business jet silhouette

## [0.3.0-beta] - 2026-03-14

### Added
- Welcome/launch screen with app info, version display, and quick links to GameChangers and GitHub
- In-app "Check for Updates" button — queries GitHub Releases API for newer versions
- 134 bundled aircraft photos as APK assets (offline, no network needed)
- Photo download script (`scripts/download_aircraft_photos.py`) for sourcing photos from Wikimedia Commons
- Coil image loading library for efficient photo display
- Aircraft photo display on detail cards with bundled asset fallback
- GitHub Actions CI workflow for automated builds on push and PR
- CHANGELOG.md

### Changed
- Detail screen now shows aircraft photos from bundled assets instead of requiring network
- Navigation updated to support welcome screen as start destination

## [0.2.0-beta] - 2025-03-14

### Added
- 120+ aircraft silhouettes mapped to ICAO type codes (10 vector drawable categories)
- Styled map markers with distinct shapes per aircraft category
- OpenStreetMap 2D map view with distance rings, compass-follow, and FOV cone
- Aircraft detail cards with full metadata (registration, operator, route, squawk)
- Detection history with persistent Room database storage
- MIT License and open-source release

### Changed
- Improved permission handling and onboarding flow
- Security review and hardening

## [0.1.0-beta] - 2025-03-12

### Added
- Initial release — AR viewfinder with floating labels
- Multi-source detection: ADS-B, FAA Remote ID (BLE), WiFi SSID, ML Kit visual
- Bayesian sensor fusion engine for multi-sensor confidence scoring
- Military aircraft classification (callsign patterns, squawk codes, operator DB)
- ARCore + compass-math hybrid orientation tracking
- Three-tier ADS-B fallback chain (adsb.fi → airplanes.live → OpenSky)
- Optional Python FastAPI backend for aircraft enrichment
- Hilt dependency injection, Room database, Retrofit networking
- List view with sortable columns
- Bottom navigation (AR, Map, List, History, About)

[0.10.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.10.0-beta
[0.9.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.9.0-beta
[0.8.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.8.0-beta
[0.7.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.7.0-beta
[0.6.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.6.0-beta
[0.5.1-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.1-beta
[0.5.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.5.0-beta
[0.4.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.4.0-beta
[0.3.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.3.0-beta
[0.2.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.2.0-beta
[0.1.0-beta]: https://github.com/lnxgod/friendorfoe/releases/tag/v0.1.0-beta

# Backend Sensor Firmware Design

**Date:** 2026-08-01
**Status:** Approved for implementation planning
**Branch:** `codex/backend-firmware`

## Goal

Create a fully independent, headless firmware family for the existing
three-board Seeed Studio XIAO ESP32-S3 sensor hardware. It carries the proven
badge sensing, scanner coordination, investigation, recovery, and update
capabilities back into the fixed sensor platform, replaces the LCD with Wi-Fi
backend upload, and uses the three onboard yellow LEDs for local health and
threat indication.

## Approved Product Boundary

One physical backend sensor is one three-board assembly:

- one XIAO ESP32-S3R8 uplink/coordinator;
- one XIAO ESP32-S3R8 BLE-primary scanner;
- one XIAO ESP32-S3R8 Wi-Fi-primary scanner.

The two scanner boards run the same binary. The uplink assigns their roles at
runtime. The unit uses the badge wiring:

| Link | Scanner pins | Uplink pins |
| --- | --- | --- |
| BLE-primary / slot 0 | TX GPIO1, RX GPIO2 | RX GPIO2, TX GPIO1 |
| Wi-Fi-primary / slot 1 | TX GPIO1, RX GPIO2 | RX GPIO4, TX GPIO3 |

All UART links run at 921600 baud with 3.3-V logic and a common ground.

The backend firmware excludes every badge presentation/game feature:

- ST7735 LCD and framebuffer;
- display lanes, themes, navigation, and display-policy controls;
- badge buttons and power chords;
- Easter egg assets and animation;
- CON CRUD game radio, state, presentation, and factory roles.

It retains the sensing product:

- BLE and Wi-Fi Remote ID;
- DJI vendor-IE, Wi-Fi Beacon/NAN/Action RID, French DRI, drone SSID, and OUI
  detection;
- BLE fingerprints, JA3-like structural evidence, company/service/name
  evidence, Meta Glasses classification, trackers, venue beacons, privacy
  devices, and behavioral BLE threat detection;
- Wi-Fi AP inventory, probe, association, anomaly, and lock-on evidence;
- Bayesian fusion and full scanner evidence fields;
- on-demand BLE investigation;
- dual-scanner coordination, time relay, flow control, recovery, UART firmware
  relay, rollback, and backend-driven OTA;
- backend heartbeat, offline buffering, status, and reporting telemetry.

## Hard Isolation From Badge Firmware

The backend firmware is a vendored source snapshot, not another build flag in
the badge projects. Its complete source and build graph lives under:

```text
esp32/backend-firmware/
  README.md
  shared/
  scanner/
    CMakeLists.txt
    platformio.ini
    main/
    partitions_backend_scanner_8mb.csv
  uplink/
    CMakeLists.txt
    platformio.ini
    main/
    partitions_backend_uplink_8mb.csv
  test/
  tools/
  web-flasher/
```

The portable source baseline is the accepted firmware contained in repository
commit `2cca5ad8df17ebd8d5f48dc72051441e30df1b8f`, including the published
`v0.67.2-badge-defcon34` work. Required code is copied into the new tree and
then simplified there.

Backend firmware must not compile or include source from these existing paths:

- `esp32/uplink/`;
- `esp32/scanner/`;
- `esp32/shared/`;
- `esp32/web-flasher/` badge manifests;
- badge flashing, recovery, or factory tooling under `scripts/`.

Implementation must not edit those paths. A source-isolation test and final
Git path audit enforce this constraint. This deliberately accepts code
duplication so later backend work cannot change a working badge binary.

## Firmware and Release Identity

The backend family starts an independent release track at
`0.1.0-backend` with a private `FOF_VERSION_BACKEND` definition inside the new
source tree.

| Binary | Firmware target | ESP app project | Hardware type |
| --- | --- | --- | --- |
| Uplink | `uplink-s3-backend` | `fof_backend_uplink` | `seeed_xiao_esp32s3` |
| Scanner | `scanner-s3-combo-backend` | `fof_backend_scanner` | `seeed_xiao_esp32s3` |

Every artifact filename, image descriptor, runtime identification line,
scanner heartbeat, catalog record, manifest, and OTA compatibility check uses
these backend identities. Badge and production targets are never aliases or
accepted substitutes.

The node's operational `device_id` remains its existing NVS value or legacy
`uplink_XXXXXX` MAC-suffix identity. This preserves registered names,
locations, calibration, and history during an upgrade. Firmware target and
hardware identity are separate fields. Status also exposes the full hardware
MAC for diagnostics without changing the backend registry key.

## Components

### Backend scanner binary

The scanner owns passive radio capture, parsing, evidence construction,
Bayesian fusion, and newline-delimited UART output. On boot it keeps its radios
quiescent until the uplink supplies a role. Slot 0 becomes `ble_primary`; slot
1 becomes `wifi_primary`. If exactly one scanner remains usable, the uplink
may assign `hybrid_failover`. Both scanners preserve command ingress while
their normal detection output is flow-controlled or quieted for firmware
relay.

The scanner accepts backend-only commands for role/profile, time, flow control,
LED state, BLE investigation, investigation cancellation, health, recovery,
and OTA. Firmware commands retain strict target, project, hardware, version,
generation, size, CRC32, SHA-256, and session validation.

### Backend uplink binary

The uplink owns:

- both scanner UART links and role enforcement;
- scanner identity/health snapshots and cross-slot deduplication;
- time synchronization and periodic scanner time broadcasts;
- local threat classification for LED selection only;
- unfiltered HTTP detection upload;
- heartbeat and reporting telemetry;
- setup-AP, USB serial, NVS, and Wi-Fi station configuration;
- bounded outage buffering and retry policy;
- pull-based backend commands and BLE investigation result delivery;
- backend-only self-OTA and serialized scanner-image relay.

The uplink BLE controller remains disabled in normal operation. It does not
act as a third scanner and does not run the badge game advertiser.

### FastAPI backend changes

The current `POST /detections/drones` contract remains the primary ingest API
so old and backend-firmware nodes coexist. The schema is extended additively to
preserve all fields already carried from scanner to uplink, including distinct
frequency/channel values, fused confidence, motion/vertical-speed fields,
Remote ID accuracy/type fields, BLE threat evidence, scanner slot/mask, queue
telemetry, and backend target/capability metadata.

The firmware catalog gains only these targets:

- `uplink-s3-backend`;
- `scanner-s3-combo-backend`.

BLE investigation uses a narrow pull contract suitable for nodes behind NAT:

- `GET /nodes/{device_id}/commands/next` returns an outstanding
  `ble_investigate` or `ble_investigate_cancel` command plus a stable command
  ID;
- `POST /nodes/{device_id}/commands/{command_id}/result` records progress,
  terminal results, truncation, authentication requirements, and errors;
- repeated polls and repeated result posts are idempotent by command ID.

## AP and Persistent Configuration

The headless uplink provides a setup portal at `http://192.168.4.1`.

The AP starts when any of these conditions is true:

- first boot has no valid Wi-Fi credentials or backend URL;
- no backend upload succeeds for five continuous minutes;
- the USB command `FOF_AP_START` requests it.

The SSID is `FriendOrFoe-Backend-XXXXXX`, using the uplink MAC suffix. The
initial password remains `friendorfoe` for compatibility and can be changed in
the portal. The portal supports:

- up to four ordered Wi-Fi SSID/password pairs;
- backend base URL;
- node display name;
- optional fixed latitude, longitude, and altitude;
- AP password;
- backend connectivity test;
- redacted health/status.

Configuration is validated before an atomic NVS commit. Existing single-network
keys (`wifi_ssid`, `wifi_password`, `backend_url`, `device_id`, and `ap_pass`)
are imported when the new ordered-network record is absent. Passwords and
credentials are never returned by HTTP or status JSON.

After a successful backend upload, the AP allows a 30-second client grace
period and then shuts down. Scanner operation continues while the AP is active.
The portal can change configuration and view health but cannot stage or flash
firmware.

## Runtime Data Flow

```text
BLE-primary scanner --- UART slot 0 ---\
                                       > backend uplink --- Wi-Fi/HTTP ---> FastAPI
Wi-Fi-primary scanner - UART slot 1 ---/
                            |
                            +--- mirrored yellow-LED state to both scanners
```

1. The uplink initializes NVS, the LED engine, UART listeners, scanner command
   ingress, and provisioning state.
2. It obtains network time through SNTP, with `GET /detections/time` as a
   fallback, and broadcasts epoch milliseconds plus validity to both scanners
   every ten seconds.
3. It assigns BLE-primary and Wi-Fi-primary roles and repeats role/profile
   enforcement until acknowledged.
4. Each authorized scanner detection receives `scanner_slot` and
   `scanner_slots_seen` metadata. The uplink deduplicates cross-slot copies.
5. Every detection enters two independent paths:
   - the complete, unfiltered record enters the HTTP batch queue;
   - a copy enters the local badge-derived threat policy for LED state only.
6. The uploader flushes by item count, encoded byte limit, or idle timeout and
   posts to `/detections/drones`.
7. An empty heartbeat posts every 60 seconds with firmware identity, scanner
   health, time health, Wi-Fi telemetry, queue depth, upload counters, and LED
   state.
8. The uplink polls for backend commands, routes BLE investigation work to the
   BLE-primary scanner, and posts bounded progress/results.

No display filter, LED policy, or local threat ranking may suppress backend
upload.

## Yellow LED Contract

Official XIAO hardware provides one active-low yellow/orange user LED on
GPIO21. There is no onboard RGB LED. All three board LEDs mirror the uplink's
selected logical state.

The uplink sends this newline-delimited command to each scanner every two
seconds and whenever state changes:

```json
{"type":"led_state","state":"drone","generation":42,"ttl_ms":6000}
```

Scanners accept only known state names and monotonic generations. If a command
expires, a scanner enters `uart_lost` locally. LED driving uses no heap
allocation and no second timer/RMT peripheral.

| State | Active-low GPIO21 pattern |
| --- | --- |
| `healthy` | 80 ms on, 2920 ms off |
| `network_degraded` | 300 ms on/off twice, then 1800 ms off |
| `drone` | 400 ms on, 120 ms off, 120 ms on, then 1360 ms off |
| `meta` | four 100 ms on / 100 ms off pulses, then 1000 ms off |
| `drone_meta` | one complete `drone` cycle followed by one complete `meta` cycle |
| `fatal` | three 120 ms on / 120 ms off pulses, then 800 ms off |
| `uart_lost` | 1000 ms on, 1000 ms off |

State priority is:

1. no usable scanning or fatal firmware failure;
2. simultaneous live drone and Meta Glasses evidence;
3. live drone evidence;
4. live Meta Glasses evidence;
5. Wi-Fi/backend degradation;
6. healthy heartbeat.

The copied badge threat classifier supplies live/stale decisions, including
its stricter 15-second drone-SSID and 90-second Meta live windows. Scanner
status does not fabricate a threat.

## Upload Reliability and Error Handling

The first release remains compatible with the existing trusted-LAN HTTP
deployment. Internet-facing TLS, enrollment, node credentials, and replay
protection are outside this port.

The uploader uses a maximum 4096-byte encoded batch and a 512-entry PSRAM FIFO.
The encoder flushes before the limit, so queued JSON is never truncated. The
FIFO preserves order. When capacity is exhausted, it drops the oldest complete
batch and increments an exposed overflow counter. The queue is intentionally
volatile across a full reboot to avoid unbounded flash wear.

Socket writes loop until the entire request is sent or the connection fails.
Retry behavior is explicit:

- retry connection failures, timeouts, HTTP 408, HTTP 429, and HTTP 5xx with
  bounded exponential backoff and jitter;
- do not retry HTTP 400/other permanent schema failures; quarantine the batch
  summary and increment a schema-error counter;
- do not clear the FIFO merely because uploads have been stale;
- validate the JSON response, echoed `device_id`, and accepted count before
  removing a live or buffered batch.

If the backend is unavailable, scanning, local threat classification, UART
recovery, and LED operation continue. One missing scanner causes
`hybrid_failover` plus degraded telemetry. Two unusable scanners produce the
`fatal` LED state. Scanner and network watchdogs report and recover their own
subsystems without using the LCD or rebooting solely because one upload failed.

## OTA and Recovery Safety

The backend updater accepts only exact backend identities. Metadata and images
must match target, app project, hardware, version ordering, byte size, SHA-256,
and partition capacity. The scanner relay also retains generation, CRC32,
session, scanner MAC/slot, post-reboot identity, role, command-ingress, radio,
and rollback-clear checks.

The XIAO 8-MB uplink layout provides dual self-OTA application slots and one
scanner staging partition large enough for the accepted scanner image. Builds
fail if either application or staged scanner artifact exceeds its partition.
ESP-IDF pending-verify rollback remains enabled.

No backend artifact accepts badge or production images. No badge manifest
lists backend images. AP configuration never mutates firmware. USB remains the
physical recovery path for the uplink; scanner recovery normally uses the
uplink's serialized UART relay.

## Testing Strategy

Implementation follows test-driven development. Host-native tests cover:

- firmware identity and target rejection;
- source-tree isolation and forbidden include paths;
- role assignment, fallback, time validity, deduplication, and flow control;
- LED selection, exact timing, mirrored commands, monotonic generation, and
  TTL fallback;
- AP trigger policy, four-network validation, legacy NVS migration, atomic
  persistence, backend test, secret redaction, and shutdown;
- complete detection serialization and 4096-byte boundary behavior;
- FIFO ordering, overflow, retry classification, partial sends, response
  validation, and reconnect behavior;
- BLE investigation command/result idempotency;
- OTA target/project/hardware/hash/version/partition rejection.

Backend `pytest` coverage includes:

- a fixture generated by the real C serializer accepted by FastAPI;
- preservation of every newly supported evidence field;
- empty heartbeat and node-status behavior for backend identities;
- legacy device ID/location preservation;
- backend firmware catalog and download metadata;
- command polling and idempotent result submission;
- invalid payload, replay, outage, and clock-skew cases.

Verification requires clean native tests plus clean PlatformIO builds of
`uplink-s3-backend` and `scanner-s3-combo-backend`. A final Git audit must show
no modifications under the protected badge/production firmware paths.

## Hardware Canary and Rollout

No hardware is flashed without explicit operator approval. The first physical
canary proceeds as follows:

1. Connect one old sensor assembly and record all three MACs, firmware
   identities, partitions, scanner roles, and current configuration.
2. Back up recoverable flash/NVS data and retain the original binaries and
   recovery commands.
3. Flash the backend uplink over USB, then stage the single backend scanner
   image through its UART relay to both scanner slots.
4. Verify first-boot AP configuration, Wi-Fi connection, time sync, scanner
   role acknowledgements, heartbeat, detection upload, all LED states, BLE
   investigation, outage buffering, OTA refusal cases, reboot recovery, and
   backend location continuity.
5. Soak the canary before changing another physical unit.
6. Roll out one unit at a time, retaining a working rollback route throughout.

## Acceptance Criteria

The design is complete when:

- every produced firmware binary and catalog record uses a backend-only
  identity;
- one XIAO uplink and two XIAO scanners operate with the approved wiring and
  roles;
- no LCD, badge UX, Easter egg, or game dependency exists in either image;
- all passive badge sensing and BLE investigation capabilities reach the
  backend without local display filtering;
- the AP can provision and recover the headless uplink;
- all three yellow LEDs show the approved synchronized patterns and safe stale
  fallback;
- network outages do not stop sensing and buffered JSON is never truncated or
  silently acknowledged;
- backend-only OTA cannot cross-flash badge or production images;
- existing node identity, position, and calibration remain associated after
  upgrade;
- automated tests and both backend firmware builds pass;
- the protected badge/production firmware paths have no Git changes.

## Explicit Non-Goals

- modifying, rebuilding, releasing, or flashing badge firmware;
- changing existing badge or production firmware target identities;
- independent HTTP upload from each scanner board;
- LCD, theme, navigation, badge button, Easter egg, or CON CRUD behavior;
- claiming battery or GPS support that the XIAO assembly does not provide;
- internet-grade authentication/TLS in the initial trusted-LAN port;
- fleet flashing before one physical canary passes and the operator approves
  expansion.

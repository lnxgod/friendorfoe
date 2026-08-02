# Friend or Foe Badge

Pocket RF awareness for Packet Village, and a path from a handheld badge to a
deployable sensor platform.

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)
[![ESP32 Build](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/esp32-web-flasher.yml)

Friend or Foe started as an Android aircraft and drone identification app. The
current center of gravity is the FoF Badge: a three-board ESP32-S3 handheld
that listens passively for nearby RF evidence, shows the most useful signals on
a small display, and can hand a live feed to Android over USB-C. Local AP and
debug status remain diagnostic surfaces, not Android control transports.

For the Packet Village talk, the badge is the story: a conference-wearable
privacy and drone awareness device that can also be converted into a fixed
sensor node. Same radios, same policy engine, same Android control surface,
same backend ingest path. Walk around with it during the day; mount it later as
part of a multi-node sensor platform.

> Current published tracks: Android/backend/production S3 firmware are on
> `0.64.68-live-follow`; the published badge/factory release is
> `0.64.76-badge-defcon34`. A local provisional canary source identity,
> `0.64.87-badge-defcon34`, is limited to the connected three-board canary;
> it is not a production-readiness or public-release version. The published
> release adds the themed four-lane instrument UI, custom USB palettes, DEF
> CON 34 Easter egg, quiet/off mode, and automatic integrity-checked scanner
> updates from the USB-connected uplink. The local canary replaces that
> physical quiet/off shortcut with a ten-second dual-button software reset.
> The badge and production sensor fleet intentionally move on separate
> firmware tracks.

## What The Badge Does

- Shows walk-up awareness for privacy and drone signals without needing a cloud
  account, SIM card, or paid API.
- Separates top-level alerts from lower-priority BLE and Wi-Fi lanes so the
  display stays readable in a noisy venue.
- Connects to Android over USB-C for a richer live view, display filters,
  theme and custom-palette controls, and diagnostics.
- Can be re-used as a sensor node by giving it stable power, a backend URL, and
  a fixed position.
- Keeps local recovery practical: USB-C status and staging, automatic scanner
  UART relay flashing, and scanner crash/status reporting are built into the
  badge flow.

## Badge Hardware

One physical badge trio contains:

| Board | PlatformIO environment | Job |
|-------|------------------------|-----|
| Uplink MCU | `uplink-s3-fof_badge` | Display, USB-C control, read-only local status, scanner relay flashing |
| BLE-primary scanner MCU | `scanner-s3-combo-fof_badge` | BLE Remote ID, BLE fingerprints, privacy-device BLE evidence |
| Wi-Fi-primary scanner MCU | `scanner-s3-combo-fof_badge` | Wi-Fi beacons/probes/data frames, SSID/OUI evidence, drone Wi-Fi evidence |

The two scanner boards run the same firmware image. The uplink assigns runtime
roles and scanner profiles, which is what lets the same physical design behave
like a handheld badge or a stationary sensor.

### Build One

The DEF CON badge is buildable from the fabrication and mechanical files in
[`hardware/badge/`](hardware/badge/). One complete badge uses:

| Qty | Component | Exact part used for the DEF CON run |
|----:|-----------|--------------------------------------|
| 3 | MCU/radio board | [Seeed Studio XIAO ESP32-S3 3-pack, SKU 102010573](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32S3-3PCS-p-5919.html) |
| 3 | External 2.4 GHz Wi-Fi/Bluetooth antenna | [Abracon APAGM2525-S2450 RHCP patch antenna](https://abracon.com/datasheets/APAGM2525-S2450.pdf) |
| 3 | Antenna coax lead | Seeed lead included with the XIAO ESP32-S3 pack |
| 1 | Battery connector | JST-clone `S2B-PH-SM4-TB(LF)(SN)` |
| 1 | Lithium-ion cell | [NDNNAS 103665 PH2.0](https://www.aliexpress.us/item/3256811602344151.html) |
| 2 | Push button | [4.5 x 4.5 x 3.8 mm, four-pin SMD, SPST-NO](https://www.amazon.com/dp/B07CJSV1ZW?th=1) |
| 1 | Display | [1.8-inch, 128 x 160, full-color SPI module](https://www.aliexpress.us/item/3256805953674718.html) |
| 1 | Badge PCB | [Single-board Gerbers](hardware/badge/fabrication/friend-or-foe-badge-single-board.zip), or one badge from the [five-badge/two-core panel](hardware/badge/fabrication/friend-or-foe-badge-oshpark-panel-5-badges-2-cores.zip) |
| 1 | Battery cage | Printed from the [battery-cage STL](hardware/badge/mechanical/battery-cage-dc34phv.stl) |

The direct board and component cost for the 45-badge DEF CON run was roughly
**$80 per badge**, excluding tools, labor, 3D-printer time or material, and
general shop supplies. Prices and availability will move. Read the
[hardware guide](hardware/badge/README.md) before ordering: it records the
battery-polarity, button-footprint, display-fit, panel, and antenna lessons from
the actual build. The original seven-part CSV is published with its supplied
rows and ordering intact alongside the fabrication files.

## What It Listens For

Friend or Foe is passive. It listens for signals already being broadcast and
tries to explain them with conservative labels.

| Category | Examples | Evidence |
|----------|----------|----------|
| Remote ID drones | ASTM/OpenDroneID, DJI DroneID, Wi-Fi Beacon RID | BLE, Wi-Fi vendor IEs, beacon frames |
| Drone Wi-Fi | DJI, Skydio, Parrot, Autel, test drones | SSID patterns, OUI hints, Bayesian fusion |
| Smart glasses | Ray-Ban Meta, Oakley Meta, Snap Spectacles, Xreal, Vuzix | BLE names, service UUIDs, manufacturer data |
| Trackers | AirTag/Find My, Tile, SmartTag, Chipolo, Pebblebee | BLE service UUIDs and company data |
| Possible listening | Apple Continuity with connected AirPods and nearby audio activity | BLE manufacturer data, activity flags, RSSI |
| Cameras and tools | Hidden/IP cameras, body cams, dash cams, Wi-Fi Pineapple, deauth tools | BLE names, Wi-Fi setup SSIDs, curated signatures |
| Flock / ALPR | Flock Safety, ELSAG-style ALPR evidence | Registered Flock Safety Wi-Fi OUI, curated ALPR signatures |
| Evil twin / rogue AP | Open clones, karma-style attacks, suspicious SSID reuse | Wi-Fi auth/channel/BSSID anomaly policy |

Flock / ALPR detection is intentionally strict. Flock Safety rows require the
IEEE registered Wi-Fi OUI `B4:1E:52`; Flock-looking Bluetooth names and SSIDs
such as `Flock-*`, `FlockOS*`, `FLK-*`, `ALPR-*`, and `Penguin-*` are not treated
as Flock evidence. Curated non-Flock ALPR signatures, such as ELSAG SSIDs, stay
separate.

## Android As Badge Console

Android is the operator console for the badge. The connected badge control
center receives live status/events and gives the operator first-class controls
from the app:

- Appearance: palette, background, brightness, and per-class threat accents.
- Display policy: enable or hide alert classes, choose BLE/Wi-Fi/both lanes,
  proximity rules, row-density presets, and display priorities.
- Firmware status: inspect the staged manifest and automatic scanner-update
  queue. Actual firmware mutation stays on the laptop USB script: UART flashes
  the uplink, then the uplink relays the scanner image automatically.
- Diagnostics: scanner role, firmware version, crash count, heap/stack, PSRAM,
  display policy hashes, scanner acknowledgement hashes, and recovery mode.

True title/detail text sizing and explicit row-count limits need the next
badge display-policy schema; the app does not fake those settings until the
firmware can apply them.

Relevant Android code lives under:

- `android/app/src/main/java/com/friendorfoe/data/badge/`
- `android/app/src/main/java/com/friendorfoe/presentation/badge/`
- `android/app/src/main/java/com/friendorfoe/presentation/privacy/`

## From Badge To Sensor Platform

The Packet Village demo is a badge, but the architecture is a sensor platform:

1. Put the badge or production S3 node in a fixed location.
2. Give the uplink Wi-Fi credentials and a backend URL.
3. Register the node with a stable name and position.
4. Let it POST detections to the FastAPI backend.
5. Add more nodes for correlation, triangulation, and RF anomaly history.

Once deployed, the backend groups BLE fingerprints, Wi-Fi probe identities, AP
inventory, drone evidence, and node health into live entities. With multiple
nodes, it can localize signals with RSSI-based triangulation and smooth them
with an EKF. The dashboard becomes the long-running view; Android remains the
field console.

## Repo Map

| Path | Purpose |
|------|---------|
| `android/` | Kotlin + Jetpack Compose app, badge console, privacy views, AR/list/map screens |
| `backend/` | FastAPI ingest, enrichment, dashboard, triangulation, calibration, firmware endpoints |
| `esp32/scanner/` | ESP32-S3 scanner firmware for BLE/Wi-Fi detection |
| `esp32/uplink/` | ESP32-S3 uplink firmware, display, USB-C control, read-only local status, UART OTA relay |
| `esp32/shared/` | Shared C detection policy, badge display policy, themes, signatures, protocol types |
| `hardware/badge/` | Badge BOM, fabrication Gerbers, panel, and battery-cage STL |
| `docs/badge/` | Badge operator notes and current badge version matrix |
| `scripts/` | Badge flashing, debug bridge, recovery, and utility scripts |

## Build And Test

Android debug build:

```sh
cd android
./gradlew assembleDebug
```

Focused Android badge/privacy tests:

```sh
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.PrivacyCategoryMappingTest
./gradlew testDebugUnitTest --tests com.friendorfoe.presentation.privacy.BadgePrivacyMapperTest
```

Backend setup and tests:

```sh
cd backend
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
pytest tests -v
```

ESP32 native policy tests:

```sh
cd esp32
pio test -e test
```

Build badge firmware:

```sh
cd esp32/uplink
pio run -e uplink-s3-fof_badge

cd ../scanner
pio run -e scanner-s3-combo-fof_badge
```

Flash a badge from the repo root:

```sh
python3 scripts/fof_badge_flash.py \
  --transport usb \
  --only all \
  --port /dev/cu.usbmodemXXXX
```

Useful recovery variants:

```sh
python3 scripts/fof_badge_flash.py --transport usb --only uplink --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --transport usb --only scanners --port /dev/cu.usbmodemXXXX
```

Only the uplink USB connection is required. If the scanner diagnostic USB
cables are intentionally connected too, add `--bind-selected-uplink` so the
operator explicitly binds `--port` to the uplink in the three-device census.
Allow up to ten minutes after the build for serialized scanner convergence and
do not unplug while progress is advancing.

Scanner USB is diagnostics-only. Flashing a scanner directly is disabled:
stage one scanner image through the uplink USB connection and let the uplink
perform the serialized UART updates for both scanner slots.
See [Badge Scanner Recovery](docs/badge_scanner_recovery.md) for exact success
evidence and the fail-closed recovery procedure.

## Runtime Checks

USB serial:

```text
FOF_PING
FOF_STATUS
FOF_CTL:{"cmd":"badge_display_policy_reset","persist":true}
```

Badge local AP (read-only):

```sh
curl http://192.168.4.1/api/badge/status
```

Badge mutations are accepted only over the uplink's USB serial connection.
`POST /api/badge/control` returns `403 badge_control_requires_usb`.

Healthy badge facts:

- Top-level `recovery_mode` is `normal`.
- Both scanners are connected and report `scanner-s3-combo-fof_badge`.
- Uplink and scanners report the current badge firmware version.
- `display_policy_hash` is non-zero.
- Scanner `display_policy_ack_hash` catches up to the uplink policy hash.

## Talk Track

The badge makes a good Packet Village talk because it is understandable at
three depths:

- Walk-up: "What is my badge hearing right now?"
- Builder: "How do three tiny ESP32-S3 boards become a useful RF instrument?"
- Platform: "How does a handheld demo become a fixed sensor network with
  backend correlation, firmware updates, and triangulation?"

The important design choice is the conversion path. The badge is not a dead-end
novelty; it is the smallest operator-friendly shape of the same system that can
run as a fleet.

## Docs

- Badge hardware and BOM: [hardware/badge/README.md](hardware/badge/README.md)
- Badge operator guide: [docs/badge/README.md](docs/badge/README.md)
- Badge recovery: [docs/badge_scanner_recovery.md](docs/badge_scanner_recovery.md)
- Badge boundary notes: [docs/fof_badge_notes.md](docs/fof_badge_notes.md)
- Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Threat model: [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md)
- Bayesian fusion: [docs/BAYESIAN_FUSION.md](docs/BAYESIAN_FUSION.md)
- Triangulation: [docs/TRIANGULATION.md](docs/TRIANGULATION.md)

## AI Workflow

Friend or Foe is built with AI-assisted engineering. Claude helped bootstrap
the earliest Android architecture and first implementation wave. Codex is now
the primary engineering orchestrator for repo maintenance, firmware work,
reviews, tests, Android badge controls, docs, and release prep. Grok contributed
design direction, Gemini helped with technology-stack research, and ML Kit runs
on-device for visual detection.

## Security And Configuration

Start from `backend/.env.example`. Do not commit filled `.env` files, OpenSky
credentials, Android signing material, or generated ESP32 Wi-Fi headers such as
`esp32/uplink/main/core/wifi_credentials.h`.

Friend or Foe is a passive awareness tool. It should explain what it heard,
show uncertainty, and avoid overclaiming identity from weak RF evidence.

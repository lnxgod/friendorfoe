# FoF Badge

This folder is the operator entry point for the handheld FoF Badge. The badge is
not the production sensor-node fleet: it is a three-board XIAO ESP32-S3 assembly
used for walk-up privacy/drone awareness and Android USB-C testing.

## Current Versions

- Android app/backend: `0.64.40-badge-ble-theme`
- FoF Badge firmware: `0.64.40-badge-ble-theme`
- Production S3 firmware: `0.63.0-svc156`

Keep those tracks separate. The badge firmware uses `FOF_BADGE_VARIANT`,
badge-specific pinning, a Waveshare ST7735 display, USB-C control, local AP
status, scanner relay flashing, and safe USB recovery. Production
`uplink-s3`, `scanner-s3-combo`, and `scanner-s3-combo-seed` remain on the
production auto-OTA track.

## Hardware Boundary

One physical badge trio contains:

- Uplink MCU: `uplink-s3-fof_badge`
- BLE-primary scanner MCU: `scanner-s3-combo-fof_badge`
- Wi-Fi-primary scanner MCU: `scanner-s3-combo-fof_badge`

The scanner firmware image is shared by the BLE and Wi-Fi scanner boards; the
uplink assigns the active role and scanner profile at runtime.

## What This Release Tests

`0.64.40-badge-ble-theme` is the badge BLE/theme control release:

- The badge keeps drone, Meta Glasses, tracker, WiFi attack, and scanner health
  evidence separated into calm top awareness tiles plus BLE/WiFi lower lanes.
- Android can connect directly over USB-C, continuously poll `FOF_STATUS`, and
  show badge evidence, source, confidence, RSSI, GPS/operator facts, scanner
  health, reset/crash state, stack, heap, and PSRAM diagnostics in Privacy.
- Badge-only BLE control can be opened with BTN2 for a 10-second pairing window;
  bonded phones can reconnect later and use the same status/control parser as
  USB-C, Badge AP, and the debug bridge.
- Android exposes safe badge appearance controls for palette, background,
  brightness, and per-class threat accents; the badge persists theme v1 in NVS
  and reports `theme`, `theme_hash`, and `ble_control` in status.
- Remote ID evidence is prioritized through scanner UART pressure, carries
  display IDs, RSSI, GPS/operator fields, and avoids ambiguous `RID SIGNAL`
  rows once a real RID entity is decoded.
- Badge scanners expose RID drop/evict counters, richer BLE hints, recovery
  mode, crash counts, and policy acknowledgements in `FOF_STATUS`.
- Safe USB and scanner safe-mode recovery remain part of the badge-only flow;
  scanner crash history can be cleared from the uplink after a successful fix.
- Android/backend stay on the same badge version so USB-C badge testing and
  backend ingest status agree with the firmware release.

## Build And Flash

From the repo root:

```sh
python3 scripts/fof_badge_flash.py --transport usb --port /dev/cu.usbmodemXXXX
```

Useful recovery-focused variants:

```sh
python3 scripts/fof_badge_flash.py --transport usb --only uplink --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --transport usb --only scanners --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --manual-scanner ble --port /dev/cu.usbmodemYYYY
python3 scripts/fof_badge_flash.py --manual-scanner wifi --port /dev/cu.usbmodemZZZZ
```

Manual scanner flashing requires unplugging the target scanner MCU from the
badge trio and connecting that scanner directly over USB.

## Android Install For Badge Testing

Build and install the matching APK:

```sh
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Then connect the badge over USB-C, grant USB permission, and check:

- Privacy screen: shows `USB-C` plus `Badge live privacy feed` when connected.
- List screen: badge panel shows scanner health, latest badge events, and the
  `Display Filters` editor.
- Display Filters: apply/reset should update `display_policy_hash`; scanner
  objects should eventually report matching `display_policy_ack_hash`.

## Runtime Checks

USB serial:

```text
FOF_PING
FOF_STATUS
FOF_CTL:{"cmd":"badge_display_policy_reset","persist":true}
```

Local AP/backend badge status:

```sh
curl http://192.168.4.1/api/badge/status
curl -X POST http://192.168.4.1/api/badge/control \
  -H 'content-type: application/json' \
  -d '{"cmd":"badge_display_policy_reset","persist":true}'
```

Expected healthy status facts:

- Top-level `recovery_mode` is `normal`.
- Both scanners are connected and report `scanner-s3-combo-fof_badge`.
- Uplink and scanners report `0.64.40-badge-ble-theme` after the matching badge
  images are flashed.
- `display_policy_hash` is non-zero.
- Scanner `display_policy_ack_hash` catches up to the uplink policy hash.

## Recovery Docs

- Badge scanner recovery: [../badge_scanner_recovery.md](../badge_scanner_recovery.md)
- Badge boundary and guardrails: [../fof_badge_notes.md](../fof_badge_notes.md)
- Production ESP32 install docs: [../../esp32/INSTALL.md](../../esp32/INSTALL.md)

When the badge is stuck, prefer safe USB recovery first. ROM bootloader entry is
still available, but safe USB keeps `FOF_STATUS`, `FOF_PING`, recovery mode, and
scanner facts visible while you repair the trio.

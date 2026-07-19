# FoF Badge

This folder is the operator entry point for the handheld FoF Badge. For the
Packet Village talk, this is the main artifact: a three-board XIAO ESP32-S3
assembly for walk-up privacy/drone awareness, Android control, and conversion
into a fixed sensor platform.

The badge is not just a demo shell. It shares the same scanner/uplink split,
badge threat policy, display policy, backend ingest shape, and firmware relay
ideas used by the production sensor-node fleet. The practical story is: wear it
or carry it during the event, then give it stable power and a backend URL to
make it part of a larger RF sensor deployment.

## Current Versions

- Android app: `0.64.68-live-follow`
- Backend: `0.64.68-live-follow`
- FoF Badge firmware: `0.64.69-badge-defcon34`
- Production S3 firmware: `0.64.68-live-follow`

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

`0.64.69-badge-defcon34` is the current badge release, paired with the
`0.64.68-live-follow` Android, backend, and production track. Badge firmware
remains separate from production node firmware. This release verifies:

- The existing four fixed lanes remain intact while named themes and custom
  palettes restyle them as one coherent instrument interface.
- Android badge status, display policy, named themes, and custom palettes use
  USB-C only. Android firmware upload is intentionally disabled.
- A laptop flashes the uplink over its USB/UART bootloader, then stages one
  scanner image over USB. The uplink automatically updates strictly older
  scanners one at a time over their UARTs.
- Staged and relayed scanner images carry exact target/project/hardware/version,
  CRC32, SHA-256, generation, and session identity. Downgrades and unordered
  same-core variants fail closed; same-version rewrites require the explicit
  recovery flag.
- Post-update success requires the same scanner MAC, exact firmware identity,
  rollback-clear state, live UART commands, and the correct BLE-primary or
  Wi-Fi-primary radio profile after reboot.
- Holding both badge buttons for nine seconds toggles volatile quiet/off mode.
  The panel sleeps and scanners stop scanning while USB control, scanner UART
  commands, and firmware recovery stay alive. Reboot always returns ACTIVE.
- Exact `fof-michagain` Remote ID coordinates for Hell, Michigan at 666 m, the
  exact `fof-goblue` SSID, or the temporary spare-button trigger opens the
  purple DEF CON 34 Wall of Sheep Easter egg once per boot. Any button dismisses
  it.
- `FOF_STATUS` exposes the staged firmware manifest, serialized update queue,
  scanner roles, power convergence, heap/stack, PSRAM, and display state.

## Build And Flash

From the repo root:

```sh
python3 scripts/fof_badge_flash.py --transport usb --port /dev/cu.usbmodemXXXX
```

Useful recovery-focused variants:

```sh
python3 scripts/fof_badge_flash.py --transport usb --only uplink --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --transport usb --only scanners --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --manual-scanner ble --port /dev/cu.usbmodemYYYY --verify-port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --manual-scanner wifi --port /dev/cu.usbmodemZZZZ --verify-port /dev/cu.usbmodemXXXX
```

Manual scanner flashing requires unplugging the target scanner MCU, connecting
it directly over USB, and keeping the uplink on `--verify-port` so the tool can
prove the exact scanner identity and refuse downgrades. Normal fleet operation
uses the uplink USB staging path and automatic serialized UART convergence.

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

## Sensor Platform Conversion

To convert a badge into a fixed sensor:

1. Give the uplink stable power and Wi-Fi credentials.
2. Point it at the FastAPI backend used for the sensor fleet.
3. Register the badge location as a sensor-node position.
4. Keep the BLE-primary and Wi-Fi-primary scanner roles intact.
5. Use Android over USB-C for field checks and local display policy. Use the
   laptop USB flasher for firmware recovery and the backend dashboard for
   long-running correlation.

The badge remains useful as an operator display even when mounted. If the local
LCD is distracting, use display filters from Android to quiet classes or move
lower-priority evidence out of the top lanes.

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
- Uplink and scanners report `0.64.69-badge-defcon34` after the matching badge
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

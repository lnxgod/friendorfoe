# FoF Badge Notes

These notes are badge-only. They describe the handheld badge trio and should
not be treated as production scanner-node behavior unless a change explicitly
ports the same behavior to production.

For the operator-facing badge quick start, current version matrix, Android
install flow, and runtime checks, start with [Badge README](badge/README.md).

## Badge Boundary

- Uplink badge target: `uplink-s3-fof_badge`.
- Badge scanner target: `scanner-s3-combo-fof_badge`.
- Badge-only firmware paths are guarded by `FOF_BADGE_VARIANT`.
- The badge trio is one uplink MCU plus two scanner MCUs: BLE-primary and
  Wi-Fi-primary.
- Production scanner nodes can share lower-level detector code, but badge
  display policy, safe USB recovery, relay flashing, and end-user UX shortcuts
  are badge concerns first.

## Flashing And Recovery Must Stay Easy

- Keep `scripts/fof_badge_flash.py` working as the end-user badge trio flasher.
- Keep `esp32/uplink/tools/recover_fof_badge.py` working for no-button,
  hard-reset, and safe-USB recovery.
- Uplink recovery must preserve early USB control for `FOF_PING`, `FOF_STATUS`,
  `FOF_BOOTLOADER`, `FOF_REBOOT`, and safe-mode clear commands.
- Full uplink recovery flashes the bootloader, partitions, OTA data, and app
  image. Scanner flashing should only be required when scanner firmware or
  scanner-side telemetry changes.
- Safe USB mode is preferred over automatic ROM bootloader entry because it
  keeps status and control available to normal users.

## Recovery Modes And Status Facts

`FOF_STATUS` over USB and `/api/badge/status` over the temporary AP/backend
session are the source of truth. The LCD is intentionally a small awareness
surface and should not be expected to show every recovery field.

Top-level badge `recovery_mode` values:

- `normal`: USB control is alive and the badge is not in safe mode.
- `usb_wait`: boot is still waiting for the USB control task heartbeat.
- `usb_stale`: USB control heartbeat has gone stale after the boot grace
  window. The runtime watchdog should arm an expected reboot and come back in
  safe USB mode.
- `safe_usb`: badge safe mode. Heavy scanner/network behavior is held back;
  USB control and simple status remain available.

Scanner-object `recovery_mode` values are separate from the top-level badge
mode:

- `normal`: scanner image is running normally.
- `ota_pending`: scanner image is pending ESP-IDF OTA validation.
- `safe_uart`: scanner UART-only recovery. Radios stay off, but the scanner
  keeps emitting recovery/status lines and accepts OTA/reboot/bootloader/safe
  clear commands.

Planned badge software restarts must call `badge_runtime_arm_expected_reboot()`
before `esp_restart()` or ROM bootloader entry. This keeps intentional USB,
OTA, rollback, and recovery restarts from incrementing the crash-loop counter.
Unplanned `ESP_RST_SW`, panic, interrupt watchdog, task watchdog, and watchdog
resets remain unhealthy.

Common USB recovery commands:

- `FOF_PING`: returns `FOF_PONG:<version>`.
- `FOF_STATUS`: returns the badge status JSON, including scanner objects.
- `FOF_BOOTLOADER`, `FOF_DOWNLOAD`, or `FOF_FLASH`: reboot into ESP32 ROM
  download mode.
- `FOF_REBOOT`: expected software reboot back into the app.
- `FOF_CTL:{"cmd":"safe_mode","enabled":false,"reason":"post_flash_clear"}`:
  clear forced badge safe mode and reset the badge crash counter.

## Badge Display Policy

- The top BLE lane is primary privacy awareness. Active Meta Glasses stay first.
  Tags and tracker evidence may be shown, but should not displace active Meta
  Glasses.
- The top Wi-Fi lane is the clean drone/Wi-Fi aggregate. Drone evidence is shown
  as `DRONE NEAR` with the large number as the source of truth.
- The lower two swim lanes are detail lanes, not repeats of the top lanes:
  one lower lane is BLE/privacy protocol evidence, and one lower lane is
  Wi-Fi/drone protocol evidence. These are not physical scanner-slot labels;
  either scanner MCU may contribute to either protocol lane when its telemetry
  reports that evidence.
- Lower lanes dedupe by true display identity. Repeated reports from both
  scanners should merge before display.
- With exactly one live drone evidence item, do not spend a lower lane repeating
  `DRONE #1` or the same `DRONE SSID`. With multiple live drone evidence items,
  the Wi-Fi lower lane may scroll `DRONE #1`, `DRONE #2`, and `DRONE SSID`.
- Lower-lane text should behave like a small marquee for long IDs, SSIDs, or
  scanner details, while preserving row height and heat bars.

## Regression Guardrails

- Do not regress drone evidence counting: Remote ID and fresh drone SSID both
  count toward `DRONE NEAR`.
- Do not inflate Meta Glasses counts from scanner status counters or duplicate
  reports from both scanners.
- Keep UART stack and reboot telemetry visible in `FOF_STATUS` and
  `BADGE_DEBUG`.
- When changing badge UI or recovery, run native tests and at least build the
  badge uplink firmware before flashing hardware.

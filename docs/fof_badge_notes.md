# FoF Badge Notes

These notes are badge-only. They describe the handheld badge trio and should
not be treated as production scanner-node behavior unless a change explicitly
ports the same behavior to production.

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

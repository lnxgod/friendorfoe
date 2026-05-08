# Badge Scanner Recovery

This is badge-only. It targets one badge trio: uplink MCU, BLE scanner MCU,
and Wi-Fi scanner MCU.

## Direct Scanner USB Flash

Use this when a scanner is stuck, quiet, or showing stale relay state.

1. Disconnect the scanner MCU from the badge trio and plug that scanner into
   USB directly.
2. Flash the BLE scanner:

   ```sh
   ./scripts/fof_badge_flash.py --manual-scanner ble --port /dev/cu.usbmodemXXXX
   ```

3. Flash the Wi-Fi scanner:

   ```sh
   ./scripts/fof_badge_flash.py --manual-scanner wifi --port /dev/cu.usbmodemYYYY
   ```

4. Reconnect both scanners to the uplink and verify the trio:

   ```sh
   ./scripts/fof_badge_flash.py --transport usb --only scanners --skip-build --port /dev/cu.usbmodemZZZZ
   ```

## Relay Recovery Guardrails

Normal badge relay flashing skips scanners that already report the target
badge scanner version. Rewriting the same version is recovery-only and needs
both flags:

```sh
./scripts/fof_badge_flash.py --transport usb --only ble --force --allow-same-version --port /dev/cu.usbmodemZZZZ
```

`--skip-command-probe` is a last-resort option. It bypasses the scanner command
ingress proof and should only be used after direct USB recovery is impossible.

## Scanner Self-Patch Path

Once the uplink has staged scanner firmware, each scanner periodically sends
`fw_check`. If a newer staged badge scanner image exists, the scanner quiets
itself, emits `fw_ready`, and the uplink relays the image over UART. Failed
relay attempts now clear the scanner's ready latch, use a short badge-only
backoff, and retry from scanner-originated `fw_check` without requiring USB.

Watch these fields in `FOF_STATUS` or `/api/badge/status`:

- `fw_state`: `current`, `offered`, `ready`, `updating`, `deferred`, `error`, or `recovery`.
- `fw_backoff_s`: retry delay after a failed relay.
- `ota_state`: `idle`, `staging`, `validating`, `flashing`, or `rebooting`.
- `recovery_mode`: `normal`, `ota_pending`, or `safe_uart`.
- `last_fw_error`: last scanner-side OTA/relay failure reason.

## Scanner Safe Mode

After repeated validated-image crashes, badge scanner firmware enters
UART-only recovery mode. Radios stay off, but the scanner keeps emitting
`scanner_recovery`, `scanner_info`, and status lines so the uplink console and
badge status show why it is waiting. Safe mode accepts OTA, reboot, bootloader,
and safe-mode clear commands.

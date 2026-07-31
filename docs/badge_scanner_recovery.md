# Badge Scanner Recovery

This is badge-only. It targets one badge trio: uplink MCU, BLE scanner MCU,
and Wi-Fi scanner MCU.

For the broader badge boundary, end-user flashing expectations, and display
policy, see [FoF Badge Notes](fof_badge_notes.md). For the operator quick
start and current badge version matrix, see [Badge README](badge/README.md).

## Supported Uplink-to-Scanner Recovery

Scanner USB is diagnostics-only. `--manual-scanner` is disabled: do not
disconnect or direct-flash a scanner for a normal field update.

1. Keep both scanner boards connected to the uplink on their established
   internal UARTs.
2. Connect the laptop to the uplink USB port. The two scanner diagnostic USB
   cables may remain disconnected.
3. From the repository root, build and stage one scanner image through the
   uplink:

   ```sh
   python3 scripts/fof_badge_flash.py \
     --transport usb \
     --only scanners \
     --port /dev/cu.usbmodemXXXX
   ```

   For the private game canary, add:

   ```text
   --platform badge-trio-xiao-s3-con-crud-canary
   ```

   If multiple ESP32 USB cables are intentionally connected, also add
   `--bind-selected-uplink`. That flag is an operator acknowledgement that
   `--port` is the uplink in the complete three-device census.

4. Allow up to ten minutes after the build for the serialized UART campaign.
   Do not unplug while stage or relay progress is advancing.

Success requires exit code zero and the final
`[done] badge flash flow complete`, plus:

- the exact uplink hardware ID, target, version, running partition, and
  rollback-clear application state;
- one committed scanner manifest with exact target/project/hardware/version,
  CRC32, SHA-256, generation, and selected slot mask;
- the same two unique scanner hardware IDs after reboot;
- `pending_mask:0`, `worker_running:false`, and each selected older scanner
  terminally `converged` with at least one relay attempt;
- acknowledged `ble_primary`/`wifi_primary` roles, idle OTA state, normal
  recovery, zero rollback/crash state, and the assigned radio active again;
- reversible USB theme control after maintenance exits.

## Relay Recovery Guardrails

Normal badge relay flashing skips scanners that already report the target
badge scanner version. Rewriting the exact same version is recovery-only and
uses the explicit recovery flag:

```sh
python3 scripts/fof_badge_flash.py \
  --transport usb \
  --only ble \
  --recovery-rewrite-same-version \
  --port /dev/cu.usbmodemXXXX
```

`--force` does not bypass version safety, and `--allow-same-version` is
rejected. Downgrades remain forbidden.

`--skip-command-probe` is a last-resort option. It bypasses the scanner command
ingress proof and should be used only with captured evidence that the normal
probe itself is the failure.

If a normal run fails, preserve its transcript and let bounded cleanup return
the badge to normal mode. Capture a fresh `FOF_STATUS`, then rerun the exact
same uplink command once. A zero-attempt readiness failure receives at most one
targeted reprompt, and only after no peer relay is active. If the rerun still
fails, stop and diagnose the terminal campaign evidence; do not pivot to a
scanner-direct USB write.

## Scanner Self-Patch Path

Once the uplink has staged scanner firmware, each scanner sends one `fw_check`
eight seconds after boot, after identity and role setup have settled. If a newer
staged badge scanner image exists, the scanner quiets itself, emits `fw_ready`,
and the uplink relays the image over UART. There is no periodic scanner or
uplink update timer in the badge build. After a failed relay, retry by rebooting
the scanner or issuing the busy-safe USB `fw_check_now` command.

Watch these fields in `FOF_STATUS` or `/api/badge/status`:

- `fw_state`: `current`, `offered`, `ready`, `updating`, `deferred`, `error`, or `recovery`.
- `fw_backoff_s`: retry delay after a failed relay.
- `ota_state`: `idle`, `staging`, `validating`, `flashing`, or `rebooting`.
- Scanner-object `recovery_mode`: `normal`, `ota_pending`, or `safe_uart`.
- `last_fw_error`: last scanner-side OTA/relay failure reason.

The top-level badge `recovery_mode` is a different uplink field. Its current
values include `normal`, `update_preparing`, `update_maintenance`, `usb_wait`,
`usb_stale`, and `safe_usb`; see
[FoF Badge Notes](fof_badge_notes.md) for the no-button USB recovery contract.

## Scanner Safe Mode

After repeated validated-image crashes, badge scanner firmware enters
UART-only recovery mode. Radios stay off, but the scanner keeps emitting
`scanner_recovery`, `scanner_info`, and status lines so the uplink console and
badge status show why it is waiting. Safe mode accepts OTA, reboot, bootloader,
and safe-mode clear commands.

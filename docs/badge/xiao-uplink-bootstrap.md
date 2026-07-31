# XIAO Uplink One-Time Bootstrap Record

## Status

**INCOMPLETE — NO ROM WRITE OR RELEASE PROMOTION IS AUTHORIZED BY THIS RECORD.**

Historical pre-canary bootstrap checks:

- USB device present at `/dev/cu.usbmodem1101`.
- The pre-canary legacy application answered with version
  `0.64.76-badge-defcon34`, firmware `uplink-s3-fof_badge`, project
  `fof_badge_uplink`, hardware type `seeed_xiao_esp32s3`, normal recovery,
  clear pending-verify state, and live USB control.
- The legacy schema cannot prove the immutable MAC, partition, rollback, and
  USB-health fields required by the current application updater.
- Guarded `--before no_reset --after no_reset` ROM probe did not return a
  valid ROM identity.
- This record contains no ROM-write, erase, reset, or factory-promotion
  evidence. The connected canary trio was subsequently flashed with the local
  provisional candidate `0.64.78-badge-defcon34`; that fact does not complete
  any `PENDING` bootstrap or physical-acceptance item.

Use this record only for a badge whose legacy uplink cannot produce the exact
current application identity required by the guarded USB updater. Keep the
battery connected throughout. This is a one-time bootstrap into hardened
firmware, not a passing recovery-chord gate.

Hardware IDs, photographs, and raw transcripts are private. Store them under
`artifacts/badge-usb-hardening/bootstrap/`, which is ignored by Git. Record
their filenames and SHA-256 hashes below; do not commit the private payloads.

## Physical control identification

- Badge/session label: `PENDING`
- Battery continuously connected: `PENDING`
- Uplink board positively distinguished from both scanner boards: `PENDING`
- Badge Menu+OK controls identified: `PENDING`
- Uplink USB port explicitly selected: `PENDING`

The assembled badge does not expose a usable BOOT/RESET workflow. Do not probe
or short an unlabeled badge pad. Legacy recovery uses the already-supported
USB `FOF_BOOTLOADER` command; hardened firmware additionally supports the
10-second Menu+OK chord followed by an explicit OK confirmation.

## Read-only ROM identity proof

Keep the battery connected. The explicit legacy bootstrap path must:

1. Validate the canonical four-region candidate layout.
2. Take a stable census of every connected serial port and require zero
   pre-existing ROM devices; abort if the selected uplink port disappears or
   the port set changes during the census.
3. On one exclusive open serial handle, obtain the exact allowlisted legacy
   status.
4. Send exactly one `FOF_BOOTLOADER` line and require the exact
   `FOF_BOOTLOADER:OK` acknowledgement.
5. Close the application serial handle, retain its exact serial and physical
   USB-location binding, and actively attempt reset-neutral ROM sync. The
   ESP32-S3 ROM may retain the same pathname and descriptor metadata; do not
   require disappearance or a newly named device.
6. Require exactly one protocol-proven ROM responder within that retained
   binding, then run only the guarded no-reset ROM identity probe before any
   write.

The supported command is:

```bash
/Users/billh/.platformio/penv/bin/python \
  scripts/fof_badge_flash.py \
  --transport usb \
  --port /dev/cu.usbmodem1101 \
  --only all \
  --legacy-usb-bootstrap \
  --skip-build
```

Omit `--skip-build` when the canonical local artifacts have not already been
built. The explicit flag is intentionally required; malformed, current-schema,
unknown-version, unsafe-mode, or silent application status must fail before
the ROM command.

Record:

- Exact legacy status proved: `PENDING`
- Stable zero-ROM census proved: `PENDING`
- Exact bootloader acknowledgement received: `PENDING`
- ROM port: `PENDING`
- Chip: `PENDING`
- Revision: `PENDING`
- Flash size: `PENDING`
- PSRAM size: `PENDING`
- Base MAC: `PENDING`
- Read-only transcript filename: `PENDING`
- Read-only transcript SHA-256: `PENDING`
- ROM identity matches the selected physical uplink: `PENDING`

The USB descriptor serial and legacy application fields are not immutable
hardware identity. The unique ROM base MAC, 8 MB flash, and 8 MB embedded
PSRAM proof are required before a write.

## Historical `.76` layout reference — not a `.78` candidate proof

The table below is the captured pre-canary `.76` layout reference. It is not
an approved layout or hash proof for the current local provisional candidate
`0.64.78-badge-defcon34`. Do not use it to write `.78`; regenerate and
independently validate all four regions before any future ROM write.

| Offset | Region | Size | SHA-256 |
| --- | --- | ---: | --- |
| `0x00000` | bootloader | 20,928 | `af3b662268292b57476a3816fe767d0baba5dedea782a9e037ae1474e6a30f81` |
| `0x08000` | partition table | 3,072 | `0730efe516b42ac83a484c509291564b9bc1f891c122e3d3525293f0baa686bd` |
| `0x0f000` | initial OTA data | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `0x20000` | uplink application | 1,229,104 | `7c77f48133b338f6107e5c472df440f4efbda2fcc78822f84a4648aa3537812d` |

Before writing, rerun the canonical layout validator and replace this table if
any byte changes. The partition proof must retain `ota_0` at `0x20000`,
`ota_1` at `0x220000`, and the scanner cache at `0x420000`.

## Bootstrap result

- Guarded four-region write completed: `PENDING`
- Independent readback verification completed: `PENDING`
- Application returned on the same base MAC: `PENDING`
- Exact current application identity/status proved: `PENDING`
- Scanner image staged exactly once: `PENDING`
- BLE-primary scanner converged over UART: `PENDING`
- Wi-Fi-primary scanner converged over UART: `PENDING`
- Battery remained connected: `PENDING`
- Write/verify transcript filename: `PENDING`
- Write/verify transcript SHA-256: `PENDING`

Do not change any item to PASS from display appearance alone. The guarded
flasher and live `FOF_STATUS` proof are authoritative.

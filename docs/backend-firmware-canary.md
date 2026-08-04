# Legacy backend firmware three-board canary

> **Current 0.2 production boundary:** scanner0 and scanner1 stay on the
> production `scanner-s3-combo-fof_badge` ComboFO firmware. They may be
> inventoried, backed up, and restored with this tool, but must never receive
> `scanner-s3-combo-backend`. Both `challenge-flash` and `flash-initial` now
> reject scanner roles before any hardware access. Only the Lite uplink is a
> backend release and only `uplink-s3-backend` is published by the web flasher.
> Scanner migration commands retained later in this historical runbook are
> obsolete and intentionally fail closed.

| Role | Image | UART wiring | Primary radio | LED |
|---|---|---|---|---|
| Lite scanner0 | production `scanner-s3-combo-fof_badge` | TX GPIO1 → uplink RX GPIO2; RX GPIO2 ← uplink TX GPIO1 | BLE | production behavior |
| Lite scanner1 | production `scanner-s3-combo-fof_badge` | TX GPIO1 → uplink RX GPIO4; RX GPIO2 ← uplink TX GPIO3 | Wi-Fi | production behavior |
| Lite uplink | `uplink-s3-backend` | slot0 RX2/TX1; slot1 RX4/TX3 | infrastructure Wi-Fi/HTTP | active-low GPIO21 yellow |

“Lite” is only the nickname for this physical no-screen three-board assembly.
It is never a firmware target, project, hardware, artifact, or API identity.
The released backend identity is `uplink-s3-backend`/`fof_backend_uplink` on
`seeed_xiao_esp32s3`, version `0.2.0-backend`. Production scanner identity is
reported through the UART adapter and is not a backend release artifact.

The native badge is a separate firmware family. Native badge
`0.67.2-badge-defcon34` remains the default USB/factory firmware. Never select
the backend/Lite release for a badge, never rename a Lite binary as a badge
binary, and never use the badge flasher during this procedure.

This runbook is the initial canary procedure only. It uses guarded direct USB
with a fresh, explicit approval for every write. The backend web flasher is not
an initial-canary fallback. A failure stops the canary; restoration from the
verified, MAC-bound full backup is the only permitted next write, after a new
approval. Preparing this runbook and its evidence tool performs no hardware
action; do not run a write command until the corresponding receipt has been
shown and explicitly approved.

## Wiring and physical admission

All UART links are 921600 baud and require a common ground. Seeed's official
[XIAO ESP32-S3 pin map](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/)
maps D0/D1/D2/D3 to GPIO1/2/3/4. The board's user LED is GPIO21 and is
active-low, as described in Seeed's
[getting-started guide](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/).

GPIO21 is a single yellow/orange LED. It cannot physically emit purple, red,
or blue. The approved threat colors are represented by temporal patterns:

- Healthy: 80 ms on, 2920 ms off.
- Drone (“purple/orange”): 400 ms on, 120 ms off, 120 ms on, 1360 ms off.
- Meta Glasses (“red/blue”): four 100 ms pulses separated by 100 ms gaps,
  followed by 1000 ms off.
- Combined: one complete drone pattern followed by one complete Meta pattern.
- Network degraded: 300 ms on, 300 ms off, 300 ms on, 1800 ms off.
- UART lost: 1000 ms on, 1000 ms off.
- Fatal: three 120 ms pulses separated by 120 ms gaps, followed by 800 ms off.

Physically inspect all three boards before connecting them. No XIAO Sense SD
expansion may be installed or connected. Its SD chip-select use of GPIO3
conflicts with the Lite uplink's slot1 TX GPIO3. If the model or expansion
cannot be identified with certainty, stop at inventory. Do not move the UART
wires after capture begins.

Required canary inputs are:

- two no-screen Lite scanner XIAO ESP32-S3 boards and one no-screen Lite
  uplink XIAO ESP32-S3 board;
- the strict, verified backend release and release index;
- a pre-provisioned, operator-owned known-good Remote ID lab transmitter whose
  firmware is not built, flashed, or changed during this canary;
- a physical Meta Glasses test device, or a dedicated and explicitly recorded
  lab advertiser configured as described below; and
- HTTP access to the FastAPI backend that owns the preserved node ID.

Do not begin if a port might be a native badge, any board has a screen, any
board has an SD expansion, secure boot or flash encryption is enabled, the
three MAC addresses are not unique, or the exact release verifier is not
green.

## Phase 1: no-write capture, inventory, and backup

Leave the original Lite assembly powered, running, and wired. First list the
ports, identify each physical board, and export the original uplink URL and
backend URL. `capture-installed`, `inventory`, `backup`, and verification are
read-only. The successful uplink backup is last because it deliberately leaves
the original uplink quiescent in ROM/reset state.

```bash
cd backend-firmware
set -euo pipefail
umask 077
pio device list

BACKEND_PIO_BIN="$(command -v pio)"
BACKEND_PYTHON_BIN="$(command -v python3)"
: "${BACKEND_PIO_BIN:?PlatformIO pio is required}"
: "${BACKEND_PYTHON_BIN:?Python 3 is required}"
test -x "$BACKEND_PYTHON_BIN"
: "${BACKEND_ORIGINAL_UPLINK_URL:?export the original Lite uplink URL, for example http://192.168.1.50}"
: "${BACKEND_BASE_URL:?export the FastAPI base URL that owns the registered node}"
: "${BACKEND_SCANNER0_PORT:?export the scanner0 USB path from pio device list}"
: "${BACKEND_SCANNER1_PORT:?export the scanner1 USB path from pio device list}"
: "${BACKEND_UPLINK_PORT:?export the uplink USB path from pio device list}"

"$BACKEND_PYTHON_BIN" tools/backend_canary.py capture-installed --state .canary/canary-state.json --uplink-url "$BACKEND_ORIGINAL_UPLINK_URL" --backend-base "$BACKEND_BASE_URL" --output-dir .canary/installed
"$BACKEND_PYTHON_BIN" tools/backend_canary.py inventory --role scanner0 --port "$BACKEND_SCANNER0_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py inventory --role scanner1 --port "$BACKEND_SCANNER1_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py inventory --role uplink --port "$BACKEND_UPLINK_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"

"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind original --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind original --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
# UPLINK MUST BE LAST. Success leaves it quiescent; do not reset it.
"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind original --role uplink --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"

"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind original --role scanner0 --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind original --role scanner1 --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind original --role uplink --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py status --state .canary/canary-state.json
```

The phase passes only with the preserved operational device ID and location,
an exact calibration-continuity receipt, three structured installed
identities, both original scanner roles, updater-admission evidence, three
unique chip MACs, disabled secure boot/encryption, exact decoded partition
maps, three independent 8 MB full backups, matching focused NVS rereads, and
recorded SHA-256 values. All three boards must be explicitly confirmed as
no-screen Lite sensors with no SD expansion.

Stop if a board cannot enter USB ROM, either scanner cannot be read directly,
the original identity is unknown, a backup differs on reread, or the uplink is
not quiescent. UART OTA and the web flasher are not substitutes.

## Phase 2: three separately approved direct-USB writes

The flash order is scanner0, scanner1, uplink. Before every challenge, rerun
the no-write quiescence gate. `challenge-flash` writes a private receipt bound
to the live port, chip MAC, release hashes, offsets, toolchain, and current
state generation. Show that receipt to the user and stop. Never reuse an
approval value or approve more than one board at once.

The private `.canary` tree assumes the logged-in UID and operator are trusted;
do not run another process under that UID that can alter its state, receipts,
or staged files. The tool retains verified firmware file descriptors through
`write_flash` and makes the target MAC probe the final subprocess before the
write. A physical USB swap still cannot be prevented in software: from final
challenge review through command completion, do not touch, unplug, replug, or
swap any USB board or cable. If anything moves, stop without approving or
writing. This boundary is especially important because the native badge
`0.67.2-badge-defcon34` remains the default and must never receive a backend
image.

The `web-flasher/firmware/...` argument below names the verified release-byte
package only. `backend_canary.py` verifies those bytes and performs the guarded
direct-USB write; no browser or Web Serial flasher is opened or used.

### Scanner0

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-flash --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-challenge.json
# STOP. Show .canary/scanner0-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py flash-initial --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner0-challenge.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 30
```

### Scanner1

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-flash --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner1-challenge.json
# STOP. Show .canary/scanner1-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner1-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner1-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py flash-initial --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner1-challenge.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 30
```

### Uplink last

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-flash --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-challenge.json
# STOP. Show .canary/uplink-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/uplink-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/uplink-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py flash-initial --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/uplink-challenge.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 30
```

Each challenge expires after five minutes and is deleted after use. Stop after
any failure, unexpected identity, or evidence that the old uplink application
restarted. Do not continue to the next board. Once the backend uplink is
running and has assigned both scanner profiles, require all three final-health
records:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py status --state .canary/canary-state.json
```

All three must be exact backend identities, preserve their MAC/configuration,
and report rollback-clear final health before functional or OTA testing.

## Phase 3: AP provisioning and baseline

1. Join `FriendOrFoe-Backend-XXXXXX` with the factory password
   `friendorfoe`.
2. Open `http://192.168.4.1`.
3. Enter one to four ordered Wi-Fi credentials, the backend URL, node name,
   optional fixed location, and a replacement AP password.
4. Run the portal connectivity test. Verify that neither Wi-Fi nor AP password
   appears in status, HTTP evidence, or serial logs.

The evidence recorder automatically binds `.canary/evidence/canary.jsonl` to
the sibling `.canary/canary-state.json` continuity and final-health receipts.
Alternatively, set `BACKEND_CANARY_STATE` to that private state file.

```bash
: "${BACKEND_BASE_URL:?export the FastAPI base URL, for example http://127.0.0.1:8000}"
: "${BACKEND_DEVICE_ID:?export the preserved uplink_XXXXXX device ID shown by canary status}"
: "${BACKEND_TEST_DRONE_ID:?export the exact pre-provisioned lab transmitter Remote ID}"
: "${BACKEND_RID_SOURCE_DESCRIPTION:?export its operator-owned model/serial/MAC description}"
: "${BACKEND_META_MAC:?export the exact Meta device or lab-advertiser BLE MAC}"
mkdir -p -m 700 .canary/evidence .canary/serial-logs
chmod 700 .canary/evidence .canary/serial-logs
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase baseline --output .canary/evidence/canary.jsonl
```

The baseline requires queue depth zero, a valid clock, exact uplink identity,
both exact scanner identities and MAC/slot bindings, explicit heartbeat
`profile` values `ble_primary` then `wifi_primary`, current nonzero
`role_generation`, `role_acked:true`, `radio_healthy:true`, valid rollback
state, and the unchanged calibration-continuity receipt. A display `role`
string never substitutes for the `profile` field.

Visually time at least two complete healthy LED cycles on all three boards.

## Phase 4: real RF and LED acceptance

### Remote ID

Use only the recorded, pre-provisioned operator-owned lab transmitter. Keep it
off initially. Its firmware is outside this canary and must not be modified.
The atomically published ready file is the sole authorization to enable it.
A merely existing or nonempty file is not authorization: `state` must equal
`ready` and `ready_cutoff_ms` must still be in the future when parsed.
Keep the Phase 1 `set -euo pipefail` boundary active in the same main shell.
Any nonzero waiter, LED wait, or snapshot means immediately turn off every RF
source and stop the runbook; never continue to the next command after failure.

```bash
BACKEND_DRONE_AFTER_MS="$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')"
BACKEND_DRONE_READY=".canary/evidence/drone-$BACKEND_DRONE_AFTER_MS.ready"
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind drone --source wifi_beacon_rid --identity-field drone_id --identity-value "$BACKEND_TEST_DRONE_ID" --after-ms "$BACKEND_DRONE_AFTER_MS" --ready-file "$BACKEND_DRONE_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_DRONE_WAIT_PID=$!
until jq -e --argjson now_ms "$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')" '.state == "ready" and (.ready_cutoff_ms | type) == "number" and .ready_cutoff_ms > $now_ms' "$BACKEND_DRONE_READY" >/dev/null 2>&1; do kill -0 "$BACKEND_DRONE_WAIT_PID" || { wait "$BACKEND_DRONE_WAIT_PID"; exit 1; }; sleep 0.1; done
# Only now may the operator enable the exact recorded Remote ID source.
wait "$BACKEND_DRONE_WAIT_PID"
unset BACKEND_DRONE_WAIT_PID
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-led --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --expected drone --after-ms "$BACKEND_DRONE_AFTER_MS" --timeout-s 120 --output .canary/evidence/canary.jsonl
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase drone --output .canary/evidence/canary.jsonl
# Keep the source on; physically confirm two complete drone cycles on all three boards. Only then disable it.
unset BACKEND_DRONE_READY BACKEND_DRONE_AFTER_MS
```

Evidence must match the preserved device ID, `source:wifi_beacon_rid`, and the
exact Remote ID. Record `BACKEND_RID_SOURCE_DESCRIPTION` in the operator notes
and label this as lab data, never as a real aircraft. Confirm two complete
drone LED cycles on all three boards before disabling the source.

### Meta Glasses

Prefer a physical Meta Glasses device and record its BLE MAC. If unavailable,
use a dedicated lab advertiser configured in nRF Connect with local name
`Ray-Ban Meta` and advertised 16-bit service UUID `0xFD5F`; record its
hardware/MAC and label the result simulated. If neither exists, this gate is
blocked. UART or HTTP injection is not RF evidence.

```bash
BACKEND_META_AFTER_MS="$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')"
BACKEND_META_READY=".canary/evidence/meta-$BACKEND_META_AFTER_MS.ready"
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind meta --source ble_fingerprint --identity-field bssid --identity-value "$BACKEND_META_MAC" --manufacturer "Meta Glasses" --service-uuid-token fd5f --after-ms "$BACKEND_META_AFTER_MS" --ready-file "$BACKEND_META_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_META_WAIT_PID=$!
until jq -e --argjson now_ms "$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')" '.state == "ready" and (.ready_cutoff_ms | type) == "number" and .ready_cutoff_ms > $now_ms' "$BACKEND_META_READY" >/dev/null 2>&1; do kill -0 "$BACKEND_META_WAIT_PID" || { wait "$BACKEND_META_WAIT_PID"; exit 1; }; sleep 0.1; done
# Only now may the operator enable the exact recorded Meta source.
wait "$BACKEND_META_WAIT_PID"
unset BACKEND_META_WAIT_PID
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-led --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --expected meta --after-ms "$BACKEND_META_AFTER_MS" --timeout-s 120 --output .canary/evidence/canary.jsonl
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase meta --output .canary/evidence/canary.jsonl
# Keep the source on; physically confirm two complete Meta cycles on all three boards. Only then disable it.
unset BACKEND_META_READY BACKEND_META_AFTER_MS
```

Evidence must match the device ID, `source:ble_fingerprint`, exact BSSID,
`manufacturer:"Meta Glasses"`, and parsed UUID token `fd5f`. Confirm two
complete Meta LED cycles on all three boards before disabling the source.

### Combined threat

Turn both RF sources off, then run the two independent waiters below. Do not
enable either source until the single two-document authorization gate passes.

```bash
BACKEND_COMBINED_AFTER_MS="$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')"
BACKEND_COMBINED_DRONE_READY=".canary/evidence/combined-drone-$BACKEND_COMBINED_AFTER_MS.ready"
BACKEND_COMBINED_META_READY=".canary/evidence/combined-meta-$BACKEND_COMBINED_AFTER_MS.ready"
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind drone --source wifi_beacon_rid --identity-field drone_id --identity-value "$BACKEND_TEST_DRONE_ID" --after-ms "$BACKEND_COMBINED_AFTER_MS" --ready-file "$BACKEND_COMBINED_DRONE_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_COMBINED_DRONE_PID=$!
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind meta --source ble_fingerprint --identity-field bssid --identity-value "$BACKEND_META_MAC" --manufacturer "Meta Glasses" --service-uuid-token fd5f --after-ms "$BACKEND_COMBINED_AFTER_MS" --ready-file "$BACKEND_COMBINED_META_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_COMBINED_META_PID=$!
until jq -e -s --argjson now_ms "$("$BACKEND_PYTHON_BIN" -c 'import time; print(int(time.time()*1000))')" 'length == 2 and all(.[]; .state == "ready" and (.ready_cutoff_ms | type) == "number" and .ready_cutoff_ms > $now_ms)' "$BACKEND_COMBINED_DRONE_READY" "$BACKEND_COMBINED_META_READY" >/dev/null 2>&1; do kill -0 "$BACKEND_COMBINED_DRONE_PID" && kill -0 "$BACKEND_COMBINED_META_PID" || { wait "$BACKEND_COMBINED_DRONE_PID"; wait "$BACKEND_COMBINED_META_PID"; exit 1; }; sleep 0.1; done
# Only now may the operator enable both exact recorded RF sources.
wait "$BACKEND_COMBINED_DRONE_PID"
wait "$BACKEND_COMBINED_META_PID"
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-led --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --expected drone_meta --after-ms "$BACKEND_COMBINED_AFTER_MS" --timeout-s 120 --output .canary/evidence/canary.jsonl
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase drone-meta --output .canary/evidence/canary.jsonl
# Keep both sources on; physically confirm two complete combined cycles on all three boards. Only then disable both.
unset BACKEND_COMBINED_AFTER_MS BACKEND_COMBINED_DRONE_READY BACKEND_COMBINED_META_READY BACKEND_COMBINED_DRONE_PID BACKEND_COMBINED_META_PID
```

Confirm two complete combined patterns on all three boards. Reusing an earlier
cutoff, enabling either source before both parsed ready gates pass, or disabling
either source before the `drone_meta` LED wait and `drone-meta` snapshot both
pass invalidates this phase.

## Phase 5: network, UART, fatal, and command recovery

With both RF sources off, disconnect only the uplink's infrastructure network
for ten minutes. Keep power and UART wiring intact. Capture `outage-start`; at
minute five confirm AP startup and two network-degraded LED cycles on all three
boards. Reconnect, capture `outage-end`, and poll until the FIFO queue is zero.
Then record the required recovery boundary:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase network-recovery --output .canary/evidence/canary.jsonl
```

Next, leave scanner0 powered but disconnect only its TX/RX wires. Confirm
scanner0 `uart_lost`, scanner1 hybrid failover, continued detections, and no
uplink reboot. While that exact degraded state is present, capture it:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase scanner0-disconnected --output .canary/evidence/canary.jsonl
```

Reconnect both wires, wait for the exact
profile/generation/ACK/radio-health bindings and final health, then capture:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase scanner0-reconnected --output .canary/evidence/canary.jsonl
```

For the bounded fatal test, turn both RF sources off and disconnect both
scanner UART pairs. The uplink must enter `fatal`. Each isolated scanner cannot
receive the mirrored fatal state and must independently show `uart_lost`; do
not claim all three show fatal while both links are absent. Capture
`both-scanners-disconnected` while that exact degraded state is present:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase both-scanners-disconnected --output .canary/evidence/canary.jsonl
```

Promptly reconnect both pairs, require final health, capture recovery, and
observe healthy timing again:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase fatal-recovered --output .canary/evidence/canary.jsonl
```

Exercise the real backend BLE command path: cancel one passive command, then
allow a second to complete.

```bash
BACKEND_CANCEL_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
BACKEND_CANCEL_ID="$(printf '%s' "$BACKEND_CANCEL_JSON" | jq -er .command_id)"
curl -fsS -X POST "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/$BACKEND_CANCEL_ID/cancel" | jq -e --arg id "$BACKEND_CANCEL_ID" 'keys == ["command_id","mode","next_sequence","request_id","result_state","target","timeout_ms","type"] and .type == "ble_investigate_cancel" and .command_id == $id and .request_id == $id and .mode == "passive_capture" and .target == null and .timeout_ms == 12000 and .next_sequence == 0 and .result_state == null'
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_CANCEL_ID" --terminal-state cancelled --timeout-s 120 --output .canary/evidence/canary.jsonl

BACKEND_COMPLETE_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
BACKEND_COMPLETE_ID="$(printf '%s' "$BACKEND_COMPLETE_JSON" | jq -er .command_id)"
"$BACKEND_PYTHON_BIN" tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_COMPLETE_ID" --terminal-state complete --timeout-s 120 --output .canary/evidence/canary.jsonl
unset BACKEND_CANCEL_JSON BACKEND_CANCEL_ID BACKEND_COMPLETE_JSON BACKEND_COMPLETE_ID
```

Evidence requires an exact begin, gap-free FIFO event sequence, exact terminal
state and IDs, and no raw characteristic value or authentication material.

## Phase 6: catalog, no-write OTA probes, and approved recovery OTA

Only continue after direct-USB migration, functional acceptance, and all three
final-health checks. Prove that the backend catalog serves the exact verified
bytes, then capture a separate known-good backend recovery baseline. These
backup operations do not modify application or NVS contents.

```bash
BACKEND_RELEASE_INDEX="release/backend-release-index.json"
BACKEND_BASELINE_CATALOG_EVIDENCE=".canary/evidence/catalog-baseline-$(date +%s)-$$.json"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-catalog --backend-base "$BACKEND_BASE_URL" --index "$BACKEND_RELEASE_INDEX" --output "$BACKEND_BASELINE_CATALOG_EVIDENCE"
# At the first backend-baseline backup prompt, enter the exact path printed above.

"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 30
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180

"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 30
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180

"$BACKEND_PYTHON_BIN" tools/backend_canary.py backup --kind backend-baseline --role uplink --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-backup --kind backend-baseline --role uplink --state .canary/canary-state.json
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-provisional --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 30
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py status --state .canary/canary-state.json

BACKEND_SCANNER_SHA="$(jq -er '.targets["scanner-s3-combo-backend"].parts[] | select(.name == "scanner-s3-combo-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"
BACKEND_UPLINK_SHA="$(jq -er '.targets["uplink-s3-backend"].parts[] | select(.name == "uplink-s3-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"
```

Each backup deliberately runs the backed-up application again. The tool
invalidates that role's provisional boot identity and all three final-health
records before reboot. Therefore each exact cycle above is indivisible:
`backup` → `verify-backup` → changed-role `verify-provisional` with a new boot
ID → `verify-final` for scanner0, scanner1, and uplink. Never start the next
role's backup until all five post-backup checks pass.

Every probe must say `admit`, validate the complete image, match index identity,
SHA-256, CRC32, size, and capacity, and leave write counters and boot IDs
unchanged. Cross-family rejection is proven in the native/API software gate;
never send a badge or deliberately wrong image to canary hardware.

One scanner relay and the uplink self-OTA may now exercise explicit
same-version recovery. A baseline catalog receipt is never an OTA receipt.
Create a new, uniquely named `refresh-ota-catalog` receipt immediately before
each component probe, and use that exact active receipt for its challenge. A
receipt, probe, or challenge older than five minutes is stale; create another
unique receipt and rerun that component's probe rather than reusing or
overwriting evidence. Each apply is a separate approval boundary:

```bash
BACKEND_OTA_CATALOG_EVIDENCE=".canary/evidence/catalog-ota-scanner0-$(date +%s)-$$.json"
BACKEND_OTA_PROBE=".canary/evidence/scanner0-backend-probe-$(date +%s)-$$.json"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py refresh-ota-catalog --backend-base "$BACKEND_BASE_URL" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --output "$BACKEND_OTA_CATALOG_EVIDENCE"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py ota-probe --component scanner0 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_OTA_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output "$BACKEND_OTA_PROBE" --timeout 300
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-ota --component scanner0 --mode same-version-recovery --probe "$BACKEND_OTA_PROBE" --catalog-evidence "$BACKEND_OTA_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-ota-challenge.json
# STOP. Show the scanner0 receipt and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-ota-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py ota-apply --component scanner0 --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner0-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN BACKEND_OTA_CATALOG_EVIDENCE BACKEND_OTA_PROBE
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180

BACKEND_OTA_CATALOG_EVIDENCE=".canary/evidence/catalog-ota-scanner1-$(date +%s)-$$.json"
BACKEND_OTA_PROBE=".canary/evidence/scanner1-backend-probe-$(date +%s)-$$.json"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py refresh-ota-catalog --backend-base "$BACKEND_BASE_URL" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --output "$BACKEND_OTA_CATALOG_EVIDENCE"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py ota-probe --component scanner1 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_OTA_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output "$BACKEND_OTA_PROBE" --timeout 300
unset BACKEND_OTA_CATALOG_EVIDENCE BACKEND_OTA_PROBE

BACKEND_OTA_CATALOG_EVIDENCE=".canary/evidence/catalog-ota-uplink-$(date +%s)-$$.json"
BACKEND_OTA_PROBE=".canary/evidence/uplink-backend-probe-$(date +%s)-$$.json"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py refresh-ota-catalog --backend-base "$BACKEND_BASE_URL" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --output "$BACKEND_OTA_CATALOG_EVIDENCE"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py ota-probe --component uplink --catalog-name uplink-s3-backend --expected-sha "$BACKEND_UPLINK_SHA" --catalog-evidence "$BACKEND_OTA_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output "$BACKEND_OTA_PROBE" --timeout 300
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-ota --component uplink --mode same-version-recovery --probe "$BACKEND_OTA_PROBE" --catalog-evidence "$BACKEND_OTA_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-ota-challenge.json
# STOP. Show the uplink receipt and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/uplink-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/uplink-ota-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py ota-apply --component uplink --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/uplink-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN BACKEND_OTA_CATALOG_EVIDENCE BACKEND_OTA_PROBE
"$BACKEND_PYTHON_BIN" tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" tools/backend_canary.py status --state .canary/canary-state.json
```

A successful apply requires a positive image-write delta, a changed boot ID
only for the selected component, exact backend image identity and digests,
rollback clearance, and full convergence.

## Failure and restore boundary

If an initial flash or OTA write fails, do not try the web flasher, do not
retry on another board, and do not issue another update. Create a restore
challenge for only the failed board and an allowed source (`original` for
whole-assembly rollback or `backend-baseline` for a proven backend recovery).
Show the private MAC-bound receipt and obtain a separate approval before
`restore-full`:

```bash
"$BACKEND_PYTHON_BIN" tools/backend_canary.py challenge-restore --role scanner0 --source original --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-restore-challenge.json
# STOP. Show the exact receipt and obtain explicit restore approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-restore-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-restore-challenge.json)"
"$BACKEND_PYTHON_BIN" tools/backend_canary.py restore-full --role scanner0 --source original --state .canary/canary-state.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
```

Substitute only the exact failed role/source authorized by its receipt. The
tool verifies the complete 8 MB restore before boot and requires the original
installed identity afterward.

## Phase 7: continuous 24-hour soak

This phase is backend/Lite-only. Native badge
`0.67.2-badge-defcon34` remains the separate default USB/factory firmware and
is not opened, selected, flashed, or monitored here. `capture-serial` rejects
a badge target and requires each selected port, MAC, role, boot ID, firmware
identity, and uplink device ID to match the private backend canary state.

Run the entire quoted heredoc below from the same `backend-firmware` directory.
The required URL, device ID, and three port variables must be exported. The
heredoc deliberately invokes `/bin/bash`, so it is safe to paste from the
default macOS zsh while Bash owns and supervises all three background jobs.
The three capture processes exclusively own their raw 921600-baud ports,
continuously preserve bounded UTF-8 USB output, and request a fresh exact
`FOF_BACKEND_BOOT` plus `FOF_BACKEND_HEALTH` every 30 seconds. Do not run
`pio device monitor`, `tee`, a web serial tool, or any other process against
these ports while the capture PIDs live.

The filenames are protocol inputs, not examples: they must be exactly
`scanner0.log`, `scanner1.log`, and `uplink.log`. Use a fresh, uniquely named
private directory for every attempt. The capture commands create each 0600
file with `O_EXCL`; never pre-create, truncate, reuse, or overwrite a role log.
The block also creates a run-specific soak JSONL and records a fresh
`network-recovery` snapshot before the first monitor row, so a retry cannot
splice evidence from an earlier attempt.

```bash
/bin/bash <<'BACKEND_PHASE7'
: "${BASH_VERSION:?Phase 7 must run in Bash}"
set -euo pipefail
umask 077
: "${BACKEND_BASE_URL:?backend URL is required}"
: "${BACKEND_DEVICE_ID:?preserved backend uplink device ID is required}"
: "${BACKEND_SCANNER0_PORT:?scanner0 USB path is required}"
: "${BACKEND_SCANNER1_PORT:?scanner1 USB path is required}"
: "${BACKEND_UPLINK_PORT:?uplink USB path is required}"

BACKEND_REPO_DIR="$(pwd -P)"
BACKEND_PYTHON_BIN="$(command -v python3)"
BACKEND_JQ_BIN="$(command -v jq)"
BACKEND_CANARY_TOOL="$BACKEND_REPO_DIR/tools/backend_canary.py"
BACKEND_EVIDENCE_TOOL="$BACKEND_REPO_DIR/tools/backend_canary_evidence.py"
BACKEND_CANARY_STATE="$BACKEND_REPO_DIR/.canary/canary-state.json"
BACKEND_CAPTURE_RUN_ID="$(date +%s)-$$"
BACKEND_SERIAL_LOG_DIR="$BACKEND_REPO_DIR/.canary/serial-runs/$BACKEND_CAPTURE_RUN_ID"
BACKEND_CAPTURE_RESULT_DIR="$BACKEND_REPO_DIR/.canary/evidence/serial-capture-$BACKEND_CAPTURE_RUN_ID"
BACKEND_SOAK_EVIDENCE="$BACKEND_REPO_DIR/.canary/evidence/canary-soak-$BACKEND_CAPTURE_RUN_ID.jsonl"
BACKEND_CAPTURE_DURATION_S=86520
test -x "$BACKEND_PYTHON_BIN"
test -x "$BACKEND_JQ_BIN"
test -f "$BACKEND_CANARY_TOOL"
test -f "$BACKEND_EVIDENCE_TOOL"
test -f "$BACKEND_CANARY_STATE"
for BACKEND_CAPTURE_PORT in "$BACKEND_SCANNER0_PORT" "$BACKEND_SCANNER1_PORT" "$BACKEND_UPLINK_PORT"; do
  case "$BACKEND_CAPTURE_PORT" in
    /dev/*) ;;
    *) echo "every capture port must be an explicit /dev path" >&2; exit 1 ;;
  esac
done
BACKEND_SCANNER0_MAC="$("$BACKEND_JQ_BIN" -er '.boards.scanner0.inventory.mac' "$BACKEND_CANARY_STATE")"
BACKEND_SCANNER1_MAC="$("$BACKEND_JQ_BIN" -er '.boards.scanner1.inventory.mac' "$BACKEND_CANARY_STATE")"
BACKEND_UPLINK_MAC="$("$BACKEND_JQ_BIN" -er '.boards.uplink.inventory.mac' "$BACKEND_CANARY_STATE")"

mkdir -p -m 700 "$BACKEND_REPO_DIR/.canary/serial-runs"
chmod 700 "$BACKEND_REPO_DIR/.canary/serial-runs"
mkdir -m 700 "$BACKEND_SERIAL_LOG_DIR"
mkdir -m 700 "$BACKEND_CAPTURE_RESULT_DIR"
test ! -e "$BACKEND_SOAK_EVIDENCE"
"$BACKEND_PYTHON_BIN" -c 'from pathlib import Path; import stat,sys; root=Path(sys.argv[1]); info=root.lstat(); assert stat.S_ISDIR(info.st_mode) and stat.S_IMODE(info.st_mode)==0o700 and not root.is_symlink() and not any(root.iterdir())' "$BACKEND_SERIAL_LOG_DIR"

# Refresh the authoritative state-bound health immediately before capture.
"$BACKEND_PYTHON_BIN" "$BACKEND_CANARY_TOOL" verify-final --role scanner0 --state "$BACKEND_CANARY_STATE" --port "$BACKEND_SCANNER0_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" "$BACKEND_CANARY_TOOL" verify-final --role scanner1 --state "$BACKEND_CANARY_STATE" --port "$BACKEND_SCANNER1_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" "$BACKEND_CANARY_TOOL" verify-final --role uplink --state "$BACKEND_CANARY_STATE" --port "$BACKEND_UPLINK_PORT" --timeout 180
"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase network-recovery --output "$BACKEND_SOAK_EVIDENCE"

BACKEND_CAPTURE_PIDS=()
backend_capture_is_owned_job() {
  local wanted_pid="$1"
  local job_pid
  while IFS= read -r job_pid; do
    if [[ "$job_pid" == "$wanted_pid" ]]; then
      return 0
    fi
  done < <(jobs -p)
  return 1
}
backend_capture_is_running_job() {
  local wanted_pid="$1"
  local job_pid
  while IFS= read -r job_pid; do
    if [[ "$job_pid" == "$wanted_pid" ]]; then
      return 0
    fi
  done < <(jobs -pr)
  return 1
}
backend_capture_forget_pid() {
  local completed_pid="$1"
  local pid
  local remaining=()
  for pid in "${BACKEND_CAPTURE_PIDS[@]}"; do
    if [[ "$pid" != "$completed_pid" ]]; then
      remaining+=("$pid")
    fi
  done
  BACKEND_CAPTURE_PIDS=("${remaining[@]}")
}
backend_capture_cleanup() {
  local pid
  trap - INT TERM HUP
  for pid in "${BACKEND_CAPTURE_PIDS[@]}"; do
    if backend_capture_is_owned_job "$pid"; then
      kill -CONT "$pid" 2>/dev/null || true
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  for pid in "${BACKEND_CAPTURE_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
}
trap backend_capture_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" capture-serial --role scanner0 --port "$BACKEND_SCANNER0_PORT" --expected-mac "$BACKEND_SCANNER0_MAC" --state "$BACKEND_CANARY_STATE" --output-dir "$BACKEND_SERIAL_LOG_DIR" --duration-s "$BACKEND_CAPTURE_DURATION_S" --interval-s 30 --response-timeout-s 10 >"$BACKEND_CAPTURE_RESULT_DIR/scanner0.json" 2>"$BACKEND_CAPTURE_RESULT_DIR/scanner0.stderr" &
BACKEND_SCANNER0_CAPTURE_PID=$!
BACKEND_CAPTURE_PIDS+=("$BACKEND_SCANNER0_CAPTURE_PID")
"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" capture-serial --role scanner1 --port "$BACKEND_SCANNER1_PORT" --expected-mac "$BACKEND_SCANNER1_MAC" --state "$BACKEND_CANARY_STATE" --output-dir "$BACKEND_SERIAL_LOG_DIR" --duration-s "$BACKEND_CAPTURE_DURATION_S" --interval-s 30 --response-timeout-s 10 >"$BACKEND_CAPTURE_RESULT_DIR/scanner1.json" 2>"$BACKEND_CAPTURE_RESULT_DIR/scanner1.stderr" &
BACKEND_SCANNER1_CAPTURE_PID=$!
BACKEND_CAPTURE_PIDS+=("$BACKEND_SCANNER1_CAPTURE_PID")
"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" capture-serial --role uplink --port "$BACKEND_UPLINK_PORT" --expected-mac "$BACKEND_UPLINK_MAC" --state "$BACKEND_CANARY_STATE" --output-dir "$BACKEND_SERIAL_LOG_DIR" --duration-s "$BACKEND_CAPTURE_DURATION_S" --interval-s 30 --response-timeout-s 10 >"$BACKEND_CAPTURE_RESULT_DIR/uplink.json" 2>"$BACKEND_CAPTURE_RESULT_DIR/uplink.stderr" &
BACKEND_UPLINK_CAPTURE_PID=$!
BACKEND_CAPTURE_PIDS+=("$BACKEND_UPLINK_CAPTURE_PID")

# All three jobs must remain live and publish their first exact BOOT+HEALTH pair.
BACKEND_CAPTURE_READY_DEADLINE=$((SECONDS + 30))
while true; do
  for BACKEND_CAPTURE_PID in "${BACKEND_CAPTURE_PIDS[@]}"; do
    if ! backend_capture_is_running_job "$BACKEND_CAPTURE_PID"; then
      echo "backend serial capture exited before soak start" >&2
      exit 1
    fi
  done
  if "$BACKEND_PYTHON_BIN" -c 'from pathlib import Path; import stat,sys,time; root=Path(sys.argv[1]); paths=list(root.iterdir()); assert {p.name for p in paths}=={"scanner0.log","scanner1.log","uplink.log"}; now=time.time_ns(); assert all(not p.is_symlink() and stat.S_ISREG((s:=p.stat()).st_mode) and stat.S_IMODE(s.st_mode)==0o600 and s.st_size>0 and 0 <= now-s.st_mtime_ns <= 300_000_000_000 for p in paths)' "$BACKEND_SERIAL_LOG_DIR" >/dev/null 2>&1; then
    break
  fi
  if (( SECONDS >= BACKEND_CAPTURE_READY_DEADLINE )); then
    echo "three exact current serial logs were not ready within 30 seconds" >&2
    exit 1
  fi
  sleep 0.2
done
for BACKEND_CAPTURE_PID in "${BACKEND_CAPTURE_PIDS[@]}"; do
  backend_capture_is_running_job "$BACKEND_CAPTURE_PID"
done

# Foreground soak: a stopped capture makes its log stale in <=300 seconds;
# the 30-second monitor cadence then fails the run no later than its next sample.
"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" monitor --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --duration-s 86400 --interval-s 30 --serial-log-dir "$BACKEND_SERIAL_LOG_DIR" --output "$BACKEND_SOAK_EVIDENCE"

# Capture duration is 120 seconds longer than the soak; readiness may consume
# up to 30 seconds of that buffer. Require all three clean exits.
BACKEND_CAPTURE_WAIT_PIDS=("${BACKEND_CAPTURE_PIDS[@]}")
for BACKEND_CAPTURE_PID in "${BACKEND_CAPTURE_WAIT_PIDS[@]}"; do
  if wait "$BACKEND_CAPTURE_PID"; then
    backend_capture_forget_pid "$BACKEND_CAPTURE_PID"
  else
    BACKEND_CAPTURE_STATUS=$?
    backend_capture_forget_pid "$BACKEND_CAPTURE_PID"
    exit "$BACKEND_CAPTURE_STATUS"
  fi
done
unset BACKEND_CAPTURE_WAIT_PIDS BACKEND_CAPTURE_STATUS
trap - EXIT INT TERM HUP
unset -f backend_capture_cleanup backend_capture_forget_pid backend_capture_is_running_job backend_capture_is_owned_job
"$BACKEND_PYTHON_BIN" "$BACKEND_EVIDENCE_TOOL" verify-soak --input "$BACKEND_SOAK_EVIDENCE" --duration-s 86400 --max-heartbeat-gap-s 90 --max-sample-gap-s 90
unset BACKEND_CAPTURE_PIDS
unset BACKEND_CAPTURE_READY_DEADLINE BACKEND_CAPTURE_PID BACKEND_SCANNER0_CAPTURE_PID BACKEND_SCANNER1_CAPTURE_PID BACKEND_UPLINK_CAPTURE_PID
unset BACKEND_SCANNER0_MAC BACKEND_SCANNER1_MAC BACKEND_UPLINK_MAC BACKEND_CAPTURE_DURATION_S BACKEND_CAPTURE_RUN_ID BACKEND_CAPTURE_RESULT_DIR BACKEND_SERIAL_LOG_DIR BACKEND_SOAK_EVIDENCE BACKEND_CANARY_STATE BACKEND_CAPTURE_PORT
unset BACKEND_EVIDENCE_TOOL BACKEND_CANARY_TOOL BACKEND_JQ_BIN BACKEND_PYTHON_BIN BACKEND_REPO_DIR
BACKEND_PHASE7
```

The monitor samples serial receipts every 30 seconds and rejects a log older
than 300 seconds, so a capture that exits during the foreground soak cannot be
silently accepted. The final `wait` also propagates any capture failure that
occurs after the monitor's last sample. The monitor and verifier independently
reject missing, stale, non-0600, renamed, extra, oversized, panic-bearing,
rollback-failure-bearing, or secret/raw-auth-bearing logs.

Any capture or monitor exit, signal, stale log, or monitor-sample gap invalidates
the attempt. Fix the cause, use another fresh unique serial directory, and
restart the full 24-hour soak from zero; never splice or resume JSONL evidence.

Acceptance requires 24 continuous hours, not 23:59:59, with:

- exact `healthy` LED state and scanner OTA state `idle` on every sample;
- no watchdog reset, unexpected boot ID, rollback, or fatal health;
- both scanner UART links current except during the earlier bounded recovery
  exercise, with exact slot/MAC/profile/generation/ACK/radio-health bindings;
- heartbeats no more than 90 seconds apart while the network is available;
- consecutive 30-second monitor samples never more than 90 seconds apart;
- monotonic FIFO sequence, no unexplained drop or quarantine increment, and
  final queue depth zero after network recovery;
- no AP/Wi-Fi password, credential, token, raw BLE characteristic value, or
  other secret in JSONL or serial evidence;
- exact complete backend detection fields and the preserved device ID;
- unchanged calibration-continuity status, session, applied value, presence,
  schema, and model digest, without raw calibration coefficients; and
- protected badge/production paths byte-identical to the branch base.

The verifier does not manufacture or assume reset/schema-error counters that
the backend did not report. Reset safety is evidenced by fixed uplink/scanner
boot IDs and monotonic uplink uptime; schema safety comes from strict exact
JSON, identity, topology, health, queue, continuity, and serial-receipt
validation on every accepted row.

Unset release-only values when evidence collection is complete:

```bash
unset BACKEND_SCANNER_SHA BACKEND_UPLINK_SHA BACKEND_BASELINE_CATALOG_EVIDENCE BACKEND_OTA_CATALOG_EVIDENCE BACKEND_OTA_PROBE BACKEND_RELEASE_INDEX
```

Passing this runbook qualifies only the explicit three-board no-screen
backend/Lite canary. It does not change the native badge USB/factory default or
authorize a fleet rollout.

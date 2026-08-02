# Backend firmware three-board canary

| Role | Image | UART wiring | Primary radio | LED |
|---|---|---|---|---|
| Lite scanner0 | `scanner-s3-combo-backend` | TX GPIO1 → uplink RX GPIO2; RX GPIO2 ← uplink TX GPIO1 | BLE | active-low GPIO21 yellow |
| Lite scanner1 | `scanner-s3-combo-backend` | TX GPIO1 → uplink RX GPIO4; RX GPIO2 ← uplink TX GPIO3 | Wi-Fi | active-low GPIO21 yellow |
| Lite uplink | `uplink-s3-backend` | slot0 RX2/TX1; slot1 RX4/TX3 | infrastructure Wi-Fi/HTTP | active-low GPIO21 yellow |

“Lite” is only the nickname for this physical no-screen three-board assembly.
It is never a firmware target, project, hardware, artifact, or API identity.
The only accepted identities are `scanner-s3-combo-backend`/
`fof_backend_scanner` and `uplink-s3-backend`/`fof_backend_uplink`, on
`seeed_xiao_esp32s3`, version `0.1.0-backend`.

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
umask 077
pio device list

BACKEND_PIO_BIN="$(command -v pio)"
: "${BACKEND_PIO_BIN:?PlatformIO pio is required}"
: "${BACKEND_ORIGINAL_UPLINK_URL:?export the original Lite uplink URL, for example http://192.168.1.50}"
: "${BACKEND_BASE_URL:?export the FastAPI base URL that owns the registered node}"
: "${BACKEND_SCANNER0_PORT:?export the scanner0 USB path from pio device list}"
: "${BACKEND_SCANNER1_PORT:?export the scanner1 USB path from pio device list}"
: "${BACKEND_UPLINK_PORT:?export the uplink USB path from pio device list}"

python tools/backend_canary.py capture-installed --state .canary/canary-state.json --uplink-url "$BACKEND_ORIGINAL_UPLINK_URL" --backend-base "$BACKEND_BASE_URL" --output-dir .canary/installed
python tools/backend_canary.py inventory --role scanner0 --port "$BACKEND_SCANNER0_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py inventory --role scanner1 --port "$BACKEND_SCANNER1_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py inventory --role uplink --port "$BACKEND_UPLINK_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"

python tools/backend_canary.py backup --kind original --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py backup --kind original --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
# UPLINK MUST BE LAST. Success leaves it quiescent; do not reset it.
python tools/backend_canary.py backup --kind original --role uplink --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"

python tools/backend_canary.py verify-backup --kind original --role scanner0 --state .canary/canary-state.json
python tools/backend_canary.py verify-backup --kind original --role scanner1 --state .canary/canary-state.json
python tools/backend_canary.py verify-backup --kind original --role uplink --state .canary/canary-state.json
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py status --state .canary/canary-state.json
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

The `web-flasher/firmware/...` argument below names the verified release-byte
package only. `backend_canary.py` verifies those bytes and performs the guarded
direct-USB write; no browser or Web Serial flasher is opened or used.

### Scanner0

```bash
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-challenge.json
# STOP. Show .canary/scanner0-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-challenge.json)"
python tools/backend_canary.py flash-initial --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner0-challenge.json
python tools/backend_canary.py verify-provisional --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 30
```

### Scanner1

```bash
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner1-challenge.json
# STOP. Show .canary/scanner1-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner1-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner1-challenge.json)"
python tools/backend_canary.py flash-initial --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner1-challenge.json
python tools/backend_canary.py verify-provisional --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 30
```

### Uplink last

```bash
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-challenge.json
# STOP. Show .canary/uplink-challenge.json and obtain explicit approval for this receipt only.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/uplink-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/uplink-challenge.json)"
python tools/backend_canary.py flash-initial --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/uplink-challenge.json
python tools/backend_canary.py verify-provisional --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 30
```

Each challenge expires after five minutes and is deleted after use. Stop after
any failure, unexpected identity, or evidence that the old uplink application
restarted. Do not continue to the next board. Once the backend uplink is
running and has assigned both scanner profiles, require all three final-health
records:

```bash
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
python tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json
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
python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase baseline --output .canary/evidence/canary.jsonl
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
The ready file is the sole authorization to enable it.

```bash
BACKEND_DRONE_AFTER_MS="$(python -c 'import time; print(int(time.time()*1000))')"
BACKEND_DRONE_READY=".canary/evidence/drone-$BACKEND_DRONE_AFTER_MS.ready"
python tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind drone --source wifi_beacon_rid --identity-field drone_id --identity-value "$BACKEND_TEST_DRONE_ID" --after-ms "$BACKEND_DRONE_AFTER_MS" --ready-file "$BACKEND_DRONE_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_DRONE_WAIT_PID=$!
until test -s "$BACKEND_DRONE_READY"; do kill -0 "$BACKEND_DRONE_WAIT_PID" || { wait "$BACKEND_DRONE_WAIT_PID"; exit 1; }; sleep 0.1; done
# Only now may the operator enable the exact recorded Remote ID source.
wait "$BACKEND_DRONE_WAIT_PID"
unset BACKEND_DRONE_WAIT_PID
# Disable the source immediately after the wait succeeds.
python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase drone --output .canary/evidence/canary.jsonl
unset BACKEND_DRONE_READY BACKEND_DRONE_AFTER_MS
```

Evidence must match the preserved device ID, `source:wifi_beacon_rid`, and the
exact Remote ID. Record `BACKEND_RID_SOURCE_DESCRIPTION` in the operator notes
and label this as lab data, never as a real aircraft. Confirm two complete
drone LED cycles on all three boards.

### Meta Glasses

Prefer a physical Meta Glasses device and record its BLE MAC. If unavailable,
use a dedicated lab advertiser configured in nRF Connect with local name
`Ray-Ban Meta` and advertised 16-bit service UUID `0xFD5F`; record its
hardware/MAC and label the result simulated. If neither exists, this gate is
blocked. UART or HTTP injection is not RF evidence.

```bash
BACKEND_META_AFTER_MS="$(python -c 'import time; print(int(time.time()*1000))')"
BACKEND_META_READY=".canary/evidence/meta-$BACKEND_META_AFTER_MS.ready"
python tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind meta --source ble_fingerprint --identity-field bssid --identity-value "$BACKEND_META_MAC" --manufacturer "Meta Glasses" --service-uuid-token fd5f --after-ms "$BACKEND_META_AFTER_MS" --ready-file "$BACKEND_META_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
BACKEND_META_WAIT_PID=$!
until test -s "$BACKEND_META_READY"; do kill -0 "$BACKEND_META_WAIT_PID" || { wait "$BACKEND_META_WAIT_PID"; exit 1; }; sleep 0.1; done
# Only now may the operator enable the exact recorded Meta source.
wait "$BACKEND_META_WAIT_PID"
unset BACKEND_META_WAIT_PID
# Disable the source immediately after the wait succeeds.
python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase meta --output .canary/evidence/canary.jsonl
unset BACKEND_META_READY BACKEND_META_AFTER_MS
```

Evidence must match the device ID, `source:ble_fingerprint`, exact BSSID,
`manufacturer:"Meta Glasses"`, and parsed UUID token `fd5f`. Confirm two
complete Meta LED cycles on all three boards.

### Combined threat

Turn both RF sources off. Capture one new `AFTER_MS`, start separate exact
drone and Meta waiters with separate ready files, and wait for both ready files
before enabling either source. Wait for both processes, disable both sources,
then capture phase `drone-meta`. Confirm two complete combined patterns on all
three boards. Reusing an earlier cutoff or enabling either source before both
ready files exist invalidates this phase.

## Phase 5: network, UART, fatal, and command recovery

With both RF sources off, disconnect only the uplink's infrastructure network
for ten minutes. Keep power and UART wiring intact. Capture `outage-start`; at
minute five confirm AP startup and two network-degraded LED cycles on all three
boards. Reconnect, capture `outage-end`, and poll until the FIFO queue is zero.
Then record the required recovery boundary:

```bash
python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase network-recovery --output .canary/evidence/canary.jsonl
```

Next, leave scanner0 powered but disconnect only its TX/RX wires. Confirm
scanner0 `uart_lost`, scanner1 hybrid failover, continued detections, and no
uplink reboot. Capture `scanner0-disconnected`. Reconnect both wires, wait for
the exact profile/generation/ACK/radio-health bindings and final health, then
capture `scanner0-reconnected`.

For the bounded fatal test, turn both RF sources off and disconnect both
scanner UART pairs. The uplink must enter `fatal`. Each isolated scanner cannot
receive the mirrored fatal state and must independently show `uart_lost`; do
not claim all three show fatal while both links are absent. Capture
`both-scanners-disconnected`, promptly reconnect both pairs, require final
health, capture `fatal-recovered`, and observe healthy timing again.

Exercise the real backend BLE command path: cancel one passive command, then
allow a second to complete.

```bash
BACKEND_CANCEL_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
BACKEND_CANCEL_ID="$(printf '%s' "$BACKEND_CANCEL_JSON" | jq -er .command_id)"
curl -fsS -X POST "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/$BACKEND_CANCEL_ID/cancel" | jq -e --arg id "$BACKEND_CANCEL_ID" 'keys == ["command_id","mode","next_sequence","request_id","result_state","target","timeout_ms","type"] and .type == "ble_investigate_cancel" and .command_id == $id and .request_id == $id and .mode == "passive_capture" and .target == null and .timeout_ms == 12000 and .next_sequence == 0 and .result_state == null'
python tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_CANCEL_ID" --terminal-state cancelled --timeout-s 120 --output .canary/evidence/canary.jsonl

BACKEND_COMPLETE_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
BACKEND_COMPLETE_ID="$(printf '%s' "$BACKEND_COMPLETE_JSON" | jq -er .command_id)"
python tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_COMPLETE_ID" --terminal-state complete --timeout-s 120 --output .canary/evidence/canary.jsonl
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
BACKEND_CATALOG_EVIDENCE=".canary/evidence/backend-catalog-preflight.json"
python tools/backend_canary.py verify-catalog --backend-base "$BACKEND_BASE_URL" --index release/backend-release-index.json --output "$BACKEND_CATALOG_EVIDENCE"

python tools/backend_canary.py backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
python tools/backend_canary.py backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
python tools/backend_canary.py backup --kind backend-baseline --role uplink --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role uplink --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json

BACKEND_RELEASE_INDEX="release/backend-release-index.json"
BACKEND_SCANNER_SHA="$(jq -er '.targets["scanner-s3-combo-backend"].parts[] | select(.name == "scanner-s3-combo-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"
BACKEND_UPLINK_SHA="$(jq -er '.targets["uplink-s3-backend"].parts[] | select(.name == "uplink-s3-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"

python tools/backend_canary.py ota-probe --component scanner0 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner0-backend-probe.json --timeout 300
python tools/backend_canary.py ota-probe --component scanner1 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner1-backend-probe.json --timeout 300
python tools/backend_canary.py ota-probe --component uplink --catalog-name uplink-s3-backend --expected-sha "$BACKEND_UPLINK_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/uplink-backend-probe.json --timeout 300
```

Every probe must say `admit`, validate the complete image, match index identity,
SHA-256, CRC32, size, and capacity, and leave write counters and boot IDs
unchanged. Cross-family rejection is proven in the native/API software gate;
never send a badge or deliberately wrong image to canary hardware.

One scanner relay and the uplink self-OTA may now exercise explicit
same-version recovery. Each apply is a separate approval boundary:

```bash
python tools/backend_canary.py challenge-ota --component scanner0 --mode same-version-recovery --probe .canary/evidence/scanner0-backend-probe.json --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-ota-challenge.json
# STOP. Show the scanner0 receipt and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-ota-challenge.json)"
python tools/backend_canary.py ota-apply --component scanner0 --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner0-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180

python tools/backend_canary.py challenge-ota --component uplink --mode same-version-recovery --probe .canary/evidence/uplink-backend-probe.json --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-ota-challenge.json
# STOP. Show the uplink receipt and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/uplink-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/uplink-ota-challenge.json)"
python tools/backend_canary.py ota-apply --component uplink --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/uplink-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json
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
python tools/backend_canary.py challenge-restore --role scanner0 --source original --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-restore-challenge.json
# STOP. Show the exact receipt and obtain explicit restore approval.
BACKEND_CHALLENGE_ID="$(jq -er '.challenge.challenge_id' .canary/scanner0-restore-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -er .approval_token .canary/scanner0-restore-challenge.json)"
python tools/backend_canary.py restore-full --role scanner0 --source original --state .canary/canary-state.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
```

Substitute only the exact failed role/source authorized by its receipt. The
tool verifies the complete 8 MB restore before boot and requires the original
installed identity afterward.

## Phase 7: continuous 24-hour soak

Capture serial output from all three boards into private files under
`.canary/serial-logs`; use one terminal per port and keep `umask 077`. Start the
monitor only after the `network-recovery` snapshot exists and all three boards
again pass final health.

```bash
python tools/backend_canary_evidence.py monitor --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --duration-s 86400 --interval-s 30 --serial-log-dir .canary/serial-logs --output .canary/evidence/canary.jsonl
python tools/backend_canary_evidence.py verify-soak --input .canary/evidence/canary.jsonl --duration-s 86400 --max-heartbeat-gap-s 90
```

Acceptance requires 24 continuous hours, not 23:59:59, with:

- no watchdog reset, unexpected boot ID, rollback, or fatal health;
- both scanner UART links current except during the earlier bounded recovery
  exercise, with exact slot/MAC/profile/generation/ACK/radio-health bindings;
- heartbeats no more than 90 seconds apart while the network is available;
- monotonic FIFO sequence, no unexplained drop or quarantine increment, and
  final queue depth zero after network recovery;
- no AP/Wi-Fi password, credential, token, raw BLE characteristic value, or
  other secret in JSONL or serial evidence;
- exact complete backend detection fields and the preserved device ID;
- unchanged calibration-continuity status, session, applied value, presence,
  schema, and model digest, without raw calibration coefficients; and
- protected badge/production paths byte-identical to the branch base.

Unset release-only values when evidence collection is complete:

```bash
unset BACKEND_SCANNER_SHA BACKEND_UPLINK_SHA BACKEND_CATALOG_EVIDENCE BACKEND_RELEASE_INDEX
```

Passing this runbook qualifies only the explicit three-board no-screen
backend/Lite canary. It does not change the native badge USB/factory default or
authorize a fleet rollout.

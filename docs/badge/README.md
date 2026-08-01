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
- Published FoF Badge firmware/factory release: `0.64.76-badge-defcon34`
- Production S3 firmware: `0.64.68-live-follow`

Keep those tracks separate. The badge firmware uses `FOF_BADGE_VARIANT`,
badge-specific pinning, a Waveshare ST7735 display, USB-C control, local AP
status, scanner relay flashing, and safe USB recovery. Production
`uplink-s3`, `scanner-s3-combo`, and `scanner-s3-combo-seed` remain on the
production auto-OTA track.

The source tree also carries a local provisional canary identity,
`0.64.87-badge-defcon34`, for the connected three-board badge trio. It is not
a published badge release: backend readiness and the public/factory web
manifests deliberately remain on `0.64.76-badge-defcon34` until every exact-
binary physical gate passes. The incomplete bootstrap and acceptance records
remain incomplete; a version string alone is not physical evidence or
promotion.

### Local canary evidence (2026-07-26)

This is engineering evidence for the provisional candidate, not a completed
acceptance record:

- The uplink updated over its single USB connection from
  `0.64.86-badge-defcon34` to `0.64.87-badge-defcon34`; pending verification
  cleared and rollback state returned to `clear`.
- The host staged one `.87` scanner image through that uplink for both slots.
  The uplink relayed it automatically to a BLE-primary scanner on `.85` and a
  Wi-Fi-primary scanner on `.86`. Both converged on attempt one with zero
  NACKs; the measured binary transfers took 165 seconds and 101 seconds.
- The final fresh status bound the same three hardware IDs, exact `.87`
  identities, healthy acknowledged roles, idle OTA state, zero crash counts,
  BLE scanning active on slot 0, and Wi-Fi scanning active on slot 1.
- The host recovery path now gives a zero-attempt readiness failure one
  targeted reprompt only after the campaign worker is idle. It tolerates one
  asynchronous stale snapshot for at most five seconds and still fails closed
  on a repeated terminal result.
- USB theme, custom display-policy, and display-navigation commands were
  acknowledged, and the non-persistent display-policy round trip preserved
  the original policy.
- Native firmware tests, backend tests, Android unit/build checks, focused host
  updater tests, artifact identity checks, and both scanner/uplink builds
  passed for the candidate.

The physical 10-second Menu+OK ROM-recovery gate and the remaining manual
acceptance checks are still pending. Until they pass on the exact candidate
binaries, do not replace factory artifacts, tag, push, or publish a release.

### Provisional game headroom

The `.87` canary build, relay transcript, and fresh post-update USB status
snapshot reported:

| Surface | Candidate headroom |
| --- | ---: |
| Uplink link-map internal RAM | 118,580 bytes |
| Uplink application partition | 630,395 bytes |
| Scanner link-map internal RAM | 169,076 bytes |
| Scanner application partition | 880,891 bytes |
| Live uplink internal heap free / minimum-ever / largest block | 37,500 / 26,296 / 28,672 bytes |
| Live uplink PSRAM free / total / largest block | 8,251,856 / 8,388,608 / 8,126,464 bytes |
| Smallest observed uplink task-stack headroom during/following relay | 4,148 bytes |

Link-map RAM headroom is not the same as live heap headroom. The current uplink
status does not expose scanner-MCU heap, PSRAM, or scanner-task stack metrics,
so scanner runtime headroom is not yet proven from the single uplink USB path.

The implemented game fits the raw flash and PSRAM envelope, but the stricter
promotion gates are now the practical limit: `.87` is only 3,892 bytes below
the static internal-RAM gate and 1,249 bytes below the conservative app-image
gate. Substantial new firmware features require removing or relocating code
first. Do not add another BLE host or a large internal allocation on the
uplink.

### Provisional CON CRUD factory roles

The private canary factory flow supports three explicit uplink seed roles:
`normal`, `infected`, and `immune`. `normal` is the CLI default, but every
batch actively sends `FOF_SET:game_seed=normal`; stale NVS is never accepted as
the default. After all three erase/write/readback operations, the flasher
resets the two scanner leaves in their established order, starts the uplink,
then discovers its application port without toggling DTR/RTS. Exact
PONG/status binds the eFuse hardware ID before mutation. After the exact seed
acknowledgment, a second fresh status records the expected-reboot generation;
the flasher then requires exact `FOF_REBOOT:OK` and closes the old handle.
Native USB may retain or re-enumerate the same device path, so the host opens a
new descriptor-bound reset-neutral session without esptool or another reset.
It accepts only an exact PONG followed by status from the same hardware ID,
version, and target, with reboot reason `usb_reboot` and the exact wrap-aware
successor generation. It then reruns the complete health gate. PASS requires
fresh status with selected seed and current state, `game_active:false`, and
integer `game_shield:0`.

If USB re-enumerates during the seed acknowledgment, fresh pre-reboot status,
or reboot receipt, the host closes that handle, rediscovers the exact
MAC/version/target, and repeats the whole idempotent transaction within one
bounded deadline. Explicit protocol or identity mismatches still fail closed.

Both physical scanner slots continue to use the same scanner image. Factory
role selection does not change their normal BLE-primary and Wi-Fi-primary
scanning duties.

Public factory output contains fixed board-role aliases and an opaque random
receipt only after PASS. MACs, compact badge IDs, native hardware IDs, and
bundle-derived identifiers stay out of the terminal transcript. The private
JSONL retains manufacturing hardware evidence plus the selected role, receipt,
and four safe game fields; failures record the role and a null receipt. The
CSV rework index remains backward-compatible.

This is a canary boundary, not a production promotion. The embedded production
factory ZIP remains unchanged. Pre-promotion physical role tests use an
explicit validated local canary bundle and USB seeding. If the selected
firmware rejects `game_seed`, the batch fails closed; embedded factory output
must not be described as game-capable until those artifacts are deliberately
promoted.

## Hardware Boundary

One physical badge trio contains:

- Uplink MCU: `uplink-s3-fof_badge`
- BLE-primary scanner MCU: `scanner-s3-combo-fof_badge`
- Wi-Fi-primary scanner MCU: `scanner-s3-combo-fof_badge`

The scanner firmware image is shared by the BLE and Wi-Fi scanner boards; the
uplink assigns the active role and scanner profile at runtime.

## What This Release Tests

`0.64.76-badge-defcon34` is the current badge release, paired with the
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
- Holding both badge buttons continuously for ten seconds performs a controlled
  software reboot. The previous physical quiet/off shortcut is removed; USB
  control can still manage volatile quiet mode, and reboot returns ACTIVE.
- Exact `fof-michagain` Remote ID coordinates for Hell, Michigan at 666 m, the
  exact `fof-goblue` SSID, or the temporary spare-button trigger opens the
  purple DEF CON 34 Wall of Sheep Easter egg once per boot. Any button dismisses
  it.
- `FOF_STATUS` exposes the staged firmware manifest, serialized update queue,
  scanner roles, power convergence, heap/stack, PSRAM, and display state.

## Build And Flash

From the repo root:

```sh
python3 scripts/fof_badge_flash.py \
  --transport usb \
  --only all \
  --port /dev/cu.usbmodemXXXX
```

Useful recovery-focused variants:

```sh
python3 scripts/fof_badge_flash.py --transport usb --only uplink --port /dev/cu.usbmodemXXXX
python3 scripts/fof_badge_flash.py --transport usb --only scanners --port /dev/cu.usbmodemXXXX
```

The supported field-update path needs only the uplink USB cable and the two
internal scanner UART connections. If scanner diagnostic USB cables are also
connected, add `--bind-selected-uplink`; the flasher otherwise refuses an
ambiguous multi-device USB census. Allow up to ten minutes for the serialized
scanner campaign and do not unplug while progress is advancing.

Do not flash either scanner through its direct USB port. Scanner USB is
diagnostics-only. The supported update path always stages one scanner image
through the uplink USB connection; the uplink then performs automatic,
serialized UART convergence for both scanner slots.
The exact recovery and verification contract is in
[Badge Scanner Recovery](../badge_scanner_recovery.md).

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

Local AP/backend badge status (read-only):

```sh
curl http://192.168.4.1/api/badge/status
```

All badge mutations—including display policy, themes, mode changes, reboot,
and firmware staging—use the uplink's USB serial connection. The badge
`POST /api/badge/control` route is a stable `403 badge_control_requires_usb`
refusal so enabling the local AP or backend cannot create a second control
authority.

Expected healthy status facts:

- Top-level `recovery_mode` is `normal`.
- Both scanners are connected and report `scanner-s3-combo-fof_badge`.
- Uplink and scanners report `0.64.76-badge-defcon34` after the matching badge
  images are flashed.
- `display_policy_hash` is non-zero.
- Scanner `display_policy_ack_hash` catches up to the uplink policy hash.

## Recovery Docs

- Badge USB hardening release acceptance: [usb-hardening-acceptance.md](usb-hardening-acceptance.md)
- Badge scanner recovery: [../badge_scanner_recovery.md](../badge_scanner_recovery.md)
- Badge boundary and guardrails: [../fof_badge_notes.md](../fof_badge_notes.md)
- Production ESP32 install docs: [../../esp32/INSTALL.md](../../esp32/INSTALL.md)

When the badge is stuck, prefer safe USB recovery first. ROM bootloader entry is
still available, but safe USB keeps `FOF_STATUS`, `FOF_PING`, recovery mode, and
scanner facts visible while you repair the trio.

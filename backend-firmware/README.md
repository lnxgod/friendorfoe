# Backend Sensor Firmware

This tree is the isolated backend-oriented ESP32 firmware. It owns its build
copies under `shared/`, `scanner/`, and `uplink/`; the immutable `vendor/` tree
is provenance evidence only and is never compiled. Nothing in this tree is a
badge build input.

The headless three-board Lite assembly uses two unchanged production ComboFO
scanner boards and one `uplink-s3-backend` image on a Seeed XIAO ESP32-S3.
Only the uplink is a backend firmware release. It consumes the production
scanner UART protocol and replaces the screen with USB/network uplink,
heartbeat, command, time, configuration-AP, and backend-only OTA workers.
GPIO21 drives the uplink board's single yellow LED with distinct timing
patterns for health, drone, Meta glasses, combined threats, and failure
states.

The native badge USB/factory firmware is a different firmware family.
`0.67.2-badge-defcon34` remains the normal badge default; never use the badge
flasher to install these Lite images, and never rename a backend image as a
badge target.

## Factory-flash a complete Lite assembly

The separate Backend Badge Lite factory flasher programs one complete,
three-board Lite assembly: two boards receive the unchanged production
`scanner-s3-combo-fof_badge` scanner image and the topology-selected center
board receives `uplink-s3-backend`. It is not the native/full-badge factory
flasher, and it is not the single-uplink maintenance web flasher.

From a repository checkout on macOS, double-click
`flash-lite-badges.command` or run:

```sh
./flash-lite-badges.command
```

The launcher uses the embedded offline bundle. Connect exactly three Seeed
XIAO ESP32-S3 boards, each with 8 MB flash and 8 MB PSRAM, only when prompted.
The operation erases and rewrites all three boards. The operator must type the
exact phrase `LITE` before a batch begins; scripted operation additionally
requires all three explicit arguments:

```sh
./flash-lite-badges.command \
  --yes --once --confirm-product badge_lite
```

This is a blank-board/rework factory operation, not a field update. Do not
connect a native badge, a configured field Lite unit, or any unrelated
Espressif USB device. See
[`docs/backend-lite-factory-flasher.md`](../docs/backend-lite-factory-flasher.md)
for the pinned bundle contents, topology and runtime PASS gates, private
manufacturing records, recovery rules, and the current candidate/release
boundary.

## Build the Lite uplink image

```sh
cd uplink
pio run -e uplink-s3-backend
```

Do not build or flash `backend-firmware/scanner` for the Lite production
assembly. Scanner0 and scanner1 remain on the production
`scanner-s3-combo-fof_badge` ComboFO image. The uplink uses an 8 MB DIO layout
with two 2 MB rollback-capable OTA slots. A successful compile is not
authorization to flash hardware.

On the Lite uplink, the read-only USB command `FOF_BACKEND_STATUS` reprints
current boot and health evidence without changing configuration or firmware.
The uplink configuration portal is available as
`FriendOrFoe-Backend-XXXXXX` when configuration is missing, after a prolonged
backend outage, or after an explicit USB AP request. Its factory password is
`friendorfoe` and should be changed during setup.

The portable scanner baseline provides BLE/Wi-Fi parsers, privacy and
detection policy, Bayesian fusion, a pure smart-glasses classifier, and two
synchronous sink boundaries. Its pure feature adapter turns typed BLE and Wi-Fi
observations into complete detection snapshots before emission. The scanner
UART task registers the sink consumers and owns cross-task queueing.
Producers pass stack-safe snapshots; the sinks copy before calling the
registered consumer and retain no caller pointers.

## Verify the portable detector baseline

```sh
pio test -e backend-native -f test_ported_detectors
python3 tools/check_source_isolation.py --root .
python3 tools/vendor_snapshot.py --repo-root .. --manifest VENDOR_MANIFEST.json --check
```

To materialize a missing snapshot from the pinned donor commit, omit
`--check`. The older `--repository` spelling remains an alias for
`--repo-root`.

The native environment links only the portable Task-2 source list. NimBLE,
ESP-Wi-Fi, NVS settings, the BLE investigator runtime, and BLE-JA3 remain
device-only. See `BACKEND_PORT_NOTES.md` for exact donor digests and adaptation
decisions.

## Canary and flashing boundary

The legacy hardware canary remains the guarded, direct-USB procedure in
[`docs/backend-firmware-canary.md`](../docs/backend-firmware-canary.md). It
still permits inventory, complete backup, and restore for scanner0/scanner1,
but it fails closed if `challenge-flash` or `flash-initial` selects either
scanner. Only the uplink can receive an initial backend image. The maintenance
web flasher likewise publishes only `uplink-s3-backend`; it contains no scanner
manifest or scanner binary.

Confirm all three boards are physical no-screen Lite XIAO ESP32-S3 sensors and
that no XIAO Sense SD expansion is connected; its GPIO3 SD chip-select conflicts
with uplink slot1 TX GPIO3. Native badge `0.67.2-badge-defcon34` remains the
normal USB/factory default and is not modified by the Lite procedure.

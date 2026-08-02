# Backend Sensor Firmware

This tree is the isolated backend-oriented ESP32 firmware. It owns its build
copies under `shared/`, `scanner/`, and `uplink/`; the immutable `vendor/` tree
is provenance evidence only and is never compiled. Nothing in this tree is a
badge build input.

The headless three-board Lite assembly uses two
`scanner-s3-combo-backend` images and one `uplink-s3-backend` image on Seeed
XIAO ESP32-S3 boards. Scanners retain the badge-derived BLE/Wi-Fi detectors.
The uplink replaces the screen with HTTP upload, heartbeat, command, time,
configuration-AP, and backend-only OTA workers. GPIO21 drives the board's
single yellow LED with distinct timing patterns for health, drone, Meta
glasses, combined threats, and failure states.

The native badge USB/factory firmware is a different firmware family.
`0.67.2-badge-defcon34` remains the normal badge default; never use the badge
flasher to install these Lite images, and never rename a backend image as a
badge target.

## Build the device images

```sh
cd scanner
pio run -e scanner-s3-combo-backend

cd ../uplink
pio run -e uplink-s3-backend
```

Both projects use an 8 MB DIO layout with two 2 MB rollback-capable OTA
slots. A successful compile is not authorization to flash hardware. Initial
migration is direct USB through the backend canary tooling after all three
boards have been identified and fully backed up; scanners are migrated first
and the uplink last.

On every board, the read-only USB command `FOF_BACKEND_STATUS` reprints the
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

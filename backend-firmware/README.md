# Backend Sensor Firmware

This tree is the isolated backend-oriented ESP32 firmware baseline. It owns
its build copies under `shared/` and `scanner/`; the immutable `vendor/` tree
is provenance evidence only and is never compiled.

The Task-2 scanner baseline provides portable BLE/Wi-Fi parsers, privacy and
detection policy, Bayesian fusion, a pure smart-glasses classifier, and two
synchronous sink boundaries. Its pure feature adapter turns typed BLE and Wi-Fi
observations into complete detection snapshots before emission. A later scanner
UART task registers the sink consumers and owns any cross-task queueing.
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

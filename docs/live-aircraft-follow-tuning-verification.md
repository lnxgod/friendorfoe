# Live Aircraft and Follow Tuning Verification

Release target: `v0.64.68-live-follow`

## Scope

- Android live-aircraft presentation frames, bounded track projection, retained
  map overlays, and stabilized compass-follow heading.
- Evidence-gated BLE follower and lingering-device classification with accurate
  location handling, bonded/ignored-device exclusions, and calm notifications.
- Android and ESP behavioral BLE pairing-spam and serial-service heuristics,
  including trusted-identity and deduplication edge cases.
- Badge Button 2 read-only BLE investigation routing and production/badge
  firmware release-track alignment.

## Automated Verification

| Surface | Command | Result |
| --- | --- | --- |
| Android | `cd android && ./gradlew testDebugUnitTest assembleDebug --rerun-tasks` | `BUILD SUCCESSFUL`; 369 tests, 0 failures, 0 errors, 0 skipped |
| Backend | `cd backend && .venv/bin/pytest tests -q` | 291 passed |
| ESP native | `cd esp32 && .venv312/bin/pio test -e test` | 374 passed |
| Scanner firmware | `pio run -e scanner-s3-combo -e scanner-s3-combo-seed -e scanner-s3-combo-fof_badge` | 3 targets succeeded |
| Uplink firmware | `pio run -e uplink-s3 -e uplink-s3-fof_badge` | 2 targets succeeded |
| Firmware metadata | `python3 esp32/scripts/verify_firmware_versions.py --repo-root .` | All 5 app descriptors match their release tracks |
| Repository | `git diff --check 5cac566...HEAD` | Clean |
| Web flasher | Parse `esp32/web-flasher/manifest-*.json` with Python `json` | 5 manifests parsed |

The focused firmware build-version guard has 5 passing tests. It forces an
ESP-IDF CMake reconfigure when generated `PROJECT_VER` metadata is stale and
the release workflow rejects any binary whose embedded app descriptor does
not match its production or badge track.

## Local Artifacts

Android debug APK:

- Path: `android/app/build/outputs/apk/debug/app-debug.apk`
- Package: `com.friendorfoe`
- Version: code `110`, name `0.64.68-live-follow`
- Size: 110,659,087 bytes
- SHA-256: `4cc8ea14d129406cf54d4b5c66907217cbca039877ac462312558c1282d4e2ff`
- Signature: APK Signature Scheme v2, local Android debug certificate

Firmware images:

| Target | Embedded version | SHA-256 |
| --- | --- | --- |
| `scanner-s3-combo` | `0.64.68-live-follow` | `cee360b03904c0045b42fa0aaddd445647448c4a3e9b91d2c448f9c605bedfcf` |
| `scanner-s3-combo-seed` | `0.64.68-live-follow` | `80f5af784913d74e5e545cdf20cdbde3254fc68384fe86d5c1fd798e9b2cbef5` |
| `scanner-s3-combo-fof_badge` | `0.64.68-badge-live-follow` | `a8089c4ef84aa453ac7223264d9f1e79ed2c84bc6d8d6f5cce15849ed9dc11a4` |
| `uplink-s3` | `0.64.68-live-follow` | `0e7086bb061b2c3268ca72997bebe0ef5f9661c02563a5f6dc415cf5ae49ca56` |
| `uplink-s3-fof_badge` | `0.64.68-badge-live-follow` | `7a444c81d7a6eae2330742dc3036410e6fb1d72f3c27e0accd049650b49d6bb1` |

## Hardware Status

No Android or badge hardware was connected during final verification:

- `adb devices -l` returned no devices.
- No badge USB device appeared in `system_profiler SPUSBDataType`.
- Only macOS Bluetooth and debug-console serial devices were present.

The following checks remain hardware-only and were not run: installing the APK
on a phone, observing aircraft coast/freeze behavior outdoors, field-tuning
Follow Me sensitivity, Android-to-badge Bluetooth operation, badge USB-C
transport, and Button 2 investigation against a physical BLE target.

## Release Gate

Before calling the release shipped, the tag workflows must be green and the
GitHub release must contain a release-signed APK plus all five standalone
firmware binaries and all five firmware bundles. The published APK version,
signature, and digest and each firmware binary's embedded version must be
verified from downloaded release assets.

# Venue Beacon Privacy Alerts Implementation Plan

> **For Bill/Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to execute this plan.

**Goal:** Add honest venue-beacon privacy alerts across Android, backend, and badge firmware without faking unsupported signals, verify the badge/sensor update path remains safe, and deploy the updated app/firmware release. Venue beacons include confirmed BLE beacon protocols such as iBeacon and Eddystone, plus existing venue-beacon signature metadata. Preserve OUI and SSID detector behavior, and avoid changing OTA/update layout or update artifact paths.

**Design Reference:** `docs/superpowers/specs/2026-07-09-venue-beacon-privacy-alerts-design.md`

## Constraints

- Use evidence-backed Bluetooth/Wi-Fi signals only.
- Do not add unsupported Flock Bluetooth/Wi-Fi claims.
- Keep scanner, badge, and sensor-node update paths flash-compatible with the current release flow.
- Add regression tests before production changes.
- Do not touch unrelated untracked files such as `docs/cfp/`.

## Task 1: Add Android parser and category regression tests

**Files:**
- `android/app/src/test/java/com/friendorfoe/detection/BlePacketParserTest.kt`
- `android/app/src/test/java/com/friendorfoe/detection/PrivacyCategoryMappingTest.kt`

**Steps:**
1. Add pure parser tests for iBeacon manufacturer data:
   - Apple company data with type `0x02`, length `0x15`
   - full UUID
   - major
   - minor
   - TX power
   - short/non-iBeacon data returns null
2. Add pure parser tests for Eddystone service data:
   - URL frame `0x10`
   - TLM frame `0x20`
   - UID frame `0x00`
   - EID frame `0x30`
3. Add category mapping tests:
   - `iBeacon` -> `VENUE_BEACON`
   - `Eddystone Beacon` -> `VENUE_BEACON`
   - `Retail Beacon` / `Location Beacon` -> `VENUE_BEACON`

**Verification:**
Run the targeted Android tests and confirm they fail before implementation:

```bash
cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BlePacketParserTest --tests com.friendorfoe.detection.PrivacyCategoryMappingTest
```

## Task 2: Implement Android venue-beacon detection details

**Files:**
- `android/app/src/main/java/com/friendorfoe/detection/BlePacketParser.kt`
- `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`

**Steps:**
1. Extract pure parser helpers from `BlePacketParser` so JVM tests do not require Android `ScanResult` objects.
2. Keep existing iBeacon, Eddystone URL, and Eddystone TLM behavior intact.
3. Add UID and EID frame parsing for evidence display.
4. Expand `parseAllDetails` to show venue-beacon protocol evidence:
   - protocol
   - frame
   - full UUID or URL or namespace/instance/EID
   - major/minor when available
   - TX power when available
5. Promote confirmed iBeacon and Eddystone signatures to `VENUE_BEACON` rather than informational/generic beacon buckets.
6. Keep generic beacons below alert threshold unless they match confirmed protocol or known venue-beacon metadata.

**Verification:**
Run the same targeted Android tests until green.

## Task 3: Add backend venue-beacon alert tests

**Files:**
- `backend/tests/test_privacy_devices.py`

**Steps:**
1. Add a test proving Apple manufacturer type `0x02` / iBeacon maps to `VENUE_BEACON`, not generic Apple Continuity.
2. Add a test proving Eddystone metadata maps to `VENUE_BEACON` and exposes useful display detail.
3. Add or preserve coverage proving generic Apple Nearby/Continuity remains `APPLE_CONTINUITY`.
4. Assert `VENUE_BEACON` produces an alert-level risk (`low` or `medium`), not `info`.

**Verification:**
Run the targeted backend tests and confirm the new tests fail before implementation:

```bash
cd backend && .venv/bin/pytest tests/test_privacy_devices.py -q
```

## Task 4: Implement backend venue-beacon classification/details

**Files:**
- `backend/app/services/privacy_devices.py`

**Steps:**
1. Detect Apple iBeacon subtype/type before generic Apple Continuity fallback.
2. Keep Eddystone UUID/service matching mapped to `VENUE_BEACON`.
3. Change venue-beacon risk to alert-level:
   - `medium` for close RSSI
   - `low` otherwise
4. Include useful detail strings for:
   - iBeacon UUID/major/minor when present
   - Eddystone URL/frame metadata when present
   - RSSI
5. Add venue-beacon evidence keys to the payload so the app can display “all the info” it receives.

**Verification:**
Run the targeted backend tests until green.

## Task 5: Add ESP32 venue-beacon firmware tests

**Files:**
- `esp32/test/test_detection_policy.c`
- `esp32/test/test_badge_threat_policy.c`
- `esp32/test/test_runner.c`

**Steps:**
1. Add BLE fingerprint test proving Apple iBeacon manufacturer data classifies as `BLE_DEV_VENUE_BEACON`.
2. Add badge policy test proving an iBeacon detection becomes a visible venue-beacon/badge alert with useful detail.
3. Preserve existing tests for Eddystone venue-beacon aggregation.

**Verification:**
Run ESP32 native tests and confirm the new tests fail before implementation:

```bash
cd esp32 && ./.venv312/bin/pio test -e test
```

## Task 6: Implement ESP32 venue-beacon firmware behavior

**Files:**
- `esp32/scanner/main/detection/ble_fingerprint.c`
- `esp32/shared/badge_threat_policy.c`

**Steps:**
1. Promote Apple iBeacon subtype `0x02` from generic beacon to `BLE_DEV_VENUE_BEACON`.
2. Set a stable `class_reason` such as `ibeacon:0x02` for downstream badge/backend evidence.
3. Ensure badge detail rendering distinguishes iBeacon from Eddystone when evidence is available.
4. Do not modify partition tables, OTA slots, flash offsets, update manifests, bootloader settings, or generated update artifact names.

**Verification:**
Run ESP32 native tests until green.

## Task 7: Final verification and review

**Steps:**
1. Run Android targeted tests.
2. Run backend targeted tests.
3. Run ESP32 native tests.
4. Check `git diff --stat` and `git diff --check`.
5. Inspect changed files for unintended update-path changes:
   - partition tables
   - OTA updater code
   - web-flasher manifests
   - sensor-node update code
6. Commit only the relevant implementation and test files.

**Verification Commands:**

```bash
cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BlePacketParserTest --tests com.friendorfoe.detection.PrivacyCategoryMappingTest
cd backend && .venv/bin/pytest tests/test_privacy_devices.py -q
cd esp32 && ./.venv312/bin/pio test -e test
git diff --check
```

## Task 8: Deploy through existing release/update path

**Steps:**
1. Inspect the current repo release conventions and versioning before changing any version/tag.
2. Build the release artifacts required by the existing deployment flow.
3. Verify the update manifest or release metadata still points to valid artifacts.
4. Push the implementation commit.
5. Push the release tag or deployment trigger used by this repo.
6. Confirm the deployment/release exists remotely and exposes the expected app/firmware update artifacts.

**Verification Commands:**

Use the repo-local release commands discovered from the existing scripts/docs. Do not invent a new release path.

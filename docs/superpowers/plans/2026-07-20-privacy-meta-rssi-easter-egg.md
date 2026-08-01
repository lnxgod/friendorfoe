# Privacy Meta, RSSI, Easter Egg, and OUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tighten Meta Glasses evidence, suppress weak non-drone badge rows, change the exact Easter-egg SSID, and add narrow privacy-infrastructure OUIs without creating phantom drones or camera claims.

**Architecture:** Keep parsing and classification separate. OUI records carry an explicit role consumed by Android and ESP32 routing; the badge shared policy remains the final display-admission gate. Meta and stale-expiry behavior stay aligned between scanner, uplink, shared badge policy, and Android.

**Tech Stack:** ESP-IDF/PlatformIO C, Kotlin/Jetpack Android, Unity native tests, JUnit coroutine tests.

## Global Constraints

- Hell, Michigan Remote ID at exactly 666 m remains unchanged.
- The Wi-Fi Easter-egg SSID is exact and case-sensitive: `GameChangersAI-67`.
- The Easter egg remains one-shot until reboot after dismissal.
- `-85 dBm` is eligible; non-drone detections at `-86 dBm` or weaker are rejected.
- Confirmed drone sources are exempt from the RSSI gate.
- OUI-only evidence says `<Vendor> Device`, never camera or recording.
- Generic Meta identifiers never become Meta Glasses.
- Do not flash hardware during implementation or verification without a separate user request.

---

### Task 1: Explicit OUI Roles and Privacy Vendors

**Files:**
- Modify: `esp32/scanner/main/detection/wifi_oui_database.h`
- Modify: `esp32/scanner/main/detection/wifi_oui_database.c`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/WifiOuiDatabase.kt`
- Test: `esp32/test/test_detection_policy.c`
- Test: `android/app/src/test/java/com/friendorfoe/detection/WifiOuiDatabaseTest.kt`

**Interfaces:**
- Produces: `oui_role_t`, `oui_entry_t.role`, Kotlin `OuiRole`, and `OuiEntry.role`.
- Consumes: existing three-byte OUI lookup APIs.

- [ ] **Step 1: Write failing ESP32 role tests**

Add assertions that `E0:A7:00`, `CC:47:BD`, `00:25:DF`, and the four Lytx prefixes resolve to `OUI_ROLE_PRIVACY_INFRASTRUCTURE`; `B4:1E:52` resolves to `OUI_ROLE_PRIVACY_FLOCK`; DJI remains `OUI_ROLE_DRONE`; shared or generic module prefixes remain ineligible.

- [ ] **Step 2: Run the native suite and verify RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: compile failure because `oui_role_t` and `entry->role` do not exist.

- [ ] **Step 3: Write failing Android role tests**

Assert:

```kotlin
assertEquals(OuiRole.PRIVACY_INFRASTRUCTURE,
    WifiOuiDatabase.lookup("00:25:DF:11:22:33")?.role)
assertFalse(WifiOuiDatabase.isDroneOui("00:25:DF:11:22:33"))
assertEquals(OuiRole.PRIVACY_FLOCK,
    WifiOuiDatabase.lookup("B4:1E:52:11:22:33")?.role)
assertTrue(WifiOuiDatabase.isDroneOui("60:60:1F:11:22:33"))
```

- [ ] **Step 4: Run Android tests and verify RED**

Run: `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.WifiOuiDatabaseTest`

Expected: compile failure because `OuiRole` and `OuiEntry.role` do not exist.

- [ ] **Step 5: Implement the explicit role contract**

Add ESP32 roles with `OUI_ROLE_DRONE = 0` so existing three-field initializers preserve their meaning, then mark Flock and the new vendor records explicitly. Add Kotlin `OuiRole` with a default `DRONE` parameter and make `isDroneOui()` require both `role == DRONE` and `!highFalsePositiveRisk`.

- [ ] **Step 6: Run both focused test sets and verify GREEN**

Run the commands from Steps 2 and 4. Expected: PASS.

- [ ] **Step 7: Commit the OUI role contract**

```bash
git add esp32/scanner/main/detection/wifi_oui_database.h \
  esp32/scanner/main/detection/wifi_oui_database.c \
  esp32/test/test_detection_policy.c \
  android/app/src/main/java/com/friendorfoe/detection/WifiOuiDatabase.kt \
  android/app/src/test/java/com/friendorfoe/detection/WifiOuiDatabaseTest.kt
git commit -m "privacy: add explicit OUI roles"
```

### Task 2: Route Privacy OUIs Without Camera Claims

**Files:**
- Modify: `esp32/scanner/main/detection/wifi_scanner.c`
- Modify: `esp32/shared/badge_threat_policy.c`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Test: `esp32/test/test_badge_threat_policy.c`
- Test: `android/app/src/test/java/com/friendorfoe/detection/PrivacyCategoryMappingTest.kt`

**Interfaces:**
- Consumes: `oui_entry_t.role` and Kotlin `OuiEntry.role` from Task 1.
- Produces: ESP32 privacy detections with `model="Privacy Infrastructure"` and `class_reason="privacy infrastructure OUI"`; Android `PrivacyCategory.SECURITY_INFRASTRUCTURE` rows.

- [ ] **Step 1: Write failing badge-policy tests**

Create a `DETECTION_SRC_WIFI_AP_INVENTORY` detection with manufacturer `Axon`, model `Privacy Infrastructure`, class reason `privacy infrastructure OUI`, and RSSI `-55`. Assert it classifies as a low-priority privacy entity labeled `Axon Device`, with no `camera`, `recording`, or `drone` text.

- [ ] **Step 2: Verify native RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: the Axon detection is ignored or receives the wrong label/category.

- [ ] **Step 3: Write failing Android privacy-OUI tests**

Call `GlassesDetector.checkWifiSsid("", "00:25:DF:11:22:33", -55)` and assert manufacturer `Axon`, device type `Privacy Infrastructure`, `hasCamera == false`, category `SECURITY_INFRASTRUCTURE`, and no drone classification through `WifiOuiDatabase.isDroneOui()`.

- [ ] **Step 4: Verify Android RED**

Run: `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.PrivacyCategoryMappingTest`

Expected: no privacy detection exists for the Axon OUI.

- [ ] **Step 5: Implement scanner and badge routing**

Before the generic drone-OUI branch, emit privacy-role records as `DETECTION_SRC_WIFI_AP_INVENTORY` with confidence `0.62`, vendor manufacturer, model `Privacy Infrastructure`, and class reason `privacy infrastructure OUI`. Add a dedicated badge classification branch before drone candidates that emits `<Vendor> Device`, detail `privacy infrastructure OUI`, class `BADGE_THREAT_OTHER`, category `BADGE_THREAT_CATEGORY_PRIVACY`, and low display score.

- [ ] **Step 6: Implement Android routing**

Add `SECURITY_INFRASTRUCTURE("Security Infrastructure", "\uD83D\uDEE1\uFE0F", 1)` and use the OUI role in `checkWifiBssid()` to produce vendor-device privacy rows without setting `hasCamera`.

- [ ] **Step 7: Verify focused tests GREEN and commit**

Run both focused commands from Steps 2 and 4, then commit the five files with subject `privacy: surface vendor OUI devices safely`.

### Task 3: Tight Meta Evidence and Android Expiry

**Files:**
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Create: `android/app/src/main/java/com/friendorfoe/detection/GlassesStalePolicy.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt`
- Test: `esp32/test/test_detection_policy.c`
- Test: `esp32/test/test_badge_threat_policy.c`
- Create: `android/app/src/test/java/com/friendorfoe/detection/GlassesStalePolicyTest.kt`

**Interfaces:**
- Produces: `GlassesStalePolicy.ttlSeconds()`, `isStale()`, and `nextExpiryDelayMillis()`.
- Consumes: existing `GlassesDetection`, scanner Meta fingerprint type, and uplink scanner status.

- [ ] **Step 1: Write failing generic-Meta native tests**

Assert a generic `0x01AB`, `0x058E`, `0xFEB7`, or `0xFEB8` fingerprint remains `BLE_DEV_META_DEVICE` and does not satisfy the strong badge Meta status path, while `0x0D53`, `0xFD5F`, and explicit Ray-Ban/Oakley names still do.

- [ ] **Step 2: Verify native RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: generic Meta currently updates `strong_fp` badge Meta status.

- [ ] **Step 3: Write failing Android stale-policy tests**

Test that Meta has a 300-second TTL, ordinary rows have a 60-second TTL, a row older than its TTL is stale, and `nextExpiryDelayMillis()` returns a finite positive delay for a non-empty list.

- [ ] **Step 4: Verify Android RED**

Run: `cd android && ./gradlew testDebugUnitTest --tests com.friendorfoe.detection.GlassesStalePolicyTest`

Expected: compile failure because `GlassesStalePolicy` does not exist.

- [ ] **Step 5: Implement strict Meta status**

Only `BLE_DEV_META_GLASSES` calls `badge_ble_note_meta(..., "strong_fp", false)`. Generic `BLE_DEV_META_DEVICE` remains normal low-confidence telemetry. Keep the uplink strong-identity guard and ensure it cannot hardcode a glasses event from a generic reason.

- [ ] **Step 6: Implement Android Meta parity and self-expiry**

Map `CID_META` and `CID_META_TECH` to non-camera generic Meta device types. Move TTL logic to `GlassesStalePolicy`. After each commit, schedule the next expiry from the earliest row deadline; expiry wakes, prunes, publishes, and schedules again even when no new BLE event arrives. Cancel the expiry job when scanning stops.

- [ ] **Step 7: Verify focused tests GREEN and commit**

Run the native suite and Android stale-policy test, then commit with subject `privacy: require specific Meta glasses evidence`.

### Task 4: Shared Non-Drone RSSI Gate

**Files:**
- Modify: `esp32/shared/badge_threat_policy.c`
- Test: `esp32/test/test_badge_threat_policy.c`

**Interfaces:**
- Produces: a single display-admission gate inside `badge_threat_classify_detection()`.
- Consumes: `source_is_confirmed_drone()` and `drone_detection_t.rssi`.

- [ ] **Step 1: Write failing boundary tests**

Assert a non-drone privacy record at `-86` is rejected, the same record at `-85` is accepted, and confirmed BLE RID, Wi-Fi DJI IE, and Wi-Fi Beacon RID remain accepted at `-95`.

- [ ] **Step 2: Verify RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: the `-86` non-drone record currently classifies.

- [ ] **Step 3: Implement the minimal gate**

At the start of `badge_threat_classify_detection()`, after null checks and before category matching, return false when `det->rssi < -85`, `det->rssi < 0`, and `!source_is_confirmed_drone(det->source)`. Non-negative unknown/status values remain exempt.

- [ ] **Step 4: Verify GREEN and commit**

Run the native suite, then commit both files with subject `privacy: filter weak non-drone badge rows`.

### Task 5: Exact GameChangersAI Easter-Egg SSID

**Files:**
- Modify: `esp32/shared/badge_easter_egg.c`
- Modify: `esp32/test/test_badge_easter_egg.c`
- Modify: `esp32/CHANGELOG.md`

**Interfaces:**
- Produces: exact case-sensitive match for the 17-byte SSID `GameChangersAI-67`.
- Consumes: existing one-shot Easter-egg state machine and UART event.

- [ ] **Step 1: Change only the test expectations**

Assert the new exact SSID matches. Assert `fof-goblue`, lowercase/case variants, prefixes, suffixes, and embedded-NUL forms do not.

- [ ] **Step 2: Verify RED**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: the new SSID fails and the old SSID still matches.

- [ ] **Step 3: Implement the exact byte match**

Use `len == sizeof("GameChangersAI-67") - 1` and `memcmp()` against the literal. Update only current changelog wording; do not alter Remote ID or button logic.

- [ ] **Step 4: Verify GREEN and commit**

Run the native suite, then commit the three files with subject `badge: change Easter egg WiFi trigger`.

### Task 6: Full Verification and Release-Safety Review

**Files:**
- Review: all files changed by Tasks 1-5

**Interfaces:**
- Consumes: all task outputs.
- Produces: build/test evidence and a clean scoped diff.

- [ ] **Step 1: Run full Android tests**

Run: `cd android && ./gradlew testDebugUnitTest`

Expected: `BUILD SUCCESSFUL` with zero failing tests.

- [ ] **Step 2: Run the full native ESP32 suite**

Run: `cd esp32 && /Users/billh/.platformio/penv/bin/pio test -e test`

Expected: all Unity tests pass with zero failures.

- [ ] **Step 3: Build both badge targets**

Run:

```bash
cd esp32/scanner && /Users/billh/.platformio/penv/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink && /Users/billh/.platformio/penv/bin/pio run -e uplink-s3-fof_badge
```

Expected: both environments report `SUCCESS`; each firmware image remains below its `0x200000` OTA-slot limit.

- [ ] **Step 4: Review scope and safety**

Run: `git diff --check HEAD~5..HEAD`, `git status --short --branch`, and inspect every changed production file. Confirm `.camera-before-zoom.jpg` remains untouched and untracked.

- [ ] **Step 5: Report without flashing**

Provide exact test counts, firmware sizes, remaining OTA headroom, and any unverified hardware behavior. Do not bump versions, tag, push, or flash unless separately requested.

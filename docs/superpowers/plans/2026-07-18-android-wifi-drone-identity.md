# Android Wi-Fi Drone Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Android Wi-Fi drone detection recover after permission or radio-state changes, distinguish observed DJI transmitters by validated BSSID, preserve that radio evidence in history, and remove the AR camera-direction warning.

**Architecture:** Split Android platform access from a pure readiness policy so the long-lived coordinator can be tested without framework mocks. Normalize observed BSSIDs in one pure identity helper used by every Wi-Fi drone path, and centralize domain/history mapping so both persistence callers and history reconstruction share one contract. Keep the AR edit presentation-only and validate the badge detector tree without modifying it.

**Tech Stack:** Kotlin 1.9.22, coroutines/Flow, Hilt, Jetpack Compose, Room 2.6.1, JUnit 4, AndroidX instrumented tests, PlatformIO native ESP32 tests.

## Global Constraints

- Badge firmware is read-only; do not modify any production file under `esp32/`.
- Do not add a foreground service or background-location tracking.
- Keep the existing normal Wi-Fi scan limit of four scans per two minutes and the 30-second ready-state cadence.
- Require `ACCESS_FINE_LOCATION` for Android Wi-Fi scan results and additionally require `NEARBY_WIFI_DEVICES` on Android 13 or newer.
- Do not claim that an observed BSSID is a permanent DJI aircraft identity.
- Display the exact label `Observed BSSID / MAC` and the exact copy `May rotate or belong to the aircraft/controller radio.`
- Remove only the AR camera-direction banner; preserve camera tracking, pitch calculation, compass behavior, ground-clutter suppression, target projection, and lock-on guidance.
- Advance Room from schema version 4 to 5 with additive nullable radio-evidence columns.

---

### Task 1: Recoverable Wi-Fi Scan Readiness

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/detection/WifiScanReadiness.kt`
- Create: `android/app/src/main/java/com/friendorfoe/detection/AndroidWifiScanPlatform.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/WifiScanCoordinator.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt`
- Test: `android/app/src/test/java/com/friendorfoe/detection/WifiScanReadinessTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/detection/WifiScanCoordinatorTest.kt`

**Interfaces:**
- Produces: `WifiScanAccessSnapshot`, `WifiScanReadiness`, `WifiScanPlatform`, `AndroidWifiScanPlatform`, `WifiScanCoordinator.readiness: StateFlow<WifiScanReadiness>`, and `WifiScanCoordinator.notifyPlatformStateChanged()`.
- Consumes: Android `WifiManager`, `LocationManager`, runtime permission state, and the existing shared scan-result Flow consumers.

- [ ] **Step 1: Write the failing pure readiness tests**

Create `WifiScanReadinessTest.kt` with a table that asserts the exact state matrix:

```kotlin
class WifiScanReadinessTest {
    @Test fun android_13_requires_fine_location_and_nearby_wifi() {
        val base = WifiScanAccessSnapshot(
            sdkInt = 33,
            fineLocationGranted = true,
            nearbyWifiGranted = true,
            locationServicesEnabled = true,
            wifiEnabled = true,
        )
        assertEquals(WifiScanReadiness.READY, base.evaluate())
        assertEquals(
            WifiScanReadiness.MISSING_FINE_LOCATION,
            base.copy(fineLocationGranted = false).evaluate(),
        )
        assertEquals(
            WifiScanReadiness.MISSING_NEARBY_WIFI_DEVICES,
            base.copy(nearbyWifiGranted = false).evaluate(),
        )
    }

    @Test fun pre_android_13_does_not_require_nearby_wifi() {
        val snapshot = WifiScanAccessSnapshot(
            sdkInt = 32,
            fineLocationGranted = true,
            nearbyWifiGranted = false,
            locationServicesEnabled = true,
            wifiEnabled = true,
        )
        assertEquals(WifiScanReadiness.READY, snapshot.evaluate())
    }

    @Test fun disabled_location_and_wifi_are_reported_before_scan() {
        val base = WifiScanAccessSnapshot(34, true, true, true, true)
        assertEquals(
            WifiScanReadiness.LOCATION_SERVICES_DISABLED,
            base.copy(locationServicesEnabled = false).evaluate(),
        )
        assertEquals(
            WifiScanReadiness.WIFI_DISABLED,
            base.copy(wifiEnabled = false).evaluate(),
        )
    }
}
```

- [ ] **Step 2: Run the readiness test and verify RED**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.WifiScanReadinessTest
```

Expected: compilation fails because `WifiScanAccessSnapshot` and `WifiScanReadiness` do not exist.

- [ ] **Step 3: Implement the minimal pure readiness policy**

Create `WifiScanReadiness.kt`:

```kotlin
package com.friendorfoe.detection

import android.os.Build

enum class WifiScanReadiness {
    READY,
    MISSING_FINE_LOCATION,
    MISSING_NEARBY_WIFI_DEVICES,
    LOCATION_SERVICES_DISABLED,
    WIFI_DISABLED,
    TRANSIENT_FAILURE,
}

data class WifiScanAccessSnapshot(
    val sdkInt: Int,
    val fineLocationGranted: Boolean,
    val nearbyWifiGranted: Boolean,
    val locationServicesEnabled: Boolean,
    val wifiEnabled: Boolean,
) {
    fun evaluate(): WifiScanReadiness = when {
        !fineLocationGranted -> WifiScanReadiness.MISSING_FINE_LOCATION
        sdkInt >= Build.VERSION_CODES.TIRAMISU && !nearbyWifiGranted ->
            WifiScanReadiness.MISSING_NEARBY_WIFI_DEVICES
        !locationServicesEnabled -> WifiScanReadiness.LOCATION_SERVICES_DISABLED
        !wifiEnabled -> WifiScanReadiness.WIFI_DISABLED
        else -> WifiScanReadiness.READY
    }
}
```

- [ ] **Step 4: Run the readiness test and verify GREEN**

Run the command from Step 2. Expected: all `WifiScanReadinessTest` tests pass.

- [ ] **Step 5: Write failing coordinator recovery tests**

Create a fake `WifiScanPlatform` in `WifiScanCoordinatorTest.kt`. Use
`runTest`, `StandardTestDispatcher(testScheduler)`, and a collector in
`backgroundScope`. Cover these behaviors separately:

```kotlin
@Test fun collector_stays_alive_until_first_permission_grant() = runTest {
    val platform = FakeWifiScanPlatform(readiness = WifiScanReadiness.MISSING_FINE_LOCATION)
    val coordinator = coordinator(platform)
    val job = backgroundScope.launch { coordinator.scanResults().collect() }
    runCurrent()
    assertTrue(job.isActive)
    assertEquals(0, platform.startScanCalls)

    platform.readiness = WifiScanReadiness.READY
    coordinator.notifyPlatformStateChanged()
    runCurrent()

    assertEquals(1, platform.startScanCalls)
    assertTrue(job.isActive)
}

@Test fun security_exception_does_not_close_stream_and_later_scan_recovers() = runTest {
    val platform = FakeWifiScanPlatform(readiness = WifiScanReadiness.READY)
    platform.startFailure = SecurityException("revoked")
    val coordinator = coordinator(platform)
    val job = backgroundScope.launch { coordinator.scanResults().collect() }
    runCurrent()
    assertEquals(WifiScanReadiness.TRANSIENT_FAILURE, coordinator.readiness.value)
    assertTrue(job.isActive)

    platform.startFailure = null
    coordinator.notifyPlatformStateChanged()
    runCurrent()
    assertEquals(WifiScanReadiness.READY, coordinator.readiness.value)
    assertTrue(platform.startScanCalls >= 2)
}
```

Also assert that missing permission, disabled location services, and disabled
Wi-Fi never invoke `startScan()` or `scanResults()`, and that receiver-triggered
cached-result publication catches `SecurityException` without closing the Flow.

- [ ] **Step 6: Run the coordinator tests and verify RED**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.WifiScanCoordinatorTest
```

Expected: compilation fails because `WifiScanPlatform`, the test constructor,
`readiness`, and `notifyPlatformStateChanged()` do not exist.

- [ ] **Step 7: Add the Android platform adapter and recoverable coordinator**

Define this exact boundary in `AndroidWifiScanPlatform.kt`:

```kotlin
internal interface WifiScanPlatform {
    fun readiness(): WifiScanReadiness
    fun registerResultsReceiver(onResultsAvailable: () -> Unit)
    fun unregisterResultsReceiver()
    fun startScan(): Boolean
    fun cachedResults(): List<ScanResult>
}

@Singleton
class AndroidWifiScanPlatform @Inject constructor(
    @ApplicationContext private val context: Context,
    private val wifiManager: WifiManager,
    private val locationManager: LocationManager,
) : WifiScanPlatform
```

The adapter computes `WifiScanAccessSnapshot` with `ContextCompat`, uses
`LocationManager.isLocationEnabled` on API 28+, falls back to GPS/network
provider checks on API 26-27, and owns receiver registration. Its scan methods
are the only code that calls `WifiManager.startScan()` or reads
`WifiManager.scanResults`.

Refactor `WifiScanCoordinator` so its injectable constructor delegates to an
internal constructor accepting `WifiScanPlatform`, `CoroutineDispatcher`,
ready and blocked intervals, and a clock. `scanResults()` must always call
`start()` and must never close solely because readiness is blocked. The worker
loop uses this shape:

```kotlin
while (isActive) {
    val state = platform.readiness()
    _readiness.value = state
    val succeeded = if (state == WifiScanReadiness.READY) {
        runScanCycleCatchingSecurityException()
    } else {
        false
    }
    val delayMs = if (state == WifiScanReadiness.READY && succeeded) {
        SCAN_INTERVAL_MS
    } else {
        BLOCKED_RECHECK_MS
    }
    withTimeoutOrNull(delayMs) { wakeEvents.first() }
}
```

Keep `MAX_SCANS_IN_WINDOW = 4`, `THROTTLE_WINDOW_MS = 120_000L`,
`SCAN_INTERVAL_MS = 30_000L`, and use `BLOCKED_RECHECK_MS = 1_000L` only while
blocked or recovering. Catch `SecurityException` around both scan methods,
publish `TRANSIENT_FAILURE`, and leave the worker alive. Make receiver callbacks
run the same readiness check before reading cached results.

- [ ] **Step 8: Notify recovery from permission and activity resume**

Inject `WifiScanCoordinator` into `MainActivity`, call
`notifyPlatformStateChanged()` in `onResume()`, pass the callback into
`FriendOrFoeApp`, and change the permission launcher callback from empty to:

```kotlin
) { wifiScanCoordinator.notifyPlatformStateChanged() }
```

Keep the existing startup permission list, including both fine location and
nearby Wi-Fi on Android 13+; do not repeatedly relaunch the prompt after denial.

- [ ] **Step 9: Run focused and neighboring tests, then commit**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.WifiScan*Test'
```

Expected: all readiness, recovery, and existing Wi-Fi tests pass. Stage only
the six Task 1 files and commit:

```bash
git commit -m "android: recover Wi-Fi scanning after permission changes"
```

---

### Task 2: Canonical Observed BSSID Identity

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/detection/WifiTransmitterIdentity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/WifiDroneScanner.kt`
- Test: `android/app/src/test/java/com/friendorfoe/detection/WifiTransmitterIdentityTest.kt`

**Interfaces:**
- Produces: `WifiTransmitterIdentity.normalize(raw: String?): String?`,
  `identityKey(ssid: String, normalizedBssid: String?): String`, and
  `detectionId(prefix: String, ssid: String, normalizedBssid: String?): String`.
- Consumes: raw Android `ScanResult.SSID` and `ScanResult.BSSID`.

- [ ] **Step 1: Write failing BSSID validation and identity tests**

Create `WifiTransmitterIdentityTest.kt` with exact positive and negative cases:

```kotlin
@Test fun canonicalizes_valid_unicast_bssid() {
    assertEquals(
        "60:60:1F:AA:BB:CC",
        WifiTransmitterIdentity.normalize("60-60-1f-aa-bb-cc"),
    )
}

@Test fun rejects_non_identity_addresses() {
    listOf(
        null, "", "02:00:00:00:00:00", "00:00:00:00:00:00",
        "FF:FF:FF:FF:FF:FF", "01:00:5E:00:00:01", "not-a-mac",
    ).forEach { assertNull(it, WifiTransmitterIdentity.normalize(it)) }
}

@Test fun same_ssid_uses_distinct_valid_bssids_for_detection_ids() {
    val first = WifiTransmitterIdentity.detectionId("wifi", "DJI-MAVIC3", "60:60:1F:00:00:01")
    val second = WifiTransmitterIdentity.detectionId("wifi", "DJI-MAVIC3", "60:60:1F:00:00:02")
    assertNotEquals(first, second)
}

@Test fun invalid_bssid_falls_back_to_normalized_ssid() {
    assertEquals(
        "wifi_dji_mavic3",
        WifiTransmitterIdentity.detectionId("wifi", "DJI-MAVIC3", null),
    )
}
```

- [ ] **Step 2: Run the identity test and verify RED**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.WifiTransmitterIdentityTest
```

Expected: compilation fails because `WifiTransmitterIdentity` does not exist.

- [ ] **Step 3: Implement the minimal identity helper**

Create `WifiTransmitterIdentity.kt`. Normalize hyphens to colons, require six
two-digit hexadecimal octets, reject the explicit placeholders, decode the
first octet, and reject `(firstOctet and 0x01) != 0`. Build valid-radio IDs from
the 12 lowercase hexadecimal digits; otherwise slug the SSID using the current
`[^a-z0-9]` to underscore rule:

```kotlin
internal object WifiTransmitterIdentity {
    private val canonicalPattern = Regex("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
    private val rejected = setOf(
        "00:00:00:00:00:00",
        "02:00:00:00:00:00",
        "FF:FF:FF:FF:FF:FF",
    )

    fun normalize(raw: String?): String? {
        val canonical = raw
            ?.trim()
            ?.replace('-', ':')
            ?.uppercase()
            ?: return null
        if (!canonicalPattern.matches(canonical) || canonical in rejected) return null
        val firstOctet = canonical.substring(0, 2).toInt(16)
        if ((firstOctet and 0x01) != 0) return null
        return canonical
    }

    fun identityKey(ssid: String, normalizedBssid: String?): String =
        normalizedBssid ?: "ssid:${ssidSlug(ssid)}"

    fun detectionId(prefix: String, ssid: String, normalizedBssid: String?): String {
        val suffix = normalizedBssid
            ?.replace(":", "")
            ?.lowercase()
            ?: ssidSlug(ssid)
        return "${prefix}_$suffix"
    }

    private fun ssidSlug(ssid: String): String =
        ssid.lowercase().replace(Regex("[^a-z0-9]"), "_")
}
```

- [ ] **Step 4: Run the helper tests and verify GREEN**

Run the command from Step 2. Expected: all identity tests pass.

- [ ] **Step 5: Write the failing scanner integration contract**

Add a test that reads `WifiDroneScanner.kt` and asserts the old
`result.BSSID ?: continue` path is absent, while the production loop calls
`WifiTransmitterIdentity.normalize(result.BSSID)`, uses `identityKey`, uses
`detectionId` for SSID/DJI/soft IDs, and assigns only the normalized nullable
BSSID to `Drone.bssid`.

Run the focused test and verify it fails because the scanner still skips
missing BSSIDs and builds an SSID-only ID.

- [ ] **Step 6: Integrate canonical identity through every Wi-Fi drone path**

At the start of each result iteration, compute:

```kotlin
val ssid = result.SSID.orEmpty()
val bssid = WifiTransmitterIdentity.normalize(result.BSSID)
val radioKey = WifiTransmitterIdentity.identityKey(ssid, bssid)
```

Use `radioKey` for duplicate suppression and Remote ID partial-state keys. Use
`detectionId("wifi_dji", ssid, bssid)`,
`detectionId("wifi", ssid, bssid)`, and
`detectionId("wifi_soft", ssid, bssid)` for the corresponding detection paths.
Attempt OUI detection only when `bssid != null`; use
`detectionId("wifi_oui", ssid, bssid)` for its ID. Store only canonical `bssid`
in every `Drone`; do not skip an SSID match
when Android supplies a null, malformed, placeholder, broadcast, or multicast
address.

- [ ] **Step 7: Run Wi-Fi detector tests, then commit**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.Wifi*Test'
```

Expected: all Wi-Fi identity, DJI pattern, OUI, privacy, and anomaly tests pass.
Stage only the three Task 2 files and commit:

```bash
git commit -m "android: retain validated Wi-Fi transmitter identity"
```

---

### Task 3: Room v5 Radio Evidence and Shared History Mapping

**Files:**
- Modify: `android/gradle/libs.versions.toml`
- Modify: `android/app/build.gradle.kts`
- Modify: `android/app/src/main/java/com/friendorfoe/data/local/HistoryEntity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/local/FriendOrFoeDatabase.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/local/HistoryMappers.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/domain/usecase/SaveDetectionUseCase.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailViewModel.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/local/HistoryMappersTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/data/local/FriendOrFoeDatabaseMigrationTest.kt`
- Generate: `android/app/schemas/com.friendorfoe.data.local.FriendOrFoeDatabase/4.json`
- Generate: `android/app/schemas/com.friendorfoe.data.local.FriendOrFoeDatabase/5.json`

**Interfaces:**
- Produces: nullable `HistoryEntity.ssid`, `bssid`, `signalStrengthDbm`,
  `frequencyMhz`, and `channelWidthMhz`; shared
  `SkyObject.toHistoryEntity(userLatitude, userLongitude)`; shared
  `HistoryEntity.toDrone()`; `FriendOrFoeDatabase.MIGRATION_4_5`.
- Consumes: canonical `Drone.bssid` and existing history DAO/navigation behavior.

- [ ] **Step 1: Enable Room schema and migration test infrastructure**

Add `room-testing` to the version catalog, add
`androidTestImplementation(libs.room.testing)`, and configure KSP with:

```kotlin
ksp {
    arg("room.schemaLocation", "$projectDir/schemas")
}
```

Before changing the database version, run:

```bash
cd android
./gradlew kspDebugKotlin
```

Expected: schema version 4 is exported under `android/app/schemas/`.

- [ ] **Step 2: Write the failing shared-mapper test**

Create `HistoryMappersTest.kt`. Construct a Wi-Fi `Drone` with
`id = "wifi_60601faabbcc"`, `ssid = "DJI-MAVIC3-TEST"`,
`bssid = "60:60:1F:AA:BB:CC"`, `signalStrengthDbm = -47`,
`frequencyMhz = 5745`, and `channelWidthMhz = 80`. Assert that
`toHistoryEntity(42.0, -83.0)` preserves all five fields and that
`entity.toDrone()` restores the same ID and radio evidence.

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.data.local.HistoryMappersTest
```

Expected: compilation fails because the fields and shared mappers do not exist.

- [ ] **Step 3: Add the v5 fields and shared mapping**

Append these default-null fields to `HistoryEntity`:

```kotlin
@ColumnInfo(name = "ssid") val ssid: String? = null,
@ColumnInfo(name = "bssid") val bssid: String? = null,
@ColumnInfo(name = "signal_strength_dbm") val signalStrengthDbm: Int? = null,
@ColumnInfo(name = "frequency_mhz") val frequencyMhz: Int? = null,
@ColumnInfo(name = "channel_width_mhz") val channelWidthMhz: Int? = null,
```

Create `HistoryMappers.kt`. Map aircraft exactly as today. Map drone
`objectId = id` and copy all five radio fields. `HistoryEntity.toDrone()` must
restore ID, position, source, category, confidence, timestamps, distance,
manufacturer, and the five radio fields; invalid enum strings keep the current
fallbacks (`ADS_B` and `UNKNOWN`). Replace both existing persistence mappings
with the shared extension and replace the history-drone constructor in
`DetailViewModel` with `historyEntity.toDrone()`.

Run the mapper test again. Expected: all mapper assertions pass.

- [ ] **Step 4: Write the failing real migration test**

Create `FriendOrFoeDatabaseMigrationTest.kt` with `MigrationTestHelper`. Create
a version-4 database, insert a complete legacy `detection_history` row, execute
`MIGRATION_4_5`, and query the row. Assert the original fields are unchanged
and each new field is SQL `NULL`.

Run:

```bash
cd android
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.local.FriendOrFoeDatabaseMigrationTest
```

Expected: compilation fails because database version 5 and `MIGRATION_4_5` do
not exist.

- [ ] **Step 5: Implement and register the additive migration**

Set `version = 5`. Add this public companion migration and register it after
the existing migrations:

```kotlin
val MIGRATION_4_5 = object : Migration(4, 5) {
    override fun migrate(db: SupportSQLiteDatabase) {
        db.execSQL("ALTER TABLE `detection_history` ADD COLUMN `ssid` TEXT")
        db.execSQL("ALTER TABLE `detection_history` ADD COLUMN `bssid` TEXT")
        db.execSQL("ALTER TABLE `detection_history` ADD COLUMN `signal_strength_dbm` INTEGER")
        db.execSQL("ALTER TABLE `detection_history` ADD COLUMN `frequency_mhz` INTEGER")
        db.execSQL("ALTER TABLE `detection_history` ADD COLUMN `channel_width_mhz` INTEGER")
    }
}
```

Run `kspDebugKotlin` to export schema 5, then rerun the migration test. Expected:
the legacy row survives and all five added columns are null.

- [ ] **Step 6: Run history and build gates, then commit**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.local.*'
./gradlew assembleDebugAndroidTest
```

Expected: unit mapper tests pass and the instrumented test APK compiles. Stage
only the Task 3 files and generated schema JSON, then commit:

```bash
git commit -m "android: preserve Wi-Fi radio evidence in history"
```

---

### Task 4: Honest Transmitter Copy and AR Warning Removal

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/detail/TransmitterCopy.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/detail/TransmitterCopyTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/ar/ArGroundWarningContractTest.kt`

**Interfaces:**
- Produces: `OBSERVED_BSSID_LABEL` and `OBSERVED_BSSID_EXPLANATION`.
- Consumes: `Drone.bssid` and the existing AR orientation/tracking state.

- [ ] **Step 1: Write the failing transmitter-copy test**

Create `TransmitterCopyTest.kt`:

```kotlin
@Test fun bssid_copy_describes_observed_radio_without_overclaiming() {
    assertEquals("Observed BSSID / MAC", OBSERVED_BSSID_LABEL)
    assertEquals(
        "May rotate or belong to the aircraft/controller radio.",
        OBSERVED_BSSID_EXPLANATION,
    )
}
```

Run the focused test. Expected: compilation fails because the constants do not
exist.

- [ ] **Step 2: Add and render the exact transmitter copy**

Create `TransmitterCopy.kt` with the two `internal const val` declarations.
Change the BSSID detail row to use `OBSERVED_BSSID_LABEL`, and render
`OBSERVED_BSSID_EXPLANATION` immediately after it with `bodySmall` typography
and the existing subdued on-surface color treatment. Keep OUI hardware lookup,
channel, bandwidth, signal, and range rows unchanged.

Run `TransmitterCopyTest`. Expected: pass.

- [ ] **Step 3: Write the failing AR removal contract**

Create `ArGroundWarningContractTest.kt`. Resolve
`src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt`, read it, and
assert:

```kotlin
assertFalse(source.contains("showGroundBanner"))
assertFalse(source.contains("Camera pointing below horizon"))
assertTrue(source.contains("currentPitch = orientation.pitchDegrees"))
assertTrue(source.contains("pitchDegrees = orientation.pitchDegrees"))
```

Run the focused test. Expected: fail because the current banner state and text
are present.

- [ ] **Step 4: Remove only the AR banner**

Delete the `showGroundBanner` remembered state, its `-10f`/`-5f` hysteresis,
and the conditional banner composable. Do not edit other pitch references or
the visual detector/tracking pipeline.

Run the two Task 4 tests. Expected: both pass.

- [ ] **Step 5: Run presentation tests, then commit**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.detail.*' \
  --tests 'com.friendorfoe.presentation.ar.*'
```

Expected: all selected tests pass. Stage only the five Task 4 files and commit:

```bash
git commit -m "android: clarify transmitter identity and clean up AR"
```

---

### Task 5: Release Verification With Badge Firmware Read-Only

**Files:**
- Verify only: all Android files changed by Tasks 1-4
- Verify only: `esp32/test/`, `esp32/scanner/test/`, and existing detector production files

**Interfaces:**
- Consumes: completed Android feature set and current unchanged badge detector tree.
- Produces: reproducible test/build evidence and a proof that this task did not alter the badge firmware diff.

- [ ] **Step 1: Capture the badge-diff fingerprint before implementation**

Run from the worktree root before Android production edits:

```bash
git diff -- esp32 | shasum -a 256
git status --short esp32
```

Retain the hash and status output for comparison; existing badge release edits
may already be dirty and must remain byte-for-byte untouched.

- [ ] **Step 2: Run the complete Android unit suite**

Run:

```bash
cd android
./gradlew testDebugUnitTest
```

Expected: `BUILD SUCCESSFUL` with no failed JVM tests.

- [ ] **Step 3: Build the Android debug release candidate**

Run:

```bash
cd android
./gradlew assembleDebug assembleDebugAndroidTest
```

Expected: both app and instrumented-test APKs build successfully.

- [ ] **Step 4: Execute the Room migration on an Android target**

Run the exact `connectedDebugAndroidTest` command from Task 3. Expected: the
version 4-to-5 migration test passes on the connected emulator or test device.

- [ ] **Step 5: Run the badge detector suite without editing badge code**

Run:

```bash
cd esp32
/Users/billh/.platformio/penv/bin/pio test -e test
```

Expected: the existing native detector suite passes, including DJI SSID/OUI,
vendor IE, and Remote ID coverage.

- [ ] **Step 6: Prove the badge tree is unchanged by this implementation**

Repeat:

```bash
git diff -- esp32 | shasum -a 256
git status --short esp32
```

Expected: hash and status exactly match Step 1. If they differ, stop and revert
only this task's accidental badge edits; never discard pre-existing user work.

- [ ] **Step 7: Inspect final Android-only diff and commit any verification fixes**

Run:

```bash
git diff --check
git status --short
git log --oneline -5
```

Expected: no whitespace errors, no uncommitted Task 1-4 Android files, and no
new badge changes. If a test-driven Android correction was required during
verification, stage only that correction and its regression test and commit:

```bash
git commit -m "android: harden Wi-Fi drone release checks"
```

# Live Aircraft, Follow Tuning, and BLE Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver smooth, truth-bounded Android aircraft motion, stable map follow, evidence-based BLE follower alerts, and matching Android/ESP behavioral BLE edge handling, then publish verified APK and firmware release assets.

**Architecture:** Raw repository objects remain authoritative. The map adds a presentation-only projection frame and retained overlays, while a map-specific circular heading filter stabilizes follow mode. BLE follower classification becomes a pure per-device evidence policy with GPS accuracy and bonded/category gates; behavioral BLE fixes preserve the existing shared detector contract and badge investigation path.

**Tech Stack:** Kotlin, coroutines/StateFlow, Jetpack Compose, osmdroid, JUnit 4, ESP-IDF C, PlatformIO, Unity, GitHub Actions.

## Global Constraints

- Aircraft extrapolation stops at 12 seconds and confidence reaches zero at 30 seconds.
- Map frames run at 250 ms; stable heading output runs no faster than 100 ms, uses a 3 degree deadband, and a 250 ms circular response.
- Predicted positions are presentation-only and must never enter repository history, Room, backend payloads, or detail state.
- A following alert requires 5 minutes, 6 sightings, GPS accuracy at most 50 meters, 3 clusters separated by 75 meters, 150 meters of clustered travel, and first/middle/final temporal coverage.
- Lingering requires 10 minutes, 10 sightings, at most 25 meters of movement, and strongest RSSI at least -70 dBm; it remains threat level 1 and neutral UI.
- Bonded, ignored, and non-mobile privacy categories must not enter follower classification.
- Pairing spam wins a simultaneous emission, but serial evidence remains pending and observable on the next qualifying packet.
- Public BLE address type alone is never trusted product identity.
- BLE investigation remains read-only: no target pairing, writes, subscriptions, credential attempts, or persistent connections.
- Preserve button-2 hold investigation and scanner restoration behavior.
- Do not add dependencies.

---

### Task 1: Pure Map Projection and Heading Contracts

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/map/MapTrackProjector.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/map/MapHeadingStabilizer.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/map/MapTrackProjectorTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/map/MapHeadingStabilizerTest.kt`

**Interfaces:**
- Consumes: `TrajectoryPredictor.predictAll(List<SkyObject>, Long)`.
- Produces: `MapTrack`, `MapTrackProjector.project`, `MapHeadingStabilizer.update`, and `MapHeadingStabilizer.reset` for Task 2.

- [ ] **Step 1: Write failing projection tests**

```kotlin
@Test fun `moving aircraft advances without mutating report`() {
    val report = aircraft(lastUpdated = Instant.ofEpochMilli(1_000), speedMps = 100f, heading = 90f)
    val frame = MapTrackProjector().project(listOf(report), 6_000).single()
    assertTrue(frame.position.longitude > report.position.longitude)
    assertEquals(report.position, frame.skyObject.position)
    assertTrue(frame.isExtrapolated)
}

@Test fun `stale aircraft freezes with zero confidence`() {
    val report = aircraft(lastUpdated = Instant.ofEpochMilli(1_000), speedMps = 100f, heading = 90f)
    val frame = MapTrackProjector().project(listOf(report), 32_000).single()
    assertEquals(0f, frame.confidence)
    assertFalse(frame.isExtrapolated)
}

private fun aircraft(
    lastUpdated: Instant,
    speedMps: Float,
    heading: Float,
) = Aircraft(
    id = "abc123",
    position = Position(
        latitude = 37.0,
        longitude = -122.0,
        altitudeMeters = 1_000.0,
        heading = heading,
        speedMps = speedMps,
    ),
    category = ObjectCategory.GENERAL_AVIATION,
    firstSeen = lastUpdated,
    lastUpdated = lastUpdated,
    icaoHex = "abc123",
)
```

- [ ] **Step 2: Write failing heading tests**

```kotlin
@Test fun `first heading emits immediately and jitter is suppressed`() {
    val filter = MapHeadingStabilizer()
    assertEquals(10f, filter.update(10f, 0), 0.001f)
    assertNull(filter.update(11f, 100))
    assertNull(filter.update(12.9f, 200))
}

@Test fun `heading crosses north by the shortest circular path`() {
    val filter = MapHeadingStabilizer()
    filter.update(359f, 0)
    val next = requireNotNull(filter.update(5f, 250))
    assertTrue(next > 359f || next < 5f)
}
```

- [ ] **Step 3: Run focused tests to verify RED**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.map.MapTrackProjectorTest' --tests 'com.friendorfoe.presentation.map.MapHeadingStabilizerTest'
```

Expected: compilation fails because the new production contracts do not exist.

- [ ] **Step 4: Implement the pure contracts**

```kotlin
data class MapTrack(
    val skyObject: SkyObject,
    val position: Position,
    val ageSeconds: Float,
    val confidence: Float,
    val isExtrapolated: Boolean,
    val headingDegrees: Float?,
)

class MapTrackProjector(
    private val predictor: TrajectoryPredictor = TrajectoryPredictor(),
) {
    fun project(objects: List<SkyObject>, nowMs: Long): List<MapTrack> =
        predictor.predictAll(objects, nowMs).map { predicted ->
            MapTrack(
                skyObject = predicted.skyObject,
                position = predicted.predictedPosition,
                ageSeconds = predicted.ageSeconds,
                confidence = predicted.confidence,
                isExtrapolated = predicted.isExtrapolated,
                headingDegrees = predicted.trackHeadingDegrees,
            )
        }
}
```

`MapHeadingStabilizer.update` must normalize inputs, enforce the 100 ms interval,
apply shortest-path circular interpolation using
`alpha = 1 - exp(-elapsedMs / 250.0)`, and return `null` while the residual
circular delta is below 3 degrees. `reset` clears time and heading state.

- [ ] **Step 5: Run focused tests to verify GREEN**

Run the Step 3 command. Expected: both suites pass.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/map/MapTrackProjector.kt android/app/src/main/java/com/friendorfoe/presentation/map/MapHeadingStabilizer.kt android/app/src/test/java/com/friendorfoe/presentation/map/MapTrackProjectorTest.kt android/app/src/test/java/com/friendorfoe/presentation/map/MapHeadingStabilizerTest.kt
git commit -m "android: add live map track projection"
```

---

### Task 2: Live Map Integration and Retained Overlays

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/map/MapOverlayController.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/map/MapOverlayPresentationTest.kt`

**Interfaces:**
- Consumes: `MapTrackProjector`, `MapHeadingStabilizer`, repository sky objects, and raw sensor orientation.
- Produces: `StateFlow<List<MapTrack>> mapTracks`, `StateFlow<Float> stabilizedMapHeading`, and retained osmdroid overlays.

- [ ] **Step 1: Write failing presentation policy tests**

```kotlin
@Test fun `fresh coasting and stale alpha remain distinguishable`() {
    assertEquals(1f, mapTrackAlpha(1f), 0.001f)
    assertEquals(0.6f, mapTrackAlpha(0.5f), 0.001f)
    assertEquals(0.25f, mapTrackAlpha(0f), 0.001f)
}
```

- [ ] **Step 2: Run the focused test to verify RED**

Run:

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.map.MapOverlayPresentationTest'
```

Expected: compilation fails for `mapTrackAlpha` and test fixture contracts.

- [ ] **Step 3: Add subscribed frame and heading flows**

`MapViewModel` must create a 250 ms monotonic frame flow and combine it with
`skyObjectRepository.skyObjects`. It must expose raw `skyObjects` for existing
selection/filter behavior and expose projected `mapTracks` for rendering.
`stabilizedMapHeading` must consume raw heading only while follow mode is on,
reset when disabled, and use `SharingStarted.WhileSubscribed(5_000)`.

```kotlin
val mapTracks: StateFlow<List<MapTrack>> = combine(
    mapFrameClock,
    skyObjects,
) { nowMs, objects -> mapTrackProjector.project(objects, nowMs) }
    .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList())
```

- [ ] **Step 4: Implement retained overlay ownership**

`MapOverlayController` owns maps keyed by raw object ID for aircraft and by the
existing stable identifiers for sensor/remote markers. Its `render` method
must update marker fields in place, remove departed keys, retain the
`MapEventsOverlay`, and invalidate once per frame. Use projected position and
heading for aircraft, `mapTrackAlpha` for marker alpha, and raw object ID for
click selection. Distance rings are recreated only when user position or zoom
bucket changes.

- [ ] **Step 5: Wire `MapViewScreen`**

Collect `mapTracks` and `stabilizedMapHeading`; remove the full
`map.overlays.clear()` rebuild path. Apply the stabilized heading to map
orientation, FOV cone, user marker, and follow icon. Keep remote search,
sensor-drone overlays, pan timeout, detail selection, and filter behavior.

- [ ] **Step 6: Run focused and map-adjacent tests**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.map.*' --tests 'com.friendorfoe.sensor.TrajectoryPredictorTest'
```

Expected: all selected suites pass.

- [ ] **Step 7: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/map android/app/src/test/java/com/friendorfoe/presentation/map
git commit -m "android: render smooth stable map tracks"
```

---

### Task 3: Evidence-Based BLE Follower Core

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/detection/BleTracker.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/BleTrackerTest.kt`

**Interfaces:**
- Consumes: sightings with bonded state, category, GPS accuracy, location, RSSI, and monotonic observation order.
- Produces: `FollowerEvidence` attached to `StalkerAlert` and deterministic following/lingering decisions.

- [ ] **Step 1: Add failing negative and positive tests**

```kotlin
@Test fun `old two minute fifty meter profile no longer alerts`() {
    val tracker = BleTracker()
    val start = Instant.parse("2026-07-16T12:00:00Z")
    listOf(0L to 37.0000, 60L to 37.0004, 121L to 37.0008).forEach { (seconds, lat) ->
        record(tracker, start.plusSeconds(seconds), lat, accuracy = 5f)
    }
    assertTrue(tracker.checkForFollowersAt(start.plusSeconds(122)).isEmpty())
}

@Test fun `gps jitter and poor accuracy do not alert`() {
    val tracker = BleTracker()
    val start = Instant.parse("2026-07-16T12:00:00Z")
    repeat(6) { index ->
        record(tracker, start.plusSeconds(index * 61L), 37.0 + index * 0.0008, accuracy = 80f)
    }
    assertTrue(tracker.checkForFollowersAt(start.plusSeconds(306)).isEmpty())
}

@Test fun `bonded device never alerts`() {
    val tracker = BleTracker()
    val start = Instant.parse("2026-07-16T12:00:00Z")
    repeat(6) { index ->
        record(tracker, start.plusSeconds(index * 61L), 37.0 + index * 0.0008, bonded = true)
    }
    assertTrue(tracker.checkForFollowersAt(start.plusSeconds(306)).isEmpty())
}

@Test fun `six sightings across three clusters and five minutes alert`() {
    val tracker = BleTracker()
    val start = Instant.parse("2026-07-16T12:00:00Z")
    listOf(37.0000, 37.0000, 37.0008, 37.0008, 37.0017, 37.0017)
        .forEachIndexed { index, lat -> record(tracker, start.plusSeconds(index * 61L), lat) }
    val alert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()
    assertEquals("following", alert.reason)
    assertEquals(2, alert.threatLevel)
    assertEquals(3, alert.evidence.clusterCount)
    assertTrue(alert.evidence.movementMeters >= 150.0)
}

@Test fun `lingering remains low after ten minutes`() {
    val tracker = BleTracker()
    val start = Instant.parse("2026-07-16T12:00:00Z")
    repeat(10) { index -> record(tracker, start.plusSeconds(index * 67L), 37.0, rssi = -55) }
    val alert = tracker.checkForFollowersAt(start.plusSeconds(603)).single()
    assertEquals("lingering", alert.reason)
    assertEquals(1, alert.threatLevel)
}

private fun record(
    tracker: BleTracker,
    timestamp: Instant,
    latitude: Double,
    accuracy: Float = 5f,
    bonded: Boolean = false,
    rssi: Int = -60,
) = tracker.recordSightingAt(
    mac = "AA:BB:CC:00:00:01",
    rssi = rssi,
    deviceName = "Test Tag",
    deviceType = "BLE Tracker",
    manufacturer = "Generic",
    hasCamera = false,
    category = PrivacyCategory.BLE_TRACKER,
    isBonded = bonded,
    userLat = latitude,
    userLon = -122.0,
    locationAccuracyMeters = accuracy,
    compassBearing = 0f,
    timestamp = timestamp,
)
```

- [ ] **Step 2: Run `BleTrackerTest` to verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleTrackerTest'
```

Expected: old permissive behavior fails negatives and new evidence fields do not compile.

- [ ] **Step 3: Implement evidence structures and policy**

Add `locationAccuracyMeters`, `isBonded`, and `category` to tracked input state.
Build sequential clusters from qualifying sightings, starting a new cluster
when distance from the current cluster anchor is at least 75 meters. Sum
distances between cluster anchors. Divide the device's own observation span
into thirds and require at least one qualifying sighting in each third.

```kotlin
data class FollowerEvidence(
    val durationMs: Long,
    val qualifyingSightings: Int,
    val clusterCount: Int,
    val movementMeters: Double,
    val temporalBands: Set<Int>,
    val strongestRssi: Int,
)
```

Reset `isFollowing` and `isStalker` before each evaluation. Following requires
all global constraints and starts at level 2. Camera level 3 requires 10
minutes and `peakRssi >= -70`. Lingering follows its separate global contract.

- [ ] **Step 4: Run `BleTrackerTest` to verify GREEN**

Run the Step 2 command. Expected: every follower and direction test passes.

- [ ] **Step 5: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/detection/BleTracker.kt android/app/src/test/java/com/friendorfoe/detection/BleTrackerTest.kt
git commit -m "android: require sustained BLE follower evidence"
```

---

### Task 4: Follower Input Plumbing and Calm Alert UI

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicy.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicyTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/repository/FollowerCandidatePolicyTest.kt`

**Interfaces:**
- Consumes: `Location.accuracy`, `GlassesDetection.isBonded`, category, ignored MAC/entity preferences, and Task 3's tracker API.
- Produces: only eligible sightings and severity-correct privacy presentation.

- [ ] **Step 1: Write failing candidate and alert tests**

```kotlin
@Test fun `bonded and stationary categories are excluded`() {
    assertFalse(detection(PrivacyCategory.BLE_TRACKER, bonded = true).isFollowerCandidate(emptySet()))
    assertFalse(detection(PrivacyCategory.VENUE_BEACON).isFollowerCandidate(emptySet()))
}

@Test fun `unknown findmy and mobile camera candidates are included`() {
    assertTrue(detection(PrivacyCategory.FINDMY).isFollowerCandidate(emptySet()))
    assertTrue(detection(PrivacyCategory.SMART_GLASSES).isFollowerCandidate(emptySet()))
}

@Test fun `lingering candidate does not notify`() {
    val candidate = PrivacyAlertPolicy.stalker("mac", "Tracker", "lingering", 1)
    assertFalse(PrivacyAlertPolicy().shouldNotify(candidate, emptySet()))
}

private fun detection(
    category: PrivacyCategory,
    bonded: Boolean = false,
) = GlassesDetection(
    mac = "AA:BB:CC:DD:EE:FF",
    deviceName = "Test device",
    deviceType = category.label,
    manufacturer = "Test",
    hasCamera = category == PrivacyCategory.SMART_GLASSES,
    rssi = -55,
    confidence = 0.9f,
    matchReason = "test",
    firstSeen = Instant.EPOCH,
    lastSeen = Instant.EPOCH,
    category = category,
    isBonded = bonded,
)
```

- [ ] **Step 2: Run focused tests to verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.repository.FollowerCandidatePolicyTest' --tests 'com.friendorfoe.presentation.privacy.PrivacyAlertPolicyTest'
```

- [ ] **Step 3: Plumb GPS accuracy and eligibility**

Add optional accuracy to `ensureStarted`/`updatePosition`; all Android
`LocationListener` call sites pass `location.accuracy` when available and
`Float.POSITIVE_INFINITY` otherwise. `SkyObjectRepository.collectGlasses`
must call `isFollowerCandidate(ignoredKeys)` before recording and pass bonded,
category, and accuracy to Task 3. Stopping or restarting detection clears
tracker state.

- [ ] **Step 4: Render severity-correct status**

In `PrivacyScreen`, use `Follower alert` with `FofTone.Danger` only for
following alerts at level 2 or 3. Render lingering as `Nearby device` with
`FofTone.Neutral`. Preserve notification filtering and cooldown.

- [ ] **Step 5: Run focused tests and full follower suite**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleTrackerTest' --tests 'com.friendorfoe.data.repository.FollowerCandidatePolicyTest' --tests 'com.friendorfoe.presentation.privacy.PrivacyAlertPolicyTest'
```

Expected: all selected suites pass.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt android/app/src/main/java/com/friendorfoe/presentation android/app/src/test/java/com/friendorfoe/data/repository/FollowerCandidatePolicyTest.kt android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicyTest.kt
git commit -m "android: calm follower alerts with location evidence"
```

---

### Task 5: Android Behavioral BLE Hardening

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/detection/BleThreatAnalyzer.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/BleThreatAnalyzerTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/GlassesDetectorBehavioralTest.kt`

**Interfaces:**
- Consumes: existing pairing-spam and serial-skimmer observations.
- Produces: prioritized collision behavior, measured serial RSSI, coherent trust suppression, and stable ignore handling.

- [ ] **Step 1: Write failing regression tests**

```kotlin
@Test fun `simultaneous prompt and serial remain independently observable`() {
    val analyzer = BleThreatAnalyzer()
    val burst = promptBurst()
    val collisionMac = burst.last().mac
    analyzer.observe(serial(0).copy(mac = collisionMac))
    analyzer.observe(serial(2_500).copy(mac = collisionMac))
    burst.dropLast(1).forEach(analyzer::observe)
    val collisionSignals = analyzer.observe(
        burst.last().copy(
            serviceUuids16 = setOf(0xFFE0),
            localName = "BT",
            connectable = true,
        )
    )
    assertTrue(collisionSignals.single() is BleThreatSignal.PairingSpam)
    val nextSerialPacket = serial(7_200).copy(mac = collisionMac)
    assertTrue(analyzer.observe(nextSerialPacket).single() is BleThreatSignal.SerialSkimmer)
}

@Test fun `serial detection uses strongest observed RSSI`() {
    val detection = GlassesDetector.behavioralDetection(serialSignal(strongestRssi = -61), Instant.EPOCH)
    assertEquals(-61, detection.rssi)
}

@Test fun `stable ignored behavioral key remains suppressed`() {
    val detection = GlassesDetector.behavioralDetection(pairingSpamSignal(), Instant.EPOCH)
    assertTrue(behavioralDetectionIsIgnored(detection, setOf("ble:pairing-spam")))
}

private fun pairingSpamSignal() = BleThreatSignal.PairingSpam(
    entityKey = "ble:pairing-spam",
    families = setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR),
    uniqueMacs = 12,
    observationCount = 24,
    strongestRssi = -48,
    rssiSpan = 4,
    windowMs = 8_000,
)
```

- [ ] **Step 2: Run focused tests to verify RED**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleThreatAnalyzerTest' --tests 'com.friendorfoe.detection.GlassesDetectorBehavioralTest'
```

- [ ] **Step 3: Implement collision and RSSI fixes**

Add `strongestRssi` to `BleThreatSignal.SerialSkimmer`. `observe` must evaluate
prompt first and call serial observation with `consumeSignal = prompt == null`;
when both mature, return only pairing spam and leave serial unalerted for the
next packet. Map serial detection RSSI from the signal.

- [ ] **Step 4: Implement trust and stable-ignore helpers**

Create a pure `isTrustedSerialProductIdentity` helper. It returns true for a
bonded device or a recognized non-serial static signature at the existing
minimum confidence. It returns false for serial UUID-only reasons, generic
UART identities, and unknown products. Check both physical MAC and behavioral
entity/fingerprint key before storing a behavioral detection.

- [ ] **Step 5: Run focused and all detection tests**

```bash
cd android
./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.*'
```

Expected: all detection suites pass.

- [ ] **Step 6: Commit**

```bash
git add android/app/src/main/java/com/friendorfoe/detection/BleThreatAnalyzer.kt android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt android/app/src/test/java/com/friendorfoe/detection
git commit -m "android: close behavioral BLE parity gaps"
```

---

### Task 6: ESP Trusted-Identity Parity and Badge Regression Gate

**Files:**
- Modify: `esp32/scanner/main/detection/ble_fingerprint.h`
- Modify: `esp32/scanner/main/detection/ble_fingerprint.c`
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/test/test_detection_policy.c`
- Modify: `esp32/test/test_ble_threat_detector.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `ble_fingerprint_t` static classification and existing `ble_threat_observation_t`.
- Produces: `ble_fingerprint_has_trusted_product_identity` and Unity evidence matching Android semantics.

- [ ] **Step 1: Write failing trusted-identity tests**

```c
void test_ble_fingerprint_known_product_is_trusted_serial_identity(void)
{
    ble_fingerprint_t fp = {.device_type = BLE_DEV_META_GLASSES};
    strcpy(fp.class_reason, "name:Ray-Ban Meta");
    TEST_ASSERT_TRUE(ble_fingerprint_has_trusted_product_identity(&fp));
}

void test_ble_fingerprint_unknown_and_serial_candidates_are_not_trusted(void)
{
    ble_fingerprint_t unknown = {.device_type = BLE_DEV_UNKNOWN};
    ble_fingerprint_t serial = {.device_type = BLE_DEV_CARD_SKIMMER};
    TEST_ASSERT_FALSE(ble_fingerprint_has_trusted_product_identity(&unknown));
    TEST_ASSERT_FALSE(ble_fingerprint_has_trusted_product_identity(&serial));
}
```

Register both tests in `test_runner.c`. Retain and strengthen the existing
simultaneous prompt/serial test to assert pairing first and serial next.

- [ ] **Step 2: Run native tests to verify RED**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
```

Expected: compile failure for the new trusted-identity helper.

- [ ] **Step 3: Implement trusted product identity**

`ble_fingerprint_has_trusted_product_identity` returns false for null,
`BLE_DEV_UNKNOWN`, `BLE_DEV_CARD_SKIMMER`, `BLE_DEV_SERIAL_SKIMMER`, empty
`class_reason`, and reasons based only on `0xFFE0`/`0xFFF0`. It returns true for
other coherent recognized classifications. `ble_remote_id.c` sets
`observation.trusted_identity` from this helper; address type alone is removed.

- [ ] **Step 4: Run native tests to verify GREEN**

Run the Step 2 command. Expected: all Unity tests pass.

- [ ] **Step 5: Build scanner and badge targets**

```bash
cd esp32/scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: all four environments build successfully and button-investigation
tests remain green in the native suite.

- [ ] **Step 6: Commit**

```bash
git add esp32/scanner/main/detection/ble_fingerprint.h esp32/scanner/main/detection/ble_fingerprint.c esp32/scanner/main/detection/ble_remote_id.c esp32/test
git commit -m "esp32: align behavioral BLE trust evidence"
```

---

### Task 7: Full Verification, Versioning, and Release

**Files:**
- Modify: `android/app/build.gradle.kts`
- Modify: release/version files selected by existing `FOF_VERSION_PROD` and `FOF_VERSION_BADGE` conventions
- Modify: `esp32/CHANGELOG.md`
- Create: `docs/live-aircraft-follow-tuning-verification.md`
- Modify only if required by existing release contract: backend firmware catalog and `esp32/web-flasher` manifests

**Interfaces:**
- Consumes: Tasks 1-6 and existing tag-driven release workflows.
- Produces: a clean release commit, pushed branch/main/tag, signed APK, production and badge firmware assets, and requirement-by-requirement evidence.

- [ ] **Step 1: Run complete Android verification**

```bash
cd android
./gradlew testDebugUnitTest --rerun-tasks
./gradlew assembleDebug --rerun-tasks
```

Expected: both commands exit 0.

- [ ] **Step 2: Run complete ESP verification**

```bash
cd esp32
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio test -e test
cd scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: native tests and all builds exit 0.

- [ ] **Step 3: Inspect physical targets without overstating evidence**

```bash
adb devices
system_profiler SPUSBDataType
```

If Android and badge hardware are present, install the debug APK and verify
aircraft coast/freeze, compass follow, follower negatives, USB/BLE badge status,
and button-2 investigation. Otherwise record each item as `NOT RUN: hardware unavailable`.

- [ ] **Step 4: Bump release versions and manifests**

Use the refreshed, currently unused release tag `v0.64.68-live-follow`.
Set Android `versionCode = 110` and `versionName = "0.64.68-live-follow"`.
Set production firmware/backend expectations to `0.64.68-live-follow` and badge
firmware to `0.64.68-badge-live-follow`. Update:

- `esp32/shared/version.h`
- `backend/app/main.py`
- `backend/app/routers/detections.py`
- `backend/tests/test_scanner_firmware_fleet.py`
- all five `esp32/web-flasher/manifest-*.json` files
- `esp32/web-flasher/index.html`
- `esp32/CHANGELOG.md`

Do not move or reuse the pushed tag.

- [ ] **Step 5: Write verification evidence**

`docs/live-aircraft-follow-tuning-verification.md` must map every design goal to
its test/build result, list exact commands and exit codes, include artifact
paths and SHA-256 digests, and state all hardware limitations.

- [ ] **Step 6: Run final hygiene and secret checks**

```bash
git diff --check origin/main...HEAD
rg -n -i '(api[_-]?key|secret|password|token)\s*[:=]\s*["'"'][^"'"']+["'"']' --glob '!*.lock' --glob '!docs/**' .
git status --short --branch
```

Review every match; expected staged content contains no credentials or generated build output.

- [ ] **Step 7: Commit and review the complete branch**

```bash
git add android/app/build.gradle.kts backend/app/main.py backend/app/routers/detections.py backend/tests/test_scanner_firmware_fleet.py esp32/shared/version.h esp32/CHANGELOG.md esp32/web-flasher docs/live-aircraft-follow-tuning-verification.md
git commit -m "v0.64.68-live-follow: release live aircraft and follow tuning"
```

Run a whole-branch code review and fix every Critical or Important finding.

- [ ] **Step 8: Push and publish**

```bash
git push origin codex/live-aircraft-follow-tuning
git push origin HEAD:main
git tag v0.64.68-live-follow
git push origin v0.64.68-live-follow
```

Wait for Android and firmware workflows. Verify the GitHub release contains the
signed APK plus production scanner/uplink and badge scanner/uplink assets.
Download the APK and verify version metadata, SHA-256, and `apksigner verify`.

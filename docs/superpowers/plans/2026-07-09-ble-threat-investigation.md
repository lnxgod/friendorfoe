# BLE Threat Detection and Investigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify behavioral BLE pairing-spam detection, combined-evidence serial-skimmer detection, and explicit read-only device investigation through Android or the FoF badge, including button-2 hold investigation on the selected badge alert.

**Architecture:** Kotlin and C detector cores implement one evidence contract with mirrored fixtures. Android uses a testable coordinator over a direct `BluetoothGatt` inspector or the existing badge command transports; badge scanner firmware owns on-demand passive capture/GATT work, while badge uplink owns routing, result assembly, USB/BLE exposure, and LCD interaction.

**Tech Stack:** Kotlin 17, Android Bluetooth LE/GATT, coroutines/StateFlow, Jetpack Compose, ESP-IDF/NimBLE C, FreeRTOS, cJSON, newline-delimited UART JSON, PlatformIO native Unity tests.

## Global Constraints

- Android remains `minSdk = 26`, `compileSdk = 35`, and `targetSdk = 34`; add no new runtime dependency.
- Active investigation is read-only toward the target: no target writes, notification subscriptions, credential attempts, or automatic pairing.
- Pairing-spam defaults are an 8-second window, 12 unique addresses, 24 deduplicated observations, 75 percent new-address churn, at least 3 observations/second, RSSI span at most 20 dB, RSSI IQR at most 12 dB, 60-second alert cooldown, and 20-second clear delay.
- Serial-skimmer classification requires `0xFFE0` or `0xFFF0`, a sparse advertised service profile, persistence across at least 3 observations and 5 seconds, plus at least 2 supporting signals; UUID-only sightings never alert.
- A coherent trusted product identity or Public Key Open Credential identity suppresses the serial-skimmer heuristic.
- Only one investigation may run per radio; total investigation deadline is 12 seconds.
- Results cap services at 16, characteristics at 32, read values at 8, and each read value at 64 bytes.
- Scanner-to-uplink result messages are chunked into newline-delimited JSON records smaller than `UART_JSON_MAX_SIZE` (1024 bytes).
- Badge button 2 keeps single-press next, double-press details, and idle long-press phone pairing.
- Every badge investigation failure path disconnects and restores BLE scanning.
- Do not publish an APK, tag a release, flash hardware, or deploy firmware in this implementation unless separately requested.

---

### Task 1: Android Behavioral BLE Analyzer

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/detection/BleThreatAnalyzer.kt`
- Create: `android/app/src/test/java/com/friendorfoe/detection/BleThreatAnalyzerTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/BleAdvertisement.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/BlePacketParser.kt`

**Interfaces:**
- Consumes: `BleAdvertisement`, monotonic observation time, and whether an existing classifier established a trusted product identity.
- Produces: `BleThreatAnalyzer.observe(BleThreatObservation): List<BleThreatSignal>` and `reset()`.

- [ ] **Step 1: Add failing pairing-spam fixture tests**

Create deterministic helpers and tests with no Android framework objects:

```kotlin
private fun prompt(
    index: Int,
    atMs: Long,
    family: BlePromptFamily = BlePromptFamily.MICROSOFT_SWIFT_PAIR,
    rssi: Int = -48,
    structuralHash: Int = 0x1234,
) = BleThreatObservation(
    mac = "02:00:00:00:00:${index.toString(16).padStart(2, '0')}",
    observedAtMs = atMs,
    rssi = rssi,
    connectable = false,
    structuralHash = structuralHash,
    promptFamily = family,
    serviceUuids16 = emptySet(),
    localName = null,
    companyId = 0x0006,
    trustedIdentity = false,
)

@Test
fun `twelve rotating Swift Pair addresses in eight seconds emit one flood`() {
    val analyzer = BleThreatAnalyzer()
    val signals = (0 until 12).flatMap { index ->
        listOf(index * 2L, index * 2L + 1L).flatMap { packet ->
            analyzer.observe(prompt(index, packet * 300L))
        }
    }
    val flood = signals.filterIsInstance<BleThreatSignal.PairingSpam>().single()
    assertEquals(12, flood.uniqueMacs)
    assertEquals(24, flood.observationCount)
    assertEquals(setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR), flood.families)
}

@Test
fun `scan overlap duplicate does not increase observation count`() {
    val analyzer = BleThreatAnalyzer()
    analyzer.observe(prompt(1, 1_000))
    analyzer.observe(prompt(1, 1_100))
    val snapshot = analyzer.debugSnapshot()
    assertEquals(1, snapshot.deduplicatedObservations)
}
```

- [ ] **Step 2: Add negative crowd, cooldown, mixed-family, and decay tests**

Cover these exact cases:

```kotlin
@Test fun `varied crowd does not alert when RSSI spread exceeds twenty dB`()
@Test fun `stable addresses do not satisfy seventy five percent churn`()
@Test fun `isolated Fast Pair and generic Apple traffic do not alert`()
@Test fun `mixed Apple Fast Pair and Swift Pair burst alerts once`()
@Test fun `sustained flood respects sixty second cooldown`()
@Test fun `pairing spam clears after twenty quiet seconds`()
```

The generic Apple fixture must set `promptFamily = null`; a recognized company
ID by itself is not prompt evidence.

- [ ] **Step 3: Add failing serial-skimmer evidence tests**

```kotlin
private fun serial(
    atMs: Long,
    services: Set<Int> = setOf(0xFFE0),
    name: String? = "BT",
    rssi: Int = -62,
    connectable: Boolean = true,
    trusted: Boolean = false,
) = BleThreatObservation(
    mac = "C0:98:E5:00:00:01",
    observedAtMs = atMs,
    rssi = rssi,
    connectable = connectable,
    structuralHash = 0xFFE0,
    promptFamily = null,
    serviceUuids16 = services,
    localName = name,
    companyId = null,
    trustedIdentity = trusted,
)

@Test
fun `persistent close sparse FFE0 device emits possible serial skimmer`() {
    val analyzer = BleThreatAnalyzer()
    analyzer.observe(serial(0))
    analyzer.observe(serial(2_500))
    val signal = analyzer.observe(serial(5_100))
        .filterIsInstance<BleThreatSignal.SerialSkimmer>()
        .single()
    assertEquals(0xFFE0, signal.serialServiceUuid)
    assertTrue(signal.evidence.contains(BleSerialEvidence.PERSISTENT))
}

@Test fun `FFE0 alone never alerts`()
@Test fun `weak nonpersistent UART device never alerts`()
@Test fun `trusted product suppresses serial heuristic`()
@Test fun `multi service device suppresses sparse profile evidence`()
@Test fun `PKOC identity suppresses FFF0 heuristic`()
```

- [ ] **Step 4: Run the focused tests and verify red state**

Run from `android/`:

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleThreatAnalyzerTest
```

Expected: compilation fails because `BleThreatAnalyzer`, observation types, and
signals do not exist yet.

- [ ] **Step 5: Implement the pure bounded analyzer**

Use these public types and retain only observations needed by active windows:

```kotlin
enum class BlePromptFamily { APPLE_CONTINUITY, GOOGLE_FAST_PAIR, MICROSOFT_SWIFT_PAIR }
enum class BleSerialEvidence { SERIAL_UUID, SPARSE_PROFILE, GENERIC_NAME, PERSISTENT, CLOSE, CONNECTABLE, UNTRUSTED }

data class BleThreatObservation(
    val mac: String,
    val observedAtMs: Long,
    val rssi: Int,
    val connectable: Boolean,
    val structuralHash: Int,
    val promptFamily: BlePromptFamily?,
    val serviceUuids16: Set<Int>,
    val localName: String?,
    val companyId: Int?,
    val trustedIdentity: Boolean,
)

sealed interface BleThreatSignal {
    val entityKey: String
    data class PairingSpam(
        override val entityKey: String,
        val families: Set<BlePromptFamily>,
        val uniqueMacs: Int,
        val observationCount: Int,
        val strongestRssi: Int,
        val rssiSpan: Int,
        val windowMs: Long,
    ) : BleThreatSignal
    data class SerialSkimmer(
        override val entityKey: String,
        val targetMac: String,
        val serialServiceUuid: Int,
        val confidence: Float,
        val evidence: Set<BleSerialEvidence>,
    ) : BleThreatSignal
}

class BleThreatAnalyzer(
    private val config: Config = Config(),
) {
    data class Config(
        val windowMs: Long = 8_000,
        val minUniqueMacs: Int = 12,
        val minObservations: Int = 24,
        val minChurnRatio: Double = 0.75,
        val minRatePerSecond: Double = 3.0,
        val maxRssiSpan: Int = 20,
        val maxRssiIqr: Int = 12,
        val cooldownMs: Long = 60_000,
        val clearAfterMs: Long = 20_000,
    )
    fun observe(observation: BleThreatObservation): List<BleThreatSignal>
    fun reset()
    internal fun debugSnapshot(): DebugSnapshot
}
```

Use `ArrayDeque` windows, per-signature first-seen maps, a 250 ms dedupe key,
sorted RSSI quartiles, and explicit maximums of 256 prompt observations and 64
serial tracks. Evict oldest state before admitting more.

- [ ] **Step 6: Extend advertisement parsing with connectability and prompt family helpers**

Add `connectable: Boolean` to `BleAdvertisement` with a default of `false`, set
it from `ScanResult.isConnectable`, and add a pure mapper:

```kotlin
fun BleAdvertisement.promptFamily(): BlePromptFamily? = when {
    microsoft?.beaconType == 0x03 -> BlePromptFamily.MICROSOFT_SWIFT_PAIR
    0xFE2C in serviceUuids16 -> BlePromptFamily.GOOGLE_FAST_PAIR
    companyId == BleSignatures.CID_APPLE && apple?.subType in setOf(0x07, 0x0F, 0x10) ->
        BlePromptFamily.APPLE_CONTINUITY
    else -> null
}
```

Do not map an undecoded Apple or Microsoft packet into a prompt family.

- [ ] **Step 7: Run tests, then commit**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleThreatAnalyzerTest
git add app/src/main/java/com/friendorfoe/detection/BleThreatAnalyzer.kt app/src/main/java/com/friendorfoe/detection/BleAdvertisement.kt app/src/main/java/com/friendorfoe/detection/BlePacketParser.kt app/src/test/java/com/friendorfoe/detection/BleThreatAnalyzerTest.kt
git commit -m "android: add behavioral BLE threat analyzer"
```

Expected: focused tests pass and the commit contains only analyzer/parser work.

---

### Task 2: Android Detection Integration and Structured Investigation Targets

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/detection/BleInvestigationModels.kt`
- Create: `android/app/src/test/java/com/friendorfoe/detection/GlassesDetectorBehavioralTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/PrivacyCategoryMappingTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`

**Interfaces:**
- Consumes: `BleThreatSignal` from Task 1.
- Produces: structured `BleInvestigationTarget` on `GlassesDetection`, stable
  behavioral rows, and origin metadata used by routing in Task 10.

- [ ] **Step 1: Write failing behavioral-row tests**

Extract a package-visible conversion function so JVM tests avoid constructing
`ScanResult`:

```kotlin
@Test
fun `pairing flood becomes one attack tool with passive capture target`() {
    val signal = BleThreatSignal.PairingSpam(
        entityKey = "ble_anomaly:pairing_spam:1234",
        families = setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR),
        uniqueMacs = 12,
        observationCount = 24,
        strongestRssi = -44,
        rssiSpan = 8,
        windowMs = 8_000,
    )
    val detection = GlassesDetector.behavioralDetection(signal, now = Instant.EPOCH)
    assertEquals(PrivacyCategory.ATTACK_TOOL, detection.category)
    assertEquals("BLE Pairing Spam", detection.deviceType)
    assertEquals(BleInvestigationMode.PASSIVE_CAPTURE, detection.investigationTarget?.mode)
    assertEquals(signal.entityKey, detection.fingerprintKey)
}

@Test
fun `serial signal keeps target MAC for GATT investigation`() {
    val detection = GlassesDetector.behavioralDetection(serialSignal(), Instant.EPOCH)
    assertEquals("C0:98:E5:00:00:01", detection.investigationTarget?.mac)
    assertEquals(BleInvestigationMode.GATT, detection.investigationTarget?.mode)
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.GlassesDetectorBehavioralTest
```

Expected: failure because structured investigation target types and conversion
do not exist.

- [ ] **Step 3: Add exact investigation target and origin models**

```kotlin
enum class PrivacyDetectionOrigin { ANDROID, BADGE, BACKEND, WIFI }
enum class BleInvestigationMode { GATT, PASSIVE_CAPTURE }

data class BleInvestigationTarget(
    val mode: BleInvestigationMode,
    val mac: String?,
    val entityKey: String,
    val observedAtElapsedMs: Long,
    val origin: PrivacyDetectionOrigin,
)
```

Add these defaulted fields to `GlassesDetection` so existing constructors remain
source-compatible:

```kotlin
val origin: PrivacyDetectionOrigin = PrivacyDetectionOrigin.ANDROID,
val investigationTarget: BleInvestigationTarget? = null,
```

- [ ] **Step 4: Integrate the analyzer before the static confidence early return**

In `checkScanResult`, parse `BleAdvertisement` before `if (bestConf < 0.60f)` and
call the analyzer for every nonignored advertisement. Build `trustedIdentity`
only from a coherent non-UART static match. Prefer a behavioral alert over an
informational static result, but do not replace a higher-confidence explicit
camera, Meta, tracker, or attack-tool signature with a serial heuristic.

Use one conversion function:

```kotlin
internal fun behavioralDetection(
    signal: BleThreatSignal,
    now: Instant,
    observedAtElapsedMs: Long = android.os.SystemClock.elapsedRealtime(),
): GlassesDetection
```

Call `bleThreatAnalyzer.reset()` from `clearAllDetections`, `stopScanning`, and
`awaitClose` cleanup.

- [ ] **Step 5: Mark backend, badge, and WiFi origins explicitly**

In `PrivacyViewModel` conversion functions set:

```kotlin
origin = PrivacyDetectionOrigin.BACKEND
origin = PrivacyDetectionOrigin.BADGE
origin = PrivacyDetectionOrigin.WIFI
```

For badge BLE entities with a nonblank BSSID, create a `GATT` target. For badge
pairing-spam entities, create a `PASSIVE_CAPTURE` target with no MAC.

- [ ] **Step 6: Run regression tests and commit**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.GlassesDetectorBehavioralTest --tests com.friendorfoe.detection.PrivacyCategoryMappingTest --tests com.friendorfoe.detection.AppleContinuityDecoderTest --tests com.friendorfoe.detection.MicrosoftSwiftPairDecoderTest
git add app/src/main/java/com/friendorfoe/detection/BleInvestigationModels.kt app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt app/src/test/java/com/friendorfoe/detection/GlassesDetectorBehavioralTest.kt app/src/test/java/com/friendorfoe/detection/PrivacyCategoryMappingTest.kt
git commit -m "android: surface BLE behavioral alerts"
```

Expected: behavioral and existing decoder/category tests pass.

---

### Task 3: Firmware Behavioral Detector With Android-Parity Fixtures

**Files:**
- Create: `esp32/scanner/main/detection/ble_threat_detector.h`
- Create: `esp32/scanner/main/detection/ble_threat_detector.c`
- Create: `esp32/test/test_ble_threat_detector.c`
- Modify: `esp32/scanner/main/CMakeLists.txt`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: MAC, monotonic time, RSSI, connectability, address type,
  `ble_fingerprint_t`, and trusted-identity flag.
- Produces: `ble_threat_detector_observe(const ble_threat_observation_t *, ble_threat_signal_t *)` and a stable structured signal.

- [ ] **Step 1: Add mirrored failing Unity fixtures**

Define helpers that generate the same timestamps, RSSI values, service UUIDs,
and structural hashes as Task 1. Required tests:

```c
void test_ble_threat_swift_pair_rotating_flood_alerts_once(void);
void test_ble_threat_scan_duplicate_is_deduped(void);
void test_ble_threat_varied_crowd_does_not_alert(void);
void test_ble_threat_stable_addresses_do_not_alert(void);
void test_ble_threat_mixed_prompt_families_alert(void);
void test_ble_threat_cooldown_and_clear(void);
void test_ble_threat_persistent_sparse_ffe0_alerts(void);
void test_ble_threat_ffe0_only_does_not_alert(void);
void test_ble_threat_trusted_product_suppresses_serial_candidate(void);
void test_ble_threat_pkoc_fff0_is_suppressed(void);
```

Register every function in `test_runner.c` and add the source to
`build_src_filter`.

- [ ] **Step 2: Run native tests and verify red state**

Run from `esp32/`:

```bash
./.venv312/bin/pio test -e test
```

Expected: compile failure because `ble_threat_detector.h` is absent.

- [ ] **Step 3: Implement the bounded C contract**

Use exact public types:

```c
typedef enum {
    BLE_PROMPT_NONE = 0,
    BLE_PROMPT_APPLE = 1,
    BLE_PROMPT_FAST_PAIR = 2,
    BLE_PROMPT_SWIFT_PAIR = 4,
} ble_prompt_family_t;

typedef enum {
    BLE_THREAT_NONE = 0,
    BLE_THREAT_PAIRING_SPAM,
    BLE_THREAT_SERIAL_SKIMMER,
} ble_threat_kind_t;

typedef struct {
    uint8_t mac[6];
    int64_t observed_ms;
    int8_t rssi;
    bool connectable;
    uint8_t addr_type;
    uint32_t structural_hash;
    ble_prompt_family_t prompt_family;
    uint16_t service_uuids[4];
    uint8_t service_uuid_count;
    const char *local_name;
    uint16_t company_id;
    bool trusted_identity;
} ble_threat_observation_t;

typedef struct {
    ble_threat_kind_t kind;
    uint32_t entity_hash;
    uint8_t prompt_family_mask;
    uint16_t unique_macs;
    uint16_t observation_count;
    uint16_t serial_service_uuid;
    uint8_t evidence_mask;
    int8_t strongest_rssi;
    uint8_t rssi_span;
    float confidence;
} ble_threat_signal_t;

void ble_threat_detector_init(void);
bool ble_threat_detector_observe(const ble_threat_observation_t *observation,
                                 ble_threat_signal_t *signal_out);
void ble_threat_detector_reset(void);
```

Use fixed arrays for 256 prompt observations and 64 serial tracks. Never
allocate in the scan callback. Match Kotlin quartile indexing and dedupe bucket
exactly.

- [ ] **Step 4: Add compile sources and run parity tests**

Add `detection/ble_threat_detector.c` to scanner `SRCS` and native
`build_src_filter`, then run:

```bash
./.venv312/bin/pio test -e test
```

Expected: all native tests pass, including the new mirrored fixtures.

- [ ] **Step 5: Commit**

```bash
git add scanner/main/detection/ble_threat_detector.c scanner/main/detection/ble_threat_detector.h scanner/main/CMakeLists.txt platformio.ini test/test_ble_threat_detector.c test/test_runner.c
git commit -m "esp32: add behavioral BLE threat detector"
```

Run from `esp32/`; expected commit contains the pure detector and tests only.

---

### Task 4: Scanner Emission, UART Evidence, and Badge Threat Policy

**Files:**
- Modify: `esp32/scanner/main/detection/ble_fingerprint.h`
- Modify: `esp32/scanner/main/detection/ble_fingerprint.c`
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/shared/detection_types.h`
- Modify: `esp32/shared/uart_protocol.h`
- Modify: `esp32/scanner/main/comms/uart_tx.c`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/shared/badge_threat_policy.h`
- Modify: `esp32/shared/badge_threat_policy.c`
- Modify: `esp32/shared/badge_display_policy.h`
- Modify: `esp32/shared/badge_display_policy.c`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- Modify: `esp32/test/test_detection_policy.c`
- Modify: `esp32/test/test_badge_threat_policy.c`
- Modify: `esp32/test/test_badge_display_policy.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `ble_threat_signal_t` from Task 3.
- Produces: UART-preserved behavioral evidence, badge-visible `BLE Spam` and
  `Skimmer` entities, and Android badge category parsing.

- [ ] **Step 1: Write failing fingerprint integration and badge policy tests**

Required assertions:

```c
void test_badge_pairing_spam_is_visible_as_ble_attack(void);
void test_badge_pairing_spam_key_is_stable_across_rotated_macs(void);
void test_badge_serial_uuid_only_is_hidden(void);
void test_badge_combined_serial_skimmer_is_visible(void);
void test_badge_ble_attack_display_policy_defaults_to_both_lanes(void);
```

Build detections with structured fields, not label-only fixtures. The UUID-only
negative case must use `ble_threat_kind = BLE_THREAT_NONE` and `0xFFE0` service
data.

- [ ] **Step 2: Run native tests and confirm the new cases fail**

```bash
./.venv312/bin/pio test -e test
```

Expected: compile failure for missing detection fields and display class.

- [ ] **Step 3: Add structured BLE threat fields without changing old field meanings**

Append to `drone_detection_t`:

```c
#define BLE_THREAT_KIND_NONE            0
#define BLE_THREAT_KIND_PAIRING_SPAM    1
#define BLE_THREAT_KIND_SERIAL_SKIMMER  2

uint8_t  ble_threat_kind;
uint8_t  ble_prompt_family_mask;
uint16_t ble_unique_macs;
uint16_t ble_observation_count;
uint16_t ble_serial_service_uuid;
uint8_t  ble_threat_evidence_mask;
```

Add UART keys:

```c
#define JSON_KEY_BLE_THREAT_KIND       "ble_tk"
#define JSON_KEY_BLE_PROMPT_FAMILIES   "ble_pf"
#define JSON_KEY_BLE_UNIQUE_MACS       "ble_um"
#define JSON_KEY_BLE_OBSERVATIONS      "ble_oc"
#define JSON_KEY_BLE_SERIAL_UUID       "ble_su"
#define JSON_KEY_BLE_THREAT_EVIDENCE   "ble_ev"
```

Serialize in `uart_tx.c` and parse in both uplink UART parse paths. Keep zero as
absent/no-threat for compatibility.

- [ ] **Step 4: Integrate the detector in both legacy and extended BLE callbacks**

Create one helper in `ble_remote_id.c` and call it immediately after
`ble_fingerprint_compute` in `BLE_GAP_EVENT_DISC` and `BLE_GAP_EVENT_EXT_DISC`:

```c
static bool apply_behavioral_ble_threat(const uint8_t mac[6],
                                        int8_t rssi,
                                        uint8_t addr_type,
                                        uint8_t props,
                                        int64_t observed_ms,
                                        ble_fingerprint_t *fp,
                                        ble_threat_signal_t *signal_out);
```

When signaled, set a stable `fp->hash = signal.entity_hash`, set a dedicated
device type (`BLE_DEV_PAIRING_SPAM` or `BLE_DEV_SERIAL_SKIMMER`), set a concise
class reason, and copy structured fields into the emitted detection. Do not
classify `0xFFE0`/`0xFFF0` in the static fingerprint table.

- [ ] **Step 5: Add badge categories and display filtering**

Append `BADGE_THREAT_CATEGORY_BLE_SPAM` so existing enum values remain stable.
Classify `ble_threat_kind == BLE_THREAT_KIND_PAIRING_SPAM` as:

```c
event->cls = BADGE_THREAT_BLE;
event->category = BADGE_THREAT_CATEGORY_BLE_SPAM;
copy_label(event->label, "BLE Spam");
event->base_score = 72.0f;
event->evidence_quality = 8;
```

Map serial-skimmer signals to existing `BADGE_THREAT_CATEGORY_SKIM` with label
`Possible Skimmer`. Add `BADGE_DISPLAY_CLASS_BLE_ATTACK` at the end, increment
the class count, default it to enabled/both/present/92, and add Android policy
metadata/defaults for key `ble_attack`.

- [ ] **Step 6: Run native and Android parser regression tests**

```bash
./.venv312/bin/pio test -e test
cd ../android
./gradlew testDebugUnitTest --tests com.friendorfoe.data.badge.BadgeControlStatusParserTest --tests com.friendorfoe.data.badge.BadgeDisplayPolicyTest --tests com.friendorfoe.presentation.privacy.BadgePrivacyMapperTest
```

Expected: new threat policy tests and existing badge parser/mapping tests pass.

- [ ] **Step 7: Commit**

Run from the repository root, stage only the listed firmware policy/protocol
files and Android badge parser, then commit:

```bash
git add esp32/scanner/main/detection/ble_fingerprint.c esp32/scanner/main/detection/ble_fingerprint.h esp32/scanner/main/detection/ble_remote_id.c esp32/shared/detection_types.h esp32/shared/uart_protocol.h esp32/scanner/main/comms/uart_tx.c esp32/uplink/main/comms/uart_rx.c esp32/shared/badge_threat_policy.c esp32/shared/badge_threat_policy.h esp32/shared/badge_display_policy.c esp32/shared/badge_display_policy.h esp32/test/test_detection_policy.c esp32/test/test_badge_threat_policy.c esp32/test/test_badge_display_policy.c esp32/test/test_runner.c android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt
git commit -m "badge: surface behavioral BLE threats"
```

---

### Task 5: Normalized Investigation Contract and Badge Chunk Protocol

**Files:**
- Expand: `android/app/src/main/java/com/friendorfoe/detection/BleInvestigationModels.kt`
- Create: `android/app/src/test/java/com/friendorfoe/detection/BleInvestigationModelsTest.kt`
- Create: `esp32/shared/ble_investigation_types.h`
- Create: `esp32/shared/ble_investigation_protocol.h`
- Create: `esp32/shared/ble_investigation_protocol.c`
- Create: `esp32/test/test_ble_investigation_protocol.c`
- Modify: `esp32/scanner/main/CMakeLists.txt`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Produces identical state names, limits, property masks, and chunk message
  types for Android, scanner, and uplink tasks.

- [ ] **Step 1: Add failing Kotlin JSON model tests**

```kotlin
@Test
fun `badge investigation chunks assemble in sequence`() {
    val assembler = BleInvestigationChunkAssembler("req-1")
    assembler.accept(BleInvestigationChunk.Begin("req-1", BleInvestigationMode.GATT, "AA:BB:CC:DD:EE:FF"))
    assembler.accept(BleInvestigationChunk.Service("req-1", 0, "FFE0"))
    assembler.accept(BleInvestigationChunk.Characteristic("req-1", 0, "FFE0", "FFE1", setOf("read", "write")))
    val result = assembler.accept(BleInvestigationChunk.End("req-1", "complete", "UART service found"))
    assertEquals(listOf("FFE0"), result!!.services)
    assertEquals("FFE1", result.characteristics.single().uuid)
}

@Test fun `out of order request id is rejected without corrupting active result`()
@Test fun `service characteristic and read limits set truncation flag`()
```

- [ ] **Step 2: Add failing C chunk protocol tests**

Required Unity cases:

```c
void test_ble_investigation_result_defaults_to_idle(void);
void test_ble_investigation_protocol_emits_bounded_begin_service_char_read_end(void);
void test_ble_investigation_protocol_caps_service_and_characteristic_counts(void);
void test_ble_investigation_protocol_rejects_mismatched_request_id(void);
```

- [ ] **Step 3: Run both test sets and verify red state**

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationModelsTest
cd ../esp32
./.venv312/bin/pio test -e test
```

Expected: both fail on missing contract types.

- [ ] **Step 4: Implement exact normalized models**

Kotlin contract:

```kotlin
enum class BleInvestigationRoute { AUTO, PHONE, BADGE }
enum class BleInvestigationState { IDLE, QUEUED, SCANNING, CONNECTING, DISCOVERING, READING, COMPLETE, FAILED, CANCELLED }

data class BleInvestigationRequest(
    val requestId: String,
    val target: BleInvestigationTarget,
    val route: BleInvestigationRoute,
    val timeoutMs: Long = 12_000,
)

data class BleGattCharacteristicInfo(
    val serviceUuid: String,
    val uuid: String,
    val properties: Set<String>,
)

data class BleInvestigationResult(
    val requestId: String,
    val transport: String,
    val mode: BleInvestigationMode,
    val targetMac: String?,
    val state: BleInvestigationState,
    val connectable: Boolean?,
    val services: List<String>,
    val characteristics: List<BleGattCharacteristicInfo>,
    val reads: Map<String, String>,
    val bonded: Boolean,
    val encrypted: Boolean,
    val authenticationRequired: Boolean,
    val summary: String,
    val error: String?,
    val truncated: Boolean,
)

sealed interface BleInvestigationChunk {
    val requestId: String
    data class Begin(
        override val requestId: String,
        val mode: BleInvestigationMode,
        val targetMac: String?,
    ) : BleInvestigationChunk
    data class Progress(
        override val requestId: String,
        val state: BleInvestigationState,
    ) : BleInvestigationChunk
    data class Service(
        override val requestId: String,
        val index: Int,
        val uuid: String,
    ) : BleInvestigationChunk
    data class Characteristic(
        override val requestId: String,
        val index: Int,
        val serviceUuid: String,
        val uuid: String,
        val properties: Set<String>,
    ) : BleInvestigationChunk
    data class Read(
        override val requestId: String,
        val index: Int,
        val uuid: String,
        val valueHex: String,
    ) : BleInvestigationChunk
    data class End(
        override val requestId: String,
        val state: String,
        val summary: String,
        val error: String? = null,
        val authenticationRequired: Boolean = false,
        val truncated: Boolean = false,
    ) : BleInvestigationChunk
}

class BleInvestigationChunkAssembler(private val requestId: String) {
    fun accept(chunk: BleInvestigationChunk): BleInvestigationResult?
}
```

C contract uses these exact bounded types:

```c
#define BLE_INV_REQUEST_ID_LEN 33
#define BLE_INV_UUID_LEN 37
#define BLE_INV_SUMMARY_LEN 128
#define BLE_INV_ERROR_LEN 64
#define BLE_INV_MAX_SERVICES 16
#define BLE_INV_MAX_CHARS 32
#define BLE_INV_MAX_READS 8
#define BLE_INV_READ_HEX_LEN 129

typedef enum {
    BLE_INV_MODE_GATT = 0,
    BLE_INV_MODE_PASSIVE_CAPTURE,
} ble_investigation_mode_t;

typedef enum {
    BLE_INV_IDLE = 0,
    BLE_INV_QUEUED,
    BLE_INV_SCANNING,
    BLE_INV_CONNECTING,
    BLE_INV_DISCOVERING,
    BLE_INV_READING,
    BLE_INV_COMPLETE,
    BLE_INV_FAILED,
    BLE_INV_CANCELLED,
} ble_investigation_state_t;

typedef struct {
    char request_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_mode_t mode;
    char target_mac[18];
    uint32_t timeout_ms;
} ble_investigation_request_t;

typedef struct {
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
} ble_investigation_characteristic_t;

typedef struct {
    char uuid[BLE_INV_UUID_LEN];
    char value_hex[BLE_INV_READ_HEX_LEN];
} ble_investigation_read_t;

typedef struct {
    char request_id[BLE_INV_REQUEST_ID_LEN];
    ble_investigation_mode_t mode;
    ble_investigation_state_t state;
    char target_mac[18];
    bool connectable;
    bool bonded;
    bool encrypted;
    bool authentication_required;
    bool truncated;
    char services[BLE_INV_MAX_SERVICES][BLE_INV_UUID_LEN];
    uint8_t service_count;
    ble_investigation_characteristic_t characteristics[BLE_INV_MAX_CHARS];
    uint8_t characteristic_count;
    ble_investigation_read_t reads[BLE_INV_MAX_READS];
    uint8_t read_count;
    char summary[BLE_INV_SUMMARY_LEN];
    char error[BLE_INV_ERROR_LEN];
} ble_investigation_result_t;

typedef enum {
    BLE_INV_CHUNK_BEGIN = 0,
    BLE_INV_CHUNK_PROGRESS,
    BLE_INV_CHUNK_SERVICE,
    BLE_INV_CHUNK_CHARACTERISTIC,
    BLE_INV_CHUNK_READ,
    BLE_INV_CHUNK_END,
} ble_investigation_chunk_kind_t;

typedef struct {
    ble_investigation_chunk_kind_t kind;
    char request_id[BLE_INV_REQUEST_ID_LEN];
    int index;
    ble_investigation_state_t state;
    ble_investigation_mode_t mode;
    char target_mac[18];
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
    char value_hex[BLE_INV_READ_HEX_LEN];
    char summary[BLE_INV_SUMMARY_LEN];
    char error[BLE_INV_ERROR_LEN];
    bool authentication_required;
    bool truncated;
} ble_investigation_chunk_t;
```

State enum names and serialized strings must match Kotlin exactly.

- [ ] **Step 5: Implement chunk records under 1024 bytes**

Use message types:

```c
#define MSG_TYPE_BLE_INVESTIGATE      "ble_investigate"
#define MSG_TYPE_BLE_INV_BEGIN        "ble_inv_begin"
#define MSG_TYPE_BLE_INV_PROGRESS     "ble_inv_progress"
#define MSG_TYPE_BLE_INV_SERVICE      "ble_inv_service"
#define MSG_TYPE_BLE_INV_CHAR         "ble_inv_char"
#define MSG_TYPE_BLE_INV_READ         "ble_inv_read"
#define MSG_TYPE_BLE_INV_END          "ble_inv_end"
```

Each record carries `request_id`; indexed records carry `index`. Serialize with
`snprintf` bounds checks and JSON-safe text helpers. Never send one monolithic
result line. Add `../../shared/ble_investigation_protocol.c` to scanner `SRCS`
and the same source to native `build_src_filter`.

- [ ] **Step 6: Run tests and commit**

```bash
cd android
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationModelsTest
cd ../esp32
./.venv312/bin/pio test -e test
git add ../android/app/src/main/java/com/friendorfoe/detection/BleInvestigationModels.kt ../android/app/src/test/java/com/friendorfoe/detection/BleInvestigationModelsTest.kt shared/ble_investigation_types.h shared/ble_investigation_protocol.c shared/ble_investigation_protocol.h scanner/main/CMakeLists.txt platformio.ini test/test_ble_investigation_protocol.c test/test_runner.c
git commit -m "shared: define BLE investigation contract"
```

Expected: model/protocol tests pass on both platforms.

---

### Task 6: Android Direct Read-Only GATT Inspector

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/detection/AndroidBleGattInspector.kt`
- Create: `android/app/src/main/java/com/friendorfoe/detection/BleInvestigationCoordinator.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/BleInvestigationModule.kt`
- Create: `android/app/src/test/java/com/friendorfoe/detection/BleInvestigationCoordinatorTest.kt`

**Interfaces:**
- Consumes: `BleInvestigationRequest` with route `PHONE` and current Bluetooth
  permissions.
- Produces: `StateFlow<BleInvestigationResult>` and `cancel()`.

- [ ] **Step 1: Write coordinator tests with a fake transport**

```kotlin
private fun completedResult(requestId: String = "r1") = BleInvestigationResult(
    requestId = requestId,
    transport = "phone",
    mode = BleInvestigationMode.GATT,
    targetMac = "AA:BB:CC:DD:EE:FF",
    state = BleInvestigationState.COMPLETE,
    connectable = true,
    services = emptyList(),
    characteristics = emptyList(),
    reads = emptyMap(),
    bonded = false,
    encrypted = false,
    authenticationRequired = false,
    summary = "complete",
    error = null,
    truncated = false,
)

private fun request(id: String) = BleInvestigationRequest(
    requestId = id,
    target = BleInvestigationTarget(
        mode = BleInvestigationMode.GATT,
        mac = "AA:BB:CC:DD:EE:FF",
        entityKey = "mac:AA:BB:CC:DD:EE:FF",
        observedAtElapsedMs = 1_000,
        origin = PrivacyDetectionOrigin.ANDROID,
    ),
    route = BleInvestigationRoute.PHONE,
)

private class FakeInspector : BleInvestigator {
    val requests = mutableListOf<BleInvestigationRequest>()
    var result: BleInvestigationResult = completedResult()
    var block: CompletableDeferred<Unit>? = null
    var cancelCount = 0
    override suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult {
        requests += request
        progress(result.copy(requestId = request.requestId, state = BleInvestigationState.CONNECTING))
        block?.await()
        return result.copy(requestId = request.requestId)
    }
    override suspend fun cancel() {
        cancelCount++
        block?.cancel()
    }
}

@Test
fun `coordinator rejects concurrent request as busy`() = runTest {
    val fake = FakeInspector().apply { block = CompletableDeferred() }
    val coordinator = BleInvestigationCoordinator(fake)
    val first = async { coordinator.investigatePhone(request("r1")) }
    runCurrent()
    val second = coordinator.investigatePhone(request("r2"))
    assertEquals(BleInvestigationState.FAILED, second.state)
    assertEquals("busy", second.error)
    fake.block!!.complete(Unit)
    first.await()
}

@Test
fun `request timeout becomes a failed result`() = runTest {
    val fake = FakeInspector().apply { block = CompletableDeferred() }
    val coordinator = BleInvestigationCoordinator(fake)
    val result = coordinator.investigatePhone(request("r1").copy(timeoutMs = 1))
    assertEquals(BleInvestigationState.FAILED, result.state)
    assertEquals("timeout", result.error)
}

@Test
fun `cancel closes active request`() = runTest {
    val fake = FakeInspector().apply { block = CompletableDeferred() }
    val coordinator = BleInvestigationCoordinator(fake)
    val running = async { coordinator.investigatePhone(request("r1")) }
    runCurrent()
    coordinator.cancel()
    assertEquals(1, fake.cancelCount)
    assertEquals(BleInvestigationState.CANCELLED, coordinator.state.value?.state)
    running.cancel()
}

@Test
fun `authentication required remains structured evidence`() = runTest {
    val fake = FakeInspector().apply {
        result = completedResult().copy(authenticationRequired = true)
    }
    val result = BleInvestigationCoordinator(fake).investigatePhone(request("r1"))
    assertTrue(result.authenticationRequired)
    assertNull(result.error)
}
```

- [ ] **Step 2: Run the focused test and verify failure**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationCoordinatorTest
```

Expected: compile failure for missing inspector/coordinator interfaces.

- [ ] **Step 3: Implement the testable coordinator**

```kotlin
interface BleInvestigator {
    suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult
    suspend fun cancel()
}

@Singleton
class BleInvestigationCoordinator @Inject constructor(
    private val phoneInspector: BleInvestigator,
) {
    val state: StateFlow<BleInvestigationResult?>
    suspend fun investigatePhone(request: BleInvestigationRequest): BleInvestigationResult
    suspend fun cancel()
}
```

Guard the active request with `Mutex`; use `withTimeout(12_000)` and always call
inspector cleanup from `finally`.

Bind the production implementation without changing the coordinator's testable
constructor:

```kotlin
@Module
@InstallIn(SingletonComponent::class)
abstract class BleInvestigationModule {
    @Binds
    @Singleton
    abstract fun bindBleInvestigator(impl: AndroidBleGattInspector): BleInvestigator
}
```

- [ ] **Step 4: Implement Android BluetoothGatt discovery without target writes**

`AndroidBleGattInspector` must:

1. Validate `BLUETOOTH_CONNECT` and target freshness before `connectGatt`.
2. Emit `CONNECTING`, `DISCOVERING`, and `READING` states.
3. Discover and normalize at most 16 services and 32 characteristics.
4. Read GAP Device Name and Device Information (`0x180A`) characteristics only
   when they advertise `PROPERTY_READ`.
5. For a serial candidate, inspect `FFE1`/`FFF1` properties and permissions. If
   encrypted permissions are declared, report authentication required without
   reading. Read only when `PROPERTY_READ` is set and permissions do not require
   encryption.
6. Treat GATT insufficient authentication/encryption statuses as
   `authenticationRequired = true`, disconnect on any unexpected bond-state
   transition, and never request bonding.
7. Disconnect and close exactly once on success, error, timeout, or cancel.

Keep callback-to-coroutine completion in a private `CompletableDeferred` and
serialize characteristic reads one at a time.

- [ ] **Step 5: Run tests and build the Android module**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.detection.BleInvestigationCoordinatorTest
./gradlew assembleDebug
```

Expected: coordinator tests pass and Android Bluetooth APIs compile on min SDK
26 with guarded API calls.

- [ ] **Step 6: Commit**

```bash
git add app/src/main/java/com/friendorfoe/detection/AndroidBleGattInspector.kt app/src/main/java/com/friendorfoe/detection/BleInvestigationCoordinator.kt app/src/main/java/com/friendorfoe/di/BleInvestigationModule.kt app/src/test/java/com/friendorfoe/detection/BleInvestigationCoordinatorTest.kt
git commit -m "android: add read-only BLE investigation"
```

---

### Task 7: Badge Scanner Passive Capture and NimBLE GATT Investigation

**Files:**
- Create: `esp32/scanner/main/detection/ble_investigator.h`
- Create: `esp32/scanner/main/detection/ble_investigator.c`
- Create: `esp32/test/test_ble_investigator_state.c`
- Modify: `esp32/scanner/main/CMakeLists.txt`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/scanner/main/detection/ble_remote_id.h`
- Modify: `esp32/scanner/main/detection/ble_remote_id.c`
- Modify: `esp32/scanner/main/main.c`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: scanner UART `ble_investigate` request.
- Produces: chunk protocol records from Task 5 and guaranteed scan restoration.

- [ ] **Step 1: Write failing pure state-machine tests**

Drive the state machine with explicit events rather than NimBLE globals:

```c
void test_ble_investigator_rejects_second_request_as_busy(void);
void test_ble_investigator_gatt_success_reaches_complete(void);
void test_ble_investigator_auth_error_sets_auth_required(void);
void test_ble_investigator_timeout_reaches_failed(void);
void test_ble_investigator_cancel_reaches_cancelled(void);
void test_ble_investigator_every_terminal_path_requests_scan_resume(void);
void test_ble_investigator_passive_capture_summarizes_prompt_families(void);
```

- [ ] **Step 2: Run native tests and verify red state**

```bash
./.venv312/bin/pio test -e test
```

Expected: missing `ble_investigator.h` compile failure.

- [ ] **Step 3: Implement the pure event-driven state core**

Public API:

```c
typedef enum {
    BLE_INVESTIGATOR_EVENT_CONNECTED = 0,
    BLE_INVESTIGATOR_EVENT_CONNECT_FAILED,
    BLE_INVESTIGATOR_EVENT_SERVICE,
    BLE_INVESTIGATOR_EVENT_CHARACTERISTIC,
    BLE_INVESTIGATOR_EVENT_READ,
    BLE_INVESTIGATOR_EVENT_AUTH_REQUIRED,
    BLE_INVESTIGATOR_EVENT_DISCOVERY_COMPLETE,
    BLE_INVESTIGATOR_EVENT_DISCONNECTED,
} ble_investigator_event_kind_t;

typedef struct {
    ble_investigator_event_kind_t kind;
    char service_uuid[BLE_INV_UUID_LEN];
    char uuid[BLE_INV_UUID_LEN];
    uint16_t properties;
    const uint8_t *value;
    size_t value_len;
    int status;
} ble_investigator_event_t;

typedef struct {
    ble_investigation_state_t state;
    ble_investigation_request_t request;
    ble_investigation_result_t result;
    int64_t deadline_ms;
    bool busy;
    bool connected;
    bool resume_scan_required;
} ble_investigator_t;

void ble_investigator_init(ble_investigator_t *state);
bool ble_investigator_start(ble_investigator_t *state,
                            const ble_investigation_request_t *request,
                            int64_t now_ms);
void ble_investigator_handle_event(ble_investigator_t *state,
                                   const ble_investigator_event_t *event,
                                   int64_t now_ms);
void ble_investigator_tick(ble_investigator_t *state, int64_t now_ms);
void ble_investigator_cancel(ble_investigator_t *state, int64_t now_ms);
bool ble_investigator_take_result(ble_investigator_t *state,
                                  ble_investigation_result_t *out);
```

Make `resume_scan_required` explicit in every terminal result and assert it in
tests.

- [ ] **Step 4: Add scanner scan-pause/resume hooks**

Expose narrow functions in `ble_remote_id.h`:

```c
bool ble_remote_id_pause_for_investigation(void);
void ble_remote_id_resume_after_investigation(void);
void ble_remote_id_note_investigation_advertisement(const uint8_t mac[6],
                                                    const ble_fingerprint_t *fp,
                                                    int8_t rssi,
                                                    uint8_t props,
                                                    int64_t now_ms);
```

Pause with `ble_gap_disc_cancel` without deinitializing NimBLE. Resume through
the existing internal scan-start path. A guard prevents the normal watchdog
from restarting scan during active GATT work.

- [ ] **Step 5: Implement NimBLE client callbacks and safe reads**

Use `ble_gap_connect`, primary service discovery, characteristic discovery, and
sequential `ble_gattc_read`. Never call `ble_gattc_write`, subscribe, or start
security. Read only standard Device Information/GAP values and readable
`FFE1`/`FFF1`. Map ATT insufficient authentication/encryption to structured
security evidence.

At terminal state, emit chunk records with `uart_tx_send_raw_json`, disconnect
if connected, and call scan resume from one cleanup function.

- [ ] **Step 6: Parse scanner command and add passive-capture mode**

In `main.c`, accept:

```json
{"type":"ble_investigate","request_id":"req-1","mode":"gatt","target":"AA:BB:CC:DD:EE:FF","timeout_ms":12000}
```

Validate request ID length, mode, MAC format, timeout range, and calibration/OTA
state. Passive mode keeps scanning and summarizes qualifying advertisements for
12 seconds without a target connection.

- [ ] **Step 7: Run native tests and badge scanner build**

```bash
./.venv312/bin/pio test -e test
cd scanner
../.venv312/bin/pio run -e scanner-s3-combo-fof_badge
```

Expected: native tests pass and badge scanner firmware links with NimBLE GATT
client symbols.

- [ ] **Step 8: Commit**

```bash
git add scanner/main/detection/ble_investigator.c scanner/main/detection/ble_investigator.h scanner/main/detection/ble_remote_id.c scanner/main/detection/ble_remote_id.h scanner/main/main.c scanner/main/CMakeLists.txt platformio.ini test/test_ble_investigator_state.c test/test_runner.c
git commit -m "badge scanner: add BLE investigation engine"
```

---

### Task 8: Badge Uplink Routing, Assembly, USB-C, and Bonded BLE Exposure

**Files:**
- Create: `esp32/uplink/main/core/badge_ble_investigation.h`
- Create: `esp32/uplink/main/core/badge_ble_investigation.c`
- Create: `esp32/shared/badge_ble_investigation_state.h`
- Create: `esp32/shared/badge_ble_investigation_state.c`
- Create: `esp32/test/test_badge_ble_investigation.c`
- Modify: `esp32/uplink/main/comms/uart_rx.h`
- Modify: `esp32/uplink/main/comms/uart_rx.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/core/badge_ble_control.h`
- Modify: `esp32/uplink/main/core/badge_ble_control.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: `ble_investigate` from USB, badge BLE, HTTP, or local button; consumes
  scanner chunk records.
- Produces: assembled latest result, compact status, USB chunk forwarding, and
  chunked bonded-BLE reads.

- [ ] **Step 1: Write failing routing and assembly tests**

```c
void test_badge_investigation_routes_only_to_ble_scanner_slot(void);
void test_badge_investigation_rejects_when_ble_scanner_is_unavailable(void);
void test_badge_investigation_assembles_scanner_chunks_by_request_id(void);
void test_badge_investigation_transport_loss_keeps_local_operation_active(void);
void test_badge_investigation_ble_chunk_cursor_is_bounded(void);
```

- [ ] **Step 2: Run native tests and verify failure**

```bash
./.venv312/bin/pio test -e test
```

Expected: missing badge investigation state API.

- [ ] **Step 3: Implement pure assembly state plus one uplink transport owner**

The native-testable shared state has no cJSON, UART, BLE, or HTTP dependency:

```c
typedef struct {
    ble_investigation_result_t result;
    bool active;
    int selected_chunk;
} badge_ble_investigation_state_t;

void badge_ble_investigation_state_init(badge_ble_investigation_state_t *state);
bool badge_ble_investigation_state_start(badge_ble_investigation_state_t *state,
                                         const ble_investigation_request_t *request,
                                         bool scanner_available,
                                         int *scanner_slot_out);
bool badge_ble_investigation_state_accept(
    badge_ble_investigation_state_t *state,
    const ble_investigation_chunk_t *chunk);
```

Add `shared/badge_ble_investigation_state.c` to native `build_src_filter` so
Unity tests exercise this exact production state core.

The uplink wrapper parses cJSON into `ble_investigation_chunk_t`. Its public API
is:

```c
void badge_ble_investigation_init(void);
bool badge_ble_investigation_start(const char *request_id,
                                   const char *mode,
                                   const char *target_mac,
                                   const char *transport,
                                   char *err,
                                   size_t err_len);
bool badge_ble_investigation_accept_scanner_json(const cJSON *root);
void badge_ble_investigation_get(ble_investigation_result_t *out);
size_t badge_ble_investigation_status_json(char *out, size_t out_len);
bool badge_ble_investigation_select_chunk(const char *request_id, int seq);
size_t badge_ble_investigation_selected_chunk_json(char *out, size_t out_len);
```

Start forwards only to scanner slot 0 using
`uart_rx_send_command_to_scanner_checked(0, payload)` and returns
`scanner_unavailable` when that command ingress is unhealthy.

- [ ] **Step 4: Feed scanner chunks into the assembler and USB output**

In both UART parser paths, detect all `ble_inv_*` message types before detection
parsing. Accept into the assembler and emit a sanitized USB line:

```text
FOF_INV:{"type":"ble_inv_service","request_id":"req-1","index":0,"uuid":"FFE0"}
```

Do not put the full result into a single scanner UART or USB line.

- [ ] **Step 5: Add commands to all badge control transports**

Accept identical payloads in `serial_config.c`, badge BLE control, and HTTP:

```json
{"cmd":"ble_investigate","request_id":"req-1","mode":"gatt","target":"AA:BB:CC:DD:EE:FF"}
```

Add `ble_investigation_chunk` with `request_id` and `seq` to select a bounded
chunk for the new encrypted/readable badge GATT characteristic `0xFF03`.
Android writes the selection command to the existing encrypted control
characteristic, then reads `0xFF03`. Never expose investigation data before the
badge connection is authorized.

- [ ] **Step 6: Add compact status without overflowing current status paths**

Add only request ID, state, mode, summary, error, service count,
characteristic count, authentication-required, and truncation to normal
`FOF_STATUS`/HTTP/badge status. Full arrays remain chunked.

- [ ] **Step 7: Run tests and build badge uplink**

```bash
./.venv312/bin/pio test -e test
cd uplink
../.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: native tests pass and badge uplink links the new USB/BLE handlers.

- [ ] **Step 8: Commit**

```bash
git add shared/badge_ble_investigation_state.c shared/badge_ble_investigation_state.h uplink/main/core/badge_ble_investigation.c uplink/main/core/badge_ble_investigation.h uplink/main/comms/uart_rx.c uplink/main/comms/uart_rx.h uplink/main/core/serial_config.c uplink/main/core/badge_ble_control.c uplink/main/core/badge_ble_control.h uplink/main/network/http_status.c platformio.ini test/test_badge_ble_investigation.c test/test_runner.c
git commit -m "badge: route and expose BLE investigations"
```

---

### Task 9: Badge Button-2 Policy and Investigation Display

**Files:**
- Create: `esp32/shared/badge_investigation_policy.h`
- Create: `esp32/shared/badge_investigation_policy.c`
- Create: `esp32/test/test_badge_investigation_policy.c`
- Modify: `esp32/uplink/main/hw/display_st7735.c`
- Modify: `esp32/uplink/main/hw/oled_display.h`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/platformio.ini`
- Modify: `esp32/test/test_runner.c`

**Interfaces:**
- Consumes: current copied `badge_focus_entry_t` and latest investigation state.
- Produces: deterministic long-press action and LCD progress/result pages.

- [ ] **Step 1: Add failing pure button-policy tests**

```c
void test_badge_hold_on_ble_entity_starts_gatt_investigation(void);
void test_badge_hold_on_pairing_spam_starts_passive_capture(void);
void test_badge_hold_on_non_ble_entity_opens_deepest_detail(void);
void test_badge_hold_without_entity_keeps_pairing_action(void);
void test_badge_investigation_copies_target_before_snapshot_reorder(void);
```

- [ ] **Step 2: Run native tests and verify red state**

```bash
./.venv312/bin/pio test -e test
```

Expected: compile failure for missing policy types.

- [ ] **Step 3: Implement pure long-press decision**

```c
typedef enum {
    BADGE_HOLD_PAIR_PHONE = 0,
    BADGE_HOLD_SHOW_DETAIL,
    BADGE_HOLD_INVESTIGATE_GATT,
    BADGE_HOLD_INVESTIGATE_PASSIVE,
} badge_hold_action_t;

typedef struct {
    bool has_entity;
    uint8_t source;
    badge_threat_category_t category;
    char key[BADGE_THREAT_KEY_LEN];
    char bssid[18];
} badge_investigation_selection_t;

badge_hold_action_t badge_investigation_hold_action(
    const badge_investigation_selection_t *selection);
```

Pairing-spam category selects passive capture; BLE fingerprint plus valid BSSID
selects GATT; other entities select detail; no entity selects pairing.

- [ ] **Step 4: Wire button 2 long press contextually**

Before invoking any command, copy the current focus entry into a local
`badge_investigation_selection_t`. Call `badge_ble_investigation_start` with a
generated request ID, the action-selected mode, the copied BSSID, transport
`"badge_button"`, and a bounded error buffer. Keep the existing pairing/QR
branch for `BADGE_HOLD_PAIR_PHONE`.

Do not change single/double gesture timing or behavior.

- [ ] **Step 5: Add progress and three bounded result pages**

Add an investigation overlay/state that renders:

1. `CHECKING` plus phase and remaining seconds.
2. Identity/route plus target or passive family summary.
3. Service/serial UUID summary.
4. Security/authentication/evidence summary.

Single press advances result pages. Double press exits. A terminal error shows
its sanitized reason and does not delete the underlying alert.

- [ ] **Step 6: Expose display diagnostics and run builds**

Extend badge display state JSON with investigation request ID, state, and page.
Then run:

```bash
./.venv312/bin/pio test -e test
cd uplink
../.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: policy tests pass and badge display compiles without stack overflow
warnings from oversized local result structs.

- [ ] **Step 7: Commit**

```bash
git add shared/badge_investigation_policy.c shared/badge_investigation_policy.h uplink/main/hw/display_st7735.c uplink/main/hw/oled_display.h uplink/main/core/serial_config.c platformio.ini test/test_badge_investigation_policy.c test/test_runner.c
git commit -m "badge: investigate selected alert on button hold"
```

---

### Task 10: Android Badge Transport, Route Selection, and Investigation UI

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeInvestigationProtocolTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/BleInvestigationRoutingTest.kt`

**Interfaces:**
- Consumes: direct inspector from Task 6 and badge transport/result contract from
  Task 8.
- Produces: user-visible `Investigate`, route selection, progress, cancel, and
  normalized results.

- [ ] **Step 1: Write failing badge chunk parser tests**

```kotlin
@Test
fun `FOF INV lines assemble into badge result`() {
    val parser = BadgeInvestigationStreamParser()
    assertNull(parser.accept("FOF_INV:{\"type\":\"ble_inv_begin\",\"request_id\":\"r1\",\"mode\":\"gatt\"}"))
    assertNull(parser.accept("FOF_INV:{\"type\":\"ble_inv_service\",\"request_id\":\"r1\",\"index\":0,\"uuid\":\"FFE0\"}"))
    val result = parser.accept("FOF_INV:{\"type\":\"ble_inv_end\",\"request_id\":\"r1\",\"state\":\"complete\",\"summary\":\"UART service found\"}")
    assertEquals(listOf("FFE0"), result!!.services)
}
```

Add malformed JSON, mismatched request, truncation, and disconnect/reconnect
cases.

- [ ] **Step 2: Write failing route tests**

Required routing matrix:

```kotlin
ANDROID + AUTO + phone available -> PHONE
BADGE + AUTO + USB connected -> BADGE
BADGE + AUTO + bonded badge BLE connected -> BADGE
BADGE + AUTO + badge unavailable + fresh target MAC -> PHONE
PASSIVE_CAPTURE + AUTO + badge available -> BADGE
explicit PHONE with stale/no MAC -> validation failure
explicit BADGE with unavailable scanner -> validation failure
```

- [ ] **Step 3: Run focused tests and verify red state**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.data.badge.BadgeInvestigationProtocolTest --tests com.friendorfoe.presentation.privacy.BleInvestigationRoutingTest
```

Expected: parser and route selector are missing.

- [ ] **Step 4: Extend BadgeUsbRepository with one investigation API**

```kotlin
fun investigateBle(request: BleInvestigationRequest)
fun cancelBleInvestigation(requestId: String)
val investigation: StateFlow<BleInvestigationResult?>
```

USB writes `FOF_CTL` and consumes `FOF_INV` chunks. Badge BLE writes the command,
then selects and reads `0xFF03` chunks until `ble_inv_end`. HTTP sends the same
command and polls compact status plus chunk endpoint/selection. Reset the
assembler only for matching request IDs.

- [ ] **Step 5: Add ViewModel route selection and lifecycle**

Inject `BleInvestigationCoordinator`. Expose:

```kotlin
val investigationResult: StateFlow<BleInvestigationResult?>
fun investigate(detection: GlassesDetection, route: BleInvestigationRoute)
fun investigateBadgeEntity(entity: BadgeThreatEntity, route: BleInvestigationRoute)
fun cancelInvestigation()
fun clearInvestigation()
```

Use a pure `selectInvestigationRoute(origin, target, badgeState, requestedRoute)`
function covered by the routing matrix. Never infer badge origin from display
text; use `detection.origin` and `detection.investigationTarget`.

- [ ] **Step 6: Add feature-complete Compose controls**

In `DeviceDetailDialog` and `BadgeEntityDetailDialog`, add an icon+text
`Investigate` command only when a structured target exists. Open a dedicated
dialog/sheet with:

- Material segmented route control for `Auto`, `Phone`, and `Badge`.
- Progress phase and cancel icon button.
- Summary, service list, characteristic properties, readable standard values,
  security/authentication status, passive evidence, and error text.
- Disabled route segments with concise availability labels.

Do not describe internal detector thresholds or implementation instructions in
the UI. Keep dialog headings compact and text wrapping bounded on narrow screens.

- [ ] **Step 7: Run Android tests and build**

```bash
./gradlew testDebugUnitTest --tests com.friendorfoe.data.badge.BadgeInvestigationProtocolTest --tests com.friendorfoe.presentation.privacy.BleInvestigationRoutingTest
./gradlew testDebugUnitTest
./gradlew assembleDebug
```

Expected: focused tests, full JVM suite, and debug APK build pass.

- [ ] **Step 8: Commit**

```bash
git add app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt app/src/test/java/com/friendorfoe/data/badge/BadgeInvestigationProtocolTest.kt app/src/test/java/com/friendorfoe/presentation/privacy/BleInvestigationRoutingTest.kt
git commit -m "android: investigate BLE devices by phone or badge"
```

---

### Task 11: Cross-Layer Audit, Builds, and Hardware Evidence

**Files:**
- Create: `docs/ble-threat-investigation-verification.md`
- Modify only if failures prove necessary: files from Tasks 1-10

**Interfaces:**
- Consumes: completed Android and badge implementations.
- Produces: requirement-by-requirement evidence and an explicit hardware status.

- [ ] **Step 1: Run source hygiene checks**

```bash
git diff --check 52e3e31..HEAD
rg -n "createBond|ble_gattc_write|setCharacteristicNotification" android/app/src/main/java/com/friendorfoe/detection esp32/scanner/main/detection esp32/uplink/main/core
```

Expected: no whitespace errors, no unfinished markers, no automatic pairing,
target write, or target notification code. Existing badge-control notification code is
allowed only outside the target inspector.

- [ ] **Step 2: Run the complete software verification matrix**

```bash
cd android
./gradlew testDebugUnitTest
./gradlew assembleDebug
cd ../esp32
./.venv312/bin/pio test -e test
cd scanner
../.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
../.venv312/bin/pio run -e uplink-s3-fof_badge
```

Expected: every command exits 0.

- [ ] **Step 3: Prove Android/C detector parity from fixture outputs**

Add one machine-readable fixture file only if needed by tests; otherwise record
the named Kotlin and Unity test cases. Confirm matching outcomes for Apple,
Fast Pair, Swift Pair, mixed flood, crowd negative, cooldown, decay, FFE0,
FFF0/PKOC, trusted identity, and multi-service negative.

- [ ] **Step 4: Inspect connected hardware availability without claiming it**

```bash
adb devices
system_profiler SPUSBDataType
```

If an Android target and FoF badge are present, perform the following with a
known safe BLE test peripheral:

1. Direct phone investigation discovers services and exits without pairing.
2. USB-C badge request returns `FOF_INV` begin/end with matching request ID.
3. Bonded badge BLE control returns the same compact result/chunks.
4. Button-2 hold starts the selected test alert, renders progress/result, and
   scanner status proves BLE scanning resumed.
5. A read-protected test characteristic reports authentication required and no
   pairing dialog opens.

If hardware is absent, mark each hardware item `NOT RUN: hardware unavailable`;
do not translate build success into a hardware claim.

- [ ] **Step 5: Write verification evidence**

`docs/ble-threat-investigation-verification.md` must list each objective item,
the exact test/build command, exit status, relevant test name, hardware result,
and remaining limitation. Include the debug APK path from Gradle output and
badge firmware `.bin` paths from PlatformIO output, but do not publish them.

- [ ] **Step 6: Review final diff and commit verification**

```bash
git status --short --branch
git diff --stat 52e3e31..HEAD
git diff --check 52e3e31..HEAD
git add docs/ble-threat-investigation-verification.md
git commit -m "docs: verify BLE threat investigation"
```

Expected: only intentional source/tests/docs remain, no generated build output
is staged, and the worktree is clean after commit.

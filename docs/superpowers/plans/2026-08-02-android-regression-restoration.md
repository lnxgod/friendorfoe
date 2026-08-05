# Android Regression Restoration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore aircraft imagery and GitHub update status while making the configured Friend or Foe sensor backend a strict, explicit Android opt-in.

**Architecture:** Extend the current v0.67.x presentation models instead of reverting the interface overhaul, reuse the existing aircraft asset resolver and GitHub update repository, and enforce backend consent at both reactive pollers and the shared backend OkHttp client. The three regression fixes have separate red/green/commit cycles, followed by one integrated APK and emulator verification gate.

**Tech Stack:** Kotlin 1.9.22, Compose compiler 1.5.9, Jetpack Compose Material 3, Navigation Compose, Hilt, StateFlow/coroutines, Retrofit/OkHttp, Coil, JUnit 4, AndroidX Compose UI tests, Gradle 8.x, API 35 emulator.

## Global Constraints

- Begin implementation from a freshly fetched `origin/main` descendant, not the stale divergent local `main`; preserve all unrelated working-tree changes.
- Android source, Android tests, and Android design/plan documentation only.
- No backend service, database, API, deployment, or configuration changes.
- No ESP32 firmware, build, upload, or hardware changes.
- No new aircraft or drone image assets and no image generation.
- No release publication, version bump, tag, APK upload, or GitHub release mutation unless separately requested.
- GitHub update checks, public ADS-B providers, weather, map tiles, and other non-Friend-or-Foe internet services remain independent of the sensor-backend opt-in.
- Missing `sensor_backend_enabled` means disabled; explicit persisted `true` and `false` remain authoritative.
- Disabled sensor-backend state must cause zero configured-backend DNS, socket, or HTTP attempts.
- The About update display remains in-app only; do not add a system notification, background worker, or persistent update cache.
- Keep the existing Material 3 visual language, seven top-level destinations, About-first landing, and summary-first Detail layout.
- Follow strict red-green-refactor: every production behavior change must be preceded by a focused test that fails for the expected missing behavior.

## File Structure

- `android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt` — canonical detection defaults, preference projection, and backend/backend-only invariant.
- `android/app/src/main/java/com/friendorfoe/data/BackendRequestInterceptor.kt` — fail-closed backend consent and configured-origin rewrite for every backend request.
- `android/app/src/main/java/com/friendorfoe/di/NetworkModule.kt` — installs the backend interceptor only on the configured backend client.
- `android/app/src/main/java/com/friendorfoe/di/InfoModule.kt` — gives backend health checks the guarded backend client while leaving GitHub on the general client.
- `android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt` — backend-only UI policy and idempotent update discovery.
- `android/app/src/main/java/com/friendorfoe/presentation/about/AppUpdateRow.kt` — one update-state renderer shared by About landing and App settings.
- `android/app/src/main/java/com/friendorfoe/presentation/about/AboutLandingScreen.kt` — main About version/update presentation.
- `android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt` — reuses `AppUpdateRow` in App settings.
- `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt` — graph-scopes `AboutViewModel`, starts one idle update check, and shares state with settings.
- `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailPresentation.kt` — carries optional aircraft visual evidence.
- `android/app/src/main/java/com/friendorfoe/presentation/detail/AircraftPhotoCard.kt` — shared Coil/bundled-asset/silhouette renderer.
- `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailOverviewContent.kt` — restores aircraft media to full Detail.
- `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt` — reuses the extracted photo card in legacy AR/Map sheet content.
- Existing JVM and Compose test files listed per task remain the regression contract.

---

### Task 0: Establish the Correct Isolated Baseline

**Files:**
- Carry forward: `docs/superpowers/specs/2026-08-02-android-regression-restoration-design.md`
- Carry forward: `docs/superpowers/plans/2026-08-02-android-regression-restoration.md`
- Do not edit: any unrelated dirty path in the current checkout

**Interfaces:**
- Consumes: refreshed `origin/main`, the two documentation-only commits on the current divergent `main`, and the `superpowers:using-git-worktrees` workflow.
- Produces: an isolated `android/regression-restoration` branch whose merge base is refreshed `origin/main` and whose only initial delta is the approved design and this plan.

- [ ] **Step 1: Record the current checkout without mutating it**

From the current repository root:

```bash
git status --short
git branch --show-current
git log -2 --oneline
```

Expected: the current checkout still contains the user's unrelated dirty files; the two newest task commits are documentation-only. Do not stash, reset, clean, or stage those unrelated paths.

- [ ] **Step 2: Refresh the remote baseline and create an isolated worktree**

Invoke `superpowers:using-git-worktrees` before creating the worktree. Fetch and verify the implementation base:

```bash
git fetch origin main
git rev-parse origin/main
git show -s --format='%h %s' origin/main
```

Through that skill's safe directory-selection flow, create branch `android/regression-restoration` from the refreshed `origin/main`, then change into the selected worktree. Do not branch from the checked-out local `main`.

- [ ] **Step 3: Carry only the approved documentation commits**

From the isolated worktree, resolve the current checkout's plan commit by path and cherry-pick the two documentation commits:

```bash
ANDROID_PLAN_DOC_COMMIT="$(git log -1 --format=%H main -- docs/superpowers/plans/2026-08-02-android-regression-restoration.md)"
test -n "$ANDROID_PLAN_DOC_COMMIT"
git show --name-only --format= "$ANDROID_PLAN_DOC_COMMIT"
git cherry-pick 8d2b107
git cherry-pick "$ANDROID_PLAN_DOC_COMMIT"
git diff --name-only origin/main...HEAD
git status --short
```

Expected: the diff lists only the approved spec and plan; the isolated worktree is clean. If the resolved plan commit is empty or is not documentation-only, stop before cherry-picking and report the mismatch.

---

### Task 1: Make Sensor Backend Consent an Explicit Preference

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/DetectionSettingsTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/DetectionPrefsTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/about/AboutViewModelTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/data/DetectionPrefsObservableTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/presentation/about/InfoScreenTest.kt`

**Interfaces:**
- Consumes: existing `DetectionSettings`, `DetectionPrefs.settings`, `InfoSettingKey`, and `infoSettingDisabledReason(...)`.
- Produces: `DetectionSettings.defaults().sensorBackendEnabled == false`; effective/persisted `backendOnlyMode == false` whenever the backend is off; `InfoSettingKey.BACKEND_ONLY` is disabled until backend consent is on.

- [ ] **Step 1: Write failing JVM tests for fresh and persisted consent**

Add these assertions to `DetectionSettingsTest` and `DetectionPrefsTest`:

```kotlin
@Test
fun snapshotDefaultsRequireBackendOptIn() {
    val value = DetectionSettings.defaults()

    assertFalse(value.sensorBackendEnabled)
    assertFalse(value.backendOnlyMode)
}

@Test
fun missingBackendPreferenceDefaultsToDisabled() {
    val prefs = DetectionPrefs(TestContext(TestSharedPreferences()))

    assertFalse(prefs.settings.value.sensorBackendEnabled)
}

@Test
fun explicitBackendPreferenceValuesRemainAuthoritative() {
    val enabled = DetectionPrefs(TestContext(TestSharedPreferences(
        mapOf("sensor_backend_enabled" to true),
    )))
    val disabled = DetectionPrefs(TestContext(TestSharedPreferences(
        mapOf("sensor_backend_enabled" to false),
    )))

    assertTrue(enabled.settings.value.sensorBackendEnabled)
    assertFalse(disabled.settings.value.sensorBackendEnabled)
}
```

Change the existing `snapshotDefaultsMatchCurrentProductDefaults` expectation from `assertTrue(value.sensorBackendEnabled)` to `assertFalse(value.sensorBackendEnabled)` rather than retaining contradictory coverage.

- [ ] **Step 2: Write failing policy tests for backend-only normalization**

Add to `AboutViewModelTest`:

```kotlin
@Test
fun backendOnlyCannotRemainEnabledWithoutBackendConsent() {
    val configured = DetectionSettings.defaults().copy(
        sensorBackendEnabled = true,
        backendOnlyMode = true,
    )

    val disabled = configured.withSetting(InfoSettingKey.SENSOR_BACKEND, false)
    val rejectedEnable = DetectionSettings.defaults()
        .withSetting(InfoSettingKey.BACKEND_ONLY, true)

    assertFalse(disabled.sensorBackendEnabled)
    assertFalse(disabled.backendOnlyMode)
    assertFalse(rejectedEnable.backendOnlyMode)
    assertEquals(
        "Enable Sensor backend connection first.",
        infoSettingDisabledReason(
            InfoSettingKey.BACKEND_ONLY,
            DetectionSettings.defaults(),
            PermissionUiState.Granted,
        ),
    )

    assertTrue(
        shouldRestartSkySourcesForInfoSetting(
            key = InfoSettingKey.SENSOR_BACKEND,
            enabled = false,
            previousSettings = configured,
        ),
    )
    assertFalse(
        shouldRestartSkySourcesForInfoSetting(
            key = InfoSettingKey.SENSOR_BACKEND,
            enabled = true,
            previousSettings = configured,
        ),
    )
    assertFalse(
        shouldRestartSkySourcesForInfoSetting(
            key = InfoSettingKey.SENSOR_BACKEND,
            enabled = false,
            previousSettings = configured.copy(backendOnlyMode = false),
        ),
    )
}
```

- [ ] **Step 3: Write failing real-preference and settings UI coverage**

Extend `DetectionPrefsObservableTest` to preserve and restore both backend keys, then seed the legacy invalid pair and assert constructor normalization:

```kotlin
sharedPreferences.edit()
    .putBoolean("sensor_backend_enabled", false)
    .putBoolean("sensor_backend_only_mode", true)
    .commit()

val normalized = DetectionPrefs(context)

assertFalse(normalized.settings.value.sensorBackendEnabled)
assertFalse(normalized.settings.value.backendOnlyMode)
assertFalse(sharedPreferences.getBoolean("sensor_backend_only_mode", true))
```

Also assert that setting `sensorBackendEnabled = true` publishes an enabled snapshot, then setting it false publishes both flags false. Restore the two keys in `finally` using the test’s existing original-value pattern.

Add this fresh-state UI contract to `InfoScreenTest` and import `assertIsOff` plus `PermissionUiState`:

```kotlin
@Test
fun freshStateShowsBackendOffAndPreventsBackendOnlyMode() {
    val settings = DetectionSettings.defaults()
    setInfoContent(
        state = state().copy(
            settings = settings,
            backendUrlCanTest = false,
        ),
        actions = InfoActions(
            settingDisabledReason = { key ->
                infoSettingDisabledReason(
                    key = key,
                    settings = settings,
                    phonePrivacyPermission = PermissionUiState.Granted,
                )
            },
        ),
    )

    scrollToSection(1)
    compose.onNodeWithTag("setting_sensor_backend")
        .performScrollTo()
        .assertIsOff()
    compose.onNodeWithTag("setting_backend_only")
        .performScrollTo()
        .assertIsNotEnabled()
    compose.onNodeWithText(
        "Enable Sensor backend connection first.",
        substring = true,
    ).assertIsDisplayed()
    compose.onNodeWithTag("backend_test").assertIsNotEnabled()
}
```

- [ ] **Step 4: Run focused tests and verify RED**

Run from `android/`:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.data.DetectionSettingsTest' \
  --tests 'com.friendorfoe.data.DetectionPrefsTest' \
  --tests 'com.friendorfoe.presentation.about.AboutViewModelTest'
```

Expected: failures show the current backend default is `true`, backend-only remains `true` after backend disable, and the BACKEND_ONLY disabled reason is absent.

If an emulator is already running, also run the new real-preferences and settings UI tests and confirm they fail on the current enabled default or missing backend-only policy:

```bash
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.DetectionPrefsObservableTest,com.friendorfoe.presentation.about.InfoScreenTest
```

- [ ] **Step 5: Implement the off-by-default preference and invariant**

In `DetectionSettings.defaults()`, change only these named arguments:

```kotlin
sensorBackendEnabled = false,
backendOnlyMode = false,
```

In `DetectionPrefs`, ensure the missing-key default is off and the backend-only getter/setter cannot contradict backend consent:

```kotlin

var sensorBackendEnabled: Boolean
    get() = prefs.getBoolean(KEY_SENSOR_BACKEND, false)
    set(value) {
        prefs.edit().apply {
            putBoolean(KEY_SENSOR_BACKEND, value)
            if (!value) putBoolean(KEY_BACKEND_ONLY, false)
        }.apply()
    }

var backendOnlyMode: Boolean
    get() = sensorBackendEnabled && prefs.getBoolean(KEY_BACKEND_ONLY, false)
    set(value) = prefs.edit()
        .putBoolean(KEY_BACKEND_ONLY, value && sensorBackendEnabled)
        .apply()
```

After registering the preference listener, normalize a legacy impossible pair without enabling anything:

```kotlin
init {
    prefs.registerOnSharedPreferenceChangeListener(listener)
    if (!sensorBackendEnabled && prefs.getBoolean(KEY_BACKEND_ONLY, false)) {
        prefs.edit().putBoolean(KEY_BACKEND_ONLY, false).apply()
    }
}
```

Update the comment to say the sensor backend is disabled until explicitly enabled.

- [ ] **Step 6: Mirror the invariant in test projections and settings availability**

In `DetectionSettings.withSetting(...)`:

```kotlin
InfoSettingKey.SENSOR_BACKEND -> copy(
    sensorBackendEnabled = enabled,
    backendOnlyMode = backendOnlyMode && enabled,
)
InfoSettingKey.BACKEND_ONLY -> copy(
    backendOnlyMode = enabled && sensorBackendEnabled,
)
```

In `infoSettingDisabledReason(...)`, handle backend-only before the existing stalker/Wi-Fi early return:

```kotlin
if (key == InfoSettingKey.BACKEND_ONLY && !settings.sensorBackendEnabled) {
    return "Enable Sensor backend connection first."
}
if (key != InfoSettingKey.STALKER && key != InfoSettingKey.WIFI_ANOMALY) return null
```

`AndroidInfoSettingsStore` continues writing through `DetectionPrefs`, which now enforces the invariant for production callers.

Replace the one-argument restart policy with a before-state-aware version:

```kotlin
internal fun shouldRestartSkySourcesForInfoSetting(
    key: InfoSettingKey,
    enabled: Boolean,
    previousSettings: DetectionSettings,
): Boolean = key in setOf(
    InfoSettingKey.ADS_B,
    InfoSettingKey.BLE_REMOTE_ID,
    InfoSettingKey.WIFI_REMOTE_ID,
    InfoSettingKey.BACKEND_ONLY,
) || (
    key == InfoSettingKey.SENSOR_BACKEND &&
        !enabled &&
        previousSettings.backendOnlyMode
)
```

In `AndroidInfoSettingsStore.set`, capture the projected settings before writing, then use the new policy after the preference setter has atomically cleared backend-only mode:

```kotlin
override fun set(key: InfoSettingKey, enabled: Boolean) {
    val previousSettings = settings.value

    when (key) {
        InfoSettingKey.ADS_B -> detectionPrefs.adsbEnabled = enabled
        InfoSettingKey.BLE_REMOTE_ID -> detectionPrefs.bleRidEnabled = enabled
        InfoSettingKey.WIFI_REMOTE_ID -> detectionPrefs.wifiEnabled = enabled
        InfoSettingKey.PHONE_PRIVACY_SCAN ->
            skyObjectRepository.setPrivacyDetectionEnabled(enabled)
        InfoSettingKey.STALKER -> detectionPrefs.stalkerDetectionEnabled = enabled
        InfoSettingKey.ULTRASONIC -> detectionPrefs.ultrasonicEnabled = enabled
        InfoSettingKey.WIFI_ANOMALY -> detectionPrefs.wifiAnomalyEnabled = enabled
        InfoSettingKey.PRIVACY_ALERTS -> detectionPrefs.privacyNotificationsEnabled = enabled
        InfoSettingKey.DRONE_ALERTS -> detectionPrefs.droneAlertsEnabled = enabled
        InfoSettingKey.HELICOPTER_ALERTS -> detectionPrefs.helicopterAlertsEnabled = enabled
        InfoSettingKey.MILITARY_ALERTS -> detectionPrefs.militaryAlertsEnabled = enabled
        InfoSettingKey.POLICE_ALERTS -> detectionPrefs.policeAlertsEnabled = enabled
        InfoSettingKey.SENSOR_BACKEND -> detectionPrefs.sensorBackendEnabled = enabled
        InfoSettingKey.BACKEND_ONLY -> detectionPrefs.backendOnlyMode = enabled
    }
    if (shouldRestartSkySourcesForInfoSetting(key, enabled, previousSettings)) {
        skyObjectRepository.restartDetectionSources()
    }
}
```

Update the existing restart-policy test to the new signature:

```kotlin
@Test
fun onlyCollectorTopologySettingsRequestASkyRestart() {
    val settings = DetectionSettings.defaults()

    assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.ADS_B, true, settings))
    assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.BLE_REMOTE_ID, true, settings))
    assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.WIFI_REMOTE_ID, true, settings))
    assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.BACKEND_ONLY, true, settings))
    assertFalse(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.STALKER, true, settings))
    assertFalse(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.ULTRASONIC, true, settings))
}
```

Disabling `SENSOR_BACKEND` restarts only when it implicitly exits an active backend-only mode; enabling it or disabling it from an ordinary mixed/local mode does not churn local collectors.

- [ ] **Step 7: Run focused tests and verify GREEN**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.data.DetectionSettingsTest' \
  --tests 'com.friendorfoe.data.DetectionPrefsTest' \
  --tests 'com.friendorfoe.presentation.about.AboutViewModelTest'
```

Expected: PASS. If an emulator is already running, also run:

```bash
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.DetectionPrefsObservableTest,com.friendorfoe.presentation.about.InfoScreenTest
```

Expected: PASS with the real preference listener and legacy normalization.

- [ ] **Step 8: Commit the preference behavior**

```bash
git add \
  android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt \
  android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt \
  android/app/src/test/java/com/friendorfoe/data/DetectionSettingsTest.kt \
  android/app/src/test/java/com/friendorfoe/data/DetectionPrefsTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/about/AboutViewModelTest.kt \
  android/app/src/androidTest/java/com/friendorfoe/data/DetectionPrefsObservableTest.kt \
  android/app/src/androidTest/java/com/friendorfoe/presentation/about/InfoScreenTest.kt
git commit -m "android: default sensor backend to opt-in"
```

---

### Task 2: Block Every Disabled Backend Request

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/BackendRequestInterceptor.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/BackendRequestInterceptorTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/di/BackendClientWiringTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/di/NetworkModule.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/di/InfoModule.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/BackendPollingGateTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/ar/ArBackendPollingTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/map/MapBackendPollingTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/BackendPrivacySourceAdapterTest.kt`

**Interfaces:**
- Consumes: `DetectionPrefs.sensorBackendEnabled`, `DetectionPrefs.backendUrl`, and `configuredBackendRequestUrl(raw, original)`.
- Produces: `BackendRequestInterceptor`, which either throws `SensorBackendDisabledException` before `chain.proceed` or rewrites/proceeds against the exact configured endpoint.

- [ ] **Step 1: Write the failing backend interceptor tests**

Create `BackendRequestInterceptorTest.kt` with a terminal interceptor that returns a synthetic response and counts downstream calls:

```kotlin
class BackendRequestInterceptorTest {
    @Test
    fun disabledBackendStopsBeforeDownstreamNetworkChain() {
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { false },
                configuredUrl = { "https://backend.example/" },
            ),
        ) { downstreamCalls++ }

        assertThrows(SensorBackendDisabledException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(0, downstreamCalls)
    }

    @Test
    fun enabledBackendUsesConfiguredOriginAndPreservesPath() {
        var observedUrl: String? = null
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { true },
                configuredUrl = { "https://field-kit.example:8443/" },
            ),
        ) { request -> observedUrl = request.url.toString() }

        client.newCall(request("http://localhost:8000/detections/drone-alerts?q=1"))
            .execute().close()

        assertEquals(
            "https://field-kit.example:8443/detections/drone-alerts?q=1",
            observedUrl,
        )
    }

    @Test
    fun invalidConfiguredUrlFailsBeforeDownstreamChain() {
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { true },
                configuredUrl = { "not a url" },
            ),
        ) { downstreamCalls++ }

        assertThrows(IOException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(0, downstreamCalls)
    }

    @Test
    fun disablingPreferenceBlocksTheNextRequestWithoutRebuildingClient() {
        var enabled = true
        var downstreamCalls = 0
        val client = testClient(
            BackendRequestInterceptor(
                enabled = { enabled },
                configuredUrl = { "https://backend.example/" },
            ),
        ) { downstreamCalls++ }

        client.newCall(request()).execute().close()
        enabled = false

        assertThrows(SensorBackendDisabledException::class.java) {
            client.newCall(request()).execute()
        }
        assertEquals(1, downstreamCalls)
    }
}
```

Use these exact no-socket helpers in the same test file:

```kotlin
private fun request(
    url: String = "http://localhost:8000/health",
): Request = Request.Builder().url(url).build()

private fun testClient(
    gate: Interceptor,
    onDownstream: (Request) -> Unit,
): OkHttpClient = OkHttpClient.Builder()
    .addInterceptor(gate)
    .addInterceptor { chain ->
        onDownstream(chain.request())
        Response.Builder()
            .request(chain.request())
            .protocol(Protocol.HTTP_1_1)
            .code(200)
            .message("OK")
            .body("{}".toResponseBody("application/json".toMediaType()))
            .build()
    }
    .build()
```

Import `okhttp3.Interceptor`, `MediaType.Companion.toMediaType`, `OkHttpClient`, `Protocol`, `Request`, `Response`, and `ResponseBody.Companion.toResponseBody`. The terminal application interceptor returns a synthetic response, so the tests create no DNS or socket traffic and precisely prove whether the guarded chain was reached.

Create `BackendClientWiringTest.kt` with a provider-level health check that remains entirely local:

```kotlin
package com.friendorfoe.di

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.BackendRequestInterceptor
import com.friendorfoe.data.SensorBackendDisabledException
import kotlinx.coroutines.test.runTest
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class BackendClientWiringTest {
    @Test
    fun backendHealthUsesTheGuardedBackendClientProvider() = runTest {
        val backendClient = NetworkModule.provideBackendOkHttpClient(
            loggingInterceptor = HttpLoggingInterceptor(),
            backendRequestInterceptor = BackendRequestInterceptor(
                enabled = { false },
                configuredUrl = { "https://backend.example/" },
            ),
        )
        val health = InfoModule.provideBackendHealthClient(backendClient)
        val endpoint = BackendEndpoint.parse("https://backend.example/").getOrThrow()

        val failure = runCatching { health.check(endpoint) }.exceptionOrNull()

        assertTrue(failure is SensorBackendDisabledException)
    }

    @Test
    fun githubUpdateRetrofitKeepsTheGeneralUnguardedClient() {
        val generalClient = OkHttpClient()

        val retrofit = InfoModule.provideAppUpdateRetrofit(generalClient)

        assertSame(generalClient, retrofit.callFactory())
        assertEquals("https://api.github.com/", retrofit.baseUrl().toString())
    }
}
```

The production provider creates the client and `HttpBackendHealthClient` issues a real OkHttp call object, but the disabled guard throws before the chain can perform DNS or socket work. Hilt's generated-component compilation remains the qualifier wiring check.

- [ ] **Step 2: Add the initial-off polling contract**

Add to `BackendPollingGateTest`:

```kotlin
@Test
fun initiallyDisabledBackendDoesNotFetchUntilExplicitlyEnabled() = runTest {
    val settings = MutableStateFlow(DetectionSettings.defaults())
    var fetches = 0
    val job = launch {
        collectBackendWhileEnabled(
            settings = settings,
            intervalMs = 5_000,
            clear = {},
            fetch = { ++fetches },
            publish = {},
        )
    }

    runCurrent()
    assertEquals(0, fetches)

    settings.value = settings.value.copy(sensorBackendEnabled = true)
    runCurrent()
    assertEquals(1, fetches)
    job.cancel()
}
```

- [ ] **Step 3: Run focused tests and verify RED**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.data.BackendRequestInterceptorTest' \
  --tests 'com.friendorfoe.di.BackendClientWiringTest' \
  --tests 'com.friendorfoe.presentation.BackendPollingGateTest'
```

Expected: compilation fails because `BackendRequestInterceptor` and `SensorBackendDisabledException` do not exist. After creating only empty declarations, the tests must fail because disabled requests still proceed or no URL rewrite occurs.

- [ ] **Step 4: Implement the fail-closed interceptor**

Create `BackendRequestInterceptor.kt`:

```kotlin
package com.friendorfoe.data

import java.io.IOException
import okhttp3.Interceptor
import okhttp3.Response

internal class SensorBackendDisabledException : IOException(
    "Sensor backend is disabled",
)

internal class BackendRequestInterceptor(
    private val enabled: () -> Boolean,
    private val configuredUrl: () -> String,
) : Interceptor {
    constructor(prefs: DetectionPrefs) : this(
        enabled = { prefs.sensorBackendEnabled },
        configuredUrl = { prefs.backendUrl },
    )

    override fun intercept(chain: Interceptor.Chain): Response {
        if (!enabled()) throw SensorBackendDisabledException()
        val original = chain.request()
        val rewritten = configuredBackendRequestUrl(configuredUrl(), original.url)
        return chain.proceed(original.newBuilder().url(rewritten).build())
    }
}
```

The preference lambdas are evaluated per request; do not snapshot them in the constructor.

- [ ] **Step 5: Install the guard only on Friend or Foe backend traffic**

In `NetworkModule.kt`, replace `provideBackendUrlInterceptor` with:

```kotlin
@Provides
@Singleton
@Named("backendRequestInterceptor")
fun provideBackendRequestInterceptor(detectionPrefs: DetectionPrefs): Interceptor =
    BackendRequestInterceptor(detectionPrefs)
```

Inject it into `provideBackendOkHttpClient` and keep it before logging:

```kotlin
fun provideBackendOkHttpClient(
    loggingInterceptor: HttpLoggingInterceptor,
    @Named("backendRequestInterceptor") backendRequestInterceptor: Interceptor,
): OkHttpClient = OkHttpClient.Builder()
    .addInterceptor(backendRequestInterceptor)
    .addInterceptor(loggingInterceptor)
    .connectTimeout(8, TimeUnit.SECONDS)
    .readTimeout(8, TimeUnit.SECONDS)
    .writeTimeout(8, TimeUnit.SECONDS)
    .build()
```

In `InfoModule.kt`, route only backend health through the guarded client:

```kotlin
@Provides
@Singleton
fun provideBackendHealthClient(
    @Named("backendClient") okHttpClient: OkHttpClient,
): BackendHealthClient = HttpBackendHealthClient(okHttpClient)
```

Leave `provideAppUpdateRetrofit(okHttpClient: OkHttpClient)` unchanged so GitHub update discovery never depends on sensor-backend consent.

- [ ] **Step 6: Update existing positive polling fixtures for the new default**

Every existing test that intends to exercise a live backend must opt in explicitly. Replace positive fixtures such as:

```kotlin
MutableStateFlow(DetectionSettings.defaults())
```

with:

```kotlin
MutableStateFlow(
    DetectionSettings.defaults().copy(sensorBackendEnabled = true),
)
```

Apply this only to positive backend tests in `BackendPollingGateTest`, `ArBackendPollingTest`, `MapBackendPollingTest`, and `BackendPrivacySourceAdapterTest`. In the large Privacy test file, add and consistently use:

```kotlin
private fun enabledBackendSettings(
    backendUrl: String = DetectionSettings.defaults().backendUrl,
) = DetectionSettings.defaults().copy(
    sensorBackendEnabled = true,
    backendUrl = backendUrl,
)
```

Retain the new initial-off test on unmodified defaults. Do not globally change non-backend tests to enabled.

- [ ] **Step 7: Add domain-level no-fetch assertions**

Add to `ArBackendPollingTest`:

```kotlin
@Test
fun initiallyDisabledBackendDoesNotFetch() = runTest {
    val settings = MutableStateFlow(DetectionSettings.defaults())
    val state = ArBackendIntegrationState()
    var fetches = 0
    val job = launch {
        collectArBackend(
            settings = settings,
            intervalMs = 100,
            state = state,
            fetchDroneCount = {
                fetches++
                0
            },
        )
    }

    runCurrent()

    assertEquals(0, fetches)
    assertFalse(state.sensorBackendOnline.value)
    job.cancel()
}
```

Add to `MapBackendPollingTest`:

```kotlin
@Test
fun initiallyDisabledBackendDoesNotFetch() = runTest {
    val settings = MutableStateFlow(DetectionSettings.defaults())
    val state = MapBackendIntegrationState(
        MutableStateFlow<List<SkyObject>>(emptyList()),
    )
    var fetches = 0
    val job = launch {
        collectMapBackend(
            settings = settings,
            intervalMs = 100,
            state = state,
            fetchSnapshot = {
                fetches++
                snapshot("unexpected", 0)
            },
        )
    }

    runCurrent()

    assertEquals(0, fetches)
    assertFalse(state.sensorMapOnline.value)
    job.cancel()
}
```

Add to `BackendPrivacySourceAdapterTest`:

```kotlin
@Test
fun initiallyDisabledBackendIsPausedWithoutFetching() = runTest {
    var fetches = 0
    val adapter = BackendPrivacySourceAdapter(
        settings = MutableStateFlow(DetectionSettings.defaults()),
        fetch = {
            fetches++
            LivePrivacyDevicesDto(devices = emptyList())
        },
        clock = FakeClock(),
        scope = backgroundScope,
    )

    runCurrent()

    assertEquals(0, fetches)
    assertEquals(SourceHealthState.PAUSED, adapter.snapshot().health.state)
}
```

- [ ] **Step 8: Run backend tests and verify GREEN**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.data.BackendRequestInterceptorTest' \
  --tests 'com.friendorfoe.data.BackendEndpointTest' \
  --tests 'com.friendorfoe.di.BackendClientWiringTest' \
  --tests 'com.friendorfoe.di.InfoModuleTest' \
  --tests 'com.friendorfoe.presentation.BackendPollingGateTest' \
  --tests 'com.friendorfoe.presentation.ar.ArBackendPollingTest' \
  --tests 'com.friendorfoe.presentation.map.MapBackendPollingTest' \
  --tests 'com.friendorfoe.presentation.privacy.BackendPrivacySourceAdapterTest'
```

Expected: PASS with zero downstream calls while disabled, exact rewrite while enabled, and unchanged endpoint-replacement cancellation behavior.

- [ ] **Step 9: Commit backend request enforcement**

```bash
git add \
  android/app/src/main/java/com/friendorfoe/data/BackendRequestInterceptor.kt \
  android/app/src/test/java/com/friendorfoe/di/BackendClientWiringTest.kt \
  android/app/src/main/java/com/friendorfoe/di/NetworkModule.kt \
  android/app/src/main/java/com/friendorfoe/di/InfoModule.kt \
  android/app/src/test/java/com/friendorfoe/data/BackendRequestInterceptorTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/BackendPollingGateTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/ar/ArBackendPollingTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/map/MapBackendPollingTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/privacy/BackendPrivacySourceAdapterTest.kt
git commit -m "android: block disabled backend requests"
```

---

### Task 3: Restore Aircraft Media to Full Detail

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/detail/AircraftPhotoCard.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/util/AircraftPhotosTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailPresentation.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailOverviewContent.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/detail/DetailPresentationTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/presentation/detail/DetailOverviewContentTest.kt`

**Interfaces:**
- Consumes: `Aircraft.photoUrl`, enriched/local aircraft type and description, `HistoryEntity.photoUrl`, `ObjectCategory`, `getAircraftPhotoUrl`, and existing silhouette helpers.
- Produces: `AircraftVisual(photoUrl, typeCode, description, category)` and `AircraftPhotoCard(visual, modifier)`; `DetailPresentation.aircraftVisual` is null for non-aircraft content.

- [ ] **Step 1: Write failing presentation-model tests**

Add to `DetailPresentationTest`:

```kotlin
@Test
fun liveAircraftCarriesPhotoEvidenceIntoTheNewDetailModel() {
    val model = presentLiveDetail(
        aircraft = aircraft().copy(photoUrl = "https://images.example/live.jpg"),
        remoteDetail = AircraftDetailDto(
            icaoHex = "abc123",
            callsign = "FOF42",
            registration = "N42FO",
            aircraftType = "B738",
            aircraftDescription = "Boeing 737-800",
            operator = null,
            photo = null,
            route = null,
            country = null,
        ),
        remoteFailure = null,
    )

    assertEquals(
        AircraftVisual(
            photoUrl = "https://images.example/live.jpg",
            typeCode = "B738",
            description = "Boeing 737-800",
            category = ObjectCategory.COMMERCIAL,
        ),
        model.aircraftVisual,
    )
}

@Test
fun historicalAircraftUsesOnlySavedPhotoAndCategoryEvidence() {
    val row = history(id = 12, objectId = "abc").copy(
        objectType = "aircraft",
        category = "commercial",
        photoUrl = "https://images.example/saved.jpg",
    )

    assertEquals(
        AircraftVisual(
            photoUrl = "https://images.example/saved.jpg",
            typeCode = null,
            description = null,
            category = ObjectCategory.COMMERCIAL,
        ),
        presentHistoricalDetail(row).aircraftVisual,
    )
    assertNull(presentHistoricalDetail(history(id = 13, objectId = "drone")).aircraftVisual)
    assertNull(presentLiveDroneDetail(drone()).aircraftVisual)
}
```

- [ ] **Step 2: Write failing bundled resolver tests**

Create `AircraftPhotosTest.kt`:

```kotlin
class AircraftPhotosTest {
    @Test
    fun normalizesExactBundledType() {
        assertEquals(
            "file:///android_asset/aircraft/B738.jpg",
            getAircraftPhotoUrl(" b738 "),
        )
    }

    @Test
    fun databaseAliasUsesItsRepresentativeBundledPhoto() {
        assertEquals(
            "file:///android_asset/aircraft/B737.jpg",
            getAircraftPhotoUrl("B736"),
        )
    }

    @Test
    fun missingTypeDoesNotInventAPhoto() {
        assertNull(getAircraftPhotoUrl(null))
        assertNull(getAircraftPhotoUrl("ZZZZ"))
    }
}
```

The exact and null tests may already pass; the red requirement for this task comes from the missing `AircraftVisual` model and renderer. Keep these resolver tests as regression coverage for the reused asset path.

- [ ] **Step 3: Write the failing Compose image-card test**

Add to `DetailOverviewContentTest`:

```kotlin
@Test
fun knownAircraftTypeLoadsItsBundledPhoto() {
    compose.setContent {
        FriendOrFoeTheme {
            DetailOverviewContent(
                model = model(isLive = true).copy(
                    aircraftVisual = AircraftVisual(
                        photoUrl = null,
                        typeCode = "B738",
                        description = "Boeing 737-800",
                        category = ObjectCategory.COMMERCIAL,
                    ),
                ),
            )
        }
    }

    compose.onNodeWithTag("detail_aircraft_photo").assertIsDisplayed()
    compose.waitUntil(timeoutMillis = 5_000) {
        compose.onAllNodesWithTag(
            "detail_aircraft_photo_image",
            useUnmergedTree = true,
        )
            .fetchSemanticsNodes().isNotEmpty()
    }
    compose.onNodeWithTag(
        "detail_aircraft_photo_image",
        useUnmergedTree = true,
    ).assertIsDisplayed()
}

@Test
fun unknownAircraftTypeKeepsAVisibleSilhouette() {
    compose.setContent {
        FriendOrFoeTheme {
            DetailOverviewContent(
                model = model(isLive = true).copy(
                    aircraftVisual = AircraftVisual(
                        photoUrl = null,
                        typeCode = "ZZZZ",
                        description = null,
                        category = ObjectCategory.COMMERCIAL,
                    ),
                ),
            )
        }
    }

    compose.onNodeWithTag(
        "detail_aircraft_silhouette",
        useUnmergedTree = true,
    ).assertIsDisplayed()
    compose.onNodeWithTag(
        "detail_aircraft_photo_image",
        useUnmergedTree = true,
    ).assertDoesNotExist()
}
```

Also assert `detail_aircraft_photo` does not exist for the existing model with `aircraftVisual = null`. Import `onAllNodesWithTag`; the B738 test is the deterministic proof that Coil decodes the packaged asset rather than merely displaying the card container.

- [ ] **Step 4: Run focused tests and verify RED**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.presentation.detail.DetailPresentationTest' \
  --tests 'com.friendorfoe.presentation.util.AircraftPhotosTest'
```

Expected: compilation fails because `AircraftVisual` and `DetailPresentation.aircraftVisual` do not exist. Run the connected `DetailOverviewContentTest` if an emulator is available; it must fail for the same missing API.

- [ ] **Step 5: Carry aircraft visual evidence in `DetailPresentation`**

Add the model, then append the optional field to the existing `DetailPresentation` constructor after `rawExpandedByDefault`:

```kotlin
data class AircraftVisual(
    val photoUrl: String?,
    val typeCode: String?,
    val description: String?,
    val category: ObjectCategory,
)

val aircraftVisual: AircraftVisual? = null,
```

In `presentLiveDetail`, populate it from the already resolved local/enriched variables:

```kotlin
aircraftVisual = AircraftVisual(
    photoUrl = aircraft.photoUrl,
    typeCode = aircraftType,
    description = model,
    category = aircraft.category,
),
```

Convert `presentHistoricalDetail` from an expression body to a block, parse the saved category once, and pass that same value to the visual:

```kotlin
fun presentHistoricalDetail(row: HistoryEntity): DetailPresentation {
    val category = historicalCategory(row.category)
    return DetailPresentation(
        title = row.displayName.takeIf(String::isNotBlank) ?: row.objectId,
        statusLabel = "Historical detection",
        isLive = false,
        summary = listOf(
            DetailField("Source", historicalSourceLabel(row.detectionSource)),
            DetailField("Category", historyCategoryLabel(row.category)),
            DetailField("Observed", formatDetailInstant(Instant.ofEpochMilli(row.lastSeen))),
        ),
        identifiers = listOfNotNull(
            identifier("Object ID", row.objectId),
        ),
        advanced = buildList {
            row.description?.takeIf(String::isNotBlank)?.let {
                add(DetailField("Description", it))
            }
            row.distanceMeters?.let {
                add(DetailField("Distance at observation", formatDetailDistance(it)))
            }
            formatKnownPosition(row.latitude, row.longitude)?.let {
                add(DetailField("Position at observation", it))
            }
            add(DetailField("Altitude at observation", formatAltitude(row.altitudeMeters)))
            add(DetailField("Confidence", formatConfidence(row.confidence)))
        },
        raw = listOf(
            DetailField("History record", row.id.toString()),
            DetailField("Object type", row.objectType),
            DetailField("First observed", formatDetailInstant(Instant.ofEpochMilli(row.firstSeen))),
            DetailField("Last observed", formatDetailInstant(Instant.ofEpochMilli(row.lastSeen))),
        ) + listOfNotNull(
            formatKnownPosition(row.userLatitude, row.userLongitude)?.let {
                DetailField("Phone position at observation", it)
            },
        ),
        retryLabel = null,
        aircraftVisual = if (row.objectType.equals("aircraft", ignoreCase = true)) {
            AircraftVisual(
                photoUrl = row.photoUrl?.takeIf(String::isNotBlank),
                typeCode = null,
                description = null,
                category = category,
            )
        } else {
            null
        },
    )
}
```

Add this parser beside `historyCategoryLabel`; retain the existing label function's raw-string fallback so an unrecognized historical value remains readable while its silhouette safely uses `UNKNOWN`:

```kotlin
private fun historicalCategory(raw: String): ObjectCategory = ObjectCategory.entries
    .firstOrNull { it.name.equals(raw.trim(), ignoreCase = true) }
    ?: ObjectCategory.UNKNOWN
```

- [ ] **Step 6: Extract and reuse the existing photo renderer**

Move the existing `PhotoPlaceholder` and `SilhouetteFallback` behavior from `DetailScreen.kt` into `AircraftPhotoCard.kt`, using this public boundary:

```kotlin
@Composable
internal fun AircraftPhotoCard(
    visual: AircraftVisual,
    modifier: Modifier = Modifier,
) {
    val silhouette = silhouetteForTypeCode(visual.typeCode)
        ?: silhouetteForCategory(visual.category)
    val drawableRes = silhouetteDrawableRes(silhouette)
    val tintColor = categoryColor(visual.category)
    val imageUrl = visual.photoUrl?.takeIf(String::isNotBlank)
        ?: getAircraftPhotoUrl(visual.typeCode)

    Card(
        modifier = modifier
            .fillMaxWidth()
            .height(180.dp)
            .testTag("detail_aircraft_photo"),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            if (imageUrl != null) {
                SubcomposeAsyncImage(
                    model = imageUrl,
                    contentDescription = visual.description
                        ?: visual.typeCode
                        ?: "Aircraft",
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                ) {
                    when (painter.state) {
                        is AsyncImagePainter.State.Success -> {
                            SubcomposeAsyncImageContent(
                                modifier = Modifier
                                    .fillMaxSize()
                                    .clip(RoundedCornerShape(12.dp))
                                    .testTag("detail_aircraft_photo_image"),
                            )
                        }
                        is AsyncImagePainter.State.Loading -> {
                            AircraftSilhouetteFallback(
                                drawableRes,
                                tintColor,
                                visual.typeCode,
                            )
                        }
                        else -> {
                            AircraftSilhouetteFallback(
                                drawableRes,
                                tintColor,
                                visual.typeCode,
                            )
                        }
                    }
                }
            } else {
                AircraftSilhouetteFallback(
                    drawableRes,
                    tintColor,
                    visual.typeCode,
                )
            }
        }
    }
}

@Composable
private fun AircraftSilhouetteFallback(
    drawableRes: Int,
    tintColor: Color,
    aircraftType: String?,
) {
    Column(
        modifier = Modifier.testTag("detail_aircraft_silhouette"),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Image(
            painter = painterResource(id = drawableRes),
            contentDescription = aircraftType ?: "Aircraft",
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .height(120.dp),
            contentScale = ContentScale.Fit,
            colorFilter = ColorFilter.tint(tintColor),
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = aircraftType ?: "Unknown",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
        )
    }
}
```

This is the extracted existing success/loading/error behavior with one added test tag. Keep these branches intact so loading, an invalid remote URL, and an unknown bundled type all retain visible aircraft evidence.

Update `AircraftDetailContent` in `DetailScreen.kt` to call:

```kotlin
AircraftPhotoCard(
    AircraftVisual(
        photoUrl = aircraft.photoUrl,
        typeCode = detail?.aircraftType ?: aircraft.aircraftType,
        description = detail?.aircraftDescription ?: aircraft.aircraftModel,
        category = aircraft.category,
    ),
)
```

Remove only imports made unused by extracting the old private functions.

- [ ] **Step 7: Render media in the new full-detail layout**

In `DetailOverviewContent`, directly after the title/supporting-message block and before “At a glance”:

```kotlin
model.aircraftVisual?.let { visual ->
    AircraftPhotoCard(visual)
}
```

No call-site flag is needed: the presentation model is the single source of truth, which prevents the same regression from recurring when `DetailScreen` changes branches.

- [ ] **Step 8: Run focused photo tests and verify GREEN**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.presentation.detail.DetailPresentationTest' \
  --tests 'com.friendorfoe.presentation.util.AircraftPhotosTest'
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.detail.DetailOverviewContentTest
```

Expected: JVM and Compose tests PASS; B738 reaches `detail_aircraft_photo_image`, ZZZZ reaches `detail_aircraft_silhouette`, and the card is absent without aircraft visual data. If no emulator is available, record the connected test as pending for Task 5 rather than treating it as passed.

- [ ] **Step 9: Commit aircraft media restoration**

```bash
git add \
  android/app/src/main/java/com/friendorfoe/presentation/detail/AircraftPhotoCard.kt \
  android/app/src/main/java/com/friendorfoe/presentation/detail/DetailPresentation.kt \
  android/app/src/main/java/com/friendorfoe/presentation/detail/DetailOverviewContent.kt \
  android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt \
  android/app/src/test/java/com/friendorfoe/presentation/detail/DetailPresentationTest.kt \
  android/app/src/test/java/com/friendorfoe/presentation/util/AircraftPhotosTest.kt \
  android/app/src/androidTest/java/com/friendorfoe/presentation/detail/DetailOverviewContentTest.kt
git commit -m "android: restore aircraft detail photos"
```

---

### Task 4: Restore GitHub Update Status to the About Landing

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/about/AppUpdateRow.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutLandingScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/about/AboutViewModelTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/presentation/about/AboutLandingScreenTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/presentation/navigation/NavigationShellTest.kt`

**Interfaces:**
- Consumes: existing `UpdateUiState`, `InfoUiState.installedVersion`, `AboutViewModel.checkForUpdates()`, `AppUpdateRepository`, and safe `Context.openUri(...)`.
- Produces: `AboutViewModel.checkForUpdatesIfIdle()`, shared `AppUpdateRow(...)`, and an About landing that starts one GitHub check per graph-scoped ViewModel lifecycle.

- [ ] **Step 1: Write failing ViewModel tests for automatic and newer-release behavior**

Add a counting repository in `AboutViewModelTest`:

```kotlin
private class CountingUpdateRepository(
    private val result: Result<AppUpdateMetadata>,
) : AppUpdateRepository {
    var calls = 0
    override suspend fun latest(): Result<AppUpdateMetadata> {
        calls++
        return result
    }
}
```

Add tests:

```kotlin
@Test
fun idleUpdateCheckRunsOnlyOnceForTheViewModelLifecycle() = runTest {
    val repository = CountingUpdateRepository(Result.success(
        AppUpdateMetadata(
            AppVersion(null, "0.67.7"),
            "https://github.com/lnxgod/friendorfoe/releases/tag/v0.67.7",
        ),
    ))
    val viewModel = viewModel(
        settings = FakeInfoSettingsStore(DetectionSettings.defaults()),
        session = sessionRepository(),
        updateRepository = repository,
        installed = AppVersion(120, "0.67.7-android-ar-overlay-range"),
    )

    viewModel.checkForUpdatesIfIdle()
    viewModel.checkForUpdatesIfIdle()
    advanceUntilIdle()
    viewModel.checkForUpdatesIfIdle()

    assertEquals(1, repository.calls)
    assertTrue(viewModel.uiState.value.updateState is UpdateUiState.UpToDate)
}

@Test
fun genuinelyNewerGitHubReleaseBecomesAvailable() = runTest {
    val remote = AppUpdateMetadata(
        AppVersion(null, "0.68.0"),
        "https://github.com/lnxgod/friendorfoe/releases/tag/v0.68.0",
    )
    val viewModel = viewModel(
        settings = FakeInfoSettingsStore(DetectionSettings.defaults()),
        session = sessionRepository(),
        updateRepository = FixedUpdateRepository(Result.success(remote)),
        installed = AppVersion(120, "0.67.7-android-ar-overlay-range"),
    )

    viewModel.checkForUpdatesIfIdle()
    advanceUntilIdle()

    assertEquals(UpdateUiState.Available(remote), viewModel.uiState.value.updateState)
}
```

- [ ] **Step 2: Write failing About landing state/action tests**

Extend `AboutLandingScreenTest` with:

```kotlin
@Test
fun landingShowsAndOpensAvailableGitHubUpdate() {
    var opened: String? = null
    val remote = AppUpdateMetadata(
        AppVersion(null, "0.68.0"),
        "https://github.com/lnxgod/friendorfoe/releases/tag/v0.68.0",
    )

    compose.setContent {
        FriendOrFoeTheme {
            AboutLandingScreen(
                installedVersionName = "0.67.7-android-ar-overlay-range",
                updateState = UpdateUiState.Available(remote),
                actions = AboutLandingActions(onOpenUpdate = { opened = it }),
            )
        }
    }

    compose.onNodeWithText("Version 0.67.7-android-ar-overlay-range")
        .performScrollTo().assertIsDisplayed()
    compose.onNodeWithText("Version 0.68.0").performScrollTo().assertIsDisplayed()
    compose.onNodeWithTag("about_open_update")
        .performScrollTo().assertIsDisplayed().performClick()
    compose.onNodeWithText("Update available").assertIsDisplayed()
    compose.runOnIdle { assertEquals(remote.releaseUrl, opened) }
}

@Test
fun landingShowsCheckingWithoutASecondAction() {
    compose.setContent {
        FriendOrFoeTheme {
            AboutLandingScreen(
                actions = AboutLandingActions(),
                updateState = UpdateUiState.Checking,
            )
        }
    }

    compose.onNodeWithTag("about_check_updates").performScrollTo().assertIsNotEnabled()
    compose.onNodeWithText("Checking for updates").assertIsDisplayed()
}

@Test
fun landingShowsUpToDateAndAllowsCheckAgain() {
    var checks = 0
    compose.setContent {
        FriendOrFoeTheme {
            AboutLandingScreen(
                actions = AboutLandingActions(onCheckForUpdates = { checks++ }),
                updateState = UpdateUiState.UpToDate(AppVersion(120, "0.67.7")),
            )
        }
    }

    compose.onNodeWithTag("about_check_updates").performScrollTo().performClick()
    compose.onNodeWithText("Up to date").assertIsDisplayed()
    compose.runOnIdle { assertEquals(1, checks) }
}

@Test
fun landingFailureShowsRetryAndInvokesCallback() {
    var checks = 0
    compose.setContent {
        FriendOrFoeTheme {
            AboutLandingScreen(
                actions = AboutLandingActions(onCheckForUpdates = { checks++ }),
                updateState = UpdateUiState.Failed("Could not check for updates"),
            )
        }
    }

    compose.onNodeWithTag("about_check_updates").performScrollTo().performClick()
    compose.onNodeWithText("Could not check for updates").assertIsDisplayed()
    compose.runOnIdle { assertEquals(1, checks) }
}
```

Keep the existing About identity/actions test unchanged. Import `assertIsNotEnabled`, `performScrollTo`, `AppVersion`, and `AppUpdateMetadata` for the new cases.

- [ ] **Step 3: Write failing production-route Compose coverage**

Add this sibling test to `NavigationShellTest`:

```kotlin
@Test
fun productionAboutUpdateActionUsesTheReleaseUrl() {
    var opened: String? = null
    val remote = AppUpdateMetadata(
        version = AppVersion(null, "0.68.0"),
        releaseUrl = "https://github.com/lnxgod/friendorfoe/releases/tag/v0.68.0",
    )

    compose.setContent {
        FriendOrFoeTheme {
            val navController = rememberNavController()
            AboutTopLevelRoute(
                navController = navController,
                state = InfoUiState(
                    installedVersion = AppVersion(120, "0.67.7"),
                    updateState = UpdateUiState.Available(remote),
                ),
                onOpenUpdate = { opened = it },
            )
        }
    }

    compose.onNodeWithTag("about_open_update").performScrollTo().performClick()
    compose.runOnIdle { assertEquals(remote.releaseUrl, opened) }
}

@Test
fun productionAboutRouteStartsIdleCheckOncePerCompositionEntry() {
    var checks = 0
    val installedName = mutableStateOf("0.67.7")

    compose.setContent {
        FriendOrFoeTheme {
            val navController = rememberNavController()
            AboutTopLevelRoute(
                navController = navController,
                state = InfoUiState(
                    installedVersion = AppVersion(120, installedName.value),
                ),
                onCheckForUpdatesIfIdle = { checks++ },
            )
        }
    }

    compose.waitForIdle()
    compose.runOnIdle { assertEquals(1, checks) }
    compose.runOnUiThread { installedName.value = "0.67.8" }
    compose.waitForIdle()
    compose.runOnIdle { assertEquals(1, checks) }
}

@Test
fun aboutAndSettingsResolveTheSameMainGraphScopeOwner() {
    var aboutOwner: Any? = null
    var settingsOwner: Any? = null

    compose.setContent {
        FriendOrFoeTheme {
            val navController = rememberNavController()
            NavHost(navController, startDestination = "main_graph") {
                navigation(
                    route = "main_graph",
                    startDestination = Screen.About.route,
                ) {
                    composable(Screen.About.route) {
                        aboutOwner = mainGraphAboutOwner(navController)
                        Button(onClick = { navController.navigate(Screen.AboutSettings.route) }) {
                            Text("Open settings owner")
                        }
                    }
                    composable(Screen.AboutSettings.route) {
                        settingsOwner = mainGraphAboutOwner(navController)
                        Text("Settings owner", Modifier.testTag("test_settings_owner"))
                    }
                }
            }
        }
    }

    compose.onNodeWithText("Open settings owner").performClick()
    compose.onNodeWithTag("test_settings_owner").assertIsDisplayed()
    compose.runOnIdle {
        assertNotNull(aboutOwner)
        assertSame(aboutOwner, settingsOwner)
    }
}
```

Use the file's existing Compose rule and navigation imports. Add imports for `AppVersion`, `AppUpdateMetadata`, `InfoUiState`, `UpdateUiState`, `performScrollTo`, `assertNotNull`, and `assertSame`. The owner test verifies the navigation-scope contract used by both production destinations; it does not claim to instantiate Hilt in the test process. These tests must be written before changing the production route. They initially fail to compile because the route has no update state, injectable idle callback, release opener, or shared-owner helper.

- [ ] **Step 4: Run focused tests and verify RED**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.presentation.about.AboutViewModelTest'
```

Expected: compilation fails because `checkForUpdatesIfIdle()` is missing. If an emulator is available, also run and record the expected connected-test compile failure:

```bash
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.AboutLandingScreenTest,com.friendorfoe.presentation.navigation.NavigationShellTest
```

The connected tests are red because landing state/actions, the injectable route callbacks, and `mainGraphAboutOwner` are missing.

- [ ] **Step 5: Add the idempotent ViewModel entry point**

In `AboutViewModel`:

```kotlin
fun checkForUpdatesIfIdle() {
    if (updateState.value == UpdateUiState.Idle) checkForUpdates()
}
```

Keep `checkForUpdates()` as the explicit retry/check-again path. It sets `Checking` synchronously before launching, so consecutive idle checks cannot start two repository jobs.

- [ ] **Step 6: Extract one reusable update row**

Create `AppUpdateRow.kt` and move the existing `UpdateRow` rendering into this signature:

```kotlin
@Composable
internal fun AppUpdateRow(
    update: UpdateUiState,
    onCheck: () -> Unit,
    onOpen: (String) -> Unit,
    testTagPrefix: String,
) {
    val checkTag = "${testTagPrefix}_check_updates"
    when (update) {
        UpdateUiState.Idle -> FofActionRow(
            title = "App updates",
            description = "Check the official GitHub release feed",
            trailingLabel = "Check",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        UpdateUiState.Checking -> FofActionRow(
            title = "Checking for updates",
            description = "Comparing ordered app versions",
            trailingLabel = "Checking…",
            enabled = false,
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        is UpdateUiState.UpToDate -> FofActionRow(
            title = "Up to date",
            description = "Version ${update.installed.name} is not older than the latest release",
            trailingLabel = "Check again",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        is UpdateUiState.Available -> FofActionRow(
            title = "Update available",
            description = "Version ${update.remote.version.name}",
            trailingLabel = "Open",
            onClick = { onOpen(update.remote.releaseUrl) },
            modifier = Modifier.testTag("${testTagPrefix}_open_update"),
        )
        is UpdateUiState.Failed -> FofActionRow(
            title = update.message,
            description = "Check your network and try again. Your installed app is unchanged.",
            trailingLabel = "Retry",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
    }
}
```

Replace the private `UpdateRow` call in `AboutScreen.kt` with:

```kotlin
AppUpdateRow(
    update = state.updateState,
    onCheck = actions.onCheckForUpdates,
    onOpen = actions.onOpenUpdate,
    testTagPrefix = "info",
)
```

Delete the old private renderer after both call sites compile.

- [ ] **Step 7: Render update state on the About landing**

Extend `AboutLandingActions` and `AboutLandingScreen`:

```kotlin
data class AboutLandingActions(
    val onOpenSettings: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onContactSupport: () -> Unit = {},
    val onOpenGithub: () -> Unit = {},
    val onCheckForUpdates: () -> Unit = {},
    val onOpenUpdate: (String) -> Unit = {},
)

@Composable
fun AboutLandingScreen(
    actions: AboutLandingActions,
    installedVersionName: String = BuildConfig.VERSION_NAME,
    updateState: UpdateUiState = UpdateUiState.Idle,
    modifier: Modifier = Modifier,
)
```

Replace the final standalone version text with a `FofSection(title = "Version & updates")` containing the version and:

```kotlin
AppUpdateRow(
    update = updateState,
    onCheck = actions.onCheckForUpdates,
    onOpen = actions.onOpenUpdate,
    testTagPrefix = "about",
)
```

Keep the About identity, evidence caveat, settings button, and helpful links unchanged.

- [ ] **Step 8: Graph-scope and share `AboutViewModel`**

In `FriendOrFoeNavGraph.kt`, add:

```kotlin
internal fun mainGraphAboutOwner(
    navController: NavHostController,
): ViewModelStoreOwner = navController.getBackStackEntry(MAIN_GRAPH_ROUTE)

@Composable
private fun mainGraphAboutViewModel(
    navController: NavHostController,
): AboutViewModel {
    val owner = remember(navController) {
        mainGraphAboutOwner(navController)
    }
    return hiltViewModel(owner)
}
```

Add imports for `com.friendorfoe.BuildConfig`, `InfoUiState`, `LaunchedEffect`, `getValue`, `remember`, `ViewModelStoreOwner`, and `collectAsStateWithLifecycle`. The non-composable owner helper is intentionally `internal` so navigation coverage can prove both destinations resolve the same graph scope without introducing a Hilt test application.

Inside the production `Screen.About` composable:

```kotlin
val viewModel = mainGraphAboutViewModel(navController)
val state by viewModel.uiState.collectAsStateWithLifecycle()
AboutTopLevelRoute(
    navController = navController,
    state = state,
    onCheckForUpdates = viewModel::checkForUpdates,
    onCheckForUpdatesIfIdle = viewModel::checkForUpdatesIfIdle,
)
```

Change `AboutTopLevelRoute` to accept testable defaults:

```kotlin
internal fun AboutTopLevelRoute(
    navController: NavHostController,
    state: InfoUiState = InfoUiState(),
    onCheckForUpdates: () -> Unit = {},
    onCheckForUpdatesIfIdle: () -> Unit = {},
    onOpenUpdate: ((String) -> Unit)? = null,
)
```

At the start of the route, trigger the injectable idle check once for that composition entry, then resolve the production opener and pass all update inputs to `AboutLandingScreen`:

```kotlin
LaunchedEffect(Unit) { onCheckForUpdatesIfIdle() }
val openUpdate = onOpenUpdate ?: { url: String -> context.openUri(url) }
AboutLandingScreen(
    installedVersionName = state.installedVersion.name
        .ifBlank { BuildConfig.VERSION_NAME },
    updateState = state.updateState,
    actions = AboutLandingActions(
        onOpenSettings = {
            navController.navigate(Screen.AboutSettings.route) { launchSingleTop = true }
        },
        onOpenReference = {
            navController.navigate(REFERENCE_GUIDE_BASE_ROUTE) { launchSingleTop = true }
        },
        onContactSupport = {
            context.openUri("mailto:lnxgod@gmail.com?subject=Friend%20or%20Foe%20feedback")
        },
        onOpenGithub = {
            context.openUri("https://github.com/lnxgod/friendorfoe")
        },
        onCheckForUpdates = onCheckForUpdates,
        onOpenUpdate = openUpdate,
    ),
)
```

Inside the production `Screen.AboutSettings` composable, obtain the same `mainGraphAboutViewModel(navController)` and pass it to `AboutSettingsRoute`. Do not call `checkForUpdatesIfIdle()` from settings; it observes the result started by the landing. Generated Hilt compilation verifies both destinations accept the graph owner; Task 5's emulator walk confirms the two production screens display the same resolved update state.

- [ ] **Step 9: Run focused update tests and verify GREEN**

Run:

```bash
./gradlew testDebugUnitTest \
  --tests 'com.friendorfoe.data.AppVersionTest' \
  --tests 'com.friendorfoe.data.repository.AppUpdateRepositoryTest' \
  --tests 'com.friendorfoe.presentation.about.AboutViewModelTest'
./gradlew connectedDebugAndroidTest \
  -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.AboutLandingScreenTest,com.friendorfoe.presentation.navigation.NavigationShellTest
```

Expected: PASS. The GitHub retrofit origin test in `InfoModuleTest` must remain unchanged and green.

- [ ] **Step 10: Commit About update restoration**

```bash
git add \
  android/app/src/main/java/com/friendorfoe/presentation/about/AppUpdateRow.kt \
  android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt \
  android/app/src/main/java/com/friendorfoe/presentation/about/AboutLandingScreen.kt \
  android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt \
  android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt \
  android/app/src/test/java/com/friendorfoe/presentation/about/AboutViewModelTest.kt \
  android/app/src/androidTest/java/com/friendorfoe/presentation/about/AboutLandingScreenTest.kt \
  android/app/src/androidTest/java/com/friendorfoe/presentation/navigation/NavigationShellTest.kt
git commit -m "android: restore About update discovery"
```

---

### Task 5: Integrated Android Verification

**Files:**
- Verify only: all Android source and test files changed in Tasks 1–4
- Do not modify: Android release metadata, backend, or firmware

**Interfaces:**
- Consumes: the four independently committed deliverables.
- Produces: one verified debug APK with recorded unit, Compose, build, and emulator evidence.

- [ ] **Step 1: Confirm implementation scope and diff hygiene**

Run from the repository root:

```bash
git status --short
git diff --check origin/main...HEAD
git diff --name-only origin/main...HEAD
```

Expected: only the approved Android source/tests and the design/plan documentation are present; no `backend/` or `esp32/` path appears; `git diff --check` prints nothing.

- [ ] **Step 2: Run the complete Android JVM suite**

From `android/`:

```bash
./gradlew testDebugUnitTest
```

Expected: BUILD SUCCESSFUL with no failed test task. Inspect HTML/XML results for any skipped new regression test; the new tests must execute, not merely compile.

- [ ] **Step 3: Assemble the debug APK**

Run:

```bash
./gradlew assembleDebug
```

Expected: BUILD SUCCESSFUL and `android/app/build/outputs/apk/debug/app-debug.apk` exists and is non-empty.

- [ ] **Step 4: Run connected Compose tests on API 35**

Start the available API 35 emulator and run:

```bash
./gradlew connectedDebugAndroidTest
```

Expected: BUILD SUCCESSFUL. At minimum, `DetectionPrefsObservableTest`, `InfoScreenTest`, `DetailOverviewContentTest`, `AboutLandingScreenTest`, and `NavigationShellTest` execute and pass.

- [ ] **Step 5: Walk the user-visible regressions in the emulator**

Use the `test-android-apps:android-emulator-qa` workflow and capture screenshots/UI trees for these exact states:

1. Clear app data, complete onboarding if shown, and confirm ordinary launch lands on About.
2. Confirm About shows Checking followed by Up to date or Update available from GitHub; if the emulator is offline, confirm Could not check plus Retry without blocking navigation. Open App settings and confirm its update row shows the same resolved state/version, then return to About and confirm no second GitHub check was started.
3. In App settings, confirm Sensor backend connection is off and Backend-only mode is disabled/inactive.
4. Visit AR, Map, and Privacy while backend is off; inspect logcat for absence of configured backend request attempts or repeated backend errors.
5. Run the deterministic connected `DetailOverviewContentTest` cases and capture their result: B738 must reach the tagged decoded-image branch and ZZZZ must reach the tagged silhouette branch. If a typed live aircraft is currently present from public ADS-B, also open its full Detail route and capture a screenshot, but do not make ambient traffic a pass condition.
6. Enable the sensor backend explicitly, confirm an eligible request is permitted, then disable it and confirm requests stop and remote live state clears.

Do not require a reachable private sensor server to pass the disabled-state checks. If no configured server is available, use `BackendClientWiringTest` to prove production health wiring is locally blocked while off and `BackendRequestInterceptorTest` to prove an enabled request rewrites to and reaches its synthetic downstream chain; report the live-server smoke step as unavailable.

- [ ] **Step 6: Review final commits and hand off**

Run:

```bash
git log --oneline --decorate -6
git status --short
```

Expected: the implementation commits are present in task order, the worktree contains no uncommitted task changes, and no release/tag action has occurred. Report the exact commands run, pass/fail counts, APK path, emulator evidence, and any unavailable hardware/private-backend check.

Do not create an extra “verification-only” commit when the tree is already clean.

## Plan Self-Review

- **Spec coverage:** Task 1 covers fresh/upgrade consent, backend-only normalization, and local-source restart; Task 2 covers zero network attempts, production health-client wiring, GitHub isolation, and reactive pollers; Task 3 covers live/historical aircraft imagery and deterministic decoded-image/fallback branches; Task 4 covers GitHub landing state, idle-check lifecycle, and the shared graph-owner contract; Task 5 covers production-screen state sharing, full build/emulator verification, and scope boundaries.
- **Placeholder scan:** The plan contains no deferred implementation markers or generic “handle errors” steps.
- **Type consistency:** `AircraftVisual`, `BackendRequestInterceptor`, `SensorBackendDisabledException`, `AppUpdateRow`, and `checkForUpdatesIfIdle` use one signature throughout all producer, consumer, and test steps.
- **Dependency boundary:** `BackendClientWiringTest` proves backend health is stopped by the production backend-client provider while the GitHub Retrofit retains the passed general client; ADS-B and unrelated services are unchanged.
- **Branch boundary:** Execution begins from refreshed `origin/main`; the current divergent checkout is documentation-only and its unrelated changes are preserved.

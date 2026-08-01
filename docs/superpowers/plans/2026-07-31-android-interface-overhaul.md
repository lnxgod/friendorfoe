# Android Interface Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a dependable Android-only Friend or Foe app with exactly seven top-level destinations, a faithful cleanup of the current Material 3 interface, a current-findings Privacy list, firmware-exact Badge configuration, and the audited trust/correctness fixes.

**Architecture:** Keep the existing Kotlin, Hilt, Room, Retrofit/OkHttp, and Jetpack Compose application, but separate app/session preferences, navigation chrome, privacy aggregation, and badge protocol state into testable units. Screen composables become thin routes over immutable UI state; singleton repositories own app-scoped collectors, while pure parsers, normalizers, reducers, freshness rules, and validators carry the correctness burden. Implement in dependency order so trust fixes and stable contracts land before the visual screen rewrites.

**Tech Stack:** Kotlin 1.9.22, Java 17, Android minSdk 26 / targetSdk 34 / compileSdk 35, Jetpack Compose Material 3, Navigation Compose 2.7.7, Hilt 2.50, coroutines/StateFlow, Room 2.6.1, Retrofit/OkHttp/Gson, AndroidX DataStore Preferences 1.1.1, JUnit 4, Compose UI tests, API 35 emulator.

## Global Constraints

- Modify `android/`, Android tests, and Android documentation only. Never edit, build, flash, upload, or otherwise change `esp32/` or `backend/`.
- Keep exactly seven top-level destinations in this order: `AR`, `Map`, `List`, `Privacy`, `Badge`, `History`, `Info`.
- The fifth triangle/tune destination configures the badge. Calibration is a secondary `Info > Advanced` route and remains de-emphasized while its backend is unavailable.
- Privacy remains the current findings list with `THREATS`, `AWARENESS`, `NEARBY`, and `INFO` grouping. It contains no badge configuration, sweep-tool cards, or sky-alert footer.
- Preserve the current Material 3 identity: system light/dark themes, Android sans typography, blue/cyan actions, compact flat rows, outline chips, tinted severity strips, soft selected navigation pill, 8–12dp shapes, and the 4/8/12/16dp spacing rhythm.
- Dark tokens remain `#0B1117`, `#101820`, `#263241`, `#7DD3FC`, `#86EFAC`, `#FBBF24`, `#F87171`, `#E5EEF7`, `#B7C4D2`. Light tokens remain `#F6F8FB`, `#FFFFFF`, `#E3EAF2`, `#0369A1`, `#2E7D32`, `#B45309`, `#D32F2F`, `#17202A`, `#566575`.
- Certify portrait widths 360dp and 412dp. Keep seven 48dp navigation targets; show the selected one-line label at 1.0x font scale and allow icon-only navigation at 1.3x/2.0x. Use a bottom bar below 600dp and in portrait; use a navigation rail only in landscape at 600dp or wider.
- Never render a physical badge drawing or simulated LCD. Device-reported focus/title/status may appear only as text.
- Theme V1 accent defaults are Drone `0xFEA0`/65184, Meta `0xF833`/63539, Tracker `0xF81F`/63519, Flock `0xA81F`/43039, Wi-Fi Attack `0x07FF`/2047, Clear `0x2F65`/12133. The canonical default hash is `0xC3AA2A8D`.
- Theme V1 backgrounds are Black/`dark`/`0x0000`, Dim/`dim`/`0x1082`, Blue-black/`scanline`/`0x0108`. Label firmware `brightness` as `Color intensity 25–100%`; never imply that it controls the fixed LCD backlight.
- Do not expose the stored-but-inactive palette field, arbitrary RGB editing, accent `0x0000`, or an editable Display Policy priority. Preserve valid device-read palette and priority values unchanged in outgoing payloads.
- Display Policy V1 has exactly 13 keys and canonical default hash `0x0DAD6299`; lanes are `off/lower/top/both`, proximity is `present/near/close`, and thresholds are close `>= -60 dBm`, near `>= -76 dBm`, present below `-76 dBm`.
- UI labels may say `Next`, `Detail`, `Back`, but every badge transport sends lowercase `next`, `detail`, `back`.
- Persisted network mode comes from status `mode`; runtime `network_mode:"off"` is the correct live-session value for persisted `usb_only` and must not overwrite it.
- BLE supports read/status and short navigation only in this release. BLE network mode, Theme V1, Display Policy V1, reboot, bootloader, and firmware upload remain unavailable.
- The debug bridge is a debug-build proxy to a physical USB badge. Reachability alone never proves a live badge or a successful mutation, and its shipped ACK parser cannot support reboot/bootloader; recovery is direct-USB-only.
- No arbitrary firmware-file picker, upload, relay, or flash action remains in Android. Direct-USB reboot/bootloader live behind verified capability checks and explicit confirmation.
- A mutating badge command is never retried automatically. HTTP `ok:false`, USB `FOF_CTL_ERROR`, timeout, readback mismatch, or scanner ACK mismatch cannot become success.
- Apple/AirPods activity is informational. It must never create the old listening claim, threat count, high-risk notification, or cross-device correlation.
- Startup requests no blanket permission batch. Camera/AR, location, phone privacy scan, alerts, Wi-Fi/Bluetooth, and ultrasonic microphone permissions are requested only in the relevant flow with permanent-denial recovery.
- Stale or paused findings never generate a new alert. Use injected monotonic time for freshness decisions and wall time only for displayed/restored age.
- History opens the exact immutable row selected, labels it historical, supports confirmed row delete and Clear all, and does not add export.
- Only an explicit `Capture > Save` action may write a photo. Object Peek, Zoom, Share, Full details, Discard, and ordinary label taps perform zero writes.
- Do not add dependencies beyond DataStore Preferences 1.1.1 unless a failing implementation step demonstrates that an existing library cannot satisfy the approved design.

## File and Responsibility Map

| Area | Files | Responsibility after this plan |
|---|---|---|
| Session/preferences | `data/preferences/AppPreferencesRepository.kt`, `data/DetectionPrefs.kt` | DataStore-backed onboarding, last route, source-aware ignores; observable existing detection settings |
| App chrome | `presentation/MainActivity.kt`, `presentation/AppChromeViewModel.kt`, `presentation/navigation/Screen.kt`, `presentation/navigation/FriendOrFoeNavGraph.kt`, `presentation/navigation/FofNavigationSuite.kt` | Launch gate, seven-route shell, bottom bar/rail, deep-link dispatch |
| Shared UI | `presentation/components/FofSurfaces.kt`, `presentation/components/FofScreenStates.kt`, `presentation/theme/Theme.kt` | Faithful reusable rows, source status, empty/error states, touch/accessibility semantics |
| Privacy domain | `presentation/privacy/PrivacyFindingNormalizer.kt`, `PrivacyModels.kt`, `PrivacyFreshness.kt`, `PrivacyViewModel.kt` | Normalize evidence, merge sources, track health/freshness/capabilities, source-aware ignore |
| Privacy UI | `presentation/privacy/PrivacyScreen.kt`, `IgnoredDevicesScreen.kt`, `PrivacyAlertPolicy.kt`, `PrivacyAlertNotifier.kt` | Grouped findings list, exact row actions, ignored-device recovery, truthful notifications/deep links |
| Badge contract | `data/badge/BadgeProtocol.kt`, `BadgeStatusModels.kt`, `BadgeStatusParser.kt`, `BadgeConnection.kt`, `BadgeCommandEvidence.kt` | Exact Theme/Policy schemas and hashes, strict parsing, transport matrix, mutation evidence |
| Badge transport | `data/badge/BadgeUsbRepository.kt`, `FriendOrFoeApplication.kt` | App-scoped USB/AP/BLE/physical-debug-bridge I/O and explicit command events |
| Badge UI | `presentation/badge/BadgeViewModel.kt`, `BadgeScreen.kt`, `BadgeAppearanceSection.kt`, `BadgeDisplayFiltersSection.kt`, `BadgeDiagnosticsScreen.kt`, `BadgeRecoveryScreen.kt` | One dedicated firmware-exact configuration surface and secondary diagnostics/recovery |
| History/detail | `data/local/HistoryDao.kt`, `data/repository/HistoryRepository.kt`, `presentation/history/*`, `presentation/detail/*` | Exact snapshot routing, local deletion, live/historical distinction |
| AR capture | `presentation/ar/ObjectPeek.kt`, `CaptureReviewScreen.kt`, `PhotoWriter.kt`, `ArViewScreen.kt`, `ArViewModel.kt`, `ZoomViewSheet.kt`, `SnapPhotoSheet.kt` | Preview-first interaction and a single explicit photo-write boundary |
| Info/tools | `presentation/about/AboutScreen.kt`, `AboutViewModel.kt`, `presentation/privacy/EmfSweep*`, `IrCameraScan*` | Top-level Info, observable settings, truthful privacy copy, Advanced routes and calibration gate |
| Remaining screens | `presentation/list/*`, `map/*`, `reference/*`, `filter/*` | Remove duplicated controls and apply shared responsive hierarchy |
| Verification | `app/src/test/**`, `app/src/androidTest/**`, `docs/testing/android-interface-overhaul-qa.md` | JVM contract/regression coverage, Compose navigation/layout tests, emulator/phone evidence |

---

### Task 1: Persist launch state and source-aware ignored identities

**Files:**
- Modify: `android/gradle/libs.versions.toml`
- Modify: `android/app/build.gradle.kts`
- Create: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferences.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/PreferencesModule.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/preferences/AppPreferenceRulesTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/data/preferences/AppPreferencesRepositoryTest.kt`

**Interfaces:**
- Consumes: existing application `Context`; the approved top-level route strings.
- Produces: `AppPreferencesRepository.launchState: Flow<AppLaunchState>`, `setOnboardingComplete()`, `setLastTopLevelRoute(String)`, `ignoredFindingKeys: Flow<Set<String>>`, `ignoreFinding(FindingPreferenceKey)`, and `restoreFinding(FindingPreferenceKey)`.

- [ ] **Step 1: Add the DataStore dependency aliases**

```toml
# android/gradle/libs.versions.toml
[versions]
datastore = "1.1.1"

[libraries]
androidx-datastore-preferences = { group = "androidx.datastore", name = "datastore-preferences", version.ref = "datastore" }
```

```kotlin
// android/app/build.gradle.kts, AndroidX dependencies
implementation(libs.androidx.datastore.preferences)
```

- [ ] **Step 2: Write failing pure-rule tests for route fallback and finding-key isolation**

```kotlin
class AppPreferenceRulesTest {
    @Test fun invalidTopLevelRouteFallsBackToAr() {
        assertEquals("ar_view", sanitizeTopLevelRoute("calibrate"))
        assertEquals("ar_view", sanitizeTopLevelRoute(null))
    }

    @Test fun sourceAndStableIdBothParticipateInIgnoredKey() {
        assertNotEquals(
            FindingPreferenceKey.create("phone_ble", "AA:BB")!!.encoded,
            FindingPreferenceKey.create("badge", "AA:BB")!!.encoded
        )
    }

    @Test fun unstableIdentityCannotBePersisted() {
        assertNull(FindingPreferenceKey.create("backend", ""))
    }

    @Test fun findingKeyRoundTripsAndMalformedValuesAreRejected() {
        val key = FindingPreferenceKey.create("phone_ble", "AA:BB")!!
        assertEquals(key, FindingPreferenceKey.decode(key.encoded))
        assertNull(FindingPreferenceKey.decode("missing-separator"))
        assertNull(FindingPreferenceKey.decode("phone_ble\u001F"))
        assertNull(FindingPreferenceKey.decode("\u001FAA:BB"))
    }
}
```

- [ ] **Step 3: Run the focused test and confirm the red state**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppPreferenceRulesTest'`

Expected: compilation fails because `sanitizeTopLevelRoute` and `FindingPreferenceKey` do not exist.

- [ ] **Step 4: Implement the repository and pure rules**

```kotlin
private val Context.fofDataStore by preferencesDataStore(name = "fof_app_state")

private val TOP_LEVEL_ROUTES = setOf(
    "ar_view", "map_view", "list_view", "privacy", "badge", "history", "info"
)

internal fun sanitizeTopLevelRoute(route: String?): String =
    route?.takeIf(TOP_LEVEL_ROUTES::contains) ?: "ar_view"

data class FindingPreferenceKey private constructor(
    val source: String,
    val stableId: String,
) {
    val encoded: String = "$source\u001F$stableId"

    companion object {
        fun create(source: String, stableId: String): FindingPreferenceKey? =
            if (source.isBlank() || stableId.isBlank()) null else FindingPreferenceKey(source, stableId)

        fun decode(encoded: String): FindingPreferenceKey? {
            val parts = encoded.split('\u001F', limit = 2)
            return if (parts.size == 2) create(parts[0], parts[1]) else null
        }
    }
}

sealed interface AppLaunchState {
    data object Loading : AppLaunchState
    data object NeedsOnboarding : AppLaunchState
    data class Ready(val startRoute: String) : AppLaunchState
}

@Singleton
class AppPreferencesRepository @Inject constructor(
    @ApplicationContext private val context: Context,
) : AppPreferences {
    private val onboarding = booleanPreferencesKey("onboarding_complete")
    private val lastRoute = stringPreferencesKey("last_top_level_route")
    private val ignored = stringSetPreferencesKey("ignored_finding_keys")

    override val launchState: Flow<AppLaunchState> = context.fofDataStore.data.map { prefs ->
        if (prefs[onboarding] != true) AppLaunchState.NeedsOnboarding
        else AppLaunchState.Ready(sanitizeTopLevelRoute(prefs[lastRoute]))
    }

    override val ignoredFindingKeys: Flow<Set<String>> = context.fofDataStore.data
        .map { it[ignored].orEmpty() }

    override suspend fun setOnboardingComplete() { context.fofDataStore.edit {
        it[onboarding] = true
        if (it[lastRoute] == null) it[lastRoute] = "ar_view"
    } }

    override suspend fun setLastTopLevelRoute(route: String) { context.fofDataStore.edit {
        it[lastRoute] = sanitizeTopLevelRoute(route)
    } }

    override suspend fun ignoreFinding(key: FindingPreferenceKey) { context.fofDataStore.edit {
        it[ignored] = it[ignored].orEmpty() + key.encoded
    } }

    override suspend fun restoreFinding(key: FindingPreferenceKey) { context.fofDataStore.edit {
        it[ignored] = it[ignored].orEmpty() - key.encoded
    } }

    @VisibleForTesting
    suspend fun resetForInstrumentation() {
        context.fofDataStore.edit { it.clear() }
    }
}
```

```kotlin
interface AppPreferences {
    val launchState: Flow<AppLaunchState>
    val ignoredFindingKeys: Flow<Set<String>>
    suspend fun setOnboardingComplete()
    suspend fun setLastTopLevelRoute(route: String)
    suspend fun ignoreFinding(key: FindingPreferenceKey)
    suspend fun restoreFinding(key: FindingPreferenceKey)
}

@Module
@InstallIn(SingletonComponent::class)
abstract class PreferencesModule {
    @Binds @Singleton
    abstract fun bindAppPreferences(implementation: AppPreferencesRepository): AppPreferences
}
```

- [ ] **Step 5: Run the focused test and dependency compile**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppPreferenceRulesTest'`

Expected: PASS.

Add an Android storage round-trip test that resets the app DataStore, writes through one repository instance, and reads through a newly constructed instance:

```kotlin
@RunWith(AndroidJUnit4::class)
class AppPreferencesRepositoryTest {
    private val context
        get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun onboardingRouteAndIgnoredKeysSurviveRepositoryRecreation() = runBlocking {
        val first = AppPreferencesRepository(context)
        first.resetForInstrumentation()
        try {
            val ignored = FindingPreferenceKey.create("phone_ble", "AA:BB")!!
            first.setOnboardingComplete()
            first.setLastTopLevelRoute("privacy")
            first.ignoreFinding(ignored)

            val recreated = AppPreferencesRepository(context)
            assertEquals(AppLaunchState.Ready("privacy"), recreated.launchState.first())
            assertEquals(setOf(ignored.encoded), recreated.ignoredFindingKeys.first())

            recreated.restoreFinding(ignored)
            assertTrue(recreated.ignoredFindingKeys.first().isEmpty())
        } finally {
            first.resetForInstrumentation()
        }
    }
}
```

Run with an API 35 emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.preferences.AppPreferencesRepositoryTest`

Expected: PASS and the second repository observes the first repository's persisted values.

- [ ] **Step 6: Commit the preference foundation**

```bash
git add android/gradle/libs.versions.toml android/app/build.gradle.kts android/app/src/main/java/com/friendorfoe/data/preferences android/app/src/main/java/com/friendorfoe/di/PreferencesModule.kt android/app/src/test/java/com/friendorfoe/data/preferences/AppPreferenceRulesTest.kt android/app/src/androidTest/java/com/friendorfoe/data/preferences/AppPreferencesRepositoryTest.kt
git commit -m "android: persist launch and privacy state"
```

### Task 2: Build the stable seven-destination application shell

**Files:**
- Modify: `android/app/src/main/AndroidManifest.xml`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/navigation/RouteCodec.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/AppChromeViewModel.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FofNavigationSuite.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeConnectionLoadingScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/components/FofSurfaces.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/components/FofScreenStates.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/navigation/TopLevelDestinationTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/AppChromeViewModelTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/navigation/NavigationShellTest.kt`

**Interfaces:**
- Consumes: the `AppPreferences` interface from Task 1; production receives `AppPreferencesRepository` through Hilt.
- Produces: `TopLevelDestination.entries`, top-level `Screen.Badge` and `Screen.Info`, `FofNavigationSuite(showNavigation, currentRoute, onNavigate, content)`, and one stable main navigation graph that never includes Welcome as its start anchor.

- [ ] **Step 1: Write the failing destination-order unit test**

```kotlin
class TopLevelDestinationTest {
    @Test fun destinationsStayInApprovedOrder() {
        assertEquals(
            listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info"),
            TopLevelDestination.entries.map { it.label }
        )
        assertEquals(7, TopLevelDestination.entries.map { it.route }.distinct().size)
        assertFalse(TopLevelDestination.entries.any { it.route == "calibrate" })
    }

    @Test fun topLevelBackExitsAndSecondaryBackPops() {
        TopLevelDestination.entries.forEach {
            assertEquals(BackDisposition.EXIT_APP, backDisposition(it.route))
        }
        assertEquals(BackDisposition.POP_SECONDARY,
            backDisposition(Screen.IgnoredDevices.route))
    }
}
```

- [ ] **Step 2: Run the test and confirm it fails**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*TopLevelDestinationTest'`

Expected: compilation fails because `TopLevelDestination` does not exist.

- [ ] **Step 3: Define routes and the fixed destination metadata**

```kotlin
sealed class Screen(val route: String) {
    data object ArView : Screen("ar_view")
    data object MapView : Screen("map_view")
    data object ListView : Screen("list_view")
    data object Privacy : Screen("privacy")
    data object Badge : Screen("badge")
    data object History : Screen("history")
    data object Info : Screen("info")
    data object Detail : Screen("detail/{objectId}") {
        fun createRoute(objectId: String) = "detail/${encodeRouteSegment(objectId)}"
    }
    data object HistoricalDetail : Screen("history_detail/{historyId}") {
        fun createRoute(historyId: Long) = "history_detail/$historyId"
    }
    data object ReferenceGuide : Screen("reference_guide?tab={tab}&query={query}")
    data object DroneGuide : Screen("drone_guide?manufacturer={manufacturer}") {
        fun createRoute(manufacturer: String? = null) = manufacturer?.let {
            "drone_guide?manufacturer=${encodeRouteSegment(it)}"
        } ?: "drone_guide"
    }
    data object AircraftGuide : Screen("aircraft_guide?type={type}") {
        fun createRoute(typeCode: String? = null) = typeCode?.let {
            "aircraft_guide?type=${encodeRouteSegment(it)}"
        } ?: "aircraft_guide"
    }
    data object IgnoredDevices : Screen("privacy/ignored")
    data object BadgeDiagnostics : Screen("badge/diagnostics")
    data object BadgeRecovery : Screen("badge/recovery")
    data object EmfSweep : Screen("info/advanced/magnetic_field")
    data object IrCameraScan : Screen("info/advanced/ir_light")
    data object Calibrate : Screen("info/advanced/calibrate")
}

enum class TopLevelDestination(
    val label: String,
    val route: String,
    val icon: ImageVector,
) {
    AR("AR", Screen.ArView.route, Icons.Default.Visibility),
    MAP("Map", Screen.MapView.route, Icons.Default.Map),
    LIST("List", Screen.ListView.route, Icons.AutoMirrored.Filled.List),
    PRIVACY("Privacy", Screen.Privacy.route, Icons.Default.Shield),
    BADGE("Badge", Screen.Badge.route, Icons.Default.Tune),
    HISTORY("History", Screen.History.route, Icons.Default.History),
    INFO("Info", Screen.Info.route, Icons.Default.Info),
}

enum class BackDisposition { EXIT_APP, POP_SECONDARY }

fun backDisposition(route: String?): BackDisposition =
    if (route in TopLevelDestination.entries.map { it.route }) BackDisposition.EXIT_APP
    else BackDisposition.POP_SECONDARY

fun encodeRouteSegment(value: String): String =
    URLEncoder.encode(value, StandardCharsets.UTF_8.name()).replace("+", "%20")
```

Register the Badge top-level route against a compilable truthful loading state until Tasks 7–10 provide the strict repository and editor:

```kotlin
@Composable
fun BadgeConnectionLoadingScreen() {
    Column(
        Modifier.fillMaxSize().padding(16.dp).testTag("screen_badge"),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("Badge", style = MaterialTheme.typography.headlineSmall)
        Text("Checking for a verified Friend or Foe badge connection")
        LinearProgressIndicator(Modifier.fillMaxWidth())
    }
}
```

Do not register diagnostics/recovery composables in the graph until Task 10 creates them; declaring their route constants does not require a destination implementation.

- [ ] **Step 4: Make launch state drive onboarding outside the main graph**

```kotlin
data class AppChromeUiState(
    val launchState: AppLaunchState = AppLaunchState.Loading,
)

@HiltViewModel
class AppChromeViewModel @Inject constructor(
    private val appPreferences: AppPreferences,
) : ViewModel() {
    val uiState = appPreferences.launchState
        .map(::AppChromeUiState)
        .stateIn(viewModelScope, SharingStarted.Eagerly, AppChromeUiState())

    fun completeOnboarding() = viewModelScope.launch {
        appPreferences.setOnboardingComplete()
    }

    fun recordTopLevelRoute(route: String) = viewModelScope.launch {
        appPreferences.setLastTopLevelRoute(route)
    }
}
```

```kotlin
@Composable
fun FriendOrFoeApp(viewModel: AppChromeViewModel = hiltViewModel()) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    when (val launch = state.launchState) {
        AppLaunchState.Loading -> FofLaunchPlaceholder()
        AppLaunchState.NeedsOnboarding -> WelcomeScreen(viewModel::completeOnboarding)
        is AppLaunchState.Ready -> MainApplicationShell(
            startRoute = launch.startRoute,
            onTopLevelSelected = viewModel::recordTopLevelRoute,
        )
    }
}

@Composable
fun MainApplicationShell(
    startRoute: String,
    onTopLevelSelected: (String) -> Unit,
    onExitRequested: (() -> Unit)? = null,
) {
    val navController = rememberNavController()
    val graphStartRoute = rememberSaveable { sanitizeTopLevelRoute(startRoute) }
    val entry by navController.currentBackStackEntryAsState()
    val currentRoute = entry?.destination?.route
    val isTopLevel = currentRoute in TopLevelDestination.entries.map { it.route }
    val activity = LocalContext.current as? Activity
    BackHandler(enabled = currentRoute != null) {
        if (isTopLevel) (onExitRequested ?: activity?.let { { it.finish() } })?.invoke()
        else navController.popBackStack()
    }
    FofNavigationSuite(
        showNavigation = isTopLevel,
        currentRoute = currentRoute,
        onNavigate = { destination ->
            navigateTopLevel(navController, destination)
            onTopLevelSelected(destination.route)
        },
    ) { padding ->
        MainNavGraph(navController, graphStartRoute, Modifier.padding(padding))
    }
}
```

Create the shared shell states now because Privacy, Badge, and Info consume them before Task 16. Task 16 adds collection reducers and stale banners without redefining these APIs:

```kotlin
@Composable
fun FofLaunchPlaceholder() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        CircularProgressIndicator(Modifier.semantics { contentDescription = "Loading app" })
    }
}

@Composable
fun FofScreenHeader(
    title: String,
    count: Int? = null,
    countLabel: String? = null,
) {
    Column(Modifier.fillMaxWidth()) {
        Text(title, style = MaterialTheme.typography.headlineSmall)
        if (count != null && countLabel != null) {
            Text("$count $countLabel", style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
fun FofSecondaryScreenHeader(title: String, onBack: () -> Unit) {
    TopAppBar(
        title = { Text(title) },
        navigationIcon = {
            IconButton(onClick = onBack, modifier = Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
            }
        },
    )
}

@Composable
fun FofLoadingState(message: String) = FofMessageState(message, showProgress = true)

@Composable
fun FofEmptyState(message: String) = FofMessageState(message)

@Composable
fun FofNoMatchesState(activeFilterCount: Int, onClearFilters: () -> Unit) {
    FofMessageState(
        message = "No matches for $activeFilterCount active filters",
        actionLabel = "Clear filters",
        onAction = onClearFilters,
    )
}

@Composable
fun FofFailureState(message: String, onRetry: (() -> Unit)? = null) {
    FofMessageState(message, actionLabel = onRetry?.let { "Retry" }, onAction = onRetry)
}

@Composable
fun FofErrorState(
    title: String,
    detail: String,
    actionLabel: String,
    onAction: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        Text(detail, style = MaterialTheme.typography.bodyMedium)
        TextButton(onClick = onAction, modifier = Modifier.heightIn(min = 48.dp)) {
            Text(actionLabel)
        }
    }
}

sealed interface CollectionBodyState<out T> {
    data object Loading : CollectionBodyState<Nothing>
    data class Content<T>(val rows: List<T>) : CollectionBodyState<T>
    data class Stale<T>(val rows: List<T>, val ageMs: Long, val message: String) : CollectionBodyState<T>
    data object Empty : CollectionBodyState<Nothing>
    data class NoMatches(val activeFilterCount: Int) : CollectionBodyState<Nothing>
    data class Failed(val message: String, val canRetry: Boolean) : CollectionBodyState<Nothing>
}

@Composable
private fun FofMessageState(
    message: String,
    showProgress: Boolean = false,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
) {
    Column(
        Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (showProgress) CircularProgressIndicator()
        Text(message, style = MaterialTheme.typography.bodyMedium)
        if (actionLabel != null && onAction != null) {
            TextButton(onClick = onAction, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(actionLabel)
            }
        }
    }
}
```

- [ ] **Step 5: Implement the 360dp-safe bar and 600dp rail breakpoint**

```kotlin
@Composable
fun FofNavigationSuite(
    showNavigation: Boolean,
    currentRoute: String?,
    onNavigate: (TopLevelDestination) -> Unit,
    content: @Composable (PaddingValues) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val useRail = maxWidth >= 600.dp && maxWidth > maxHeight
        val showNavLabel = LocalDensity.current.fontScale < 1.3f
        Scaffold(
            bottomBar = {
                if (showNavigation && !useRail) {
                    NavigationBar(Modifier.testTag("navigation_bar")) {
                        TopLevelDestination.entries.forEach { destination ->
                            NavigationBarItem(
                                alwaysShowLabel = false,
                                selected = currentRoute == destination.route,
                                onClick = { onNavigate(destination) },
                                icon = { Icon(destination.icon, contentDescription = null) },
                                label = if (showNavLabel) {
                                    {
                                        Text(destination.label, Modifier.clearAndSetSemantics {},
                                            maxLines = 1, softWrap = false)
                                    }
                                } else null,
                                modifier = Modifier
                                    .testTag("nav_destination")
                                    .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
                                    .semantics(mergeDescendants = true) {
                                        contentDescription = destination.label
                                    },
                            )
                        }
                    }
                }
            },
        ) { scaffoldPadding ->
            Row(Modifier.fillMaxSize().padding(scaffoldPadding)) {
                if (showNavigation && useRail) {
                NavigationRail(Modifier.testTag("navigation_rail")) {
                    TopLevelDestination.entries.forEach { destination ->
                        NavigationRailItem(
                            selected = currentRoute == destination.route,
                            onClick = { onNavigate(destination) },
                            icon = { Icon(destination.icon, contentDescription = null) },
                            label = if (showNavLabel) {
                                {
                                    Text(destination.label, Modifier.clearAndSetSemantics {},
                                        maxLines = 1, softWrap = false)
                                }
                            } else null,
                            modifier = Modifier
                                .testTag("nav_destination")
                                .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
                                .semantics(mergeDescendants = true) {
                                    contentDescription = destination.label
                                },
                        )
                    }
                }
                }
                Box(Modifier.weight(1f).fillMaxHeight()) { content(PaddingValues()) }
            }
        }
    }
}
```

- [ ] **Step 6: Rebuild the main graph and top-level navigation behavior**

Use a stable nested main graph; Welcome never appears in either graph:

```kotlin
NavHost(navController, startDestination = "main_graph") {
    navigation(
        route = "main_graph",
        startDestination = sanitizeTopLevelRoute(startRoute),
    ) {
        registerSevenTopLevelDestinations(navController)
        registerSecondaryDestinations(navController)
    }
}
```

Keep `DroneGuide` and `AircraftGuide` registered with their compile-compatible builders so current reference call sites compile; Task 17 moves those callers to the encoded combined route and deletes both constants. `RouteCodec.kt` is created in this task with the pure UTF-8 segment encoder later reused by Task 12/17. On a top-level tap use:

```kotlin
navController.navigate(destination.route) {
    popUpTo("main_graph") {
        saveState = true
    }
    launchSingleTop = true
    restoreState = true
}
onTopLevelSelected(destination.route)
```

Pass `showNavigation = currentRoute in TopLevelDestination.entries`; Info and Badge therefore retain navigation, while detail/reference/ignored/diagnostics/recovery/EMF/IR/Calibration do not. `FofNavigationSuite` keeps one permanent `BoxWithConstraints > Scaffold > Row > content Box` hierarchy, conditionally adding only the rail/bar siblings, and invokes `content` at exactly one composition call site. `MainApplicationShell` contains exactly one `MainNavGraph` call, so moving between a top-level and secondary route never replaces the `NavHost` composition or loses its back stack. Freeze the sanitized initial route once with `rememberSaveable`; later DataStore emissions caused by selecting tabs must not change the active graph's `startDestination`. Install a `BackHandler` while an entry exists: a top-level route calls `activity.finish()`, while a secondary route calls `popBackStack()`. This guarantees that the graph's restored start destination never reappears as tab history.

Wrap each registered top-level destination in a stable root tag at this commit boundary, without changing the screen's own content:

```kotlin
@Composable
internal fun TopLevelRouteRoot(destination: TopLevelDestination, content: @Composable () -> Unit) {
    Box(
        Modifier.fillMaxSize().testTag("screen_${destination.label.lowercase()}"),
    ) { content() }
}
```

Every destination passes its matching enum value to `TopLevelRouteRoot`. As each screen is cleaned later, keep this root tag and render `FofScreenHeader(destination.label)` (or the AR equivalent inside its wrapping top-status surface), so the current route remains visibly named even when navigation labels become icon-only.

Replace the old backend calibration-count `AppChromeViewModelTest` with a fake `AppPreferences` test that emits `NeedsOnboarding`, verifies `completeOnboarding()` calls the fake once, emits `Ready("privacy")`, and asserts the ViewModel exposes that route. The shell no longer polls calibration events.

- [ ] **Step 7: Remove the startup permission launcher from `MainActivity`**

Delete `rememberLauncherForActivityResult(RequestMultiplePermissions())` and its `LaunchedEffect(Unit)`. This task must leave launch with zero calls to a runtime-permission contract.

Remove `android:screenOrientation="portrait"` from `MainActivity` in `AndroidManifest.xml`. This allows the deterministic bottom-bar/rail breakpoint to be exercised after rotation; do not add a replacement orientation lock.

- [ ] **Step 8: Add the navigation Compose test**

```kotlin
class NavigationShellTest {
    @get:Rule val compose = createComposeRule()

    @Test fun sevenDestinationsAreReachableWithoutHiltScreenDependencies() {
        val selected = mutableStateOf(TopLevelDestination.AR)
        compose.setContent {
            FofNavigationSuite(
                showNavigation = true,
                currentRoute = selected.value.route,
                onNavigate = { selected.value = it },
            ) { padding ->
                TopLevelRouteRoot(selected.value) {
                    Text(selected.value.label, Modifier.padding(padding))
                }
            }
        }
        listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info")
            .forEach { label ->
                compose.onNodeWithContentDescription(label).assertHasClickAction().performClick()
                compose.onNodeWithTag("screen_${label.lowercase()}").assertIsDisplayed()
            }
    }
}
```

Add a second instrumentation test around a minimal real `NavHost`: start on AR, navigate to Info, push `Screen.IgnoredDevices`, assert the secondary tag remains while `showNavigation` becomes false, press Back and assert Info returns, then mutate a supplied persisted-start state to Badge and assert the active route remains Info. Count a `DisposableEffect` on the host and assert it was never disposed during those transitions. Add a pure `backDisposition(route)` test for all seven routes versus one secondary route; production maps `EXIT_APP` to `Activity.finish()`/the injected callback and `POP_SECONDARY` to `popBackStack()`. Add a separate `AppChromeViewModelTest` with `FakeAppPreferences` for the launch-state transition. Task 18 performs the real process-recreation/last-route test.

- [ ] **Step 9: Run unit and connected navigation tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*TopLevelDestinationTest' --tests '*AppChromeViewModelTest'`

Expected: PASS.

Run with `Pixel8_API35` running: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.navigation.NavigationShellTest`

Expected: PASS with seven reachable destinations.

- [ ] **Step 10: Commit the shell**

```bash
git add android/app/src/main/AndroidManifest.xml android/app/src/main/java/com/friendorfoe/presentation android/app/src/test/java/com/friendorfoe/presentation android/app/src/androidTest/java/com/friendorfoe/presentation/navigation
git commit -m "android: establish seven-destination shell"
```

### Task 3: Neutralize the false Apple/AirPods listening classification

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt:479-540,1333-1348`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyFindingNormalizer.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt:90-130,284-438,439-550`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicy.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/AppleContinuityDecoderTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyFindingNormalizerTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicyTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/BadgePrivacyMapperTest.kt`

**Interfaces:**
- Consumes: `GlassesDetection`, backend `LivePrivacyDeviceDto`, and badge `BadgeThreatEntity` mappings.
- Produces: `PrivacyFindingNormalizer.normalize(GlassesDetection): GlassesDetection`; no Android flow can emit an Apple-related `REMOTE_LISTENING` result.

- [ ] **Step 1: Replace the current positive listening test with failing neutralization cases**

```kotlin
@Test fun `AirPods with active phone activity is informational not listening`() {
    val apple = AppleContinuityDecoder.decode(byteArrayOf(
        0x10, 0x01, 0x02, 0x03, 0x12, 0x01
    ))!!

    assertNull(GlassesDetector.appleRemoteListeningMatchForTest(apple, rssi = -45))
    val activity = GlassesDetector.appleActivityMatchForTest(apple, rssi = -45)
    assertNotNull(activity)
    assertEquals("AirPods connection/activity nearby", activity!!.second)
}
```

```kotlin
class PrivacyFindingNormalizerTest {
    @Test fun appleBackendListeningWordingBecomesInformational() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                manufacturer = "Apple",
                deviceType = "Possible Remote Listening",
                matchReason = "backend:REMOTE_LISTENING",
                category = PrivacyCategory.REMOTE_LISTENING,
                bleCompanyId = 0x004C,
                details = mapOf("apple_activity" to "2", "apple_flags" to "1"),
            )
        )
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("AirPods connection/activity nearby", normalized.deviceType)
        assertFalse(normalized.hasCamera)
        assertFalse(normalized.deviceType.contains("listening", ignoreCase = true))
    }

    @Test fun unrelatedNonAppleListeningCategoryIsPreserved() {
        val original = detection(manufacturer = "Other", category = PrivacyCategory.REMOTE_LISTENING)
        assertEquals(original, PrivacyFindingNormalizer.normalize(original))
    }
}
```

In `PrivacyFindingNormalizerTest.kt`, define `detection(...)` as a complete factory over the real `GlassesDetection` constructor (including `mac`, `firstSeen`, `lastSeen`, BLE fields, `fingerprintKey`, and `seenMacs`), with named overrides for every field used above. Do not leave a pseudocode fixture or introduce a second test-only model.

- [ ] **Step 2: Run the focused tests and confirm failure**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppleContinuityDecoderTest' --tests '*PrivacyFindingNormalizerTest'`

Expected: the old positive listening assertion or missing normalizer fails.

- [ ] **Step 3: Remove the listening matcher and produce only cautious Apple activity**

```kotlin
internal fun appleActivityMatchForTest(
    apple: AppleContinuityDecoder.AppleContinuity?,
    rssi: Int,
): Triple<Float, String, String>? = appleActivityMatch(apple, rssi)

private fun appleRemoteListeningMatch(
    apple: AppleContinuityDecoder.AppleContinuity?,
    rssi: Int,
): Triple<Float, String, String>? = null

private fun appleActivityMatch(
    apple: AppleContinuityDecoder.AppleContinuity?,
    rssi: Int,
): Triple<Float, String, String>? {
    val continuity = apple ?: return null
    val hasAirPods = (continuity.flagsByte ?: 0) and
        BleSignatures.APPLE_FLAG_AIRPODS_IN != 0
    val hasActivity = continuity.activity in setOf(1, 2, 3)
    if (!hasAirPods && !hasActivity) return null
    val title = if (hasAirPods) "AirPods connection/activity nearby"
        else "Apple device activity nearby"
    val confidence = if (rssi >= -70) 0.70f else 0.64f
    return Triple(confidence, title, "apple_activity")
}
```

In `checkScanResult`, call `appleActivityMatch` instead of `appleRemoteListeningMatch` and set `bestType` from the returned title. In the final category assignment, put `bestReason == "apple_activity" -> PrivacyCategory.APPLE_CONTINUITY` before string-based category inference so this cautious title cannot fall through to another category.

- [ ] **Step 4: Implement the cross-source normalizer before merge/count/notify**

```kotlin
object PrivacyFindingNormalizer {
    fun normalize(input: GlassesDetection): GlassesDetection {
        val appleEvidence = input.bleCompanyId == 0x004C ||
            input.bleAppleType != null ||
            input.manufacturer.equals("Apple", ignoreCase = true) ||
            input.details.keys.any { it.startsWith("apple_", ignoreCase = true) }
        val listeningClaim = input.category == PrivacyCategory.REMOTE_LISTENING ||
            listOf(input.deviceType, input.deviceName, input.matchReason)
                .filterNotNull().any { it.contains("listening", ignoreCase = true) }
        val alreadyMappedAppleActivity = input.matchReason == "apple_activity"
        if (!appleEvidence || (!listeningClaim && !alreadyMappedAppleActivity)) return input

        val airPodsEvidence = input.deviceType.contains("airpods", ignoreCase = true) ||
            input.details.values.any { it.contains("airpods", ignoreCase = true) } ||
            ((input.bleAppleFlags ?: 0) and BleSignatures.APPLE_FLAG_AIRPODS_IN != 0)
        val safeOwnedName = input.deviceName?.takeIf {
            input.isBonded && it.isNotBlank() && !it.contains("listening", ignoreCase = true)
        }
        val title = safeOwnedName ?: if (airPodsEvidence) "AirPods connection/activity nearby"
        else "Apple device activity nearby"
        val safeDetails = input.details.filterNot { (key, value) ->
            listOf(key, value).any { text ->
                text.contains("listening", ignoreCase = true) ||
                    text.contains("eavesdrop", ignoreCase = true)
            }
        }
        return input.copy(
            deviceType = title,
            deviceName = safeOwnedName,
            hasCamera = false,
            matchReason = "apple_activity",
            category = PrivacyCategory.APPLE_CONTINUITY,
            details = safeDetails + mapOf(
                "evidence" to if (airPodsEvidence)
                    "An Apple device reports connected AirPods and media, call, or video activity."
                else "An Apple device reports a nearby activity state; the specific activity is unavailable.",
                "limitation" to "Live Listen and microphone use cannot be determined from BLE.",
            ),
        )
    }
}
```

Normalize each record immediately after local/backend/badge/Wi-Fi mapping and before `mergePrivacyDetections`, `categorizedDetections`, `threatCount`, or notifier collection:

```kotlin
val normalizedLocal = local.map(PrivacyFindingNormalizer::normalize)
val normalizedRemote = (backend + badge.toPrivacyDetections() + wifi)
    .map(PrivacyFindingNormalizer::normalize)
mergePrivacyDetections(normalizedLocal, normalizedRemote)
```

For a `BadgeThreatEntity`, create `AppleListeningEvidence` only when that same entity has an explicit Apple vendor/AirPods field in `label`, `detail`, `evidence`, `category`, or `code` *and* the same entity contains listening-oriented wording. The badge mapper must not infer Apple from another entity, a neighboring row, the aggregate threat class, or a matching identifier. Rows emitted by `WiFiPrivacyScanner` currently sharing `SkyObjectRepository.glassesDetections` are separated by their producer provenance and mapped by `WifiPrivacySourceAdapter` to `WIFI_ANALYSIS`; they must not be mislabeled as `PHONE_BLE` or dropped during the adapter split.

- [ ] **Step 5: Add a defense-in-depth alert-policy exclusion**

```kotlin
fun fromDetection(detection: GlassesDetection): PrivacyAlertCandidate? {
    val normalized = PrivacyFindingNormalizer.normalize(detection)
    if (normalized.category == PrivacyCategory.APPLE_CONTINUITY) return null
    if (normalized.category.threatLevel < 2 || normalized.isBonded) return null
    val label = normalized.deviceName?.takeIf { it.isNotBlank() } ?: normalized.deviceType
    return PrivacyAlertCandidate(
        key = listOf(normalized.fingerprintKey, normalized.category.name,
            normalized.matchReason).joinToString(":"),
        title = "${normalized.category.label} detected",
        body = "$label nearby (${normalized.rssi} dBm)",
        threatLevel = normalized.category.threatLevel,
        macs = normalized.seenMacs + normalized.mac,
        isBonded = normalized.isBonded,
    )
}
```

- [ ] **Step 6: Run all Privacy and Apple unit tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppleContinuityDecoderTest' --tests '*PrivacyFindingNormalizerTest' --tests '*PrivacyAlertPolicyTest' --tests '*BadgePrivacyMapperTest'`

Expected: PASS, including local, backend-shaped, and badge-shaped Apple listening inputs; unrelated non-Apple categories remain unchanged.

- [ ] **Step 7: Commit the neutralization boundary**

```bash
git add android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/test/java/com/friendorfoe/detection/AppleContinuityDecoderTest.kt android/app/src/test/java/com/friendorfoe/presentation/privacy
git commit -m "android: remove false Apple listening alerts"
```

### Task 4: Make settings observable and gate every configured-backend poller

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/BackendEndpoint.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/di/NetworkModule.kt:202-224`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt:1159-1190`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt:157-238`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt:40-110`
- Create: `android/app/src/test/java/com/friendorfoe/data/BackendEndpointTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/DetectionSettingsTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/BackendPollingGateTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/data/DetectionPrefsObservableTest.kt`

**Interfaces:**
- Consumes: existing `fof_settings` SharedPreferences so current installs keep their values.
- Produces: `DetectionPrefs.settings: StateFlow<DetectionSettings>`, validated `BackendEndpoint`, and `collectBackendWhileEnabled` for AR/Map/Privacy remote collectors.

- [ ] **Step 1: Write failing endpoint and observable-snapshot tests**

```kotlin
class BackendEndpointTest {
    @Test fun acceptsHttpAndNormalizesTrailingSlash() {
        assertEquals(
            "http://192.168.4.20:8000/",
            BackendEndpoint.parse("http://192.168.4.20:8000").getOrThrow().baseUrl
        )
    }

    @Test fun rejectsMissingHostCredentialsAndUnsupportedScheme() {
        listOf("hello", "ftp://host/", "http://user:pass@host/")
            .forEach { assertTrue(BackendEndpoint.parse(it).isFailure) }
    }
}

class DetectionSettingsTest {
    @Test fun snapshotDefaultsMatchCurrentProductDefaults() {
        val value = DetectionSettings.defaults()
        assertTrue(value.sensorBackendEnabled)
        assertFalse(value.phonePrivacyScanEnabled)
        assertFalse(value.privacyNotificationsEnabled)
        assertFalse(value.droneAlertsEnabled)
        assertFalse(value.ultrasonicEnabled)
    }
}
```

Add `DetectionPrefsObservableTest` over the real `fof_settings` SharedPreferences file: construct `DetectionPrefs`, collect `settings`, mutate `privacyEnabled`, `backendUrl`, and one alert setter through the public APIs, and assert that each write produces the expected next snapshot. Restore the original values in `finally`. This proves the listener path rather than only the pure default factory.

- [ ] **Step 2: Run focused tests and confirm the missing-type failure**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BackendEndpointTest' --tests '*DetectionSettingsTest'`

Expected: compilation fails because the new types do not exist.

- [ ] **Step 3: Add an observable settings snapshot without replacing calibration storage**

```kotlin
data class DetectionSettings(
    val adsbEnabled: Boolean,
    val bleRidEnabled: Boolean,
    val wifiEnabled: Boolean,
    val phonePrivacyScanEnabled: Boolean,
    val stalkerEnabled: Boolean,
    val ultrasonicEnabled: Boolean,
    val wifiAnomalyEnabled: Boolean,
    val privacyNotificationsEnabled: Boolean,
    val droneAlertsEnabled: Boolean,
    val helicopterAlertsEnabled: Boolean,
    val militaryAlertsEnabled: Boolean,
    val policeAlertsEnabled: Boolean,
    val sensorBackendEnabled: Boolean,
    val backendOnlyMode: Boolean,
    val backendUrl: String,
) {
    companion object {
        fun defaults() = DetectionSettings(
            adsbEnabled = true,
            bleRidEnabled = true,
            wifiEnabled = true,
            phonePrivacyScanEnabled = false,
            stalkerEnabled = true,
            ultrasonicEnabled = false,
            wifiAnomalyEnabled = true,
            privacyNotificationsEnabled = false,
            droneAlertsEnabled = false,
            helicopterAlertsEnabled = false,
            militaryAlertsEnabled = false,
            policeAlertsEnabled = false,
            sensorBackendEnabled = true,
            backendOnlyMode = false,
            backendUrl = "http://fof-server.local:8000/",
        )
    }
}
```

Permission-gated settings default to false only when their SharedPreferences key is absent. Existing explicit values remain unchanged on upgrade; Task 13 computes effective permission/channel state and never silently turns an existing stored true value into a granted capability.

In `DetectionPrefs`, register one `OnSharedPreferenceChangeListener`, build `snapshot()` from the existing keys, and publish it:

```kotlin
private val _settings = MutableStateFlow(snapshot())
val settings: StateFlow<DetectionSettings> = _settings.asStateFlow()

private val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
    _settings.value = snapshot()
}

init {
    prefs.registerOnSharedPreferenceChangeListener(listener)
}
```

Keep the existing synchronous properties required by `CalibrationSettingsStore`; every setter still writes the same key, so the listener updates Compose consumers.

- [ ] **Step 4: Implement strict backend URL parsing and save only valid values**

```kotlin
@JvmInline
value class BackendEndpoint private constructor(val baseUrl: String) {
    companion object {
        fun parse(raw: String): Result<BackendEndpoint> = runCatching {
            val parsed = raw.trim().toHttpUrlOrNull()
                ?: error("Enter a complete http:// or https:// URL")
            require(parsed.scheme == "http" || parsed.scheme == "https")
            require(parsed.host.isNotBlank())
            require(parsed.username.isBlank() && parsed.password.isBlank())
            BackendEndpoint(parsed.newBuilder().encodedPath("/").query(null).fragment(null).build().toString())
        }
    }
}
```

```kotlin
fun setBackendUrl(raw: String): Result<BackendEndpoint> =
    BackendEndpoint.parse(raw).onSuccess { endpoint ->
        detectionPrefs.backendUrl = endpoint.baseUrl
    }
```

The `backendUrlInterceptor` must call `BackendEndpoint.parse(detectionPrefs.backendUrl).getOrElse { throw IOException("Configured backend URL is invalid") }`; a legacy invalid saved value becomes a controlled request failure surfaced by Info, never a silent request to localhost or a process crash.

- [ ] **Step 5: Add and test the reusable backend poll gate**

```kotlin
internal suspend fun <T> collectBackendWhileEnabled(
    settings: Flow<DetectionSettings>,
    intervalMs: Long,
    clear: () -> Unit,
    fetch: suspend () -> T,
    publish: (T) -> Unit,
    onFailure: (Throwable) -> Unit = {},
) = settings.map { current ->
    BackendPollGate(
        enabled = current.sensorBackendEnabled,
        endpoint = BackendEndpoint.parse(current.backendUrl).getOrNull(),
    )
}.distinctUntilChanged().collectLatest { gate ->
    clear()
    if (!gate.enabled || gate.endpoint == null) return@collectLatest
    while (currentCoroutineContext().isActive) {
        try {
            publish(fetch())
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            onFailure(failure)
        }
        delay(intervalMs)
    }
}

data class BackendPollGate(val enabled: Boolean, val endpoint: BackendEndpoint?)
```

```kotlin
@Test fun disablingBackendCancelsPollingAndClearsRemoteState() = runTest {
    val settings = MutableStateFlow(DetectionSettings.defaults())
    var fetches = 0
    var clears = 0
    val job = launch {
        collectBackendWhileEnabled(settings, 5_000, { clears++ }, { ++fetches }, {})
    }
    runCurrent()
    assertEquals(1, fetches)
    settings.value = settings.value.copy(sensorBackendEnabled = false)
    runCurrent()
    advanceTimeBy(10_000)
    assertEquals(1, fetches)
    assertEquals(2, clears) // initial endpoint start, then disable
    job.cancel()
}

@Test fun changingEndpointCancelsOldLoopAndClearsBeforeRefetch() = runTest {
    val settings = MutableStateFlow(DetectionSettings.defaults())
    val events = mutableListOf<String>()
    val job = launch {
        collectBackendWhileEnabled(settings, 5_000,
            clear = { events += "clear" }, fetch = { "row" }, publish = { events += it })
    }
    runCurrent()
    settings.value = settings.value.copy(backendUrl = "http://field-kit:8000/")
    runCurrent()
    assertEquals(listOf("clear", "row", "clear", "row"), events)
    job.cancel()
}
```

- [ ] **Step 6: Apply the gate to all three configured-backend consumers**

In `ArViewModel`, clear `_sensorBackendOnline` and `_sensorDroneCount`. In `MapViewModel`, clear `_sensorDrones`, `_remoteSensors`, `_sensorMapOnline`, and `_droneAlertCount`. In `PrivacyViewModel`, clear `_backendPrivacyDetections` and publish an interim internal backend-poll state of `Disabled` (or `Failed` on error); Task 11 adapts it to `PrivacySourceHealth.PAUSED`/`FAILED` after that model exists. Do not reference Task 11 types early and do not clear Room History.

```kotlin
data class MapBackendSnapshot(
    val map: DroneMapDto,
    val activeDroneAlertCount: Int?,
)
```

```kotlin
viewModelScope.launch(Dispatchers.IO) {
    collectBackendWhileEnabled(
        settings = detectionPrefs.settings,
        intervalMs = 5_000L,
        clear = {
            _sensorDrones.value = emptyList()
            _remoteSensors.value = emptyList()
            _sensorMapOnline.value = false
            _droneAlertCount.value = 0
        },
        fetch = {
            MapBackendSnapshot(
                map = sensorMapApiService.getDroneMap(),
                activeDroneAlertCount = try {
                    sensorMapApiService.getDroneAlerts().activeDroneCount
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    null
                },
            )
        },
        publish = { response ->
            _sensorDrones.value = response.map.drones.filter { drone ->
                drone.classification in DRONE_CLASSIFICATIONS ||
                    drone.droneId.startsWith("rid_") ||
                    drone.droneId.startsWith("probe_") ||
                    drone.droneId.startsWith("FOF-Drone-") ||
                    drone.droneId.startsWith("FoF-Drone-") ||
                    drone.positionSource == "gps"
            }
            _remoteSensors.value = response.map.sensors
            _sensorMapOnline.value = true
            response.activeDroneAlertCount?.let { _droneAlertCount.value = it }
        },
        onFailure = { _sensorMapOnline.value = false },
    )
}
```

- [ ] **Step 7: Change About/Info state from getter snapshots to StateFlow**

```kotlin
data class InfoSettingsUiState(
    val settings: DetectionSettings = DetectionSettings.defaults(),
    val backendValidationError: String? = null,
    val connectionStatus: ConnectionTestState = ConnectionTestState.Idle,
)

sealed interface ConnectionTestState {
    data object Idle : ConnectionTestState
    data class Checking(val endpoint: BackendEndpoint) : ConnectionTestState
    data class Connected(val endpoint: BackendEndpoint, val serverVersion: String?) : ConnectionTestState
    data class Failed(val endpoint: BackendEndpoint, val message: String) : ConnectionTestState
}

private val backendValidationError = MutableStateFlow<String?>(null)
private val connectionStatus = MutableStateFlow<ConnectionTestState>(ConnectionTestState.Idle)

val uiState: StateFlow<InfoSettingsUiState> = combine(
    detectionPrefs.settings,
    backendValidationError,
    connectionStatus,
) { settings, error, connection ->
    InfoSettingsUiState(settings, error, connection)
}
    .stateIn(viewModelScope, SharingStarted.Eagerly, InfoSettingsUiState())
```

Every retained toggle writes a real `DetectionPrefs` key and restarts only affected local collectors. Remove any UI control that still has no runtime consumer.

Give each AR, Map, and Privacy backend collector exactly one ViewModel-owned job. Endpoint replacement or disable cancels it through `collectLatest`; `onCleared()` cancels the ViewModel scope. Add one test per consumer proving disable clears only its remote fields, endpoint change cancels the old in-flight fetch, and a late cancelled response cannot republish. Local observations and Room History remain intact.

- [ ] **Step 8: Run the settings/backend suite**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BackendEndpointTest' --tests '*DetectionSettingsTest' --tests '*BackendPollingGateTest'`

Expected: PASS with cancellation/clear behavior proven.

Run with an API 35 emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.DetectionPrefsObservableTest`

Expected: PASS with real SharedPreferences mutations observed as new snapshots.

- [ ] **Step 9: Commit observable settings and backend gating**

```bash
git add android/app/src/main/java/com/friendorfoe/data android/app/src/main/java/com/friendorfoe/di/NetworkModule.kt android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt android/app/src/test/java/com/friendorfoe android/app/src/androidTest/java/com/friendorfoe/data/DetectionPrefsObservableTest.kt
git commit -m "android: make settings observable and gate backend"
```

### Task 5: Route History to the exact immutable snapshot

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/data/local/HistoryDao.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/repository/HistoryStore.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/repository/HistoryRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/HistoryModule.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/history/HistoryViewModel.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/history/HistoryUiState.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/history/HistoryScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailSelectionLoader.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/detail/HistoricalDetailScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/history/HistorySelectionTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/history/HistoryScreenTest.kt`

**Interfaces:**
- Consumes: `Screen.HistoricalDetail` introduced in Task 2.
- Produces: `HistoryStore`, an exact-row `DetailSelectionLoader.loadHistorical(Long)`, and `DetailViewModel.loadHistoricalDetail(Long)` that never consults the live object stream.

- [ ] **Step 1: Write the failing selection regression test**

```kotlin
@Test fun selectedHistoryIdWinsOverNewerRowWithSameObjectId() = runTest {
    val old = history(id = 11L, objectId = "abc123", lastSeen = 100L)
    val newer = history(id = 12L, objectId = "abc123", lastSeen = 200L)
    val lookup = RecordingDetailLookup(
        live = liveAircraft(id = "abc123", lastUpdated = 300L),
        snapshots = listOf(old, newer),
    )
    val loader = DetailSelectionLoader(lookup)

    val loaded = loader.loadHistorical(11L)

    assertEquals(11L, loaded?.id)
    assertEquals(100L, loaded?.lastSeen)
    assertEquals(0, lookup.liveReads)
    assertEquals(listOf(11L), lookup.historicalReads)
}

private class RecordingDetailLookup(
    private val live: SkyObject,
    snapshots: List<HistoryEntity>,
) : DetailLookup {
    private val byId = snapshots.associateBy(HistoryEntity::id)
    var liveReads = 0
    val historicalReads = mutableListOf<Long>()

    override fun currentObject(objectId: String): SkyObject? {
        liveReads += 1
        return live.takeIf { it.id == objectId }
    }

    override suspend fun newestSnapshot(objectId: String): HistoryEntity? =
        byId.values.filter { it.objectId == objectId }.maxByOrNull { it.lastSeen }

    override suspend fun snapshot(historyId: Long): HistoryEntity? {
        historicalReads += historyId
        return byId[historyId]
    }
}
```

The fixture creates a live object plus two historical rows sharing the same object ID. The assertion proves that the historical path invokes only `snapshot(historyId)`.

`HistorySelectionTest.kt` defines complete `history(...)` and `liveAircraft(...)` factories over the repository's real `HistoryEntity` and top-level `Aircraft : SkyObject` constructors, with named overrides for the fields above. Reuse `com.friendorfoe.test.MainDispatcherRule` for every ViewModel test in this task; no helper name in this plan is left undefined.

- [ ] **Step 2: Run the test and confirm it fails on missing API**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*HistorySelectionTest'`

Expected: compilation fails because `DetailLookup`, `DetailSelectionLoader`, and the exact-row API do not exist.

- [ ] **Step 3: Add a testable History boundary and exact-row DAO operation**

```kotlin
@Query("SELECT * FROM detection_history WHERE id = :id LIMIT 1")
suspend fun getById(id: Long): HistoryEntity?
```

```kotlin
interface HistoryStore {
    fun observeAll(): Flow<List<HistoryEntity>>
    fun observeByType(objectType: String): Flow<List<HistoryEntity>>
    suspend fun getById(id: Long): HistoryEntity?
    suspend fun getNewestByObjectId(objectId: String): HistoryEntity?
    suspend fun save(entity: HistoryEntity): Long
    suspend fun deleteById(id: Long)
    suspend fun clearAll()
    suspend fun prune(beforeTimeMillis: Long)
}

@Singleton
class HistoryRepository @Inject constructor(
    private val historyDao: HistoryDao,
) : HistoryStore {
    override fun observeAll() = historyDao.getAllHistory()
    override fun observeByType(objectType: String) = historyDao.getHistoryByType(objectType)
    override suspend fun getById(id: Long) = historyDao.getById(id)
    override suspend fun getNewestByObjectId(objectId: String) = historyDao.getByObjectId(objectId)
    override suspend fun save(entity: HistoryEntity) = historyDao.insert(entity)
    override suspend fun deleteById(id: Long) = historyDao.deleteById(id)
    override suspend fun clearAll() = historyDao.deleteAll()
    override suspend fun prune(beforeTimeMillis: Long) = historyDao.deleteOlderThan(beforeTimeMillis)
}

@Module
@InstallIn(SingletonComponent::class)
abstract class HistoryModule {
    @Binds abstract fun bindHistoryStore(implementation: HistoryRepository): HistoryStore
    @Binds abstract fun bindDetailLookup(implementation: RepositoryDetailLookup): DetailLookup
}
```

Update existing repository call sites in this same step. Keep `getByObjectId` only behind `getNewestByObjectId` for the legacy live-detail fallback; History navigation must never call it. `HistoryDao.deleteById` and `deleteAll` already exist and are reused rather than duplicated.

- [ ] **Step 4: Separate route intent in the detail lookup**

```kotlin
interface DetailLookup {
    fun currentObject(objectId: String): SkyObject?
    suspend fun newestSnapshot(objectId: String): HistoryEntity?
    suspend fun snapshot(historyId: Long): HistoryEntity?
}

@Singleton
class RepositoryDetailLookup @Inject constructor(
    private val skyObjects: SkyObjectRepository,
    private val history: HistoryStore,
) : DetailLookup {
    override fun currentObject(objectId: String) =
        skyObjects.skyObjects.value.firstOrNull { it.id == objectId }
    override suspend fun newestSnapshot(objectId: String) = history.getNewestByObjectId(objectId)
    override suspend fun snapshot(historyId: Long) = history.getById(historyId)
}

class DetailSelectionLoader @Inject constructor(
    private val lookup: DetailLookup,
) {
    fun loadCurrent(objectId: String) = lookup.currentObject(objectId)
    suspend fun loadFallback(objectId: String) = lookup.newestSnapshot(objectId)
    suspend fun loadHistorical(historyId: Long) = lookup.snapshot(historyId)
}
```

`loadHistorical` deliberately has no object-ID parameter and no path to `currentObject`.

- [ ] **Step 5: Add historical detail without renaming current states**

Keep `DetailState.AircraftLoaded` and `DetailState.DroneLoaded` so this task does not break the existing `DetailScreen`. Add only:

```kotlin
// Add this member inside the existing sealed DetailState declaration.
data class HistoricalLoaded(val snapshot: HistoryEntity) : DetailState()

fun loadHistoricalDetail(historyId: Long) {
    _nearbyCandidates.value = emptyList()
    _positionTrail.value = emptyList()
    _detailState.value = DetailState.Loading
    viewModelScope.launch {
        _detailState.value = selectionLoader.loadHistorical(historyId)
            ?.let(DetailState::HistoricalLoaded)
            ?: DetailState.Error("Historical detection is no longer available.")
    }
}
```

Replace the current direct `HistoryDao` injection with `DetailSelectionLoader`. `loadDetail(objectId)` continues to use `loadCurrent` followed by `loadFallback`, preserving current non-History links. Add an exhaustive `HistoricalLoaded` branch to `DetailScreen` so the app compiles at this commit boundary.

- [ ] **Step 6: Route the row ID and create the historical screen**

```kotlin
HistoryScreen(onEntryTapped = { rowId ->
    navController.navigate(Screen.HistoricalDetail.createRoute(rowId))
})
```

```kotlin
composable(
    route = Screen.HistoricalDetail.route,
    arguments = listOf(navArgument("historyId") { type = NavType.LongType }),
) { entry ->
    HistoricalDetailScreen(
        historyId = requireNotNull(entry.arguments).getLong("historyId"),
        onBack = navController::popBackStack,
    )
}
```

`HistoricalDetailScreen` invokes `loadHistoricalDetail(historyId)` from `LaunchedEffect(historyId)`. It renders `FofSecondaryScreenHeader(title = "Historical detection", onBack = onBack)`, the selected row's `displayName`, timestamp, source, location-at-detection, confidence, and description. It must not render a live pulse, `Now`, current distance, or current-source status. Its `Error` branch exposes Back and a `Return to History` action.

- [ ] **Step 7: Add confirmed row-delete and Clear-all state**

Define one History state now and reuse it unchanged in Task 16:

```kotlin
data class HistoryUiState(
    val filter: FilterState = FilterState(),
    val totalCount: Int = 0,
    val activeFilterCount: Int = 0,
    val body: CollectionBodyState<HistoryEntity> = CollectionBodyState.Loading,
    val pendingDeletion: PendingHistoryDeletion? = null,
)
```

Task 5 needs `Loading`, `Content`, `Empty`, and deletion state; Task 16 wires `NoMatches`, `Failed`, and retry without defining a competing `HistoryUiState` or body hierarchy.

```kotlin
sealed interface PendingHistoryDeletion {
    data class Row(val id: Long, val label: String) : PendingHistoryDeletion
    data object All : PendingHistoryDeletion
}

fun requestDelete(row: HistoryEntity) {
    _pendingDeletion.value = PendingHistoryDeletion.Row(row.id, row.displayName)
}

fun requestClearAll() { _pendingDeletion.value = PendingHistoryDeletion.All }
fun dismissDeletion() { _pendingDeletion.value = null }

fun confirmDeletion() = viewModelScope.launch {
    when (val pending = _pendingDeletion.value) {
        is PendingHistoryDeletion.Row -> historyStore.deleteById(pending.id)
        PendingHistoryDeletion.All -> historyStore.clearAll()
        null -> return@launch
    }
    _pendingDeletion.value = null
}
```

Use separate `AlertDialog` copy for one row and all rows. A cancel action only dismisses; confirmation invokes exactly one store operation. Add permanent visible copy: `History stays on this device until you delete it or clear app data.` Do not add export.

- [ ] **Step 8: Add reducer and Compose regressions**

```kotlin
@Test fun deleteRequiresConfirmationAndClearUsesDistinctOperation() = runTest {
    val store = FakeHistoryStore(listOf(history(id = 11L)))
    val viewModel = HistoryViewModel(store)

    viewModel.requestDelete(store.rows.single())
    assertEquals(0, store.deletedIds.size)
    viewModel.dismissDeletion()
    assertEquals(0, store.deletedIds.size)

    viewModel.requestDelete(store.rows.single())
    viewModel.confirmDeletion().join()
    assertEquals(listOf(11L), store.deletedIds)

    viewModel.requestClearAll()
    viewModel.confirmDeletion().join()
    assertEquals(1, store.clearCalls)
}

@Test fun tappingRowPassesImmutableDatabaseId() {
    var selected: Long? = null
    compose.setContent {
        HistoryContent(
            state = HistoryUiState(
                totalCount = 2,
                body = CollectionBodyState.Content(listOf(
                    history(id = 11L, objectId = "same"),
                    history(id = 12L, objectId = "same"),
                )),
            ),
            onEntryTapped = { selected = it },
            onRequestDelete = {},
            onRequestClearAll = {},
        )
    }
    compose.onNodeWithTag("history_row_11").performClick()
    assertEquals(11L, selected)
}
```

The test file defines complete `history(...)`, `liveAircraft(...)`, and `FakeHistoryStore` fixtures using every required production-model field. `HistoryContent` groups `CollectionBodyState.Content.rows` for display without replacing the ordered row list in state. The instrumentation test separately opens each confirmation dialog, cancels it, then confirms it; it never calls `setContent` more than once per test.

- [ ] **Step 9: Run History unit/UI tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*HistorySelectionTest'`

Expected: PASS.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.history.HistoryScreenTest`

Expected: PASS for exact row selection, immutable historical presentation, and both confirmation dialogs.

- [ ] **Step 10: Commit exact History routing**

```bash
git add android/app/src/main/java/com/friendorfoe/data/local/HistoryDao.kt android/app/src/main/java/com/friendorfoe/data/repository/HistoryStore.kt android/app/src/main/java/com/friendorfoe/data/repository/HistoryRepository.kt android/app/src/main/java/com/friendorfoe/di/HistoryModule.kt android/app/src/main/java/com/friendorfoe/presentation/history android/app/src/main/java/com/friendorfoe/presentation/detail android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt android/app/src/test/java/com/friendorfoe/presentation/history android/app/src/androidTest/java/com/friendorfoe/presentation/history
git commit -m "android: open exact history snapshots"
```

### Task 6: Put every AR photo write behind explicit Capture then Save

**Files:**
- Modify: `android/app/src/main/AndroidManifest.xml`
- Create: `android/app/src/main/res/xml/share_file_paths.xml`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureArtifacts.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureReviewViewModel.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/AndroidPhotoWriter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/AndroidShareImageFactory.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/CaptureModule.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/ObjectPeek.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureReviewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt:240-590,645-790`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt:235-430,606-790`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ZoomViewSheet.kt:75-438`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/SnapPhotoSheet.kt:96-340`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/ar/CaptureReviewViewModelTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/ar/ObjectPeekTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/ar/AndroidCaptureArtifactsTest.kt`

**Interfaces:**
- Consumes: current CameraX frame/capture callbacks and `SkyObject` selection.
- Produces: platform-neutral `CapturePayload`, `PhotoWriter`, `ShareImageFactory`, and a review state in which only `save()` invokes the MediaStore writer.

- [ ] **Step 1: Write the failing zero-write/one-write contract test**

```kotlin
@get:Rule val mainDispatcherRule = MainDispatcherRule()

@Test fun onlyExplicitSaveWritesPhoto() = runTest {
    val writer = FakePhotoWriter()
    val shares = FakeShareImageFactory()
    val viewModel = CaptureReviewViewModel(writer, shares)
    val draft = CaptureDraft(
        payload = CapturePayload(byteArrayOf(1, 2, 3), "image/jpeg", 640, 480),
        displayName = "friendorfoe_test.jpg",
        description = "Visual capture",
    )

    viewModel.inspect(draft)
    viewModel.share()
    advanceUntilIdle()
    viewModel.discard()
    assertEquals(0, writer.writes.size)
    assertEquals(listOf(draft), shares.requests)

    viewModel.inspect(draft)
    viewModel.save()
    advanceUntilIdle()
    assertEquals(listOf(draft), writer.writes)
}
```

- [ ] **Step 2: Run the focused test and confirm failure**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*CaptureReviewViewModelTest'`

Expected: compilation fails because the explicit writer boundary does not exist.

- [ ] **Step 3: Create platform-neutral artifact boundaries and the reducer**

```kotlin
data class CapturePayload(
    val bytes: ByteArray,
    val mimeType: String,
    val widthPx: Int,
    val heightPx: Int,
)

data class CaptureDraft(
    val payload: CapturePayload,
    val displayName: String,
    val description: String,
)

fun interface PhotoWriter {
    suspend fun write(draft: CaptureDraft): Result<SavedPhoto>
}

@JvmInline value class SavedPhoto(val contentUri: String)

data class ShareRequest(val contentUri: String, val mimeType: String)

fun interface ShareImageFactory {
    suspend fun create(draft: CaptureDraft): Result<ShareRequest>
}

sealed interface CaptureReviewState {
    data object Empty : CaptureReviewState
    data class Reviewing(val draft: CaptureDraft) : CaptureReviewState
    data class Saving(val draft: CaptureDraft) : CaptureReviewState
    data class Saved(val photo: SavedPhoto) : CaptureReviewState
    data class SaveFailed(val draft: CaptureDraft, val message: String) : CaptureReviewState
    data class ShareFailed(val draft: CaptureDraft, val message: String) : CaptureReviewState
}

@HiltViewModel
class CaptureReviewViewModel @Inject constructor(
    private val writer: PhotoWriter,
    private val shareFactory: ShareImageFactory,
) : ViewModel() {
    private val _state = MutableStateFlow<CaptureReviewState>(CaptureReviewState.Empty)
    val state = _state.asStateFlow()
    private val _effects = MutableSharedFlow<CaptureReviewEffect>(extraBufferCapacity = 1)
    val effects = _effects.asSharedFlow()

    fun inspect(draft: CaptureDraft) { _state.value = CaptureReviewState.Reviewing(draft) }
    fun discard() { _state.value = CaptureReviewState.Empty }
    fun share() {
        val draft = currentDraft() ?: return
        viewModelScope.launch {
            shareFactory.create(draft).onSuccess { request ->
                _effects.emit(CaptureReviewEffect.LaunchShare(request))
            }.onFailure {
                _state.value = CaptureReviewState.ShareFailed(draft, "Could not prepare photo to share.")
            }
        }
    }
    fun save() {
        val draft = currentDraft() ?: return
        _state.value = CaptureReviewState.Saving(draft)
        viewModelScope.launch {
            _state.value = writer.write(draft).fold(
                onSuccess = CaptureReviewState::Saved,
                onFailure = { CaptureReviewState.SaveFailed(draft, "Could not save photo.") },
            )
        }
    }

    fun retrySave() {
        if (_state.value is CaptureReviewState.SaveFailed) save()
    }

    private fun currentDraft(): CaptureDraft? = when (val value = _state.value) {
        is CaptureReviewState.Reviewing -> value.draft
        is CaptureReviewState.SaveFailed -> value.draft
        is CaptureReviewState.ShareFailed -> value.draft
        else -> null
    }
}

sealed interface CaptureReviewEffect {
    data class LaunchShare(val request: ShareRequest) : CaptureReviewEffect
}
```

No Android `Bitmap` or `Uri` appears in the JVM-tested reducer contract. Guard Save with one active `Job`; a rapid second tap while `Saving` performs no second write. `CaptureReviewViewModelTest` defines complete fakes for both boundaries and covers Save success, Save failure then Retry, Share failure followed by no gallery retry, double-tap Save suppression, and Discard.

- [ ] **Step 4: Remove every automatic MediaStore call**

Delete the `ZoomViewSheet` `LaunchedEffect` that sets `autoSaved` and calls `saveDetectionPhotos`. Remove `attemptAutoCapture`, `snapAndAutoCapture`, the default-on auto-capture state, its toolbar toggle, and their `LaunchedEffect` call sites. Retain detection/tracking logic, but it may only create an in-memory `CaptureDraft`.

Delete the claim `NOT in any aircraft database`; replace it with evidence-limited text such as `No radio match is currently available`.

- [ ] **Step 5: Make label taps open Object Peek**

```kotlin
data class ObjectPeekState(
    val objectId: String,
    val title: String,
    val evidence: String,
    val canCapture: Boolean,
)

@Composable
fun ObjectPeek(
    state: ObjectPeekState,
    onInspect: () -> Unit,
    onCapture: () -> Unit,
    onFullDetails: () -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text(state.title, style = MaterialTheme.typography.titleMedium)
        Text(state.evidence, style = MaterialTheme.typography.bodyMedium)
        FofActionRow("Inspect", "Open zoom without saving", onClick = onInspect)
        FofActionRow("Capture", "Take a photo and review it before saving",
            enabled = state.canCapture, onClick = onCapture)
        FofActionRow("Full details", "Open identification details", onClick = onFullDetails)
    }
}
```

`ZoomViewSheet` becomes inspect-only. CameraX `ImageCapture.takePicture` uses `OnImageCapturedCallback`, converts the returned `ImageProxy` to JPEG bytes, closes the proxy in `finally`, and creates a `CaptureDraft` without inserting into MediaStore. `SnapPhotoSheet` opens `CaptureReviewScreen`; its camera button text becomes `Capture`, and review actions are `Save`, `Share`, `Discard`. A failed save shows `Retry save` and `Discard`.

- [ ] **Step 6: Implement one gallery writer and one cache-only share factory**

`AndroidPhotoWriter.write` is the only implementation allowed to call `MediaStore.Images.Media.EXTERNAL_CONTENT_URI`, `ContentResolver.insert`, or `openOutputStream` for gallery output. Put those operations behind an injected Android-only `MediaStoreSink` (`insert`, `openOutputStream`, `delete`) so the partial-write path is deterministic in instrumentation. It writes `draft.payload.bytes`; on any failure after URI creation it calls `delete` for that exact URI before returning failure. `AndroidCaptureArtifactsTest` injects a sink whose stream fails after a prefix and asserts one insert, one delete of that URI, and no surviving row.

`AndroidShareImageFactory.create` writes the bytes only beneath `context.cacheDir/shared_captures`, obtains a URI through `FileProvider.getUriForFile(context, "${BuildConfig.APPLICATION_ID}.fileprovider", file)`, and returns `ShareRequest`. Its ACTION_SEND consumer sets `FLAG_GRANT_READ_URI_PERMISSION`. Cache sharing never calls `PhotoWriter` and never inserts a gallery row.

Add:

```xml
<provider
    android:name="androidx.core.content.FileProvider"
    android:authorities="${applicationId}.fileprovider"
    android:exported="false"
    android:grantUriPermissions="true">
    <meta-data
        android:name="android.support.FILE_PROVIDER_PATHS"
        android:resource="@xml/share_file_paths" />
</provider>
```

```xml
<paths xmlns:android="http://schemas.android.com/apk/res/android">
    <cache-path name="shared_captures" path="shared_captures/" />
</paths>
```

Bind `AndroidPhotoWriter` to `PhotoWriter` and `AndroidShareImageFactory` to `ShareImageFactory` in `CaptureModule`. The module takes `@ApplicationContext`; no new dependency is required because AndroidX Core already supplies `FileProvider`.

- [ ] **Step 7: Prove every existing entry path performs zero implicit gallery writes**

`ObjectPeekTest` gets separate tests (one `setContent` per test) for: label tap, Inspect, Zoom, Full details, Capture, Share, Discard, and explicit Save. Each injects a recording `PhotoWriter`; only `Capture` followed by `Save` records one write. The existing main-screen shutter and `SnapPhotoSheet` path receive the same test. `AndroidCaptureArtifactsTest` verifies the FileProvider share URI is readable and that Share creates zero MediaStore rows; a separate test verifies Save creates exactly one row and deletes a partial row after injected stream failure.

- [ ] **Step 8: Run AR capture tests and verify all legacy write paths are gone**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*CaptureReviewViewModelTest'`

Expected: PASS.

Run: `rg -n "autoSaved|attemptAutoCapture|snapAndAutoCapture|NOT in any aircraft database|saveDetectionPhotos|capturePhotoToGallery|captureDualPhoto|saveBitmapToGallery" android/app/src/main/java/com/friendorfoe/presentation/ar`

Expected: no matches.

Run: `rg -n "MediaStore|openOutputStream|ContentResolver.insert" android/app/src/main/java/com/friendorfoe/presentation/ar`

Expected: every match is inside `AndroidPhotoWriter.kt`; `AndroidShareImageFactory.kt` may use a cache `FileOutputStream` but not a `ContentResolver` or MediaStore.

- [ ] **Step 9: Run the Android capture tests**

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.ar.ObjectPeekTest,com.friendorfoe.presentation.ar.AndroidCaptureArtifactsTest`

Expected: all entry paths are read-only until Save; Share remains cache-only; failed partial writes are cleaned up.

- [ ] **Step 10: Commit the explicit capture workflow**

```bash
git add android/app/src/main/AndroidManifest.xml android/app/src/main/res/xml/share_file_paths.xml android/app/src/main/java/com/friendorfoe/di/CaptureModule.kt android/app/src/main/java/com/friendorfoe/presentation/ar android/app/src/test/java/com/friendorfoe/presentation/ar android/app/src/androidTest/java/com/friendorfoe/presentation/ar
git commit -m "android: require explicit photo saves"
```

### Task 7: Split and lock the firmware-exact Badge protocol contract

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeProtocol.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeStatusModels.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeStatusParser.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt:61-782`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeAppearanceSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeDisplayFiltersSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Replace: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeDisplayPolicyTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlStatusParserTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeThemeContractTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeDisplayPolicyContractTest.kt`

**Interfaces:**
- Consumes: the already-shipped firmware JSON observed through Android transports; no firmware source is changed.
- Produces: typed `BadgeDisplayAction`, `BadgeNetworkMode`, `BadgeTheme`, `BadgeDisplayPolicy`, firmware-parity hashes, strict `BadgeConfigReadback<T>`, and `parseBadgeControlStatus` with nullable unknown configuration.

- [ ] **Step 1: Write exact default/hash/payload tests before moving models**

```kotlin
class BadgeThemeContractTest {
    @Test fun defaultsAndHashMatchFirmware() {
        val theme = BadgeTheme.firmwareDefaults()
        assertEquals(
            mapOf(
                "drone" to 0xFEA0, "meta" to 0xF833, "tracker" to 0xF81F,
                "flock" to 0xA81F, "wifi_attack" to 0x07FF, "clear" to 0x2F65,
            ),
            theme.accents
        )
        assertEquals(0xC3AA2A8DL, theme.firmwareHash())
    }

    @Test fun outgoingThemeUsesUnsignedDecimalAndPreservesPalette() {
        val theme = BadgeTheme.firmwareDefaults().copy(palette = "night")
        val json = theme.toJsonObject()
        assertEquals("night", json["palette"].asString)
        assertEquals(65184, json["accents"].asJsonObject["drone"].asInt)
    }

    @Test fun zeroAccentAndUnknownPaletteAreRejected() {
        assertTrue(BadgeTheme.validate(BadgeTheme.firmwareDefaults().copy(
            accents = BadgeTheme.firmwareDefaults().accents + ("drone" to 0)
        )).isFailure)
        assertTrue(BadgeTheme.validate(BadgeTheme.firmwareDefaults().copy(palette = "future")).isFailure)
    }
}
```

```kotlin
class BadgeDisplayPolicyContractTest {
    @Test fun defaultsContainExactThirteenClassesAndHash() {
        val policy = BadgeDisplayPolicy.firmwareDefaults()
        assertEquals(13, policy.classes.size)
        assertEquals(100, policy.classes.getValue("drone").priority)
        assertEquals(BadgeDisplayLane.BOTH, policy.classes.getValue("drone").lane)
        assertEquals(BadgeMinimumProximity.CLOSE, policy.classes.getValue("hid").minProximity)
        assertEquals(0x0DAD6299L, policy.firmwareHash())
    }

    @Test fun disableAndReenablePreserveStoredPriority() {
        val original = BadgeDisplayPolicy.firmwareDefaults()
        val disabled = original.withEnabled("tracker", false)
        val enabled = disabled.withEnabled("tracker", true)
        assertEquals(70, enabled.classes.getValue("tracker").priority)
    }
}
```

- [ ] **Step 2: Add strict-parser failure cases**

```kotlin
@Test fun blankOrMissingVersionIsNotValidStatus() {
    assertNull(parseBadgeControlStatus("{}", receivedAtElapsedMs = 10))
    assertNull(parseBadgeControlStatus("{\"version\":\"\"}", receivedAtElapsedMs = 10))
}

@Test fun missingThemeFieldsNeverBecomeEditableDefaults() {
    val status = parseBadgeControlStatus(
        """{"version":"0.64.65","theme_hash":1,"theme":{"version":1}}""",
        receivedAtElapsedMs = 10,
    )!!
    assertFalse(status.themeReadback.isEditable)
    assertNull(status.themeReadback.value)
}

@Test fun zeroHashesRemainUnknown() {
    val status = parseBadgeControlStatus(validStatusJson(themeHash = 0, policyHash = 0), 10)!!
    assertFalse(status.themeReadback.isEditable)
    assertFalse(status.policyReadback.isEditable)
}

@Test fun persistedModeWinsOverRuntimeNetworkOff() {
    val status = parseBadgeControlStatus(
        """{
          "version":"0.64.65",
          "mode":"usb_only",
          "network_mode":"off",
          "reporting":{"network_mode":"off"}
        }""",
        receivedAtElapsedMs = 10,
    )!!
    assertEquals(BadgeNetworkMode.USB_ONLY, status.networkModeReadback.value)
    assertTrue(status.networkModeReadback.isEditable)
}
```

- [ ] **Step 3: Run the Badge contract tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.badge.BadgeThemeContractTest' --tests 'com.friendorfoe.data.badge.BadgeDisplayPolicyContractTest' --tests 'com.friendorfoe.data.badge.BadgeControlStatusParserTest'`

Expected: missing types/signatures and the current permissive `{}` parser fail.

- [ ] **Step 4: Implement wire enums and immutable contract types**

```kotlin
enum class BadgeDisplayLane(val wireValue: String, val firmwareByte: Int) {
    OFF("off", 0), LOWER("lower", 1), TOP("top", 2), BOTH("both", 3),
}

enum class BadgeMinimumProximity(val wireValue: String, val firmwareByte: Int) {
    PRESENT("present", 0), NEAR("near", 1), CLOSE("close", 2),
}

enum class BadgeDisplayAction(val wireValue: String) {
    NEXT("next"), DETAIL("detail"), BACK("back"),
}

enum class BadgeNetworkMode(val wireValue: String) {
    USB_ONLY("usb_only"), LOCAL_AP("local_ap"), BACKEND("backend"),
}
```

```kotlin
data class BadgeTheme(
    val version: Int,
    internal val palette: String,
    val background: String,
    val intensity: Int,
    val accents: Map<String, Int>,
) {
    companion object {
        val accentOrder = listOf("drone", "meta", "tracker", "flock", "wifi_attack", "clear")
        val allowedPalettes = setOf("field", "night", "neon", "mono")
        val allowedBackgrounds = setOf("dark", "dim", "scanline")

        fun firmwareDefaults() = BadgeTheme(
            version = 1,
            palette = "field",
            background = "dark",
            intensity = 100,
            accents = linkedMapOf(
                "drone" to 0xFEA0, "meta" to 0xF833, "tracker" to 0xF81F,
                "flock" to 0xA81F, "wifi_attack" to 0x07FF, "clear" to 0x2F65,
            ),
        )

        fun validate(value: BadgeTheme): Result<BadgeTheme> = runCatching {
            require(value.version == 1)
            require(value.palette in allowedPalettes)
            require(value.background in allowedBackgrounds)
            require(value.intensity in 25..100)
            require(value.accents.keys == accentOrder.toSet())
            require(value.accents.values.all { it in 1..0xFFFF })
            value
        }
    }

    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        addProperty("palette", palette)
        addProperty("background", background)
        addProperty("brightness", intensity)
        add("accents", JsonObject().apply {
            accentOrder.forEach { key -> addProperty(key, accents.getValue(key)) }
        })
    }
}
```

Create and validate the exact 13 policy rows without coercing invalid values during serialization:

```kotlin
data class BadgeDisplayRule(
    val enabled: Boolean,
    val lane: BadgeDisplayLane,
    val minProximity: BadgeMinimumProximity,
    val priority: Int,
)

data class BadgeDisplayPolicy(
    val version: Int,
    val classes: Map<String, BadgeDisplayRule>,
) {
    companion object {
        val classOrder = listOf(
            "drone", "meta", "tracker", "wifi_attack", "skimmer", "camera", "flock",
            "lock", "hid", "beacon", "event_badge", "auracast", "scanner_status",
        )

        fun firmwareDefaults() = BadgeDisplayPolicy(1, linkedMapOf(
            "drone" to BadgeDisplayRule(true, BadgeDisplayLane.BOTH, BadgeMinimumProximity.PRESENT, 100),
            "meta" to BadgeDisplayRule(true, BadgeDisplayLane.BOTH, BadgeMinimumProximity.PRESENT, 95),
            "tracker" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 70),
            "wifi_attack" to BadgeDisplayRule(true, BadgeDisplayLane.BOTH, BadgeMinimumProximity.PRESENT, 90),
            "skimmer" to BadgeDisplayRule(true, BadgeDisplayLane.BOTH, BadgeMinimumProximity.NEAR, 88),
            "camera" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 65),
            "flock" to BadgeDisplayRule(true, BadgeDisplayLane.BOTH, BadgeMinimumProximity.PRESENT, 85),
            "lock" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 55),
            "hid" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.CLOSE, 45),
            "beacon" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 30),
            "event_badge" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 35),
            "auracast" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.NEAR, 20),
            "scanner_status" to BadgeDisplayRule(true, BadgeDisplayLane.LOWER, BadgeMinimumProximity.PRESENT, 10),
        ))

        fun validate(value: BadgeDisplayPolicy): Result<BadgeDisplayPolicy> = runCatching {
            require(value.version == 1)
            require(value.classes.keys == classOrder.toSet())
            value.classes.values.forEach { row ->
                require(row.priority in 0..100)
                require(!row.enabled || row.lane != BadgeDisplayLane.OFF)
            }
            value
        }
    }

    fun withEnabled(key: String, enabled: Boolean): BadgeDisplayPolicy {
        val current = classes.getValue(key)
        val defaults = firmwareDefaults().classes.getValue(key)
        val next = if (enabled) current.copy(
            enabled = true,
            lane = if (current.lane == BadgeDisplayLane.OFF) defaults.lane else current.lane,
            minProximity = if (current.lane == BadgeDisplayLane.OFF) defaults.minProximity
                else current.minProximity,
            priority = current.priority,
        ) else current.copy(enabled = false, lane = BadgeDisplayLane.OFF)
        return copy(classes = LinkedHashMap(classes).apply { put(key, next) })
    }

    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        add("classes", JsonObject().apply {
            classOrder.forEach { key ->
                val row = classes.getValue(key)
                add(key, JsonObject().apply {
                    addProperty("enabled", row.enabled)
                    addProperty("lane", row.lane.wireValue)
                    addProperty("min_proximity", row.minProximity.wireValue)
                    addProperty("priority", row.priority)
                })
            }
        })
    }
}
```

- [ ] **Step 5: Implement firmware byte-order hash parity**

```kotlin
private class FirmwareFnv1a {
    private var hash = 0x811C9DC5u
    fun byte(value: Int) { hash = (hash xor (value and 0xFF).toUInt()) * 0x01000193u }
    fun asciiZ(value: String) { value.encodeToByteArray().forEach { byte(it.toInt()) }; byte(0) }
    fun value(): Long = hash.toLong() and 0xFFFF_FFFFL
}

fun BadgeTheme.firmwareHash(): Long = FirmwareFnv1a().apply {
    byte(version)
    byte(intensity)
    asciiZ(palette)
    asciiZ(background)
    BadgeTheme.accentOrder.forEach { key ->
        val color = accents.getValue(key)
        byte(color ushr 8)
        byte(color)
    }
}.value()

fun BadgeDisplayPolicy.firmwareHash(): Long = FirmwareFnv1a().apply {
    byte(version)
    BadgeDisplayPolicy.classOrder.forEach { key ->
        val row = classes.getValue(key)
        byte(if (row.enabled) 1 else 0)
        byte(row.lane.firmwareByte)
        byte(row.minProximity.firmwareByte)
        byte(row.priority)
    }
}.value()
```

- [ ] **Step 6: Implement strict independent readbacks**

```kotlin
data class BadgeConfigReadback<T>(
    val value: T?,
    val hash: Long?,
    val issue: String?,
) {
    val isEditable: Boolean get() = value != null && hash != null && hash != 0L && issue == null
}

data class BadgeNetworkModeReadback(
    val value: BadgeNetworkMode?,
    val issue: String?,
) {
    val isEditable: Boolean get() = value != null && issue == null
}

data class BadgeControlStatus(
    val version: String,
    val receivedAtElapsedMs: Long,
    val themeReadback: BadgeConfigReadback<BadgeTheme>,
    val policyReadback: BadgeConfigReadback<BadgeDisplayPolicy>,
    val networkModeReadback: BadgeNetworkModeReadback,
    val entities: List<BadgeThreatEntity>,
    val scanners: List<BadgeScannerStatus>,
    val displayState: BadgeDisplayState?,
    val debugBridge: BadgeDebugBridgeEvidence?,
    val reporting: BadgeReportingStatus,
    val counts: BadgeThreatCounts,
    val bleControl: BadgeBleControlStatus,
    val safeMode: Boolean,
    val safeReason: String,
    val resetReason: String,
    val crashCount: Int,
    val recoveryMode: String,
    val stackFreeBytes: Map<String, Int>,
    val heapInternalFreeBytes: Long,
    val heapInternalMinimumFreeBytes: Long,
    val psramFreeBytes: Long,
)

data class BadgeDebugBridgeEvidence(
    val physicalSerialPort: String?,
    val physicalResponseAtElapsedMs: Long?,
    val lastError: String?,
)
```

`parseBadgeControlStatus(json, receivedAtElapsedMs)` returns `null` unless the outer JSON parses and `version` is nonblank. Theme and Policy parsing each return an `issue` instead of a fallback object when a required key, accent/class, enum, version, hash, or safe range is invalid. A Theme/Policy computed hash mismatch also returns unknown configuration, while valid privacy entities remain usable. Persisted network mode is read only from top-level `mode`, which shipped status builds from `badge_mode_to_string`; only `usb_only`, `local_ap`, and `backend` become editable. Top-level/reporting `network_mode` is runtime session state (`off`, `local_ap`, or `backend`) and must never override persisted `mode`. A missing, blank, or unknown persisted `mode` remains an issue rather than becoming a default.

- [ ] **Step 7: Serialize exact command payloads and typed navigation**

```kotlin
fun badgeThemeCommandJson(theme: BadgeTheme): JsonObject = JsonObject().apply {
    BadgeTheme.validate(theme).getOrThrow()
    addProperty("cmd", "badge_theme")
    addProperty("persist", true)
    add("theme", theme.toJsonObject())
}

fun badgeDisplayNavCommandJson(action: BadgeDisplayAction): JsonObject = JsonObject().apply {
    addProperty("cmd", "display_nav")
    addProperty("action", action.wireValue)
}

fun badgeDisplayPolicyCommandJson(policy: BadgeDisplayPolicy): JsonObject = JsonObject().apply {
    BadgeDisplayPolicy.validate(policy).getOrThrow()
    addProperty("cmd", "badge_display_policy")
    addProperty("persist", true)
    add("policy", policy.toJsonObject())
}

fun badgeNetworkModeCommandJson(mode: BadgeNetworkMode): JsonObject = JsonObject().apply {
    addProperty("cmd", "set_mode")
    addProperty("mode", mode.wireValue)
    addProperty("persist", true)
}

fun badgeRebootCommandJson(): JsonObject = JsonObject().apply {
    addProperty("cmd", "reboot")
}

fun badgeBootloaderCommandJson(): JsonObject = JsonObject().apply {
    addProperty("cmd", "bootloader")
}
```

Add exact serializer assertions for `{"cmd":"reboot"}` and `{"cmd":"bootloader"}`. Recovery is a distinct serial acknowledgement class; these payloads must not be routed through an invented `FOF_CTL_OK` response.

Lock the exact compact UTF-8 navigation payloads used by the BLE MTU gate:

```kotlin
@Test fun navigationSerializerLengthsMatchMtuContract() {
    val payloads = BadgeDisplayAction.entries.associateWith { action ->
        badgeDisplayNavCommandJson(action).toString()
    }
    assertEquals("{\"cmd\":\"display_nav\",\"action\":\"next\"}", payloads.getValue(BadgeDisplayAction.NEXT))
    assertEquals("{\"cmd\":\"display_nav\",\"action\":\"detail\"}", payloads.getValue(BadgeDisplayAction.DETAIL))
    assertEquals("{\"cmd\":\"display_nav\",\"action\":\"back\"}", payloads.getValue(BadgeDisplayAction.BACK))
    assertEquals(listOf(37, 39, 37), BadgeDisplayAction.entries.map {
        payloads.getValue(it).encodeToByteArray().size
    })
}
```

The Theme JSON property remains named `brightness` on the wire even though the Kotlin/UI property is `intensity`.

- [ ] **Step 8: Migrate every current Android call site in the same compile unit**

Update repository parsers to pass one captured `receivedAtElapsedMs`; update List/Privacy/Badge callers from `status.theme/status.themeHash` to `status.themeReadback.value/hash`, from `status.displayPolicy/status.displayPolicyHash` to `status.policyReadback.value/hash`, and from untyped `mode` fallbacks to `networkModeReadback`; update draft controls from `brightness` and string lane/proximity values to `intensity` and typed enums. An invalid/unknown readback renders disabled rather than falling back to defaults. This mechanical compatibility work is required in Task 7 so its commit compiles; Tasks 8 and 10 subsequently remove the duplicated List/Privacy controls.

- [ ] **Step 9: Run all Badge contract/parser tests and compile all call sites**

Run: `cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.badge.*ContractTest' --tests 'com.friendorfoe.data.badge.BadgeControlStatusParserTest'`

Expected: PASS with hashes `0xC3AA2A8D` and `0x0DAD6299`.

Run: `cd android && ./gradlew testDebugUnitTest`

Expected: PASS; no old Badge field/parser signature remains.

- [ ] **Step 10: Commit the protocol split**

```bash
git add android/app/src/main/java/com/friendorfoe/data/badge android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeAppearanceSection.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeDisplayFiltersSection.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt android/app/src/test/java/com/friendorfoe/data/badge
git commit -m "android: lock badge firmware contracts"
```

### Task 8: Enforce badge freshness, capabilities, acknowledgements, and app lifecycle

**Files:**
- Modify: `android/app/build.gradle.kts`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeConnection.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeCommand.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeCommandEvidence.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeHttpClients.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeDebugBridgeConfig.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeReleaseCertification.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeControlPort.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/time/MonotonicClock.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/BadgeModule.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/ApplicationCoroutineModule.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt:783-2106`
- Modify: `android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeConnectionCapabilityTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeControlAcknowledgementTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbLineParserTest.kt`

**Interfaces:**
- Consumes: strict status/readback types from Task 7.
- Produces: `BadgeControlPort`, transport evidence, pure capability matrix, typed commands/outcomes, and an app-scoped concrete repository that blocks unsupported operations itself.

- [ ] **Step 1: Write failing freshness/capability matrix tests**

```kotlin
@Test fun usbApAndDebugUseTenSecondLiveWindowAndSixtySecondExpiry() {
    listOf(BadgeTransport.USB_SERIAL, BadgeTransport.LOCAL_AP_HTTP, BadgeTransport.DEBUG_BRIDGE)
        .forEach { transport ->
            assertEquals(BadgeConnectionPhase.LIVE, badgeFreshness(transport, 0, 9_999))
            assertEquals(BadgeConnectionPhase.STALE, badgeFreshness(transport, 0, 10_000))
            assertEquals(BadgeConnectionPhase.EXPIRED, badgeFreshness(transport, 0, 60_000))
        }
}

@Test fun bleOnlySupportsStatusAndMtuQualifiedNavigation() {
    val evidence = liveEvidence(BadgeTransport.BLE_GATT, negotiatedBleMtu = 41).copy(
        releaseCertifiedMutations = setOf(BadgeCapability.DISPLAY_NAV),
    )
    assertEquals(SUPPORTED, badgeCapability(evidence, READ_STATUS))
    assertEquals(SUPPORTED, badgeCapability(evidence, DISPLAY_NAV, payloadBytes = 37))
    assertEquals(UNSUPPORTED, badgeCapability(evidence, DISPLAY_NAV, payloadBytes = 39))
    assertEquals(UNSUPPORTED, badgeCapability(evidence, NETWORK_MODE))
    assertEquals(UNSUPPORTED, badgeCapability(evidence, THEME_V1))
    assertEquals(UNSUPPORTED, badgeCapability(evidence, DISPLAY_POLICY_V1))
    assertEquals(UNKNOWN, badgeCapability(
        evidence.copy(releaseCertifiedMutations = emptySet()),
        DISPLAY_NAV,
        payloadBytes = 37,
    ))
}

@Test fun bleNavigationRequiresBondedEncryptedGattEvidence() {
    val certified = liveEvidence(BadgeTransport.BLE_GATT, negotiatedBleMtu = 64).copy(
        releaseCertifiedMutations = setOf(BadgeCapability.DISPLAY_NAV),
    )
    assertEquals(UNKNOWN, badgeCapability(certified.copy(bleBonded = false),
        DISPLAY_NAV, payloadBytes = 37))
    assertEquals(UNKNOWN, badgeCapability(certified.copy(bleEncrypted = false),
        DISPLAY_NAV, payloadBytes = 37))
}

@Test fun debugMutationRequiresPresentBlankPhysicalLastError() {
    val certified = liveEvidence(BadgeTransport.DEBUG_BRIDGE).copy(
        releaseCertifiedMutations = setOf(BadgeCapability.THEME_V1),
    )
    assertEquals(SUPPORTED, badgeCapability(certified.copy(debugBridgeLastError = ""), THEME_V1))
    assertEquals(UNKNOWN, badgeCapability(certified.copy(debugBridgeLastError = null), THEME_V1))
    assertEquals(UNKNOWN, badgeCapability(certified.copy(debugBridgeLastError = "serial timeout"), THEME_V1))
}

@Test fun debugBridgeNeverSupportsRecovery() {
    val certified = liveEvidence(BadgeTransport.DEBUG_BRIDGE).copy(
        releaseCertifiedMutations = setOf(BadgeCapability.REBOOT, BadgeCapability.BOOTLOADER),
    )
    assertEquals(UNSUPPORTED, badgeCapability(certified, REBOOT))
    assertEquals(UNSUPPORTED, badgeCapability(certified, BOOTLOADER))
}

@Test fun timeAdvancingWithoutAnotherStatusMakesConnectionStaleThenExpired() = runTest {
    val clock = FakeMonotonicClock(0)
    val repository = repositoryWithValidUsbStatus(clock)
    assertEquals(BadgeConnectionPhase.LIVE, repository.state.value.connection.phase)
    clock.advanceBy(10_000)
    runCurrent()
    assertEquals(BadgeConnectionPhase.STALE, repository.state.value.connection.phase)
    clock.advanceBy(50_000)
    runCurrent()
    assertEquals(BadgeConnectionPhase.EXPIRED, repository.state.value.connection.phase)
}

@Test fun oldTimestampNeverResurrectsDisconnectedOrErrorTransport() {
    listOf(BadgeConnectionPhase.DISCONNECTED, BadgeConnectionPhase.ERROR).forEach { phase ->
        val evidence = liveEvidence(BadgeTransport.USB_SERIAL).copy(phase = phase)
        assertEquals(phase, evidence.aged(nowElapsedMs = 2_000L).phase)
    }
}
```

- [ ] **Step 2: Write failing HTTP/USB acknowledgement tests**

```kotlin
@Test fun http2xxOkFalseIsFailure() {
    assertEquals(
        BadgeCommandOutcome.Failed("theme save failed"),
        parseHttpCommandOutcome(200, """{"ok":false,"error":"theme save failed"}""")
    )
}

@Test fun malformedOrMissingOkHttpBodiesFailClosed() {
    assertTrue(parseHttpCommandOutcome(200, "not-json") is BadgeCommandOutcome.Failed)
    assertTrue(parseHttpCommandOutcome(200, "{\"theme_hash\":1}") is BadgeCommandOutcome.Failed)
}

@Test fun networkAppliedFalseFailsEvenWhenOkIsTrue() {
    assertTrue(parseHttpCommandOutcome(
        200,
        """{"ok":true,"applied":false,"network_mode":"off"}""",
    ) is BadgeCommandOutcome.Failed)
    assertTrue(parseUsbControlLine(
        "FOF_CTL_OK:{\"applied\":false,\"network_mode\":\"off\"}",
    ) is BadgeCommandOutcome.Failed)
}

@Test fun usbErrorBodyIsFailureAndOkHashIsRetained() {
    assertTrue(parseUsbControlLine("FOF_CTL_ERROR:{\"error\":\"bad policy\"}") is BadgeCommandOutcome.Failed)
    val ok = parseUsbControlLine("FOF_CTL_OK:{\"ok\":true,\"theme_hash\":3282709133}")
        as BadgeCommandOutcome.Acknowledged
    assertEquals(0xC3AA2A8DL, ok.acknowledgement.themeHash)
}

@Test fun recoveryLinesCompleteOnlyTheirMatchingPendingUsbCommand() {
    assertEquals(BadgeCommandOutcome.Acknowledged(
            BadgeControlAcknowledgement("Reboot acknowledged")),
        parseUsbCommandLine(BadgeCommand.Reboot, "FOF_REBOOT:OK"))
    assertEquals(BadgeCommandOutcome.Acknowledged(
            BadgeControlAcknowledgement("Bootloader acknowledged")),
        parseUsbCommandLine(BadgeCommand.EnterBootloader, "FOF_BOOTLOADER:OK"))
    assertNull(parseUsbCommandLine(BadgeCommand.Reboot, "FOF_BOOTLOADER:OK"))
    assertNull(parseUsbCommandLine(BadgeCommand.ApplyTheme(BadgeTheme.firmwareDefaults()),
        "FOF_REBOOT:OK"))
}

@Test fun commandClientOutlivesShortPollButLateAckStillTimesOut() {
    val clients = badgeHttpClients(OkHttpClient())
    assertEquals(1_200, clients.status.readTimeoutMillis)
    assertEquals(6_000, clients.command.readTimeoutMillis)
    assertFalse(clients.command.retryOnConnectionFailure)
    val acknowledged = acknowledged(themeHash = 0xC3AA2A8DL)
    assertEquals(acknowledged, enforceAckDeadline(acknowledged, elapsedMs = 5_000))
    assertEquals(BadgeCommandOutcome.TimedOut,
        enforceAckDeadline(acknowledged, elapsedMs = 5_001))
}
```

- [ ] **Step 3: Run the focused tests and confirm failure**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeConnectionCapabilityTest' --tests '*BadgeControlAcknowledgementTest' --tests '*BadgeUsbLineParserTest'`

Expected: compilation fails because the evidence/command APIs do not exist.

- [ ] **Step 4: Implement connection phases and the conservative matrix**

```kotlin
enum class BadgeTransport { USB_SERIAL, LOCAL_AP_HTTP, BLE_GATT, DEBUG_BRIDGE }
enum class BadgeConnectionPhase { DISCONNECTED, PERMISSION_NEEDED, CONNECTING, TRANSPORT_OPEN, LIVE, STALE, EXPIRED, ERROR }
enum class BadgeCapability { READ_STATUS, DISPLAY_NAV, NETWORK_MODE, THEME_V1, DISPLAY_POLICY_V1, REBOOT, BOOTLOADER }
enum class BadgeCapabilitySupport { SUPPORTED, UNSUPPORTED, UNKNOWN }

data class BadgeConnectionEvidence(
    val transport: BadgeTransport?,
    val phase: BadgeConnectionPhase,
    val lastValidStatusAtElapsedMs: Long?,
    val protocolVersion: String?,
    val targetId: String?,
    val usbCandidateCount: Int?,
    val exactEspressifVendorMatch: Boolean,
    val serialInterfaceReadable: Boolean,
    val badgeApEndpoint: String?,
    val negotiatedBleMtu: Int?,
    val fofBleServicePresent: Boolean,
    val bleStatusCharacteristicPresent: Boolean,
    val bleControlCharacteristicPresent: Boolean,
    val bleBonded: Boolean,
    val bleEncrypted: Boolean,
    val debugBridgeSerialPort: String?,
    val debugPhysicalStatusAtElapsedMs: Long?,
    val debugBridgeLastError: String?,
    val releaseCertifiedMutations: Set<BadgeCapability>,
)
```

Freshness is pure and uses the approved transport windows:

```kotlin
fun badgeFreshness(
    transport: BadgeTransport,
    receivedAtElapsedMs: Long,
    nowElapsedMs: Long,
): BadgeConnectionPhase {
    val ageMs = (nowElapsedMs - receivedAtElapsedMs).coerceAtLeast(0L)
    val staleAfterMs = when (transport) {
        BadgeTransport.BLE_GATT -> 20_000L
        BadgeTransport.USB_SERIAL,
        BadgeTransport.LOCAL_AP_HTTP,
        BadgeTransport.DEBUG_BRIDGE -> 10_000L
    }
    return when {
        ageMs >= 60_000L -> BadgeConnectionPhase.EXPIRED
        ageMs >= staleAfterMs -> BadgeConnectionPhase.STALE
        else -> BadgeConnectionPhase.LIVE
    }
}

fun BadgeConnectionEvidence.aged(nowElapsedMs: Long): BadgeConnectionEvidence {
    val activeTransport = transport ?: return copy(phase = BadgeConnectionPhase.DISCONNECTED)
    if (phase != BadgeConnectionPhase.LIVE && phase != BadgeConnectionPhase.STALE) return this
    val receivedAt = lastValidStatusAtElapsedMs ?: return this
    return copy(phase = badgeFreshness(activeTransport, receivedAt, nowElapsedMs))
}
```

`badgeCapability` returns `UNKNOWN` unless phase is `LIVE`, the protocol version is nonblank, and the transport-specific evidence is complete. USB also requires exactly one `0x303A` candidate and a readable serial interface. AP requires `badgeApEndpoint == "http://192.168.4.1"`. BLE requires the expected FoF service plus status/control characteristics; mutation additionally requires `bleBonded == true` and `bleEncrypted == true`. BLE can support navigation only when `(payloadBytes ?: Int.MAX_VALUE) <= negotiatedMtu - 3`; it supports no configuration/recovery classes otherwise. A known insufficient MTU or intrinsically unsupported transport/command is `UNSUPPORTED`; otherwise-valid live evidence missing certification, bond/encryption evidence, or debug physical error evidence is `UNKNOWN`. Debug bridge can support configuration/navigation only when its physical serial metadata is nonblank and `debugBridgeLastError` is present and blank; its `LIVE` phase is derived from the physical timestamp as described in Step 6. `REBOOT` and `BOOTLOADER` additionally require a nonblank derived `targetId` and remain **direct-USB-only**; the existing debug bridge cannot parse firmware's dedicated recovery ACK prefixes and is intrinsically unsupported for recovery. Every command capability—`DISPLAY_NAV`, `NETWORK_MODE`, `THEME_V1`, `DISPLAY_POLICY_V1`, `REBOOT`, and `BOOTLOADER`—requires that exact capability in `releaseCertifiedMutations` for the active transport. Only `READ_STATUS` is evidence-only. The checked-in certification starts empty; Task 18 may add one transport/command pair only after recorded physical evidence. Tests inject a certification without changing the release default.

Populate target identity from transport evidence, not from an optional status JSON field: USB uses the one claimed device/serial-interface identity, AP uses the fixed badge endpoint, BLE uses the connected GATT address, and debug bridge uses its physical serial port. Redact these values in normal diagnostics as needed. Configuration does not require the optional status body to invent an ID; recovery requires the derived direct-USB target and an unambiguous connection.

```kotlin
data class BadgeReleaseCertification(
    val mutationsByTransport: Map<BadgeTransport, Set<BadgeCapability>> = emptyMap(),
) {
    fun forTransport(transport: BadgeTransport?): Set<BadgeCapability> =
        transport?.let { mutationsByTransport[it] }.orEmpty()
}

internal val CheckedInBadgeReleaseCertification = BadgeReleaseCertification()
```

`BadgeModule` provides `CheckedInBadgeReleaseCertification` as a singleton, so release certification starts empty. Inject it into the app-scoped repository and, whenever transport evidence is rebuilt, set `releaseCertifiedMutations = certification.forTransport(transport)`. Unit tests construct the repository with a nonempty certification explicitly; opening a transport never adds certification at runtime.

Make the debug bridge an injected debug-build-only transport. In `android/app/build.gradle.kts`, enable `buildConfig`, read an optional `badgeDebugBridgeUrl` Gradle property for the debug variant, default debug to `http://10.0.2.2:8765/`, and emit an empty URL for release. Do not hard-code a phone loopback URL in production:

```kotlin
data class BadgeDebugBridgeConfig(
    val enabled: Boolean,
    val baseUrl: HttpUrl?,
)

fun badgeDebugBridgeConfig(
    isDebug: Boolean,
    configuredUrl: String,
): BadgeDebugBridgeConfig = BadgeDebugBridgeConfig(
    enabled = isDebug && configuredUrl.toHttpUrlOrNull() != null,
    baseUrl = configuredUrl.toHttpUrlOrNull().takeIf { isDebug },
)
```

`BadgeModule` provides this from `BuildConfig.DEBUG` and `BuildConfig.BADGE_DEBUG_BRIDGE_BASE_URL`. The repository does not poll, discover, or execute `DEBUG_BRIDGE` when `enabled == false`; the transport is absent rather than merely hidden in UI. Add JVM tests for default emulator debug configuration, an overridable `-PbadgeDebugBridgeUrl=http://127.0.0.1:8765/`, malformed URL rejection, and release configuration that cannot discover or execute the bridge.

- [ ] **Step 5: Implement typed commands and outcomes**

```kotlin
sealed interface BadgeCommand {
    data class ApplyTheme(val theme: BadgeTheme) : BadgeCommand
    data class ApplyPolicy(val policy: BadgeDisplayPolicy) : BadgeCommand
    data class SetNetworkMode(val mode: BadgeNetworkMode) : BadgeCommand
    data class NavigateDisplay(val action: BadgeDisplayAction) : BadgeCommand
    data object Reboot : BadgeCommand
    data object EnterBootloader : BadgeCommand
}

sealed interface BadgeCommandOutcome {
    data class Acknowledged(val acknowledgement: BadgeControlAcknowledgement) : BadgeCommandOutcome
    data class Accepted(val message: String) : BadgeCommandOutcome
    data class Failed(val message: String) : BadgeCommandOutcome
    data class Unsupported(val reason: String) : BadgeCommandOutcome
    data object TimedOut : BadgeCommandOutcome
}

enum class BadgeRuntimeNetworkMode(val wireValue: String) {
    OFF("off"), LOCAL_AP("local_ap"), BACKEND("backend"),
}

fun BadgeNetworkMode.expectedRuntimeMode(): BadgeRuntimeNetworkMode = when (this) {
    BadgeNetworkMode.USB_ONLY -> BadgeRuntimeNetworkMode.OFF
    BadgeNetworkMode.LOCAL_AP -> BadgeRuntimeNetworkMode.LOCAL_AP
    BadgeNetworkMode.BACKEND -> BadgeRuntimeNetworkMode.BACKEND
}

data class BadgeControlAcknowledgement(
    val message: String,
    val themeHash: Long? = null,
    val policyHash: Long? = null,
    val networkApplied: Boolean? = null,
    val runtimeNetworkMode: BadgeRuntimeNetworkMode? = null,
)

data class BadgeRepositoryState(
    val connection: BadgeConnectionEvidence,
    val controlStatus: BadgeControlStatus?,
    val lastCommandOutcome: BadgeCommandOutcome? = null,
)

fun BadgeCommand.requiredCapability(): BadgeCapability = when (this) {
    is BadgeCommand.ApplyTheme -> BadgeCapability.THEME_V1
    is BadgeCommand.ApplyPolicy -> BadgeCapability.DISPLAY_POLICY_V1
    is BadgeCommand.SetNetworkMode -> BadgeCapability.NETWORK_MODE
    is BadgeCommand.NavigateDisplay -> BadgeCapability.DISPLAY_NAV
    BadgeCommand.Reboot -> BadgeCapability.REBOOT
    BadgeCommand.EnterBootloader -> BadgeCapability.BOOTLOADER
}

fun BadgeCommand.payloadSizeOrNull(): Int? = when (this) {
    is BadgeCommand.NavigateDisplay -> when (action) {
        BadgeDisplayAction.NEXT, BadgeDisplayAction.BACK -> 37
        BadgeDisplayAction.DETAIL -> 39
    }
    else -> null
}

interface BadgeControlPort {
    val state: StateFlow<BadgeRepositoryState>
    fun start()
    fun stop()
    fun requestConnection()
    fun refreshStatus()
    suspend fun execute(command: BadgeCommand): BadgeCommandOutcome
}

data class BadgeHttpClients(val status: OkHttpClient, val command: OkHttpClient)

fun badgeHttpClients(base: OkHttpClient): BadgeHttpClients = BadgeHttpClients(
    status = base.newBuilder()
        .readTimeout(1_200, TimeUnit.MILLISECONDS)
        .callTimeout(1_500, TimeUnit.MILLISECONDS)
        .retryOnConnectionFailure(false)
        .build(),
    command = base.newBuilder()
        .readTimeout(6_000, TimeUnit.MILLISECONDS)
        .callTimeout(6_000, TimeUnit.MILLISECONDS)
        .retryOnConnectionFailure(false)
        .build(),
)

fun enforceAckDeadline(
    outcome: BadgeCommandOutcome,
    elapsedMs: Long,
): BadgeCommandOutcome = if (elapsedMs <= 5_000L) outcome else BadgeCommandOutcome.TimedOut
```

- [ ] **Step 6: Make repository verification depend on parsed status, not connection open**

USB interface claim and BLE service discovery publish `TRANSPORT_OPEN`. Only a successfully parsed status stores `lastValidStatusAtElapsedMs`, version, and phase `LIVE`. Store the status receive time once in the repository; Privacy mappers consume that timestamp and never replace it with the current mapping time. On disconnect retain diagnostics only until the 60-second expiry, then clear Current feed/config state.

```kotlin
interface MonotonicClock {
    fun nowElapsedMs(): Long
    fun ticks(periodMs: Long = 1_000): Flow<Long>
}

@Singleton
class AndroidMonotonicClock @Inject constructor() : MonotonicClock {
    override fun nowElapsedMs(): Long = SystemClock.elapsedRealtime()
    override fun ticks(periodMs: Long): Flow<Long> = flow {
        while (currentCoroutineContext().isActive) {
            emit(nowElapsedMs())
            delay(periodMs)
        }
    }
}
```

Define the qualifier and Hilt providers exactly so every app-scoped collector shares one supervised scope, clock, control port, and certification:

```kotlin
@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class ApplicationScope

@Module
@InstallIn(SingletonComponent::class)
object ApplicationCoroutineModule {
    @Provides
    @Singleton
    @ApplicationScope
    fun provideApplicationScope(): CoroutineScope =
        CoroutineScope(SupervisorJob() + Dispatchers.Default)
}

@Module
@InstallIn(SingletonComponent::class)
abstract class BadgeModule {
    @Binds
    @Singleton
    abstract fun bindBadgeControlPort(repository: BadgeUsbRepository): BadgeControlPort

    @Binds
    @Singleton
    abstract fun bindMonotonicClock(clock: AndroidMonotonicClock): MonotonicClock

    companion object {
        @Provides
        @Singleton
        fun provideBadgeReleaseCertification(): BadgeReleaseCertification =
            CheckedInBadgeReleaseCertification
    }
}
```

Combine the raw transport evidence with `clock.ticks()` and derive `LIVE/STALE/EXPIRED` on every tick even when the transport emits nothing. `aged()` may advance only a previously `LIVE`/`STALE` status; it must never turn `DISCONNECTED`, `PERMISSION_NEEDED`, `CONNECTING`, `TRANSPORT_OPEN`, or `ERROR` back into `LIVE`. On disconnect/error, preserve that transport phase while independently using `lastValidStatusAtElapsedMs` to clear retained `controlStatus` at the 60-second expiry.

For a debug response containing `status_age_s`, capture Android receipt time once and compute `debugPhysicalStatusAtElapsedMs = receiptElapsedMs - statusAgeMs`; set `lastValidStatusAtElapsedMs` to the earlier of the Android receipt-derived status timestamp and that physical timestamp. This makes the tick-derived `LIVE` phase prove physical freshness without asking `badgeCapability` to compare a timestamp without a clock. Never carry both an age and timestamp that can diverge.

```kotlin
@ApplicationScope private val applicationScope: CoroutineScope,

private val freshnessJob = applicationScope.launch {
    clock.ticks().collect { nowElapsedMs ->
        _state.update { current ->
            val aged = current.connection.aged(nowElapsedMs)
            val expired = current.connection.transport?.let { transport ->
                current.connection.lastValidStatusAtElapsedMs?.let { receivedAt ->
                    badgeFreshness(transport, receivedAt, nowElapsedMs) ==
                        BadgeConnectionPhase.EXPIRED
                }
            } ?: false
            current.copy(
                connection = aged,
                controlStatus = current.controlStatus.takeUnless { expired },
            )
        }
    }
}
```

`FakeMonotonicClock` backs `ticks()` with a `MutableStateFlow`; `advanceBy` changes that flow immediately. `AndroidMonotonicClock` emits immediately and then every second. This makes the no-new-transport-emission regression deterministic.

Implement `onMtuChanged` and retain negotiated MTU. The exact compact payload byte lengths are 37 (`next`), 39 (`detail`), and 37 (`back`), so required ATT MTUs are 40, 42, and 40.

- [ ] **Step 7: Serialize mutations and parse real acknowledgement evidence**

Guard `execute` with one `Mutex`. USB installs one pending pair of `(BadgeCommand, CompletableDeferred)` before writing. Ordinary controls complete only from `FOF_CTL_OK`/`FOF_CTL_ERROR`; `Reboot` completes only from `FOF_REBOOT:OK`; `EnterBootloader` completes only from `FOF_BOOTLOADER:OK`. A mismatched recovery line is retained as diagnostic input and cannot complete the pending or next command. HTTP uses the separate six-second command client, parses both status code and JSON body, and applies `enforceAckDeadline` using monotonic send/receive times; BLE waits for `onCharacteristicWrite` and may return only `Accepted` until readback proves more. Wrap USB/BLE acknowledgement waits in `withTimeoutOrNull(5_000)` and return `TimedOut`; do not retry. The short 1.2-second status-poll client must never be used for a command.

```kotlin
override suspend fun execute(command: BadgeCommand): BadgeCommandOutcome = commandMutex.withLock {
    when (command) {
        is BadgeCommand.ApplyTheme -> BadgeTheme.validate(command.theme).getOrElse {
            return BadgeCommandOutcome.Failed("Theme draft is invalid")
        }
        is BadgeCommand.ApplyPolicy -> BadgeDisplayPolicy.validate(command.policy).getOrElse {
            return BadgeCommandOutcome.Failed("Display policy draft is invalid")
        }
        else -> Unit
    }
    val required = command.requiredCapability()
    val support = badgeCapability(state.value.connection, required, command.payloadSizeOrNull())
    if (support != BadgeCapabilitySupport.SUPPORTED) {
        return BadgeCommandOutcome.Unsupported("${required.name.lowercase()} is unavailable on this connection")
    }
    when (state.value.connection.transport) {
        BadgeTransport.USB_SERIAL -> executeUsbOnce(command)
        BadgeTransport.LOCAL_AP_HTTP -> executeHttpOnce(BADGE_AP_CONTROL_URL, command)
        BadgeTransport.BLE_GATT -> executeBleOnce(command)
        BadgeTransport.DEBUG_BRIDGE -> executeHttpOnce(DEBUG_BRIDGE_CONTROL_URL, command)
        null -> BadgeCommandOutcome.Unsupported("No badge transport is active")
    }
}
```

For USB, put the pending deferred in an `AtomicReference`, await it once, and clear that exact deferred in `finally`; disconnect completes and clears it with failure. A late line after timeout is logged as unmatched status and cannot satisfy a later command. HTTP treats non-2xx, malformed JSON, absent Boolean `ok`, `ok:false`, and a present `applied:false` as failure. USB likewise rejects `FOF_CTL_OK` with `applied:false`. Parse network acknowledgements into the raw runtime enum above; persisted `usb_only` legitimately acknowledges runtime `off`.

The existing debug bridge returns physical metadata only from `GET /api/badge/status`; `/api/badge/control` returns the badge's raw `FOF_CTL_OK`/`FOF_CTL_ERROR` body. Therefore a debug configuration/navigation command requires fresh physical pre-command status, a present blank `debug_bridge.last_error`, the real control acknowledgement, and a fresh post-command status/readback. The bridge script does not recognize firmware's `FOF_REBOOT:OK`/`FOF_BOOTLOADER:OK`, so Android must classify debug-bridge recovery as `UNSUPPORTED` without sending. Direct USB recovery stops at `ACKNOWLEDGED`, never `Applied`/`Verified`, because a deliberate disconnect may follow, and shows reconnect guidance. Do not require nonexistent `debug_bridge` fields in the control response and do not change the bridge script.

- [ ] **Step 8: Remove all Android firmware upload/relay behavior**

Delete `BadgeFirmwareProgress`, `relayScannerFirmware`, `flashScannerFirmware`, `writeBytes`, `uploadScannerFirmwareOverHttp`, `parseFirmwareProgress`, CRC/media-type/URL-encoding imports, `/api/fw` requests, `fw_upload`/`fw_relay` control creation, and firmware-progress line handling. In the same step remove the firmware file pickers/callers and screen-owned badge `start/stop` methods/effects from List and Privacy so the commit compiles. Keep scanner `fw_state`, `target_ver`, `ota_state`, and `last_fw_error` as read-only diagnostic fields. List/Privacy configuration controls remain temporarily but observe process-scoped state until Task 10 removes them.

- [ ] **Step 9: Move badge start/stop to process lifecycle**

```kotlin
@Inject lateinit var badgeControlPort: BadgeControlPort

override fun onStart(owner: LifecycleOwner) {
    badgeControlPort.start()
}

override fun onStop(owner: LifecycleOwner) {
    badgeControlPort.stop()
}
```

Register the application as a `DefaultLifecycleObserver` with `ProcessLifecycleOwner.get().lifecycle.addObserver(this)` in `onCreate`. Bind `BadgeUsbRepository` to `BadgeControlPort` in `BadgeModule` with a singleton Hilt binding. Location-dependent sky collection remains owned by its contextual feature flow and is never started with a fabricated coordinate.

- [ ] **Step 10: Run Badge transport tests and static firmware-removal scan**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeConnectionCapabilityTest' --tests '*BadgeControlAcknowledgementTest' --tests '*BadgeUsbLineParserTest'`

Expected: PASS.

Run: `rg -n 'OpenDocument|fw_upload|/api/fw|flashScannerFirmware|relayScannerFirmware' android/app/src/main android/app/src/test android/app/src/androidTest`

Expected: no matches. The List/Privacy pickers and callers are removed in Step 8 of this task together with the repository APIs, so this commit is independently compilable.

- [ ] **Step 11: Commit safe transport execution**

```bash
git add android/app/build.gradle.kts android/app/src/main/java/com/friendorfoe/data/badge android/app/src/main/java/com/friendorfoe/data/time/MonotonicClock.kt android/app/src/main/java/com/friendorfoe/di/BadgeModule.kt android/app/src/main/java/com/friendorfoe/di/ApplicationCoroutineModule.kt android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/test/java/com/friendorfoe/data/badge
git commit -m "android: enforce badge transport evidence"
```

### Task 9: Centralize one Badge draft/apply reducer

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeUiState.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeViewModel.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeViewModelTest.kt`

**Interfaces:**
- Consumes: `BadgeControlPort` and strict readbacks from Tasks 7–8.
- Produces: a single route-scoped `BadgeUiState`, draft-edit actions, local defaults/revert, one-shot apply, and guarded recovery confirmation state.

- [ ] **Step 1: Write failing draft-retention and unknown-readback tests**

```kotlin
@Test fun fallbackObjectsNeverEnableApply() = runTest {
    val port = FakeBadgeControlPort(statusWithUnknownConfig())
    val viewModel = BadgeViewModel(port, testClock)
    advanceUntilIdle()
    assertNull(viewModel.uiState.value.draftTheme)
    assertFalse(viewModel.uiState.value.canApply)
}

@Test fun freshReadbackDoesNotOverwriteDirtyDraft() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig())
    val viewModel = BadgeViewModel(port, testClock)
    advanceUntilIdle()
    viewModel.updateTheme { it.copy(intensity = 70) }
    port.emit(statusWithDefaultConfig(receivedAt = 2_000))
    advanceUntilIdle()
    assertEquals(70, viewModel.uiState.value.draftTheme!!.intensity)
}

@Test fun revertAndDefaultsAreLocalUntilApply() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig())
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.useFirmwareDefaultsInDraft()
    viewModel.revertDraft()
    assertTrue(port.commands.isEmpty())
}
```

- [ ] **Step 2: Write failing evidence/timeout/scanner tests**

```kotlin
@Test fun hashMismatchCannotBecomeVerified() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig())
    port.nextOutcome = acknowledged(themeHash = 123)
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updateTheme { it.copy(intensity = 70) }
    viewModel.applyChanges()
    advanceUntilIdle()
    assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.theme.phase)
}

@Test fun zeroConnectedScannersStopsAtAppliedOnBadge() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig(scanners = emptyList()))
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updatePolicy { it.withEnabled("beacon", false) }
    viewModel.applyChanges()
    port.completeWithMatchingPolicyReadback(scanners = emptyList())
    advanceUntilIdle()
    assertEquals(BadgeApplyPhase.APPLIED_ON_BADGE, viewModel.uiState.value.applyState.policy.phase)
}

@Test fun defaultsPreserveEveryAppliedPriorityAndPalette() = runTest {
    val applied = statusWithDefaultConfig(
        palette = "night",
        priorities = BadgeDisplayPolicy.classOrder.withIndex()
            .associate { (index, key) -> key to (index + 3) },
    )
    val viewModel = BadgeViewModel(FakeBadgeControlPort(applied), testClock)
    advanceUntilIdle()

    viewModel.useFirmwareDefaultsInDraft()

    assertEquals("night", viewModel.uiState.value.draftTheme!!.palette)
    BadgeDisplayPolicy.classOrder.forEachIndexed { index, key ->
        assertEquals(index + 3, viewModel.uiState.value.draftPolicy!!.classes.getValue(key).priority)
    }
}

@Test fun themeSuccessCannotHidePolicyFailure() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig())
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updateTheme { it.copy(intensity = 70) }
    viewModel.updatePolicy { it.withEnabled("beacon", false) }
    port.enqueue(acknowledged(themeHash = expectedThemeHash(intensity = 70)))
    port.enqueue(BadgeCommandOutcome.Failed("policy rejected"))

    viewModel.applyChanges()
    port.completeWithMatchingThemeReadback(intensity = 70)
    advanceUntilIdle()

    assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.theme.phase)
    assertEquals(BadgeApplyPhase.FAILED, viewModel.uiState.value.applyState.policy.phase)
}

@Test fun networkModeNeedsAcknowledgementAndMatchingReadback() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig(networkMode = BadgeNetworkMode.USB_ONLY))
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updateNetworkMode(BadgeNetworkMode.BACKEND)
    port.enqueue(acknowledged(
        networkApplied = true,
        runtimeNetworkMode = BadgeRuntimeNetworkMode.BACKEND,
    ))

    viewModel.applyChanges()
    port.completeWithNetworkMode(BadgeNetworkMode.BACKEND)
    advanceUntilIdle()

    assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.network.phase)
    assertEquals(listOf(BadgeCommand.SetNetworkMode(BadgeNetworkMode.BACKEND)), port.commands)
}

@Test fun persistedUsbOnlyVerifiesAgainstRuntimeOffAndPersistedModeReadback() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig(networkMode = BadgeNetworkMode.BACKEND))
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updateNetworkMode(BadgeNetworkMode.USB_ONLY)
    port.enqueue(acknowledged(
        networkApplied = true,
        runtimeNetworkMode = BadgeRuntimeNetworkMode.OFF,
    ))

    viewModel.applyChanges()
    port.completeWithNetworkStatus(
        persistedMode = BadgeNetworkMode.USB_ONLY,
        runtimeMode = BadgeRuntimeNetworkMode.OFF,
    )
    advanceUntilIdle()

    assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.network.phase)
}

@Test fun displayNavigationSupportIsDerivedPerSerializedAction() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig(
        connection = certifiedBleConnection(mtu = 41),
    ))
    val viewModel = BadgeViewModel(port, testClock)
    advanceUntilIdle()
    assertEquals(BadgeCapabilitySupport.SUPPORTED,
        viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.NEXT))
    assertEquals(BadgeCapabilitySupport.UNSUPPORTED,
        viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.DETAIL))
    assertEquals(BadgeCapabilitySupport.SUPPORTED,
        viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.BACK))
}

@Test fun rapidApplyCallsSendOneCommandAndDisableApplyWhilePending() = runTest {
    val port = FakeBadgeControlPort(statusWithDefaultConfig())
    port.blockNextCommand()
    val viewModel = BadgeViewModel(port, testClock)
    viewModel.updateTheme { it.copy(intensity = 70) }
    viewModel.applyChanges()
    runCurrent()
    assertTrue(viewModel.uiState.value.applyInFlight)
    assertFalse(viewModel.uiState.value.canApply)
    viewModel.applyChanges()
    assertEquals(1, port.commands.size)
    port.releaseBlockedCommand(acknowledged(themeHash = expectedThemeHash(intensity = 70)))
}
```

- [ ] **Step 3: Run ViewModel tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeViewModelTest'`

Expected: compilation fails because the centralized reducer is absent.

- [ ] **Step 4: Define the immutable UI/apply state**

```kotlin
enum class BadgeApplyPhase {
    CLEAN, DIRTY, PENDING, ACCEPTED, ACKNOWLEDGED, APPLIED_ON_BADGE,
    VERIFIED, VERIFIED_ON_SCANNERS, NOT_VERIFIED, FAILED, UNSUPPORTED,
}

enum class BadgeConfigSection { THEME, DISPLAY_POLICY, NETWORK_MODE }

data class BadgeSectionApplyResult(
    val section: BadgeConfigSection,
    val phase: BadgeApplyPhase = BadgeApplyPhase.CLEAN,
    val message: String? = null,
    val expectedHash: Long? = null,
    val acknowledgementHash: Long? = null,
    val readbackHash: Long? = null,
)

data class BadgeApplyState(
    val theme: BadgeSectionApplyResult = BadgeSectionApplyResult(BadgeConfigSection.THEME),
    val policy: BadgeSectionApplyResult = BadgeSectionApplyResult(BadgeConfigSection.DISPLAY_POLICY),
    val network: BadgeSectionApplyResult = BadgeSectionApplyResult(BadgeConfigSection.NETWORK_MODE),
) {
    val activeResults get() = listOf(theme, policy, network).filter {
        it.phase != BadgeApplyPhase.CLEAN
    }
}

enum class BadgeRecoveryAction(val capability: BadgeCapability, val command: BadgeCommand) {
    REBOOT(BadgeCapability.REBOOT, BadgeCommand.Reboot),
    ENTER_BOOTLOADER(BadgeCapability.BOOTLOADER, BadgeCommand.EnterBootloader),
}

enum class BadgeRecoveryPhase {
    IDLE, CONFIRMING, PENDING, ACKNOWLEDGED, NOT_VERIFIED, FAILED,
}

data class BadgeRecoveryAvailability(
    val enabled: Boolean,
    val reason: String,
)

data class BadgeRecoveryState(
    val action: BadgeRecoveryAction? = null,
    val targetId: String? = null,
    val phase: BadgeRecoveryPhase = BadgeRecoveryPhase.IDLE,
    val message: String? = null,
    val reconnectGuidance: String? = null,
)

data class BadgeUiState(
    val connection: BadgeConnectionEvidence,
    val capabilities: Map<BadgeCapability, BadgeCapabilitySupport>,
    val displayNavigationSupport: Map<BadgeDisplayAction, BadgeCapabilitySupport>,
    val appliedTheme: BadgeTheme?,
    val draftTheme: BadgeTheme?,
    val appliedPolicy: BadgeDisplayPolicy?,
    val draftPolicy: BadgeDisplayPolicy?,
    val appliedNetworkMode: BadgeNetworkMode?,
    val draftNetworkMode: BadgeNetworkMode?,
    val controlStatus: BadgeControlStatus?,
    val applyState: BadgeApplyState,
    val applyInFlight: Boolean = false,
    val recoveryAvailability: Map<BadgeRecoveryAction, BadgeRecoveryAvailability>,
    val displayNavigationResult: BadgeCommandOutcome? = null,
    val recovery: BadgeRecoveryState = BadgeRecoveryState(),
) {
    val themeDirty = draftTheme != appliedTheme
    val policyDirty = draftPolicy != appliedPolicy
    val networkDirty = draftNetworkMode != appliedNetworkMode
    val isDirty = themeDirty || policyDirty || networkDirty
    val canApply = isDirty && !applyInFlight &&
        (!themeDirty || (draftTheme != null &&
            capabilities[BadgeCapability.THEME_V1] == BadgeCapabilitySupport.SUPPORTED)) &&
        (!policyDirty || (draftPolicy != null &&
            capabilities[BadgeCapability.DISPLAY_POLICY_V1] == BadgeCapabilitySupport.SUPPORTED)) &&
        (!networkDirty || (draftNetworkMode != null &&
            capabilities[BadgeCapability.NETWORK_MODE] == BadgeCapabilitySupport.SUPPORTED))
}
```

- [ ] **Step 5: Implement draft initialization and local actions**

A valid fresh readback initializes each applied/draft section only while that section is clean. `updateTheme`, `updatePolicy`, `updateNetworkMode`, `useFirmwareDefaultsInDraft`, and `revertDraft` change local state only. Section expansion is presentation state and never owns a draft. An unknown Theme, Policy, or network readback leaves that section null and disabled; another valid section remains independently editable.

On every connection update, derive `displayNavigationSupport = BadgeDisplayAction.entries.associateWith { action -> badgeCapability(connection, DISPLAY_NAV, BadgeCommand.NavigateDisplay(action).payloadSizeOrNull()) }`. `navigateDisplay` returns without calling the port unless that exact action is `SUPPORTED`; one aggregate `DISPLAY_NAV` entry never enables all three buttons.

```kotlin
fun revertDraft() = _uiState.update { state ->
    state.copy(
        draftTheme = state.appliedTheme,
        draftPolicy = state.appliedPolicy,
        draftNetworkMode = state.appliedNetworkMode,
        applyState = BadgeApplyState(),
    )
}

fun useFirmwareDefaultsInDraft() = _uiState.update { state ->
    val defaults = BadgeDisplayPolicy.firmwareDefaults()
    val nextPolicy = state.appliedPolicy?.let { applied ->
        defaults.copy(classes = BadgeDisplayPolicy.classOrder.associateWithTo(linkedMapOf()) { key ->
            defaults.classes.getValue(key).copy(
                priority = applied.classes.getValue(key).priority,
            )
        })
    }
    val nextTheme = state.appliedTheme?.let { applied ->
        BadgeTheme.firmwareDefaults().copy(palette = applied.palette)
    }
    state.copy(
        draftTheme = nextTheme,
        draftPolicy = nextPolicy,
        applyState = state.applyState.copy(
            theme = state.applyState.theme.copy(
                phase = if (nextTheme != null && nextTheme != state.appliedTheme)
                    BadgeApplyPhase.DIRTY else BadgeApplyPhase.CLEAN,
            ),
            policy = state.applyState.policy.copy(
                phase = if (nextPolicy != null && nextPolicy != state.appliedPolicy)
                    BadgeApplyPhase.DIRTY else BadgeApplyPhase.CLEAN,
            ),
        ),
    )
}
```

Firmware defaults do not define a user-safe network-mode choice, so this action never changes `draftNetworkMode`.

- [ ] **Step 6: Implement one-shot apply proof**

For each dirty configuration, snapshot the draft and call `port.execute` exactly once, in Theme, Policy, then network-mode order. A Theme-only edit does not require a Policy or network capability/readback, and each other section is equally independent. Record each section in its own `BadgeSectionApplyResult`; do not compute a single overall phase. The UI may summarize, but it must render all active typed results.

`Accepted` is transport completion only. `Acknowledged` requires an explicit result/hash or a subsequent matching result inside five seconds. Theme reaches `VERIFIED` only if acknowledgement hash, full canonical readback, and readback hash match its draft. Policy reaches `APPLIED_ON_BADGE` after top-level full-field/hash match and `VERIFIED_ON_SCANNERS` only when at least one scanner is connected and every connected scanner has `displayPolicyAckHash == policyHash`. Network mode reaches `VERIFIED` only when the response has `applied:true`, its runtime mode equals `submitted.expectedRuntimeMode()`, and a fresh status has persisted top-level `networkModeReadback.value == submitted`; for `usb_only`, runtime `off` is correct. `bleSent` and `wifiSent` are not proof. Timeout, partial mismatch, missing readback, `applied:false`, or a rejected body produces that section's explicit non-success phase without retry.

```kotlin
private fun scannersVerified(status: BadgeControlStatus, expectedHash: Long): Boolean {
    val connected = status.scanners.filter { it.connected }
    return connected.isNotEmpty() && connected.all { it.displayPolicyAckHash == expectedHash }
}
```

Expose one explicit contract and implement it on `BadgeViewModel` in this task so Task 10 is wiring-only:

```kotlin
interface BadgeViewModelContract {
    fun refresh()
    fun reconnect()
    fun updateTheme(transform: (BadgeTheme) -> BadgeTheme)
    fun updatePolicy(transform: (BadgeDisplayPolicy) -> BadgeDisplayPolicy)
    fun updateNetworkMode(mode: BadgeNetworkMode)
    fun useFirmwareDefaultsInDraft()
    fun revertDraft()
    fun applyChanges()
    fun navigateDisplay(action: BadgeDisplayAction)
    fun requestRecovery(action: BadgeRecoveryAction)
    fun cancelRecovery()
    fun confirmRecovery()
}

override fun refresh() { port.refreshStatus() }
override fun reconnect() { port.requestConnection() }

override fun updateTheme(transform: (BadgeTheme) -> BadgeTheme) = _uiState.update { state ->
    val next = state.draftTheme?.let(transform) ?: return@update state
    state.copy(
        draftTheme = next,
        applyState = state.applyState.copy(theme = state.applyState.theme.copy(
            phase = if (next == state.appliedTheme) BadgeApplyPhase.CLEAN else BadgeApplyPhase.DIRTY,
        )),
    )
}

override fun updatePolicy(transform: (BadgeDisplayPolicy) -> BadgeDisplayPolicy) =
    _uiState.update { state ->
        val next = state.draftPolicy?.let(transform) ?: return@update state
        state.copy(
            draftPolicy = next,
            applyState = state.applyState.copy(policy = state.applyState.policy.copy(
                phase = if (next == state.appliedPolicy) BadgeApplyPhase.CLEAN
                    else BadgeApplyPhase.DIRTY,
            )),
        )
    }

override fun updateNetworkMode(mode: BadgeNetworkMode) = _uiState.update { state ->
    if (state.appliedNetworkMode == null) state else state.copy(
        draftNetworkMode = mode,
        applyState = state.applyState.copy(network = state.applyState.network.copy(
            phase = if (mode == state.appliedNetworkMode) BadgeApplyPhase.CLEAN
                else BadgeApplyPhase.DIRTY,
        )),
    )
}

override fun navigateDisplay(action: BadgeDisplayAction) {
    if (_uiState.value.displayNavigationSupport[action] != BadgeCapabilitySupport.SUPPORTED) return
    viewModelScope.launch {
        val outcome = port.execute(BadgeCommand.NavigateDisplay(action))
        _uiState.update { it.copy(displayNavigationResult = outcome) }
    }
}
```

`applyChanges`, defaults, revert, and recovery implement the state transitions defined in Steps 5–7; none is left as a bodyless class function. `applyChanges` owns one `Job`, returns immediately if that job is active or `canApply` is false, sets `applyInFlight = true` before launching, snapshots the three drafts once, and executes the exact per-section proof in Step 6 without rereading a changing draft. Clear `applyInFlight` in `finally`, including cancellation/failure. `navigateDisplay` is a transient command with its own inline result and never dirties the configuration draft.

- [ ] **Step 7: Implement guarded recovery state**

Derive `recoveryAvailability` for both actions on every connection update. Unsupported/unverified actions use the visible reason `Verified direct USB is required`; an ambiguous target uses `Connect exactly one badge over USB before recovery`. `requestRecovery` enters `CONFIRMING` only for an enabled action and stores the exact direct-USB `targetId`. `confirmRecovery` rechecks that the current target and capability still match, executes exactly once, disables both recovery controls while `PENDING`, and maps the matching dedicated USB recovery line to `ACKNOWLEDGED`; timeout/mismatch/failure maps to `NOT_VERIFIED` or `FAILED`, all with `Reconnect and refresh badge status` guidance. `cancelRecovery` returns to `IDLE` and sends nothing. Debug bridge, AP, and BLE never call a recovery command.

`BadgeViewModelTest.kt` includes a complete `FakeBadgeControlPort` implementing every `BadgeControlPort` method, a queue/blocking deferred for command outcomes, `FakeMonotonicClock`, full valid/unknown status builders, certified USB/BLE evidence builders, matching Theme/Policy/network readback emitters, and acknowledgement helpers. Builders must populate every `BadgeConnectionEvidence` and `BadgeControlStatus` field introduced in Tasks 7–8. These are concrete test helpers in the file (or one shared `BadgeFixtures.kt`), not prose placeholders.

- [ ] **Step 8: Run the reducer suite**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeViewModelTest'`

Expected: PASS for unknown state, per-section dirty retention, priority/palette-preserving defaults, network-mode proof, one-send timeout, acknowledged/readback mismatches, typed partial success, scanner verification, and guarded recovery.

- [ ] **Step 9: Commit the Badge reducer**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeUiState.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeViewModel.kt android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeViewModelTest.kt
git commit -m "android: centralize badge draft and apply state"
```

### Task 10: Build the dedicated firmware-exact Badge route and remove duplicates

**Files:**
- Delete: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeConnectionLoadingScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeActions.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeThemeOptions.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeConnectionSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeAppearanceSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeDisplayFiltersSection.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeApplySection.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeDeviceStatusSection.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeDiagnosticsScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeRecoveryScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt:82-490`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt:42-174`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt:81-1027`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt:190-274`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/badge/BadgeScreenTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/badge/BadgeRecoveryScreenTest.kt`

**Interfaces:**
- Consumes: `BadgeUiState`/actions from Task 9 and Badge routes from Task 2.
- Produces: the only top-level badge configuration surface; List and Privacy retain zero badge configuration/lifecycle/recovery code.

- [ ] **Step 1: Write the Badge UI contract tests first**

```kotlin
@Test fun showsExactThemeCodesAndNoSimulationOrInactiveEditors() {
    compose.setContent { BadgeContent(state = editableBadgeState(), actions = fakeActions) }
    mapOf(
        "Drone" to ("drone" to "0xFEA0 · 65184"),
        "Meta" to ("meta" to "0xF833 · 63539"),
        "Tracker" to ("tracker" to "0xF81F · 63519"),
        "Flock" to ("flock" to "0xA81F · 43039"),
        "Wi-Fi Attack" to ("wifi_attack" to "0x07FF · 2047"),
        "Clear" to ("clear" to "0x2F65 · 12133"),
    ).forEach { (label, keyAndCode) ->
        compose.onNodeWithText(label).assertIsDisplayed()
        compose.onNodeWithText(keyAndCode.first).assertIsDisplayed()
        compose.onNodeWithText(keyAndCode.second).assertIsDisplayed()
    }
    listOf("Palette", "Brightness", "Priority", "LCD preview", "Badge simulation")
        .forEach { compose.onNodeWithText(it, substring = true, ignoreCase = true).assertDoesNotExist() }
}

@Test fun lcdButtonsSerializeAllThreeExactLowercaseActions() {
    val actions = RecordingBadgeActions()
    compose.setContent { BadgeContent(editableBadgeState(), actions.asActions()) }
    compose.onNodeWithText("Next").performClick()
    compose.onNodeWithText("Detail").performClick()
    compose.onNodeWithText("Back").performClick()
    assertEquals(
        listOf("next", "detail", "back"),
        actions.displayActions.map(BadgeDisplayAction::wireValue),
    )
}

@Test fun staleUnknownAndUncertifiedConnectionsExplainDisabledControls() {
    compose.setContent { BadgeContent(staleUncertifiedBadgeState(), noOpBadgeActions()) }
    compose.onNodeWithText("Status is stale").assertIsDisplayed()
    compose.onNodeWithText("Theme readback is unavailable").assertIsDisplayed()
    compose.onNodeWithText("Verified direct USB is required")
        .assertIsDisplayed()
    compose.onNodeWithTag("badge_apply").assertIsNotEnabled()
}
```

Use one `setContent` per instrumentation test. Additional tests in this class cover all 13 rule keys, background tags/seeds, intensity bounds, network-mode choices/results, diagnostics fields, partial per-section results, and absence of palette, editable priority, firmware upload, hardware art, and LCD simulation.

- [ ] **Step 2: Run the Badge instrumentation class and confirm red**

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.badge.BadgeScreenTest`

Expected: the missing route/content and duplicated controls fail.

- [ ] **Step 3: Implement the top-level route hierarchy**

```kotlin
@Composable
fun BadgeRoute(
    onDiagnostics: () -> Unit,
    onRecovery: () -> Unit,
    viewModel: BadgeViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    BadgeContent(
        state = state,
        actions = BadgeActions(
            refresh = viewModel::refresh,
            reconnect = viewModel::reconnect,
            updateTheme = viewModel::updateTheme,
            updatePolicy = viewModel::updatePolicy,
            updateNetworkMode = viewModel::updateNetworkMode,
            useDefaults = viewModel::useFirmwareDefaultsInDraft,
            revert = viewModel::revertDraft,
            apply = viewModel::applyChanges,
            navigateDisplay = viewModel::navigateDisplay,
            requestRecovery = viewModel::requestRecovery,
            openDiagnostics = onDiagnostics,
            openRecovery = onRecovery,
        ),
    )
}
```

Define the complete callback contract once:

```kotlin
data class BadgeActions(
    val refresh: () -> Unit,
    val reconnect: () -> Unit,
    val updateTheme: ((BadgeTheme) -> BadgeTheme) -> Unit,
    val updatePolicy: ((BadgeDisplayPolicy) -> BadgeDisplayPolicy) -> Unit,
    val updateNetworkMode: (BadgeNetworkMode) -> Unit,
    val useDefaults: () -> Unit,
    val revert: () -> Unit,
    val apply: () -> Unit,
    val navigateDisplay: (BadgeDisplayAction) -> Unit,
    val requestRecovery: (BadgeRecoveryAction) -> Unit,
    val openDiagnostics: () -> Unit,
    val openRecovery: () -> Unit,
)

data class BadgeThemeAccentInfo(val firmwareKey: String, val label: String)
data class BadgeThemeSwatch(val label: String, val rgb565: Int)

val SafeThemeSwatches = listOf(
    BadgeThemeSwatch("Ice", 0x07FF),
    BadgeThemeSwatch("Gold", 0xFEA0),
    BadgeThemeSwatch("Fire", 0xF800),
    BadgeThemeSwatch("Rose", 0xF833),
    BadgeThemeSwatch("Violet", 0xA81F),
    BadgeThemeSwatch("Green", 0x2F65),
)
```

Render in this order: screen title; connection summary; LCD accent colors; background and Color intensity; Display rules; shared apply area; device-reported textual status; Advanced diagnostics/recovery. No hardware artwork or screen facsimile is created.

- [ ] **Step 4: Replace Theme controls with exact code rows**

```kotlin
@Composable
private fun AccentRow(
    info: BadgeThemeAccentInfo,
    selectedRgb565: Int,
    onSelect: (Int) -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(20.dp).background(rgb565Color(selectedRgb565), RoundedCornerShape(4.dp)))
            Text(info.label, Modifier.padding(start = 12.dp).weight(1f))
            Text(info.firmwareKey, style = MaterialTheme.typography.labelSmall)
            Text(
                "0x%04X · %d".format(selectedRgb565, selectedRgb565),
                fontFamily = FontFamily.Monospace,
            )
        }
        LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            items(SafeThemeSwatches, key = { it.rgb565 }) { swatch ->
                FilterChip(
                    selected = swatch.rgb565 == selectedRgb565,
                    onClick = { onSelect(swatch.rgb565) },
                    label = { Text(swatch.label) },
                    modifier = Modifier.heightIn(min = 48.dp),
                )
            }
        }
    }
}
```

Show backgrounds as `Black — dark — 0x0000`, `Dim — dim — 0x1082`, `Blue-black — scanline — 0x0108`. Show `Color intensity NN%`, the fixed-backlight limitation, and `Theme hash 0xHHHHHHHH` from readback. Keep the palette compatibility value only in the draft model.

Render a separate `BadgeNetworkModeSection` with the exact persisted choices `USB only — usb_only`, `Local AP — local_ap`, and `Backend — backend`. It edits only `draftNetworkMode`; it never aliases runtime `off`. Show the applied mode, draft mode, capability reason, and the network section's typed apply result adjacent to the choices.

Render visible `Next`, `Detail`, and `Back` buttons in the textual Device-reported status section. Each button reads its own `state.displayNavigationSupport[action]`; for example, BLE MTU 41 enables Next/Back while disabling Detail with the MTU reason. The buttons do not draw an LCD; they only send `BadgeDisplayAction` and show Accepted/Acknowledged/Not verified/Failed text.

- [ ] **Step 5: Replace Policy controls and preserve priority invisibly**

Remove the priority label and slider. Render all 13 firmware classes in `BadgeDisplayPolicy.classOrder`. Presets may change only `enabled`, lane, and proximity; construct each next row with `priority = current.priority`. Disable/hide lane and proximity when a class is off. Show `Policy hash 0xHHHHHHHH`, `Close ≥ -60 dBm · Near ≥ -76 dBm · Present < -76 dBm`, and: `Off is not an absolute suppression guarantee; firmware safety rules may still show high-confidence evidence.`

- [ ] **Step 6: Use one shared apply area**

Show dirty fields, `Use firmware defaults`, `Revert draft`, and `Apply changes`. Defaults and Revert are local. Adjacent result rows distinguish `Accepted`, `Acknowledged`, `Applied on badge`, `Verified`, `Verified on scanners`, `Not verified`, and `Failed` independently for Theme, Display policy, and Network mode; no snackbar or aggregate result alone carries the outcome.

- [ ] **Step 7: Add read-only diagnostics and guarded recovery routes**

Diagnostics display version, transport, scanner health, hashes, stack/heap, firmware target/OTA/error fields as text. Recovery always lists Reboot and Bootloader so support is understandable: unsupported controls are disabled beside `Verified direct USB is required`; ambiguous targets show their separate reason. Enabled actions require a modal confirmation naming the physical USB target, disable both controls while pending, and show acknowledged/timeout/failure plus reconnect guidance. It exposes no file selection or firmware upload.

Define the recovery callback surface in `BadgeRecoveryScreen.kt`; test recorders and no-op fixtures implement this exact contract:

```kotlin
data class BadgeRecoveryActions(
    val request: (BadgeRecoveryAction) -> Unit,
    val confirm: () -> Unit,
    val cancel: () -> Unit,
    val refresh: () -> Unit,
)
```

`BadgeRecoveryScreenTest` contains concrete separate tests:

```kotlin
@Test fun unsupportedRecoveryIsExplainedAndCannotSend() {
    val actions = RecordingRecoveryActions()
    compose.setContent {
        BadgeRecoveryContent(
            state = recoveryState(rebootEnabled = false, bootloaderEnabled = false),
            actions = actions.asActions(),
        )
    }
    compose.onNodeWithText("Verified direct USB is required")
        .assertIsDisplayed()
    compose.onNodeWithTag("recovery_reboot").assertIsNotEnabled()
    compose.onNodeWithTag("recovery_bootloader").assertIsNotEnabled()
    assertTrue(actions.confirmed.isEmpty())
}

@Test fun confirmationNamesTargetAndCancelSendsNothing() {
    val actions = RecordingRecoveryActions()
    compose.setContent {
        BadgeRecoveryContent(
            recoveryState(targetId = "badge-7", rebootEnabled = true),
            actions.asActions(),
        )
    }
    compose.onNodeWithTag("recovery_reboot").performClick()
    compose.onNodeWithText("Reboot badge-7?").assertIsDisplayed()
    compose.onNodeWithText("Cancel").performClick()
    assertTrue(actions.confirmed.isEmpty())
}

@Test fun pendingDisablesBothRecoveryControls() {
    compose.setContent {
        BadgeRecoveryContent(recoveryPendingState(targetId = "badge-7"), noOpRecoveryActions())
    }
    compose.onNodeWithTag("recovery_reboot").assertIsNotEnabled()
    compose.onNodeWithTag("recovery_bootloader").assertIsNotEnabled()
}

@Test fun acknowledgedRecoveryShowsReconnectGuidance() {
    compose.setContent {
        BadgeRecoveryContent(recoveryAcknowledgedState(targetId = "badge-7"), noOpRecoveryActions())
    }
    compose.onNodeWithText("Reconnect and refresh badge status").assertIsDisplayed()
}
```

`BadgeScreenTest.kt`/`BadgeRecoveryScreenTest.kt` define complete `editableBadgeState`, stale/uncertified/action-specific navigation states, all 13 policy rows, `RecordingBadgeActions.asActions()`, `RecordingRecoveryActions.asActions()`, and no-op callback fixtures. Every factory supplies all required `BadgeUiState` fields. No recorder is passed where the final callback data class is required, and pending versus acknowledged fixtures are distinct.

- [ ] **Step 8: Remove Badge duplication and screen-owned lifecycle**

Delete `BadgeUsbPanel` and panel-only helpers from List; remove `BadgeUsbRepository` and all badge methods from `ListViewModel`. Delete `BadgeUsbStatusRow`, `BadgeDetailPanel`, `BadgeOperationsSection`, firmware picker, Theme/Policy controls, focused-display/config dialogs, and config/lifecycle/recovery methods from Privacy. Privacy temporarily retains only its direct read-only mapping of badge status to findings/source health; Task 11 migrates that mapping into `PrivacyFindingRepository` after this task compiles.

- [ ] **Step 9: Run Badge/ownership tests and static scans**

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.package=com.friendorfoe.presentation.badge`

Expected: PASS.

Run: `rg -n 'OpenDocument|fw_upload|/api/fw|flashScannerFirmware|relayScannerFirmware' android/app/src/main android/app/src/test android/app/src/androidTest`

Expected: no matches.

Run: `rg -n 'BadgeAppearanceSection|BadgeDisplayFiltersSection|rebootBadge|enterBootloader' android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy`

Expected: no matches.

- [ ] **Step 10: Commit the dedicated Badge experience**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/badge android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt android/app/src/androidTest/java/com/friendorfoe/presentation/badge
git commit -m "android: add firmware-exact badge screen"
```

### Task 11: Build the source-aware Privacy current-state reducer

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyModels.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyFreshness.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacySourceAdapter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PhonePrivacySourceAdapter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/BackendPrivacySourceAdapter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/BadgePrivacySourceAdapter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/WifiPrivacySourceAdapter.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyFindingRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/PrivacyModule.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyFindingNormalizer.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyFreshnessTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyCurrentReducerTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyCapabilityTest.kt`

**Interfaces:**
- Consumes: source-mapped findings from phone BLE, configured backend, badge USB/AP/BLE/debug bridge, and Wi-Fi analysis; `ignoredFindingKeys` from Task 1; an injected monotonic clock.
- Produces: `PrivacyFindingRepository.currentState: StateFlow<PrivacyCurrentState>`, exact `PrivacySourceHealth` rows, source-isolated identity and ignore keys, freshness/removal decisions, action capabilities, deterministic grouping/sort order, and alert-eligible rows.

- [ ] **Step 1: Write failing freshness-boundary tests**

```kotlin
class PrivacyFreshnessTest {
    private val clock = FakeMonotonicClock(100_000L)

    @Test fun eachSourceUsesTheApprovedStaleAndRemovalWindow() {
        assertEquals(FindingFreshness.LIVE,
            freshnessFor(PrivacySourceKind.PHONE_BLE, seenAt = 70_001, now = clock.nowElapsedMs()))
        assertEquals(FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.PHONE_BLE, seenAt = 70_000, now = clock.nowElapsedMs()))
        assertEquals(FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.PHONE_BLE, seenAt = 10_000, now = clock.nowElapsedMs()))

        assertEquals(FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.BACKEND, seenAt = 85_000, now = clock.nowElapsedMs()))
        assertEquals(FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.BADGE_USB, seenAt = 40_000, now = clock.nowElapsedMs()))
        assertEquals(FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.BADGE_BLE, seenAt = 80_000, now = clock.nowElapsedMs()))
        assertEquals(FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.WIFI_ANALYSIS, seenAt = 70_000, now = clock.nowElapsedMs()))
    }

    @Test fun shorterProtocolTtlCanOnlyShortenLifetime() {
        assertEquals(FindingFreshness.STALE,
            freshnessFor(PrivacySourceKind.BACKEND, 85_000, 100_000, protocolTtlMs = 20_000))
        assertEquals(FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.BACKEND, 80_000, 100_000, protocolTtlMs = 20_000))
        assertEquals(FindingFreshness.LIVE,
            freshnessFor(PrivacySourceKind.BACKEND, 99_000, 100_000, protocolTtlMs = 120_000))
    }

    @Test fun pausedRowsKeepAgingAndResumeDoesNotMakeThemLive() {
        assertEquals(FindingFreshness.PAUSED_CACHED,
            freshnessFor(PrivacySourceKind.PHONE_BLE, 95_000, 100_000,
                sourceHealth = SourceHealthState.PAUSED))
        assertEquals(FindingFreshness.EXPIRED,
            freshnessFor(PrivacySourceKind.PHONE_BLE, 9_999, 100_000,
                sourceHealth = SourceHealthState.PAUSED))
        assertEquals(SourceHealthState.STALE,
            resumedHealth(hasRetainedRows = true))
        assertEquals(SourceHealthState.LOADING,
            resumedHealth(hasRetainedRows = false))
    }
}
```

- [ ] **Step 2: Write failing identity, capability, merge, and threat-count tests**

```kotlin
class PrivacyCurrentReducerTest {
    @Test fun sameIdentifierFromDifferentSourcesNeverCollapses() {
        val phone = finding(PrivacySourceKind.PHONE_BLE, stableId = "AA:BB", title = "Phone row")
        val badge = finding(PrivacySourceKind.BADGE_USB, stableId = "AA:BB", title = "Badge row")
        val state = reducer().reduce(listOf(liveSource(phone), liveSource(badge)), emptySet(), 10_000)
        assertEquals(listOf("Phone row", "Badge row"), state.findings.map { it.title }.sorted())
    }

    @Test fun ignoredIdentitySuppressesOnlyItsSource() {
        val phone = finding(PrivacySourceKind.PHONE_BLE, stableId = "AA:BB")
        val badge = finding(PrivacySourceKind.BADGE_USB, stableId = "AA:BB")
        val ignored = setOf(requireNotNull(phone.ignoreKey).encoded)
        val state = reducer().reduce(listOf(liveSource(phone), liveSource(badge)), ignored, 10_000)
        assertEquals(listOf(PrivacySourceKind.BADGE_USB), state.findings.map { it.source })
    }

    @Test fun threatCountIncludesLiveUnownedThreatsEvenWhenNotRoutable() {
        val state = reducer().reduce(
            sources = listOf(
                liveSource(finding(severity = FindingSeverity.CRITICAL, routable = true)),
                liveSource(finding(severity = FindingSeverity.AWARENESS, routable = true)),
                liveSource(finding(severity = FindingSeverity.CRITICAL, owned = true)),
                staleSource(finding(severity = FindingSeverity.CRITICAL)),
                pausedSource(finding(severity = FindingSeverity.CRITICAL)),
                liveSource(finding(severity = FindingSeverity.INFO)),
            ),
            ignoredKeys = emptySet(),
            nowElapsedMs = 10_000,
        )
        assertEquals(2, state.threatCount)
        assertEquals(1, state.alertEligible.size)
        assertEquals(FindingSeverity.CRITICAL, state.findings.first().severity)
    }
}

class PrivacyCapabilityTest {
    @Test fun actionsRequireStableSourceIdentityAndLiveLocalSamples() {
        assertEquals(
            PrivacyCapabilities(canIgnore = true, canTrack = true, canOpenDirectionSweep = true),
            capabilitiesFor(PrivacySourceKind.PHONE_BLE, stableId = "fp:one",
                hasLiveLocalSamples = true, freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE),
        )
        assertEquals(PrivacyCapabilities(),
            capabilitiesFor(PrivacySourceKind.BACKEND, stableId = null,
                hasLiveLocalSamples = false, freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE))
        assertEquals(
            PrivacyCapabilities(canIgnore = true, canTrack = false, canOpenDirectionSweep = false),
            capabilitiesFor(PrivacySourceKind.BADGE_USB, stableId = "entity:7",
                hasLiveLocalSamples = false, freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE),
        )

        assertEquals(PrivacyCapabilities(canIgnore = true),
            capabilitiesFor(PrivacySourceKind.PHONE_BLE, stableId = "fp:one",
                hasLiveLocalSamples = true, freshness = FindingFreshness.STALE,
                sourceHealth = SourceHealthState.STALE))
    }
}
```

- [ ] **Step 3: Run the focused Privacy-domain tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*PrivacyFreshnessTest' --tests '*PrivacyCurrentReducerTest' --tests '*PrivacyCapabilityTest'`

Expected: compilation fails because the reducer contract is not present.

- [ ] **Step 4: Define source, finding, health, and capability models**

```kotlin
enum class PrivacySourceKind(val preferenceId: String) {
    PHONE_BLE("phone_ble"),
    PHONE_ULTRASONIC("phone_ultrasonic"),
    BACKEND("backend"),
    BADGE_USB("badge_usb"),
    BADGE_AP("badge_ap"),
    BADGE_BLE("badge_ble"),
    BADGE_DEBUG_BRIDGE("badge_debug_bridge"),
    WIFI_ANALYSIS("wifi_analysis"),
}

enum class SourceHealthState {
    LOADING, LIVE, STALE, PAUSED, PERMISSION_BLOCKED, UNSUPPORTED, FAILED
}

enum class FindingFreshness { LIVE, STALE, PAUSED_CACHED, EXPIRED }
enum class FindingSeverity(val rank: Int) { INFO(0), NEARBY(1), AWARENESS(2), CRITICAL(3) }
enum class Ownership { UNKNOWN, OWNED }

data class PrivacyFindingKey(
    val source: PrivacySourceKind,
    val sourceRecordId: String,
) {
    init { require(sourceRecordId.isNotBlank()) }
    val encoded: String = "${source.preferenceId}\u001F$sourceRecordId"
}

data class PrivacyCapabilities(
    val canIgnore: Boolean = false,
    val canTrack: Boolean = false,
    val canOpenDirectionSweep: Boolean = false,
)

data class PrivacyFinding(
    val displayId: String,
    val observationKey: PrivacyFindingKey,
    val source: PrivacySourceKind,
    val stableSourceId: String?,
    val routableKey: PrivacyFindingKey?,
    val title: String,
    val evidence: String?,
    val limitation: String?,
    val category: PrivacyCategory,
    val severity: FindingSeverity,
    val ownership: Ownership,
    val signalDbm: Int?,
    val firstSeenWallMs: Long?,
    val lastSeenWallMs: Long?,
    val lastObservedElapsedMs: Long,
    val protocolTtlMs: Long?,
    val hasLiveLocalSamples: Boolean,
    val appleEvidence: AppleListeningEvidence? = null,
    val capabilities: PrivacyCapabilities = PrivacyCapabilities(),
    val freshness: FindingFreshness = FindingFreshness.LIVE,
) {
    val ignoreKey: FindingPreferenceKey?
        get() = stableSourceId?.let { FindingPreferenceKey.create(source.preferenceId, it) }
}

data class AppleListeningEvidence(
    val appleFamilyEvidence: Boolean,
    val airPodsAssociationEvidence: Boolean,
    val listeningOrientedCategoryOrWording: Boolean,
)

data class PrivacySourceHealth(
    val source: PrivacySourceKind,
    val state: SourceHealthState,
    val lastSuccessElapsedMs: Long?,
    val lastSuccessWallMs: Long?,
    val recoveryLabel: String?,
    val message: String?,
)

data class PrivacySourceSnapshot(
    val health: PrivacySourceHealth,
    val findings: List<PrivacyFinding>,
    val emittedAtElapsedMs: Long,
)

data class PrivacyCurrentState(
    val sources: List<PrivacySourceHealth>,
    val findings: List<PrivacyFinding>,
    val threatCount: Int,
    val alertEligible: List<PrivacyFinding>,
    val initialResolutionComplete: Boolean = false,
)
```

`observationKey` is required, source-qualified, and is the only reducer identity/Compose key. Each adapter derives it from that source's actual record/entity ID; if a source lacks one, the adapter allocates a monotonically increasing source-local observation ID and retains it only for that observation lifetime. `displayId` is presentation text/test-tag material and is never used for merge, routing, notification, or persistence. `stableSourceId` independently enables Ignore. `routableKey` independently enables exact detail/notification routing. Never construct a durable key from a display label, category, JA3 value alone, or a rotating address known to be unstable.

Every adapter creates `AppleListeningEvidence` from fields belonging to the same raw record before calling `PrivacyFindingNormalizer.normalize`. It never infers Apple correlation from a different row. Backend and Badge mappers preserve their structured Apple/AirPods/listening fields when present; absent structured evidence stays absent rather than being recreated from a merged display title.

- [ ] **Step 5: Implement named freshness policies and pause/resume rules**

```kotlin
data class FreshnessPolicy(val staleAfterMs: Long, val removeAfterMs: Long)

val PrivacyFreshnessPolicies = mapOf(
    PrivacySourceKind.PHONE_BLE to FreshnessPolicy(30_000, 90_000),
    PrivacySourceKind.PHONE_ULTRASONIC to FreshnessPolicy(30_000, 60_000),
    PrivacySourceKind.BACKEND to FreshnessPolicy(15_000, 60_000),
    PrivacySourceKind.BADGE_USB to FreshnessPolicy(10_000, 60_000),
    PrivacySourceKind.BADGE_AP to FreshnessPolicy(10_000, 60_000),
    PrivacySourceKind.BADGE_DEBUG_BRIDGE to FreshnessPolicy(10_000, 60_000),
    PrivacySourceKind.BADGE_BLE to FreshnessPolicy(20_000, 60_000),
    PrivacySourceKind.WIFI_ANALYSIS to FreshnessPolicy(30_000, 60_000),
)

fun freshnessFor(
    source: PrivacySourceKind,
    seenAt: Long,
    now: Long,
    protocolTtlMs: Long? = null,
    sourceHealth: SourceHealthState = SourceHealthState.LIVE,
): FindingFreshness {
    val policy = PrivacyFreshnessPolicies.getValue(source)
    val removeAt = minOf(policy.removeAfterMs, protocolTtlMs ?: Long.MAX_VALUE)
    val staleAt = minOf(policy.staleAfterMs, removeAt)
    val age = (now - seenAt).coerceAtLeast(0)
    return when {
        age >= removeAt -> FindingFreshness.EXPIRED
        sourceHealth == SourceHealthState.PAUSED -> FindingFreshness.PAUSED_CACHED
        sourceHealth != SourceHealthState.LIVE -> FindingFreshness.STALE
        age >= staleAt -> FindingFreshness.STALE
        else -> FindingFreshness.LIVE
    }
}

fun resumedHealth(hasRetainedRows: Boolean): SourceHealthState =
    if (hasRetainedRows) SourceHealthState.STALE else SourceHealthState.LOADING

fun agedSourceHealth(
    health: PrivacySourceHealth,
    nowElapsedMs: Long,
): PrivacySourceHealth {
    if (health.state != SourceHealthState.LIVE) return health
    val success = health.lastSuccessElapsedMs ?: return health.copy(state = SourceHealthState.LOADING)
    val staleAfter = PrivacyFreshnessPolicies.getValue(health.source).staleAfterMs
    return if (nowElapsedMs - success >= staleAfter) health.copy(state = SourceHealthState.STALE)
    else health
}
```

Use elapsed realtime from the Task 8 `MonotonicClock` for `seenAt/now`; use wall time only for user-facing ages. Age source health on every tick before aging rows. A `FAILED`, `STALE`, `PERMISSION_BLOCKED`, or `UNSUPPORTED` source can retain cached rows, but those rows become `STALE` and non-actionable; `PAUSED` rows become `PAUSED_CACHED`. A failure never becomes an empty success. Expired rows leave Current regardless of source state.

- [ ] **Step 6: Implement action capabilities and the pure reducer**

```kotlin
fun capabilitiesFor(
    source: PrivacySourceKind,
    stableId: String?,
    hasLiveLocalSamples: Boolean,
    freshness: FindingFreshness,
    sourceHealth: SourceHealthState,
): PrivacyCapabilities {
    val canIgnore = !stableId.isNullOrBlank()
    val canTrack = source == PrivacySourceKind.PHONE_BLE &&
        canIgnore &&
        hasLiveLocalSamples &&
        freshness == FindingFreshness.LIVE &&
        sourceHealth == SourceHealthState.LIVE
    return PrivacyCapabilities(canIgnore, canTrack, canTrack)
}

class PrivacyCurrentReducer {
    fun reduce(
        sources: List<PrivacySourceSnapshot>,
        ignoredKeys: Set<String>,
        nowElapsedMs: Long,
    ): PrivacyCurrentState {
        val effectiveSources = sources.map { snapshot ->
            snapshot.copy(health = agedSourceHealth(snapshot.health, nowElapsedMs))
        }
        val rows = effectiveSources.flatMap { snapshot ->
            snapshot.findings.mapNotNull { raw ->
                val normalized = PrivacyFindingNormalizer.normalize(raw)
                val freshness = freshnessFor(
                    source = normalized.source,
                    seenAt = normalized.lastObservedElapsedMs,
                    now = nowElapsedMs,
                    protocolTtlMs = normalized.protocolTtlMs,
                    sourceHealth = snapshot.health.state,
                )
                normalized.copy(
                    freshness = freshness,
                    capabilities = capabilitiesFor(
                        source = normalized.source,
                        stableId = normalized.stableSourceId,
                        hasLiveLocalSamples = normalized.hasLiveLocalSamples,
                        freshness = freshness,
                        sourceHealth = snapshot.health.state,
                    ),
                )
                    .takeUnless { freshness == FindingFreshness.EXPIRED }
                    ?.takeUnless { it.ignoreKey?.encoded in ignoredKeys }
            }
        }.groupBy(PrivacyFinding::observationKey)
            .map { (_, duplicates) ->
                duplicates.maxWith(
                    compareBy<PrivacyFinding> { it.lastObservedElapsedMs }
                        .thenBy { it.title }
                        .thenBy { it.evidence.orEmpty() },
                )
            }.sortedWith(
                compareByDescending<PrivacyFinding> { it.severity.rank }
                    .thenByDescending { it.lastObservedElapsedMs }
                    .thenBy { it.source.preferenceId }
                    .thenBy { it.observationKey.encoded },
            )

        val liveThreats = rows.filter {
            it.freshness == FindingFreshness.LIVE &&
                it.ownership != Ownership.OWNED &&
                it.severity.rank >= FindingSeverity.AWARENESS.rank
        }
        val alertEligible = liveThreats.filter {
            it.severity == FindingSeverity.CRITICAL && it.routableKey != null
        }
        return PrivacyCurrentState(
            sources = effectiveSources.map { it.health },
            findings = rows,
            threatCount = liveThreats.size,
            alertEligible = alertEligible,
            initialResolutionComplete = effectiveSources.isNotEmpty() &&
                effectiveSources.none { it.health.state == SourceHealthState.LOADING },
        )
    }
}
```

Add this `PrivacyFinding` overload to `PrivacyFindingNormalizer`; apply it at the end of every phone/backend/badge/Wi-Fi mapper before inserting the row into its source snapshot. It consumes only that row's structured evidence and never correlates separate records:

```kotlin
fun normalize(input: PrivacyFinding): PrivacyFinding {
    val apple = input.appleEvidence ?: return input
    if (!apple.appleFamilyEvidence || !apple.listeningOrientedCategoryOrWording) return input
    val safeOwnedTitle = input.title.takeIf {
        input.ownership == Ownership.OWNED && it.isNotBlank() &&
            !it.contains("listening", ignoreCase = true) &&
            !it.contains("eavesdrop", ignoreCase = true)
    }
    val title = safeOwnedTitle ?: if (apple.airPodsAssociationEvidence) {
        "AirPods connection/activity nearby"
    } else {
        "Apple device activity nearby"
    }
    return input.copy(
        title = title,
        evidence = if (apple.airPodsAssociationEvidence) {
            "An Apple device reports connected AirPods and media, call, or video activity."
        } else {
            "An Apple device reports a nearby activity state; the specific activity is unavailable."
        },
        limitation = "Live Listen and microphone use cannot be determined from BLE.",
        category = PrivacyCategory.APPLE_CONTINUITY,
        severity = FindingSeverity.INFO,
    )
}
```

Add explicit adapter tests for local AirPods, backend Apple evidence, badge Apple evidence, Wi-Fi camera/anomaly rows, BLE stalker alerts, and ultrasonic results so no current finding path disappears during migration.

- [ ] **Step 7: Move source ownership into an app-scoped repository**

```kotlin
interface PrivacySourceAdapter {
    val adapterId: String
    val snapshots: StateFlow<List<PrivacySourceSnapshot>>
}

@Singleton
class PrivacyFindingRepository @Inject constructor(
    sourceAdapters: Set<@JvmSuppressWildcards PrivacySourceAdapter>,
    appPreferences: AppPreferences,
    private val clock: MonotonicClock,
    @ApplicationScope scope: CoroutineScope,
) {
    private val adapters = sourceAdapters.sortedBy(PrivacySourceAdapter::adapterId)
    private val sourceSnapshots = combine(adapters.map(PrivacySourceAdapter::snapshots)) {
        snapshotLists -> snapshotLists.flatMap { it }
    }

    val currentState: StateFlow<PrivacyCurrentState> = combine(
        sourceSnapshots,
        appPreferences.ignoredFindingKeys,
        clock.ticks(),
        PrivacyCurrentReducer()::reduce,
    ).stateIn(
        scope,
        SharingStarted.Eagerly,
        PrivacyCurrentState(emptyList(), emptyList(), 0, emptyList()),
    )
}
```

`PrivacyModule` binds all four concrete adapters into the Hilt set with `@Binds @IntoSet`; it also binds `PrivacyFindingRepository` as the single repository consumed by `PrivacyViewModel`. An adapter returns a list so it can represent independent source-health clocks without merging them. `PhonePrivacySourceAdapter` always emits separate `PHONE_BLE` and `PHONE_ULTRASONIC` snapshots, even when one has no rows; current `GlassesDetector` BLE rows and `BleTracker` stalker alerts belong to the BLE snapshot, while ultrasonic results belong only to the ultrasonic snapshot. `BackendPrivacySourceAdapter` emits one snapshot from Task 4's gated configured-backend stream. `BadgePrivacySourceAdapter` emits exactly the active transport-specific Badge snapshot. `WifiPrivacySourceAdapter` takes rows from `WiFiPrivacyScanner`, including any rows currently multiplexed through `SkyObjectRepository.glassesDetections`, and emits one `WIFI_ANALYSIS` snapshot; provenance is retained at ingestion so those rows never become `PHONE_BLE`. Every snapshot asserts that each finding's `source` equals `health.source`, and each adapter has mapping tests with stable observation keys and actual source timestamps.

For `BadgeThreatEntity`, populate `AppleListeningEvidence` only when Apple/AirPods vendor evidence and listening-oriented wording coexist on that same entity's `label`, `detail`, `evidence`, `category`, or `code`. Do not infer Apple from a different entity, aggregate count, adjacent observation, or source label. Test positive same-entity and negative split-across-two-entities cases.

Adapters—not screens—start/stop their collectors from observable settings and permission state. Until Task 13 centralizes the permission producer, they translate the existing Android permission checks to `PERMISSION_BLOCKED`; Task 13 replaces that producer without changing this adapter contract. Disabled settings emit `PAUSED`, unsupported hardware emits `UNSUPPORTED`, permission failures emit `PERMISSION_BLOCKED`, exceptions emit `FAILED`, and only a successful current source update emits `LIVE`. `PrivacyViewModel` projects `currentState`; it does not call `BadgeUsbRepository.start/stop`, register a BLE callback, or own a polling loop.

Delete `DetectionPrefs.KEY_IGNORED_MACS`, its cache/get/ignore/unignore methods, `GlassesDetector.ignoreDevice/unignoreDevice`, and the hot-path prefilter. Do not migrate the old ambiguous global MAC set: its entries lack source kind and may be rotating, so silently mapping them would suppress unrelated rows. All new suppression happens after source mapping through Task 1's source-aware DataStore keys.

- [ ] **Step 8: Add stable key decode and Ignore/Restore operations**

```kotlin
fun ignore(finding: PrivacyFinding) {
    val key = finding.ignoreKey ?: return
    viewModelScope.launch { appPreferences.ignoreFinding(key) }
}

fun restore(encoded: String) {
    val key = FindingPreferenceKey.decode(encoded) ?: return
    viewModelScope.launch { appPreferences.restoreFinding(key) }
}
```

`FindingPreferenceKey.decode` must reject blank or malformed values and round-trip `source`, `stableId`, and `encoded`. Store no global MAC-ignore set and do not suppress a row from another source.

- [ ] **Step 9: Run the Privacy reducer suite**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*PrivacyFreshnessTest' --tests '*PrivacyCurrentReducerTest' --tests '*PrivacyCapabilityTest' --tests '*PrivacyFindingNormalizerTest'`

Expected: PASS at all exact stale/removal boundaries, with source-isolated suppression and no alerts from stale/paused/owned/informational rows.

- [ ] **Step 10: Commit the Privacy state foundation**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt android/app/src/main/java/com/friendorfoe/data/DetectionPrefs.kt android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt android/app/src/main/java/com/friendorfoe/di/PrivacyModule.kt android/app/src/test/java/com/friendorfoe/presentation/privacy
git commit -m "android: make privacy findings source aware"
```

### Task 12: Reformat Privacy as the faithful current-findings list

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/RouteCodec.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyFilterState.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IgnoredDevicesScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/DirectionScanOverlay.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicy.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertNotifier.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertCoordinator.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyNotificationIdStore.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyNotificationRouteTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/DirectionSweepControllerTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/privacy/PrivacyScreenTest.kt`

**Interfaces:**
- Consumes: `PrivacyCurrentState` and Ignore/Restore operations from Task 11.
- Produces: a thin `PrivacyRoute`, Faithful-cleanup grouped list, `Privacy > Ignored devices`, exact-item routes, truthful empty/partial/failure states, capability-driven actions, cancellable `RssiDirectionSweepController`, and routable critical notifications.

- [ ] **Step 1: Write the failing Privacy presentation tests**

```kotlin
@RunWith(AndroidJUnit4::class)
class PrivacyScreenTest {
    @get:Rule val compose = createComposeRule()

    @Test fun keepsCurrentGroupsAndRemovesConfigurationAndSweepCards() {
        compose.setContent { PrivacyContent(stateWithAllSeverities(), PrivacyActions()) }
        listOf("THREATS", "AWARENESS", "NEARBY", "INFO")
            .forEach { compose.onNodeWithText(it).assertIsDisplayed() }
        listOf("Badge appearance", "Display policy", "Magnetic-field sweep", "IR-like light scan")
            .forEach { compose.onNodeWithText(it, substring = true).assertDoesNotExist() }
    }

    @Test fun rowShowsOnlyKnownFieldsAndSupportedActions() {
        compose.setContent { PrivacyContent(stateWithPhoneAndBackendRows(), PrivacyActions()) }
        compose.onNodeWithTag("finding_phone").assertTextContains("Critical")
        compose.onNodeWithTag("finding_phone").assertTextContains("-54 dBm")
        compose.onNodeWithTag("finding_phone_ignore").assertHasClickAction()
        compose.onNodeWithTag("finding_phone_track").assertHasClickAction()
        compose.onNodeWithTag("finding_backend_track").assertDoesNotExist()
        compose.onNodeWithTag("finding_backend_signal").assertDoesNotExist()
    }

    @Test fun resolvedEmptyHasNoClearFiltersAction() {
        compose.setContent { PrivacyContent(resolvedEmptyState(), PrivacyActions()) }
        compose.onNodeWithText("No current findings").assertIsDisplayed()
        compose.onNodeWithText("Clear filters").assertDoesNotExist()
    }

    @Test fun filteredEmptyOffersClearFilters() {
        compose.setContent { PrivacyContent(filteredEmptyState(), PrivacyActions()) }
        compose.onNodeWithText("No matches").assertIsDisplayed()
        compose.onNodeWithText("Clear filters").assertHasClickAction()
    }

    @Test fun partialFailureKeepsValidRowsButFullFailureShowsRetry() {
        compose.setContent { PrivacyContent(partialFailureState(), PrivacyActions()) }
        compose.onNodeWithTag("finding_phone").assertIsDisplayed()
        compose.onNodeWithText("Backend failed").assertIsDisplayed()
        compose.onNodeWithText("Privacy sources unavailable").assertDoesNotExist()
    }

    @Test fun fullRetryableFailureShowsRetryWithoutPretendingPermissionFailure() {
        compose.setContent { PrivacyContent(retryableFailureState(), PrivacyActions()) }
        compose.onNodeWithText("Privacy sources unavailable").assertIsDisplayed()
        compose.onNodeWithText("Retry").assertHasClickAction()
        compose.onNodeWithText("Open app settings").assertDoesNotExist()
    }

    @Test fun noSkyAlertFooterOrToolsRemain() {
        compose.setContent { PrivacyContent(stateWithAllSeverities(), PrivacyActions()) }
        listOf("Drone alerts", "Military alerts", "Helicopter alerts", "Calibration", "Sweep tools")
            .forEach { compose.onNodeWithText(it, substring = true).assertDoesNotExist() }
    }
}
```

- [ ] **Step 2: Write failing route and direction-lifecycle tests**

```kotlin
class PrivacyNotificationRouteTest {
    @Test fun notificationIdentityIncludesSourceAndRecord() {
        val ids = FakePrivacyNotificationIdStore()
        val key = PrivacyFindingKey(PrivacySourceKind.BADGE_USB, "entity:42")
        val target = PrivacyNotificationRoute.from(key, ids)
        assertEquals("privacy/finding/badge_usb/entity%3A42", target.route)
        val other = PrivacyNotificationRoute.from(
            key = PrivacyFindingKey(PrivacySourceKind.BADGE_USB, "entity:43"),
            ids = ids,
        )
        assertNotEquals(target.pendingIntentId, other.pendingIntentId)
        assertNotEquals(target.dataUri, other.dataUri)
    }
}

class DirectionSweepControllerTest {
    @Test fun cancelStopsSamplingAndNeverReportsLocated() = runTest {
        val samples = MutableSharedFlow<RssiSample>()
        val finding = phoneFinding(canTrack = true)
        val controller = RssiDirectionSweepController(
            sampleSource = FakeRssiSampleSource(finding.observationKey, samples),
            scope = backgroundScope,
        )
        controller.start(finding)
        runCurrent()
        samples.emit(rssiSample(finding.observationKey, -55))
        controller.cancel()
        samples.emit(rssiSample(finding.observationKey, -20))
        assertEquals(DirectionSweepState.Idle, controller.state.value)
        assertFalse(controller.resultText.value.contains("located", ignoreCase = true))
    }

    @Test fun finishNeedsEnoughTargetBoundSamples() = runTest {
        val samples = MutableSharedFlow<RssiSample>()
        val finding = phoneFinding(canTrack = true)
        val controller = RssiDirectionSweepController(
            FakeRssiSampleSource(finding.observationKey, samples), backgroundScope,
        )
        controller.start(finding)
        runCurrent()
        repeat(5) { samples.emit(rssiSample(finding.observationKey, -60 + it)) }
        controller.finish()
        assertTrue(controller.state.value is DirectionSweepState.InsufficientSamples)
    }
}
```

- [ ] **Step 3: Run the focused UI/domain tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*PrivacyNotificationRouteTest' --tests '*DirectionSweepControllerTest'`

Expected: compilation fails on the missing routes/controller.

- [ ] **Step 4: Define the faithful Privacy UI state and grouping**

```kotlin
enum class PrivacySection(val label: String) {
    THREATS("THREATS"), AWARENESS("AWARENESS"), NEARBY("NEARBY"), INFO("INFO")
}

fun PrivacyFinding.section(): PrivacySection = when (severity) {
    FindingSeverity.CRITICAL -> PrivacySection.THREATS
    FindingSeverity.AWARENESS -> PrivacySection.AWARENESS
    FindingSeverity.NEARBY -> PrivacySection.NEARBY
    FindingSeverity.INFO -> PrivacySection.INFO
}

data class PrivacyFilterState(
    val query: String = "",
    val categories: Set<PrivacyCategory> = emptySet(),
    val sources: Set<PrivacySourceKind> = emptySet(),
) {
    val activeFilterCount: Int
        get() = (if (query.isBlank()) 0 else 1) +
            (if (categories.isEmpty()) 0 else 1) +
            (if (sources.isEmpty()) 0 else 1)
}

data class PrivacyUiState(
    val sourceHealth: List<PrivacySourceHealth> = emptyList(),
    val visibleFindings: List<PrivacyFinding> = emptyList(),
    val totalCurrentCount: Int = 0,
    val filters: PrivacyFilterState = PrivacyFilterState(),
    val body: PrivacyBodyState = PrivacyBodyState.Loading,
    val lastUpdatedWallMs: Long? = null,
    val partialFailureCount: Int = 0,
    val focusedKey: PrivacyFindingKey? = null,
    val focusedFinding: PrivacyFinding? = null,
    val focusedFindingExpired: Boolean = false,
)

sealed interface PrivacyBodyState {
    data object Loading : PrivacyBodyState
    data object Content : PrivacyBodyState
    data object Empty : PrivacyBodyState
    data class NoMatches(val activeFilterCount: Int) : PrivacyBodyState
    data class RetryableFailure(val message: String) : PrivacyBodyState
    data class PermissionBlocked(val message: String) : PrivacyBodyState
    data class Unsupported(val message: String) : PrivacyBodyState
}

data class PrivacyActions(
    val onRecoverSource: (PrivacySourceKind) -> Unit = {},
    val onQueryChanged: (String) -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onRetryAllSources: () -> Unit = {},
    val onOpenPermissionSettings: () -> Unit = {},
    val onIgnore: (PrivacyFinding) -> Unit = {},
    val onTrack: (PrivacyFinding) -> Unit = {},
    val onOpenDetails: (PrivacyFindingKey) -> Unit = {},
)
```

Project search/filter state in `PrivacyViewModel` and keep the reducer's severity/recency order within each section. `PrivacyFilterState.activeFilterCount` counts nonblank query, nonempty category selection, and nonempty source selection exactly once each. Resolve `PrivacyBodyState` from source evidence: retained rows produce `Content` even with partial failure; all retryable failures produce `RetryableFailure`; all permission-blocked sources produce `PermissionBlocked`; all unsupported/disabled sources produce `Unsupported`; mixed terminal states choose the one with an actionable recovery rather than always showing Retry. One failed source sets `partialFailureCount` and remains in the source summary without hiding good rows. `lastUpdatedWallMs` is the newest actual source success, never the recomposition time.

- [ ] **Step 5: Build the screen in the approved hierarchy**

```kotlin
@Composable
fun PrivacyContent(state: PrivacyUiState, actions: PrivacyActions) {
    LazyColumn(Modifier.fillMaxSize()) {
        item { FofScreenHeader("Privacy", state.totalCurrentCount, "current findings") }
        item { PrivacySourceHealthSummary(state.sourceHealth, actions.onRecoverSource) }
        state.lastUpdatedWallMs?.let { updated ->
            item { Text("Last updated ${formatWallTime(updated)}") }
        }
        item { PrivacySearchAndFilters(state, actions) }

        when (val body = state.body) {
            PrivacyBodyState.Loading -> item {
                FofLoadingState("Checking Phone, Backend, Badge, and Wi-Fi sources")
            }
            PrivacyBodyState.Empty -> item { FofEmptyState("No current findings") }
            is PrivacyBodyState.NoMatches -> item {
                FofNoMatchesState(body.activeFilterCount, actions.onClearFilters)
            }
            is PrivacyBodyState.RetryableFailure -> item {
                FofErrorState("Privacy sources unavailable", body.message, "Retry",
                    actions.onRetryAllSources)
            }
            is PrivacyBodyState.PermissionBlocked -> item {
                FofErrorState("Permission needed", body.message, "Open app settings",
                    actions.onOpenPermissionSettings)
            }
            is PrivacyBodyState.Unsupported -> item {
                FofEmptyState(body.message)
            }
            PrivacyBodyState.Content -> {
            PrivacySection.entries.forEach { section ->
                val rows = state.visibleFindings.filter { it.section() == section }
                if (rows.isNotEmpty()) {
                    item { PrivacySectionStrip(section.label, rows.size) }
                    items(rows, key = { it.observationKey.encoded }) { finding ->
                        PrivacyFindingRow(finding, actions)
                    }
                }
            }
            }
        }
    }
}
```

`PrivacySourceHealthSummary` has compact Phone, Backend, Badge, and Wi-Fi rows. Phone rolls up only enabled/supported sub-sources by worst actionable state (`FAILED/PERMISSION_BLOCKED`, then `STALE/PAUSED`, then `LOADING`, then `LIVE`) while preserving each source's detail in the expanded summary. A disabled optional ultrasonic source cannot turn a live BLE source into `PAUSED`; if every phone sub-source is disabled, the rollup is explicitly `Paused`. Badge rolls up only the active transport-specific Badge source; inactive transports do not create failures. Backend and Wi-Fi retain their own rows. Keep tinted group strips, compact flat rows, dividers, outline chips, and blue actions. Use Material icons, 48dp targets, multiline title/evidence/limitation, visible severity text, source label, freshness/age, `Your device`, and optional dBm. Omit missing values instead of writing `Unknown` into every field. Add rollup tests for BLE-live/ultrasonic-disabled, BLE-permission-blocked/ultrasonic-live, and all-disabled.

- [ ] **Step 6: Render only capability-backed row actions**

```kotlin
@Composable
private fun PrivacyFindingActions(finding: PrivacyFinding, actions: PrivacyActions) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        if (finding.capabilities.canIgnore) {
            TextButton(
                onClick = { actions.onIgnore(finding) },
                modifier = Modifier.heightIn(min = 48.dp).testTag("finding_${finding.displayId}_ignore"),
            ) { Text("Ignore") }
        }
        if (finding.capabilities.canTrack) {
            TextButton(
                onClick = { actions.onTrack(finding) },
                modifier = Modifier.heightIn(min = 48.dp).testTag("finding_${finding.displayId}_track"),
            ) { Text("RSSI direction sweep") }
        }
    }
}
```

Make every row with `routableKey != null` expose a visible `Details` affordance and make the row clickable through the same exact-key callback; unroutable rows omit both. Add `liveRegion = LiveRegionMode.Polite` only for a newly inserted live critical row, and combine severity/source/title into one TalkBack description without duplicating every visible child.

- [ ] **Step 7: Add Ignored devices and exact-item routes**

Add `Screen.PrivacyFinding` as `privacy/finding/{source}/{record}` and keep `Screen.IgnoredDevices` as `privacy/ignored`. Use a JVM-safe segment codec and validate the source enum on decode:

```kotlin
fun encodeRouteSegment(value: String): String =
    URLEncoder.encode(value, StandardCharsets.UTF_8.name()).replace("+", "%20")

fun decodeRouteSegment(value: String): String? = runCatching {
    URLDecoder.decode(value, StandardCharsets.UTF_8.name())
}.getOrNull()

fun openPrivacyFinding(key: PrivacyFindingKey) {
    navController.navigate(
        "privacy/finding/${encodeRouteSegment(key.source.preferenceId)}/${encodeRouteSegment(key.sourceRecordId)}"
    )
}
```

`PrivacyFindingRepository` exposes exact lookup only and distinguishes startup from expiry:

```kotlin
sealed interface PrivacyFindingLookupState {
    data object Loading : PrivacyFindingLookupState
    data class Present(val finding: PrivacyFinding) : PrivacyFindingLookupState
    data object Expired : PrivacyFindingLookupState
}

fun finding(key: PrivacyFindingKey): Flow<PrivacyFindingLookupState> = currentState
    .map { state ->
        state.findings.singleOrNull { it.routableKey == key }
            ?.let(PrivacyFindingLookupState::Present)
            ?: if (state.initialResolutionComplete) PrivacyFindingLookupState.Expired
            else PrivacyFindingLookupState.Loading
    }
    .distinctUntilChanged()
```

`IgnoredDevicesScreen` lists decoded source + stable ID and a 48dp `Restore` action. The item route collects `repository.finding(exactKey)`; if absent at first resolution, or if the same item later expires, it renders `Item no longer current` with `Back to Privacy`. It never selects another finding with the same label or identifier.

- [ ] **Step 8: Replace the direction overlay with a cancellable RSSI sweep**

```kotlin
data class RssiSample(
    val findingKey: PrivacyFindingKey,
    val dbm: Int,
    val azimuthDegrees: Float,
    val observedAtElapsedMs: Long,
)

sealed interface DirectionSweepState {
    data object Idle : DirectionSweepState
    data class Sampling(val key: PrivacyFindingKey) : DirectionSweepState
    data class InsufficientSamples(val count: Int) : DirectionSweepState
    data class Complete(val key: PrivacyFindingKey) : DirectionSweepState
}

fun summarizeStrongestDirection(samples: List<RssiSample>): String {
    require(samples.isNotEmpty())
    val strongestSector = samples.groupBy {
        ((((it.azimuthDegrees % 360f) + 360f) % 360f) / 45f).toInt().coerceIn(0, 7)
    }.maxWith(compareBy<Map.Entry<Int, List<RssiSample>>> {
        it.value.map(RssiSample::dbm).average()
    }.thenBy { -it.key }).key
    val start = strongestSector * 45
    return "Strongest signal was toward $start°–${start + 44}° during this sweep."
}

fun interface RssiSampleSource {
    fun samplesFor(key: PrivacyFindingKey): Flow<RssiSample>
}

class RssiDirectionSweepController(
    private val sampleSource: RssiSampleSource,
    private val scope: CoroutineScope,
) {
    private var samplingJob: Job? = null
    private var targetKey: PrivacyFindingKey? = null
    private val captured = mutableListOf<RssiSample>()
    private val _state = MutableStateFlow<DirectionSweepState>(DirectionSweepState.Idle)
    val state = _state.asStateFlow()
    private val _resultText = MutableStateFlow("")
    val resultText = _resultText.asStateFlow()

    fun start(finding: PrivacyFinding) {
        if (!finding.capabilities.canOpenDirectionSweep) return
        val key = finding.observationKey
        samplingJob?.cancel()
        targetKey = key
        captured.clear()
        _resultText.value = ""
        _state.value = DirectionSweepState.Sampling(key)
        samplingJob = scope.launch {
            withTimeoutOrNull(30_000L) {
                sampleSource.samplesFor(key)
                    .filter { it.findingKey == key }
                    .take(40)
                    .collect(captured::add)
            }
            if (isActive) completeFromCaptured()
        }
    }

    fun finish() {
        samplingJob?.cancel()
        samplingJob = null
        completeFromCaptured()
    }

    fun cancel() {
        samplingJob?.cancel()
        samplingJob = null
        targetKey = null
        captured.clear()
        _resultText.value = ""
        _state.value = DirectionSweepState.Idle
    }


    private fun completeFromCaptured() {
        if (captured.size < 6) {
            _resultText.value = "Not enough samples; keep turning slowly and try again."
            _state.value = DirectionSweepState.InsufficientSamples(captured.size)
            return
        }
        _resultText.value = summarizeStrongestDirection(captured) +
            " Signal strength is approximate and does not locate the device."
        _state.value = DirectionSweepState.Complete(requireNotNull(targetKey))
    }
}
```

The target key comes from a live phone-BLE row whose `canOpenDirectionSweep` is true; stale/paused/other-source rows cannot start. The producer flow itself is keyed to that finding, and the controller rejects mismatched samples again. The sweep completes after 40 samples, a 30-second timeout, or explicit Finish; fewer than six samples never produce a direction. The result says only where signal was strongest during this sweep and includes the RSSI limitation. Closing the route and `ViewModel.onCleared()` both call `cancel()`.

- [ ] **Step 9: Make critical notifications routable and unique**

```kotlin
data class PrivacyNotificationRoute(
    val route: String,
    val dataUri: String,
    val pendingIntentId: Int,
) {
    companion object {
        fun from(
            key: PrivacyFindingKey,
            ids: PrivacyNotificationIdStore,
        ): PrivacyNotificationRoute {
            val source = encodeRouteSegment(key.source.preferenceId)
            val record = encodeRouteSegment(key.sourceRecordId)
            val route = "privacy/finding/$source/$record"
            return PrivacyNotificationRoute(
                route = route,
                dataUri = "friendorfoe://privacy/finding/$source/$record",
                pendingIntentId = ids.idFor(key),
            )
        }
    }
}
```

Define `PrivacyNotificationIdStore` as an interface and implement `SharedPreferencesPrivacyNotificationIdStore` with one private lock. Inside `synchronized(lock)`, return an existing `id_<encoded-key>` mapping or scan every stored `id_` value, advance a persisted `next_id` to the next unused positive Int (wrapping from `Int.MAX_VALUE` to 1), and commit the key plus next counter atomically before returning. A failed commit throws and suppresses notification creation. Add concurrent-allocation, process-reconstruction, and forced-collision tests. Bind the singleton in `PrivacyModule`. Use that same collision-safe ID as notification ID and PendingIntent request code. The Intent also sets the unique `dataUri`, package, immutable/update-current flags, and exact route extra.

`PrivacyAlertPolicy` accepts only `PrivacyCurrentState.alertEligible`. An app-scoped `PrivacyAlertCoordinator` observes it once and notifies only on an edge: a key newly becomes alert-eligible, or its severity rises after the stored five-minute cooldown. Inject a singleton `PrivacyAlertBootstrap` into `FriendOrFoeApplication` and call its idempotent `start()` once; no screen starts the coordinator. The one-second freshness tick cannot re-notify an unchanged row. Move and delete direct stalker/ultrasonic/Wi-Fi notification calls from `PrivacyViewModel` and source callbacks; all findings pass through this coordinator. Do not issue an item notification when `routableKey` is null.

`MainActivity` parses the exact Intent data/extra in both `onCreate` and `onNewIntent`, validates it, and queues one pending route until both onboarding and the main NavHost are ready. Completing onboarding consumes the queued route once. A deep link whose exact row has expired still opens the exact item route, which renders `Item no longer current`; it never falls back to the Privacy root or a similarly named row.

- [ ] **Step 10: Run Privacy unit and Compose suites**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*Privacy*Test' --tests '*DirectionSweepControllerTest'`

Expected: PASS.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.privacy.PrivacyScreenTest`

Expected: PASS with all four groups, correct actions/states, and no Badge/tool configuration content.

- [ ] **Step 11: Commit the Privacy list overhaul**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/main/java/com/friendorfoe/presentation/navigation android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt android/app/src/test/java/com/friendorfoe/presentation/privacy android/app/src/androidTest/java/com/friendorfoe/presentation/privacy
git commit -m "android: clarify the privacy findings list"
```

### Task 13: Request permissions only inside the feature that needs them

**Files:**
- Modify: `android/app/src/main/AndroidManifest.xml`
- Modify: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferences.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/permissions/FeaturePermissions.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/permissions/FeaturePermissionGate.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/permissions/PermissionStateRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/permissions/PermissionLauncher.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/welcome/WelcomeScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/PermissionHandler.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraScanScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/permissions/FeaturePermissionsTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/permissions/ContextualPermissionTest.kt`

**Interfaces:**
- Consumes: Android SDK version, granted permissions, rationale state, notification/channel state, and persisted `requested_permissions` history.
- Produces: pure feature-specific permission requirements, `PermissionUiState`, an explanation-first `FeaturePermissionGate`, and `Open app settings` recovery. Launch and unrelated destinations request nothing.

- [ ] **Step 1: Write failing API-level permission-set tests**

```kotlin
class FeaturePermissionsTest {
    @Test fun startupHasNoPermissionRequirement() {
        assertTrue(requiredPermissions(AppFeature.APP_LAUNCH, sdk = 35).isEmpty())
    }

    @Test fun api35PhonePrivacyUsesNearbyBluetoothAndWifi() {
        assertEquals(
            setOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.NEARBY_WIFI_DEVICES,
            ),
            requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 35),
        )
    }

    @Test fun api35LocalRadioDiscoveryUsesTheSameNearbySet() {
        assertEquals(
            requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 35),
            requiredPermissions(AppFeature.LOCAL_RADIO_DISCOVERY, sdk = 35),
        )
    }

    @Test fun legacyPhonePrivacyUsesFineLocation() {
        assertEquals(
            setOf(Manifest.permission.ACCESS_FINE_LOCATION),
            requiredPermissions(AppFeature.PHONE_PRIVACY_SCAN, sdk = 30),
        )
    }

    @Test fun notificationAndMicrophoneAreSeparateFeatures() {
        assertEquals(setOf(Manifest.permission.POST_NOTIFICATIONS),
            requiredPermissions(AppFeature.PRIVACY_ALERTS, sdk = 35))
        assertEquals(setOf(Manifest.permission.RECORD_AUDIO),
            requiredPermissions(AppFeature.ULTRASONIC, sdk = 35))
    }

    @Test fun approximateLocationIsRepresentedInsteadOfGrantedFine() {
        assertEquals(
            PermissionUiState.Approximate,
            evaluateLocationPermission(fineGranted = false, coarseGranted = true,
                requestedBefore = true, shouldShowRationale = false),
        )
    }
}
```

- [ ] **Step 2: Run the focused test and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*FeaturePermissionsTest'`

Expected: compilation fails because the contextual-permission contract is absent.

- [ ] **Step 3: Define the feature matrix and truthful states**

```kotlin
enum class AppFeature {
    APP_LAUNCH, AR_CAMERA, IR_CAMERA, AR_MAP_LOCATION,
    LOCAL_RADIO_DISCOVERY, PHONE_PRIVACY_SCAN, PRIVACY_ALERTS, SKY_ALERTS, ULTRASONIC,
}

sealed interface PermissionUiState {
    data object Loading : PermissionUiState
    data object Granted : PermissionUiState
    data object Approximate : PermissionUiState
    data object Denied : PermissionUiState
    data object PermanentlyDenied : PermissionUiState
    data object NotificationsBlocked : PermissionUiState
    data object NotificationChannelBlocked : PermissionUiState
}

fun requiredPermissions(feature: AppFeature, sdk: Int): Set<String> = when (feature) {
    AppFeature.APP_LAUNCH -> emptySet()
    AppFeature.AR_CAMERA, AppFeature.IR_CAMERA -> setOf(Manifest.permission.CAMERA)
    AppFeature.AR_MAP_LOCATION -> setOf(
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION,
    )
    AppFeature.LOCAL_RADIO_DISCOVERY, AppFeature.PHONE_PRIVACY_SCAN -> when {
        sdk >= 33 -> setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.NEARBY_WIFI_DEVICES,
        )
        sdk >= 31 -> setOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )
        else -> setOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }
    AppFeature.PRIVACY_ALERTS ->
        if (sdk >= 33) setOf(Manifest.permission.POST_NOTIFICATIONS) else emptySet()
    AppFeature.SKY_ALERTS ->
        if (sdk >= 33) setOf(Manifest.permission.POST_NOTIFICATIONS) else emptySet()
    AppFeature.ULTRASONIC -> setOf(Manifest.permission.RECORD_AUDIO)
}
```

Define the evaluators and user-facing copy in the same file rather than leaving helper APIs implicit:

```kotlin
data class PermissionEvidence(
    val permission: String,
    val granted: Boolean,
    val requestedBefore: Boolean,
    val shouldShowRationale: Boolean,
)

fun evaluateLocationPermission(
    fineGranted: Boolean,
    coarseGranted: Boolean,
    requestedBefore: Boolean,
    shouldShowRationale: Boolean,
): PermissionUiState = when {
    fineGranted -> PermissionUiState.Granted
    coarseGranted -> PermissionUiState.Approximate
    requestedBefore && !shouldShowRationale -> PermissionUiState.PermanentlyDenied
    else -> PermissionUiState.Denied
}

fun evaluateFeaturePermission(evidence: List<PermissionEvidence>): PermissionUiState = when {
    evidence.isEmpty() || evidence.all(PermissionEvidence::granted) -> PermissionUiState.Granted
    evidence.any { !it.granted && it.requestedBefore && !it.shouldShowRationale } ->
        PermissionUiState.PermanentlyDenied
    else -> PermissionUiState.Denied
}

fun permissionTitle(feature: AppFeature): String = when (feature) {
    AppFeature.AR_CAMERA -> "Camera for AR"
    AppFeature.IR_CAMERA -> "Camera for IR-like light"
    AppFeature.AR_MAP_LOCATION -> "Location for nearby results"
    AppFeature.LOCAL_RADIO_DISCOVERY -> "Nearby radio access"
    AppFeature.PHONE_PRIVACY_SCAN -> "Nearby-device access"
    AppFeature.PRIVACY_ALERTS, AppFeature.SKY_ALERTS -> "Notifications"
    AppFeature.ULTRASONIC -> "Microphone for ultrasonic checks"
    AppFeature.APP_LAUNCH -> "Friend or Foe"
}
```

`permissionExplanation` and `permissionRecovery` are exhaustive `when(feature)` functions with evidence-limited copy. `FofPermissionState` is implemented in `FeaturePermissionGate.kt` as a scrollable title/body/action surface with a 48dp action and optional secondary Back action. Treat either location permission as enough to enter the flow, but label coarse-only state `Approximate location`; suppress precise distance/bearing claims. For notifications, `PermissionStateRepository` combines runtime permission, `NotificationManagerCompat.areNotificationsEnabled()`, and the relevant channel's importance.

Define the repository contract rather than letting each screen infer permanent denial differently:

```kotlin
interface PlatformPermissionEvidence {
    fun isGranted(permission: String): Boolean
    fun notificationsEnabled(): Boolean
    fun channelEnabled(channelId: String): Boolean
}

@Singleton
class PermissionStateRepository @Inject constructor(
    private val platform: PlatformPermissionEvidence,
    private val preferences: AppPreferences,
) {
    private val _states = MutableStateFlow<Map<AppFeature, PermissionUiState>>(emptyMap())
    val states: StateFlow<Map<AppFeature, PermissionUiState>> = _states.asStateFlow()

    fun refresh(
        sdk: Int,
        shouldShowRationale: (String) -> Boolean,
        requestedBefore: Set<String>,
    ) { /* evaluate every feature from the pure functions above */ }
}
```

The repository never retains an `Activity`; the route supplies the current rationale function on initial composition and `ON_RESUME`. `PRIVACY_ALERTS` checks channel `privacy_alerts`, `SKY_ALERTS` checks `sky_alerts`, and other features do not query notification channels. Add repository tests for first denial versus permanent denial, global notification block, channel-only block, and refresh after settings.

- [ ] **Step 4: Persist whether each permission has been requested**

Extend the Task 1 interface and implementation exactly:

```kotlin
val requestedPermissions: Flow<Set<String>>
suspend fun markPermissionsRequested(permissions: Set<String>)
```

`AppPreferencesRepository` stores `requested_permissions` as a DataStore string set and unions new values atomically. `PermissionStateRepository` combines that flow with current platform evidence. Permanent denial is true only when a permission is in that persisted set, remains denied, and Activity rationale is false; first request is never mislabeled permanent.

```kotlin
fun evaluatePermission(
    granted: Boolean,
    requestedBefore: Boolean,
    shouldShowRationale: Boolean,
): PermissionUiState = when {
    granted -> PermissionUiState.Granted
    requestedBefore && !shouldShowRationale -> PermissionUiState.PermanentlyDenied
    else -> PermissionUiState.Denied
}
```

- [ ] **Step 5: Implement the explanation-first gate and settings recovery**

```kotlin
@Composable
fun FeaturePermissionGate(
    feature: AppFeature,
    state: PermissionUiState,
    onRequest: () -> Unit,
    onOpenSettings: () -> Unit,
    grantedContent: @Composable () -> Unit,
) {
    when (state) {
        PermissionUiState.Loading -> FofLoadingState("Checking permission")
        PermissionUiState.Granted, PermissionUiState.Approximate -> grantedContent()
        PermissionUiState.Denied -> FofPermissionState(
            title = permissionTitle(feature),
            explanation = permissionExplanation(feature),
            actionLabel = "Continue",
            onAction = onRequest,
        )
        PermissionUiState.PermanentlyDenied,
        PermissionUiState.NotificationsBlocked,
        PermissionUiState.NotificationChannelBlocked -> FofPermissionState(
            title = permissionTitle(feature),
            explanation = permissionRecovery(feature, state),
            actionLabel = "Open app settings",
            onAction = onOpenSettings,
        )
    }
}
```

Use one permission-backed toggle implementation so request-then-commit is not reimplemented by every setting row:

```kotlin
@Composable
fun PermissionBackedToggle(
    tag: String,
    label: String,
    checked: Boolean,
    permissionState: PermissionUiState,
    onOpenExplanation: () -> Unit,
    onCommitChecked: (Boolean) -> Unit,
) {
    val usable = permissionState == PermissionUiState.Granted ||
        permissionState == PermissionUiState.Approximate
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .testTag(tag)
            .toggleable(
                value = checked && usable,
                role = Role.Switch,
                onValueChange = { next ->
                    when {
                        !next -> onCommitChecked(false)
                        usable -> onCommitChecked(true)
                        else -> onOpenExplanation()
                    }
                },
            ),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, Modifier.weight(1f))
        Switch(checked = checked && usable, onCheckedChange = null)
    }
}
```

The settings action uses `Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))`. Notification-channel denial uses `Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS` with the exact channel owned by the feature: `privacy_alerts` for `PRIVACY_ALERTS` and `sky_alerts` for `SKY_ALERTS`.

Wrap the platform launcher behind a tiny testable boundary and persist request history before the system dialog:

```kotlin
fun interface PermissionLauncher {
    fun launch(permissions: Array<String>)
}

fun interface FeaturePermissionRequester {
    fun request(feature: AppFeature)
}

fun requestFeaturePermissions(
    missing: Set<String>,
    scope: CoroutineScope,
    preferences: AppPreferences,
    launcher: PermissionLauncher,
) {
    if (missing.isEmpty()) return
    scope.launch {
        preferences.markPermissionsRequested(missing)
        withContext(Dispatchers.Main.immediate) {
            launcher.launch(missing.sorted().toTypedArray())
        }
    }
}
```

Each feature Composable creates one `rememberLauncherForActivityResult(RequestMultiplePermissions())`, adapts it to `PermissionLauncher`, and forwards results to `PermissionStateRepository.refresh()`. A lifecycle `ON_RESUME` observer also refreshes runtime, global notification, and channel state after Settings. Location evaluation handles the fine/coarse pair as a unit; general multi-permission evaluation uses the permanent-denial precedence above.

For settings toggles, the row tap only stores a `pendingPermissionFeature` and opens an in-app explanation dialog/sheet using `permissionExplanation(feature)`; it launches no system contract. The dialog's `Continue` invokes the feature's `FeaturePermissionRequester`, which delegates to `requestFeaturePermissions`; Cancel clears pending state. After the launcher result and repository refresh, commit `true` only if the complete effective state is usable. Thus the explanation always precedes the system prompt.

- [ ] **Step 6: Wire only the approved contextual triggers**

- AR asks for camera on entering AR and location when location-dependent content starts.
- Map asks for location on first location-dependent use; its accessible non-location state still opens.
- AR/List local BLE Remote ID, Wi-Fi Remote ID, stalker, and Wi-Fi-anomaly collection share `LOCAL_RADIO_DISCOVERY`; on API 33+ it requires Bluetooth scan/connect plus nearby Wi-Fi, and on legacy releases it follows the same location fallback as the phone scanner. Their stored toggles can remain true from an upgrade, but their effective collectors are false until this gate is usable.
- Enabling `Phone privacy scan` asks for the API-level phone-privacy set and leaves the toggle off if denied.
- Enabling privacy alerts asks for notifications and leaves the toggle off if denied or globally blocked.
- Enabling any retained Drone, Helicopter, Military, or Police sky-alert toggle uses `SKY_ALERTS` and leaves that requested toggle off if notification permission/global delivery is blocked.
- Enabling ultrasonic asks for microphone and leaves the toggle off if denied.
- IR is wrapped once by `FeaturePermissionGate(IR_CAMERA)` in the Advanced route. `IrCameraScanScreen` and the Task 15 camera state consume the granted result and contain no second permission launcher.
- Denial in any flow leaves the other six top-level destinations usable.

Every toggle follows request-then-commit: keep the stored setting false, show the explanation, launch only after Continue, and write true only after the complete permission/channel state resolves usable. Denial, dismiss, or permanent denial keeps it false. Approximate location permits coarse location content but passes `isPrecise = false` to AR/Map presenters so distance/bearing copy is suppressed.

For an upgrade with a previously stored true value but missing current OS permission/channel delivery, the collector's effective state is false and the row shows `Permission needed`; do not claim it is active. Preserve that stored preference so granting access restores the user's prior intent. Fresh installs use Task 4's false defaults, so their first enable follows request-then-commit literally.

Remove update checking from Welcome. Place the primary `Continue` action before optional help/version links and state: sky/privacy detection scope, uncertainty limits, local History/configured-service data behavior, and contextual permission timing.

- [ ] **Step 7: Add a startup/no-cross-feature instrumentation regression**

```kotlin
@RunWith(AndroidJUnit4::class)
class ContextualPermissionTest {
    @get:Rule val compose = createComposeRule()

    @Test fun welcomeShowsNoPermissionGate() {
        compose.setContent { WelcomeScreen(onGetStarted = {}) }
        compose.onNodeWithText("Continue").assertIsDisplayed()
        listOf("Camera for AR", "Nearby-device access", "Notifications", "Microphone")
            .forEach { compose.onNodeWithText(it, substring = true).assertDoesNotExist() }
    }

    @Test fun denyingCameraDoesNotBlockInfo() {
        val selected = mutableStateOf(TopLevelDestination.AR)
        compose.setContent {
            FofNavigationSuite(
                showNavigation = true,
                currentRoute = selected.value.route,
                onNavigate = { selected.value = it },
            ) { padding ->
                Box(Modifier.padding(padding).fillMaxSize()) {
                    if (selected.value == TopLevelDestination.AR) {
                        FeaturePermissionGate(
                            feature = AppFeature.AR_CAMERA,
                            state = PermissionUiState.PermanentlyDenied,
                            onRequest = {},
                            onOpenSettings = {},
                            grantedContent = {},
                        )
                    } else if (selected.value == TopLevelDestination.INFO) {
                        Box(Modifier.fillMaxSize().testTag("screen_info")) { Text("Info") }
                    }
                }
            }
        }
        compose.onNodeWithText("Camera for AR").assertIsDisplayed()
        compose.onNodeWithContentDescription("Info").performClick()
        compose.onNodeWithTag("screen_info").assertIsDisplayed()
    }

    @Test fun deniedNotificationRequestLeavesSkyToggleOff() {
        val enabled = mutableStateOf(false)
        var permissionRequests = 0
        compose.setContent {
            PermissionBackedToggle(
                tag = "military_alerts",
                label = "Military alerts",
                checked = enabled.value,
                permissionState = PermissionUiState.Denied,
                onOpenExplanation = { permissionRequests += 1 },
                onCommitChecked = { enabled.value = it },
            )
        }
        compose.onNodeWithTag("military_alerts").performClick()
        compose.onNodeWithTag("military_alerts").assertIsOff()
        assertEquals(1, permissionRequests) // explanation opened; launcher is still zero
        assertFalse(enabled.value)
    }

    @Test fun explanationPrecedesSystemPermissionLaunch() {
        var explanationOpen by mutableStateOf(false)
        var launches = 0
        compose.setContent {
            PermissionToggleHarness(
                explanationOpen = explanationOpen,
                onOpenExplanation = { explanationOpen = true },
                onContinue = { launches += 1 },
            )
        }
        compose.onNodeWithTag("phone_privacy_scan").performClick()
        assertEquals(0, launches)
        compose.onNodeWithText("Continue").performClick()
        assertEquals(1, launches)
    }
}
```

The fixtures use an injected `FeaturePermissionRequester`/`PermissionLauncher`; no UiAutomator dependency is added. Separate tests cover Phone scan, Privacy alerts, all four sky-alert toggles, microphone, approximate location, permanent denial/Open settings, globally blocked notifications, channel-disabled notifications, and `ON_RESUME` refresh after returning from Settings. Each Compose test calls `setContent` once.

- [ ] **Step 8: Run permission tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*FeaturePermissionsTest'`

Expected: PASS.

Run on a clean API 35 emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.permissions.ContextualPermissionTest`

Expected: PASS with zero launch-time permission requests and feature-specific denial/recovery behavior.

- [ ] **Step 9: Commit contextual permissions and Welcome cleanup**

```bash
git add android/app/src/main/AndroidManifest.xml android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferences.kt android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt android/app/src/main/java/com/friendorfoe/presentation/welcome android/app/src/main/java/com/friendorfoe/presentation/permissions android/app/src/main/java/com/friendorfoe/presentation/ar android/app/src/main/java/com/friendorfoe/presentation/map android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/main/java/com/friendorfoe/presentation/about android/app/src/test/java/com/friendorfoe/presentation/permissions android/app/src/androidTest/java/com/friendorfoe/presentation/permissions
git commit -m "android: request permissions in context"
```

### Task 14: Make Info the truthful settings, update, and Advanced home

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/AppVersion.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/repository/AppUpdateRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/remote/AppUpdateApi.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/repository/BackendSessionHealthRepository.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/remote/BackendHealthClient.kt`
- Create: `android/app/src/main/java/com/friendorfoe/di/InfoModule.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/welcome/WelcomeScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/AppVersionTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/about/CalibrationEntryGateTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/about/AboutViewModelTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/about/InfoScreenTest.kt`

**Interfaces:**
- Consumes: observable `DetectionSettings`, validated `BackendEndpoint`, endpoint-bound `BackendHealthClient`, BuildConfig version values, permission state, and app update metadata.
- Produces: `InfoUiState`, ordered version comparison, neutral connection-test state, current-session `calibrationEntryAvailable`, and the approved Info section hierarchy. The calibration workflow itself is not changed.

- [ ] **Step 1: Write failing version-order and calibration-gate tests**

```kotlin
class AppVersionTest {
    @Test fun unequalLabelsDoNotAutomaticallyMeanUpdate() {
        assertFalse(isUpdateAvailable(
            installed = AppVersion(108, "0.64.65-privacy-beacons"),
            remote = AppVersion(null, "0.64.65"),
        ))
        assertFalse(isUpdateAvailable(AppVersion(108, "0.64.65"), AppVersion(107, "9.0.0")))
        assertTrue(isUpdateAvailable(AppVersion(108, "0.64.65"), AppVersion(109, "0.1.0")))
        assertTrue(isUpdateAvailable(AppVersion(108, "0.64.65"), AppVersion(null, "0.65.0")))
    }

    @Test fun malformedRemoteVersionIsNotAnUpdate() {
        assertFalse(isUpdateAvailable(AppVersion(108, "0.64.65"), AppVersion(null, "latest")))
        assertFalse(isUpdateAvailable(AppVersion(108, "0.64.65"), AppVersion(null, "0.65.0garbage")))
        assertTrue(isUpdateAvailable(AppVersion(108, "0.64.65-dev+4"),
            AppVersion(null, "v0.65.0-release+1")))
    }
}

class CalibrationEntryGateTest {
    @Test fun requiresEnabledBackendAndSessionHealthForCurrentEndpoint() {
        val endpointA = BackendEndpoint.parse("http://badge-lab:8000").getOrThrow()
        val endpointB = BackendEndpoint.parse("http://field-kit:8000").getOrThrow()
        assertFalse(calibrationEntryAvailable(false, endpointA, SessionHealth.Healthy(endpointA)))
        assertTrue(calibrationEntryAvailable(true, endpointA, SessionHealth.Healthy(endpointA)))
        assertFalse(calibrationEntryAvailable(true, endpointB, SessionHealth.Healthy(endpointA)))
        assertFalse(calibrationEntryAvailable(true, endpointA, SessionHealth.Untested))
    }
}
```

- [ ] **Step 2: Run the focused tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppVersionTest' --tests '*CalibrationEntryGateTest'`

Expected: compilation fails because ordered versions and session health are not implemented.

- [ ] **Step 3: Implement ordered version comparison**

```kotlin
data class AppVersion(val code: Long?, val name: String)

private fun numericVersionParts(value: String): List<Long>? {
    val match = Regex(
        "^v?(\\d+(?:\\.\\d+){1,3})(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$"
    ).matchEntire(value.trim()) ?: return null
    return match.groupValues[1].split('.').map { component ->
        component.toLongOrNull()?.takeIf { it <= 999_999L } ?: return null
    }
}

fun isUpdateAvailable(installed: AppVersion, remote: AppVersion): Boolean {
    if (installed.code != null && remote.code != null) return remote.code > installed.code
    val current = numericVersionParts(installed.name) ?: return false
    val latest = numericVersionParts(remote.name) ?: return false
    val width = maxOf(current.size, latest.size)
    repeat(width) { index ->
        val comparison = (latest.getOrElse(index) { 0 })
            .compareTo(current.getOrElse(index) { 0 })
        if (comparison != 0) return comparison > 0
    }
    return false
}
```

Move the GitHub release request from `WelcomeScreen` behind a complete testable boundary:

```kotlin
data class ReleaseMetadataDto(
    @SerializedName("tag_name") val tagName: String?,
    @SerializedName("version_code") val versionCode: Long?,
    @SerializedName("html_url") val htmlUrl: String?,
)

interface AppUpdateApi {
    @GET("repos/lnxgod/friendorfoe/releases/latest")
    suspend fun latestRelease(): ReleaseMetadataDto
}

data class AppUpdateMetadata(val version: AppVersion, val releaseUrl: String)

interface AppUpdateRepository {
    suspend fun latest(): Result<AppUpdateMetadata>
}

sealed interface UpdateUiState {
    data object Idle : UpdateUiState
    data object Checking : UpdateUiState
    data class UpToDate(val installed: AppVersion) : UpdateUiState
    data class Available(val remote: AppUpdateMetadata) : UpdateUiState
    data class Failed(val message: String) : UpdateUiState
}
```

`HttpAppUpdateRepository` rejects blank/malformed tags, non-HTTPS release URLs, and missing URLs. `InfoModule` provides the GitHub Retrofit base URL and binds the HTTP repository; tests inject `FakeAppUpdateRepository`. `Info` shows `Update available` only when the comparator returns true; failure says `Could not check for updates` and never fabricates availability.

- [ ] **Step 4: Define neutral backend/session-health state**

Reuse `ConnectionTestState` from Task 4; do not define a second version. Add only current-session health:

```kotlin
sealed interface SessionHealth {
    data object Untested : SessionHealth
    data class Checking(val endpoint: BackendEndpoint) : SessionHealth
    data class Healthy(val endpoint: BackendEndpoint) : SessionHealth
    data class Failed(val endpoint: BackendEndpoint, val message: String) : SessionHealth
}

fun calibrationEntryAvailable(
    backendEnabled: Boolean,
    endpoint: BackendEndpoint,
    health: SessionHealth,
): Boolean = backendEnabled && health == SessionHealth.Healthy(endpoint)
```

The connection test text always includes the validated configured endpoint. Convert exceptions to `Connection failed` plus concise recovery copy; do not render raw exception/HTTP bodies. A successful ordinary `testConnection()` publishes `SessionHealth.Healthy(endpoint)` through the same session repository used by Calibration.

- [ ] **Step 5: Reset and refresh Calibration availability correctly**

```kotlin
@Singleton
class BackendSessionHealthRepository @Inject constructor(
    private val healthClient: BackendHealthClient,
    @ApplicationScope private val scope: CoroutineScope,
) {
    private val _health = MutableStateFlow<SessionHealth>(SessionHealth.Untested)
    val health: StateFlow<SessionHealth> = _health.asStateFlow()
    private var checkJob: Job? = null
    private var generation = 0L

    fun invalidate() {
        generation += 1
        checkJob?.cancel()
        checkJob = null
        _health.value = SessionHealth.Untested
    }

    fun check(endpoint: BackendEndpoint, enabled: Boolean) {
        invalidate()
        if (!enabled) return
        val requestGeneration = generation
        _health.value = SessionHealth.Checking(endpoint)
        checkJob = scope.launch(Dispatchers.IO) {
            val next = try {
                val response = healthClient.check(endpoint)
                SessionHealth.Healthy(endpoint) to response.version
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                SessionHealth.Failed(endpoint, "Connection failed") to null
            }
            if (generation == requestGeneration) _health.value = next.first
        }
    }

    fun recordConnected(endpoint: BackendEndpoint) {
        generation += 1
        checkJob?.cancel()
        _health.value = SessionHealth.Healthy(endpoint)
    }
}
```

`BackendHealthClient.check(endpoint)` constructs the request URL from the passed immutable `BackendEndpoint` (for example `endpoint.baseUrl.toHttpUrl().newBuilder().addPathSegment("health")`) and executes it with an injected base `OkHttpClient`; it must not use the mutable settings URL interceptor. Add a race test: block endpoint A, start A, switch/check endpoint B, complete B healthy, then complete A; state must remain `Healthy(B)`. This is the same endpoint-bound client used by ordinary Info `testConnection()`.

The repository is app-session scoped, so navigating away from and back to Info does not erase valid evidence. `AboutViewModel` observes `(sensorBackendEnabled, validated endpoint)` and calls `invalidate()` when either changes or becomes invalid/disabled. Generation checking plus cancellation prevents an obsolete endpoint response from publishing later. `InfoUiState.calibrationEntryAvailable` combines current settings, the current validated endpoint, and `sessionHealthRepository.health`. A false entry is disabled, visibly says `Unavailable`, and cannot invoke navigation. A true entry opens the unchanged `CalibrateScreen`; its token/preflight rules remain authoritative.

- [ ] **Step 6: Replace getter snapshots with one Info state**

```kotlin
data class InfoUiState(
    val settings: DetectionSettings = DetectionSettings.defaults(),
    val permissions: Map<AppFeature, PermissionUiState> = emptyMap(),
    val sourceStatus: List<InfoSourceStatus> = emptyList(),
    val backendUrlDraft: String = "",
    val backendUrlError: String? = null,
    val connection: ConnectionTestState = ConnectionTestState.Idle,
    val sessionHealth: SessionHealth = SessionHealth.Untested,
    val calibrationEntryAvailable: Boolean = false,
    val updateState: UpdateUiState = UpdateUiState.Idle,
)

data class InfoSourceStatus(
    val label: String,
    val configured: Boolean,
    val effective: Boolean,
    val statusText: String,
    val recoveryFeature: AppFeature? = null,
)

enum class InfoSettingKey {
    ADS_B, BLE_REMOTE_ID, WIFI_REMOTE_ID, PHONE_PRIVACY_SCAN, STALKER,
    ULTRASONIC, WIFI_ANOMALY, PRIVACY_ALERTS, DRONE_ALERTS,
    HELICOPTER_ALERTS, MILITARY_ALERTS, POLICE_ALERTS, SENSOR_BACKEND,
    BACKEND_ONLY,
}

data class InfoActions(
    val onSetSetting: (InfoSettingKey, Boolean) -> Unit = { _, _ -> },
    val onEditBackendUrl: (String) -> Unit = {},
    val onSaveBackendUrl: () -> Unit = {},
    val onTestConnection: () -> Unit = {},
    val onCheckForUpdates: () -> Unit = {},
    val onRefreshCalibration: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onOpenMagneticField: () -> Unit = {},
    val onOpenIrLikeLight: () -> Unit = {},
    val onOpenCalibration: () -> Unit = {},
)
```

`AboutViewModel.uiState` combines observable settings, Task 13 permission state, backend draft/validation, Task 4 connection state, singleton session health, update state, and source-health/effective-enable evidence into one `StateFlow<InfoUiState>`. `sourceStatus` includes ADS-B, BLE Remote ID, Wi-Fi Remote ID, Phone privacy scan, Ultrasonic, configured backend, and notification delivery; a configured true value with missing permission renders `Permission needed`, never `On`. `saveBackendUrl` parses first and publishes an inline error without writing on failure. `testConnection` uses the validated immutable endpoint through `BackendHealthClient` and calls `sessionHealthRepository.recordConnected(endpoint)` only on actual health success. Whole-row toggle handlers either write the setting and trigger its runtime consumer or do not appear. Rename `Privacy Scanner` to `Phone privacy scan`.

- [ ] **Step 7: Rebuild Info in the approved order**

```kotlin
@Composable
fun InfoContent(state: InfoUiState, actions: InfoActions) {
    LazyColumn(Modifier.fillMaxSize()) {
        item { FofScreenHeader("Info") }
        item { InfoSection("Source & permission status") { SourcePermissionRows(state, actions) } }
        item { InfoSection("Settings") { RuntimeSettingsRows(state, actions) } }
        item { InfoSection("Guide & category legend") { GuideAndLegendRows(actions) } }
        item { InfoSection("Privacy & Data") { PrivacyDataCopy() } }
        item { InfoSection("About, support, version & updates") { AboutAndUpdateRows(state, actions) } }
        item { InfoSection("Advanced") { AdvancedRows(state, actions) } }
    }
}
```

Implement `InfoSection`, `SourcePermissionRows`, `RuntimeSettingsRows`, `GuideAndLegendRows`, `PrivacyDataCopy`, `AboutAndUpdateRows`, and `AdvancedRows` as internal composables in `AboutScreen.kt`; they take only `InfoUiState`/`InfoActions`, keep one vertical `LazyColumn`, and assign stable tags (`info_section_<index>`, `setting_<key>`, `backend_url`, `calibration_entry`). `FofScreenHeader("Info")` has no Back action because Info is top-level.

`PrivacyDataCopy` states all four facts explicitly, including the exact first sentence `History may store observations and phone coordinates locally.` It also says ADS-B/weather may receive location; the configured sensor backend exchanges detection data; and Calibration sends operator/session GPS when used. Remove `No personal data is collected or transmitted` and `All detection data stays on your device`.

Advanced contains, in order:

1. `Magnetic-field sweep` — secondary route.
2. `IR-like light scan` — secondary route.
3. `Triangulation Calibration` — de-emphasized, with session-health state and Refresh.

Keep the top-level bar on Info and hide it after entering one of these secondary routes.

- [ ] **Step 8: Add Info Compose tests**

```kotlin
@Test fun sectionsAndAdvancedEntriesUseApprovedOrder() {
    compose.setContent { InfoContent(infoState(calibrationAvailable = false), InfoActions()) }
    assertEquals(
        listOf("sources", "settings", "guide", "privacy_data", "about", "advanced"),
        infoSectionOrder(),
    )
    (0..5).forEach { index ->
        compose.onNodeWithTag("info_section_$index").performScrollTo().assertIsDisplayed()
    }
    compose.onNodeWithTag("calibration_entry").assertIsNotEnabled()
    compose.onNodeWithText("Unavailable").assertIsDisplayed()
}

@Test fun settingsUseLiveStateAfterToggle() {
    lateinit var enabled: MutableState<Boolean>
    compose.setContent {
        enabled = remember { mutableStateOf(false) }
        InfoContent(
            infoState(phonePrivacyEnabled = enabled.value),
            InfoActions(onSetSetting = { key, value ->
                if (key == InfoSettingKey.PHONE_PRIVACY_SCAN) enabled.value = value
            }),
        )
    }
    compose.onNodeWithTag("setting_phone_privacy_scan").performClick()
    compose.onNodeWithTag("setting_phone_privacy_scan").assertIsOn()
}

@Test fun invalidBackendDoesNotSaveAndCalibrationCannotNavigate() {
    var saved = false
    var opened = false
    compose.setContent {
        InfoContent(
            infoState(backendDraft = "not-a-url", backendError = "Enter a complete URL"),
            InfoActions(
                onSaveBackendUrl = { saved = true },
                onOpenCalibration = { opened = true },
            ),
        )
    }
    compose.onNodeWithTag("backend_save").assertIsNotEnabled()
    compose.onNodeWithText("Enter a complete URL").assertIsDisplayed()
    compose.onNodeWithTag("calibration_entry").assertIsNotEnabled()
    assertFalse(saved)
    assertFalse(opened)
}
```

Separate tests (one `setContent` each) assert the configured endpoint appears in Connected/Failed copy, exact Privacy & Data facts are present, both old false privacy claims are absent, available Calibration invokes navigation, unavailable Calibration never does, update unavailable/failure states are neutral, and returning to Info retains current-session healthy state.

- [ ] **Step 9: Run Info/version/calibration tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppVersionTest' --tests '*CalibrationEntryGateTest' --tests '*AboutViewModelTest'`

Expected: PASS.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.InfoScreenTest`

Expected: PASS, including disabled Calibration navigation.

- [ ] **Step 10: Commit Info and the Calibration entry gate**

```bash
git add android/app/src/main/java/com/friendorfoe/data/AppVersion.kt android/app/src/main/java/com/friendorfoe/data/remote/AppUpdateApi.kt android/app/src/main/java/com/friendorfoe/data/remote/BackendHealthClient.kt android/app/src/main/java/com/friendorfoe/data/repository/AppUpdateRepository.kt android/app/src/main/java/com/friendorfoe/data/repository/BackendSessionHealthRepository.kt android/app/src/main/java/com/friendorfoe/di/InfoModule.kt android/app/src/main/java/com/friendorfoe/presentation/about android/app/src/main/java/com/friendorfoe/presentation/welcome/WelcomeScreen.kt android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt android/app/src/test/java/com/friendorfoe/data/AppVersionTest.kt android/app/src/test/java/com/friendorfoe/presentation/about android/app/src/androidTest/java/com/friendorfoe/presentation/about
git commit -m "android: make info settings and advanced truthful"
```

### Task 15: Make Magnetic-field and IR-like-light tools evidence-limited

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/detection/EmfDetector.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/IrCameraDetector.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/EmfSweepViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/EmfSweepScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraScanViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraScanScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IrPreviewTransform.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraBinder.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/MagneticFieldSweepTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/privacy/IrPreviewTransformTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/privacy/AdvancedPrivacyToolsTest.kt`

**Interfaces:**
- Consumes: magnetometer availability/accuracy/readings, CameraX analysis crop/rotation/lens metadata, and contextual camera permission from Task 13.
- Produces: truthful magnetic-field states, baseline/reset behavior, normalized IR-like-light wording, and crop/rotation/mirror-correct preview points. Neither tool identifies a hidden device.

- [ ] **Step 1: Write failing initial-state and coordinate-transform tests**

```kotlin
class MagneticFieldSweepTest {
    @Test fun initialStateDoesNotInventZeroOrNormal() {
        val reducer = MagneticFieldReducer()
        assertTrue(reducer.state.value is MagneticFieldUiState.Initializing)
    }

    @Test fun resetUsesNextAccurateSampleAsBaseline() {
        val reducer = MagneticFieldReducer()
        reducer.onSample(MagneticSample(42f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))
        reducer.requestBaselineReset()
        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)
        reducer.onSample(MagneticSample(55f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))
        val live = reducer.state.value as MagneticFieldUiState.Live
        assertEquals(55f, live.baselineMicroTesla, 0.01f)
        assertEquals(0f, live.deviationMicroTesla, 0.01f)
    }

    @Test fun lowAndMediumAccuracyCannotSetBaseline() {
        val reducer = MagneticFieldReducer()
        reducer.onSample(MagneticSample(42f, SensorManager.SENSOR_STATUS_ACCURACY_LOW))
        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)
        reducer.onSample(MagneticSample(45f, SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM))
        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)
    }

    @Test fun unavailableAndFailureEventsAreVisibleStates() {
        val reducer = MagneticFieldReducer()
        reducer.onUnavailable()
        assertTrue(reducer.state.value is MagneticFieldUiState.SensorUnavailable)
        reducer.onFailure("Could not read magnetometer")
        assertEquals(MagneticFieldUiState.Failed("Could not read magnetometer"),
            reducer.state.value)
    }
}

class IrPreviewTransformTest {
    @Test fun appliesCropRotationAndFrontCameraMirror() {
        val metadata = AnalysisFrameMetadata(
            imageWidth = 1920,
            imageHeight = 1080,
            crop = IntRect(240, 0, 1680, 1080),
            rotationDegrees = 90,
            frontCamera = true,
        )
        val point = transformAnalysisPoint(
            source = FloatPoint(240f, 0f),
            metadata = metadata,
            previewWidth = 1080f,
            previewHeight = 1440f,
        )
        assertEquals(0f, point.x, 0.5f)
        assertEquals(0f, point.y, 0.5f)
    }

    @Test fun centerRemainsCenterForAllQuarterTurns() {
        listOf(0, 90, 180, 270).forEach { rotation ->
            val point = transformAnalysisPoint(
                FloatPoint(500f, 250f),
                AnalysisFrameMetadata(1000, 500, IntRect(0, 0, 1000, 500), rotation, false),
                400f,
                800f,
            )
            assertEquals(200f, point.x, 0.5f)
            assertEquals(400f, point.y, 0.5f)
        }
    }
}
```

- [ ] **Step 2: Run the focused tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*MagneticFieldSweepTest' --tests '*IrPreviewTransformTest'`

Expected: compilation fails because the truthful state/transform types do not exist.

- [ ] **Step 3: Replace the magnetic-field reducer and UI states**

```kotlin
data class MagneticSample(val totalMicroTesla: Float, val accuracy: Int)

fun sensorAccuracyLabel(accuracy: Int): String = when (accuracy) {
    SensorManager.SENSOR_STATUS_ACCURACY_HIGH -> "High accuracy"
    SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM -> "Medium accuracy"
    SensorManager.SENSOR_STATUS_ACCURACY_LOW -> "Low accuracy"
    else -> "Unreliable"
}

sealed interface MagneticSensorEvent {
    data object Unavailable : MagneticSensorEvent
    data class Sample(val value: MagneticSample) : MagneticSensorEvent
    data class Failed(val message: String) : MagneticSensorEvent
}

sealed interface MagneticFieldUiState {
    data object Initializing : MagneticFieldUiState
    data object SensorUnavailable : MagneticFieldUiState
    data class AwaitingAccurateBaseline(val accuracyLabel: String) : MagneticFieldUiState
    data class Live(
        val totalMicroTesla: Float,
        val baselineMicroTesla: Float,
        val deviationMicroTesla: Float,
        val peakDeviationMicroTesla: Float,
        val accuracyLabel: String,
    ) : MagneticFieldUiState
    data class Failed(val message: String) : MagneticFieldUiState
}

class MagneticFieldReducer {
    private var resetPending = true
    private var baseline: Float? = null
    private var peakDeviation = 0f
    private val _state = MutableStateFlow<MagneticFieldUiState>(MagneticFieldUiState.Initializing)
    val state = _state.asStateFlow()

    fun onUnavailable() {
        _state.value = MagneticFieldUiState.SensorUnavailable
    }

    fun onFailure(message: String) {
        _state.value = MagneticFieldUiState.Failed(message)
    }

    fun requestBaselineReset() {
        resetPending = true
        baseline = null
        peakDeviation = 0f
        _state.value = MagneticFieldUiState.AwaitingAccurateBaseline("Waiting for high accuracy")
    }

    fun onSample(sample: MagneticSample) {
        if ((resetPending || baseline == null) &&
            sample.accuracy != SensorManager.SENSOR_STATUS_ACCURACY_HIGH) {
            _state.value = MagneticFieldUiState.AwaitingAccurateBaseline(
                sensorAccuracyLabel(sample.accuracy),
            )
            return
        }
        if (resetPending || baseline == null) {
            baseline = sample.totalMicroTesla
            peakDeviation = 0f
            resetPending = false
        }
        val delta = abs(sample.totalMicroTesla - requireNotNull(baseline))
        peakDeviation = maxOf(peakDeviation, delta)
        _state.value = MagneticFieldUiState.Live(
            sample.totalMicroTesla, requireNotNull(baseline), delta, peakDeviation,
            sensorAccuracyLabel(sample.accuracy),
        )
    }
}
```

`EmfDetector` retains the latest `SensorEventListener.onAccuracyChanged` value and emits `MagneticSensorEvent.Sample(MagneticSample(magnitude, accuracy))`; no `EmfLevel` or electronics/threat threshold remains. It emits `Unavailable` when no magnetometer exists and `Failed` on registration failure rather than silently closing. `EmfSweepViewModel.start()` is invoked by the route's lifecycle `ON_START`, `stop()` cancels the flow at `ON_STOP`, and `onCleared()` also stops. It no longer starts from `init`.

The screen title is `Magnetic-field sweep`. Initial/unavailable states explain the magnetometer, accuracy, baseline, and Reset. Only `SENSOR_STATUS_ACCURACY_HIGH` may establish a baseline; low, medium, and unreliable samples remain `AwaitingAccurateBaseline`. Live state says that a deviation is a magnetic-field change and cannot identify electronics, cameras, or intent. Remove `NORMAL`, threat bands, hidden-device claims, and any zero-valued initial reading.

- [ ] **Step 4: Implement preview coordinate transformation**

```kotlin
data class FloatPoint(val x: Float, val y: Float)

data class AnalysisFrameMetadata(
    val imageWidth: Int,
    val imageHeight: Int,
    val crop: IntRect,
    val rotationDegrees: Int,
    val frontCamera: Boolean,
)

fun transformAnalysisPoint(
    source: FloatPoint,
    metadata: AnalysisFrameMetadata,
    previewWidth: Float,
    previewHeight: Float,
): FloatPoint {
    val u = ((source.x - metadata.crop.left) / metadata.crop.width).coerceIn(0f, 1f)
    val v = ((source.y - metadata.crop.top) / metadata.crop.height).coerceIn(0f, 1f)
    val rotated = when (metadata.rotationDegrees) {
        0 -> FloatPoint(u, v)
        90 -> FloatPoint(1f - v, u)
        180 -> FloatPoint(1f - u, 1f - v)
        270 -> FloatPoint(v, 1f - u)
        else -> error("Rotation must be 0, 90, 180, or 270")
    }
    val mirrored = if (metadata.frontCamera) FloatPoint(1f - rotated.x, rotated.y) else rotated
    val rotatedWidth = if (metadata.rotationDegrees % 180 == 0) metadata.crop.width else metadata.crop.height
    val rotatedHeight = if (metadata.rotationDegrees % 180 == 0) metadata.crop.height else metadata.crop.width
    val scale = maxOf(previewWidth / rotatedWidth, previewHeight / rotatedHeight)
    val xCrop = (rotatedWidth * scale - previewWidth) / 2f
    val yCrop = (rotatedHeight * scale - previewHeight) / 2f
    return FloatPoint(
        x = mirrored.x * rotatedWidth * scale - xCrop,
        y = mirrored.y * rotatedHeight * scale - yCrop,
    )
}
```

Change `IrCameraDetector.IrSource` from normalized coordinates to `centerPx: FloatPoint` in analyzer bitmap pixels, and include analyzed width/height in `FrameAnalysis`. Pass that pixel point plus CameraX `ImageProxy.cropRect`, `imageInfo.rotationDegrees`, lens facing, and PreviewView dimensions with every result. In production, derive the preview matrix from `PreviewView.outputTransform`/CameraX `CoordinateTransform` when available; the pure `transformAnalysisPoint` above is the tested fill-center equivalent and fallback. Apply crop, quarter-turn rotation, front-camera mirror, and PreviewView fill-center exactly once at the preview boundary; never separately rotate the drawn overlay. Tests cover center, all four rotations, front/back mirror, cropped corners, and out-of-crop clamping with matching source/preview aspects.

- [ ] **Step 5: Replace IR claims and failure states**

Rename all visible occurrences of `IR Camera Scan`/`camera detected` to `IR-like light scan`/`possible IR-like light`. The detail text must say that bright or blinking pixels can come from displays, LEDs, reflections, compression, or sensor noise and do not identify a camera.

```kotlin
sealed interface IrCameraUiState {
    data object BindingCamera : IrCameraUiState
    data class Live(val frame: IrPreviewFrame) : IrCameraUiState
    data class BindFailed(val message: String) : IrCameraUiState
}

data class IrPreviewFrame(
    val analysis: IrCameraDetector.FrameAnalysis,
    val metadata: AnalysisFrameMetadata,
    val previewWidthPx: Float,
    val previewHeightPx: Float,
)

interface IrCameraBinder {
    fun bind(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        onFrame: (IrPreviewFrame) -> Unit,
        onFailure: (Throwable) -> Unit,
    )
    fun unbind()
}
```

The Task 13 outer route owns permission request/recovery and passes only the granted path inward; this screen contains no launcher and `IrCameraUiState` contains no permission states. A route-scoped `IrCameraBinder` owns `LifecycleOwner`/`PreviewView`: the Composable calls `bind` from `DisposableEffect`/`ON_START`, calls `unbind` on `ON_STOP` and disposal, and sends only frame/failure events to `IrCameraScanViewModel`. The ViewModel never stores a `LifecycleOwner`, `PreviewView`, or untyped `previewOwner`. Retry increments a route binding generation and binds the current owner/view again once. Each `Live` frame carries analysis, crop/rotation/lens metadata, and actual preview dimensions needed by the transform. Do not display a live preview behind a failed-state surface.

- [ ] **Step 6: Add tool-screen Compose assertions**

```kotlin
data class MagneticFieldActions(
    val onResetBaseline: () -> Unit = {},
    val onRetry: () -> Unit = {},
    val onBack: () -> Unit = {},
)

data class IrActions(
    val onRetryBind: () -> Unit = {},
    val onOpenSettings: () -> Unit = {},
    val onBack: () -> Unit = {},
)

@Test fun magneticInitialStateDoesNotInventReading() {
    compose.setContent {
        MagneticFieldContent(MagneticFieldUiState.Initializing, MagneticFieldActions())
    }
    compose.onNodeWithText("Waiting for a reliable magnetometer sample").assertIsDisplayed()
    compose.onNodeWithText("0 µT", substring = true).assertDoesNotExist()
    compose.onNodeWithText("NORMAL", substring = true).assertDoesNotExist()
}

@Test fun irResultUsesCautiousLanguage() {
    compose.setContent { IrCameraContent(irStateWithBrightPoint(), IrActions()) }
    compose.onNodeWithText("Possible IR-like light", substring = true).assertIsDisplayed()
    compose.onNodeWithText("Camera detected", substring = true).assertDoesNotExist()
}

@Test fun bindFailureShowsRetryWithoutPreview() {
    compose.setContent { IrCameraContent(IrCameraUiState.BindFailed("Could not start camera"), IrActions()) }
    compose.onNodeWithText("Retry").assertHasClickAction()
    compose.onNodeWithTag("ir_preview").assertDoesNotExist()
}
```

`irStateWithBrightPoint()` is a complete fixture: construct a real `FrameAnalysis` containing one bright source at a known analyzer pixel, wrap it in `IrPreviewFrame` with a nonempty crop, rotation, lens-facing flag, and positive preview dimensions, then wrap that in `IrCameraUiState.Live`. The Compose test asserts the transformed overlay point remains inside `ir_preview`, not only the wording.

- [ ] **Step 7: Run tool tests and static wording scan**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*MagneticFieldSweepTest' --tests '*IrPreviewTransformTest'`

Expected: PASS.

Run: `rg -n 'EMF Sweep|IR Camera Scan|Camera detected|hidden electronics|0 µT.*NORMAL' android/app/src/main/java/com/friendorfoe/presentation/privacy android/app/src/main/java/com/friendorfoe/presentation/about`

Expected: no Android UI matches.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.privacy.AdvancedPrivacyToolsTest`

Expected: PASS for initializing/unavailable/live magnetic states, reset waiting, cautious IR wording, permission ownership, bind Retry, and no preview behind failure. Each test calls `setContent` once.

- [ ] **Step 8: Commit the Advanced tool corrections**

```bash
git add android/app/src/main/java/com/friendorfoe/detection/EmfDetector.kt android/app/src/main/java/com/friendorfoe/detection/IrCameraDetector.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/EmfSweepViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/EmfSweepScreen.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraScanViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraScanScreen.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/IrPreviewTransform.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/IrCameraBinder.kt android/app/src/test/java/com/friendorfoe/presentation/privacy android/app/src/androidTest/java/com/friendorfoe/presentation/privacy/AdvancedPrivacyToolsTest.kt
git commit -m "android: make advanced sensing tools truthful"
```

### Task 16: Clean List, Map, History, filters, and shared screen states

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/domain/model/FilterState.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/domain/usecase/FilterEngine.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/components/FofSurfaces.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/components/FofScreenStates.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/filter/FilterBar.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/filter/FilterAdvancedSection.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListSurfacePresentation.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/map/MapPresentation.kt`
- Create: `android/app/src/main/java/com/friendorfoe/data/repository/SkySourceHealth.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/history/HistoryViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/history/HistoryScreen.kt`
- Create: `android/app/src/test/java/com/friendorfoe/domain/usecase/FilterEngineTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/filter/FilterSummaryTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/list/ListUiStateTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/data/repository/SkySourceHealthTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/map/MapPresentationTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/CoreDestinationCleanupTest.kt`

**Interfaces:**
- Consumes: `SkyObjectRepository` data/status/errors, Room History flow, `ObjectPeek` from Task 6, and contextual location state from Task 13.
- Produces: one filter-count/reset contract, an explicit unknown-distance policy, shared loading/empty/no-match/stale/failure surfaces, result-first List, map-first Map with visible controls/accessibility alternative, and complete History states.

- [ ] **Step 1: Write failing filter and state-reducer tests**

```kotlin
class FilterSummaryTest {
    @Test fun activeCountCountsEachNonDefaultDimensionOnce() {
        val filter = FilterState(
            searchQuery = "drone",
            selectedCategories = setOf(ObjectCategory.DRONE),
            selectedSources = setOf(SourceFilterGroup.REMOTE_ID),
            maxDistanceNm = 5f,
            isAdvancedExpanded = true,
        )
        assertEquals(4, activeFilterCount(filter))
        assertEquals(0, activeFilterCount(FilterState()))
    }
}

class FilterEngineTest {
    @Test fun activeDistanceLimitExcludesObjectsWithUnknownDistance() {
        val unknown = drone(distanceMeters = null)
        val nearby = drone(distanceMeters = 1_000.0)
        assertEquals(
            listOf(nearby),
            FilterEngine.applyFilters(listOf(unknown, nearby), FilterState(maxDistanceNm = 1f)),
        )
    }
}

class ListUiStateTest {
    @Test fun emptyFeedAndNoFilterMatchesAreDifferent() {
        assertTrue(reduceListBody(raw = emptyList(), visible = emptyList(),
            resolved = true, failure = null) is ListBodyState.NoDetections)
        assertTrue(reduceListBody(raw = listOf(drone()), visible = emptyList(),
            resolved = true, failure = null) is ListBodyState.NoMatches)
    }

    @Test fun failureKeepsCachedRowsOnlyAsStale() {
        val state = reduceListBody(
            raw = listOf(drone()), visible = listOf(drone()), resolved = true,
            failure = "Source unavailable", cacheAgeMs = 20_000,
        )
        assertTrue(state is ListBodyState.StaleResults)
    }
}
```

- [ ] **Step 2: Run focused tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*FilterEngineTest' --tests '*FilterSummaryTest' --tests '*ListUiStateTest'`

Expected: the unknown-distance assertion fails and the presentation reducers are missing.

- [ ] **Step 3: Make filter count, reset, and unknown-distance behavior explicit**

```kotlin
fun activeFilterCount(filter: FilterState): Int = listOf(
    filter.searchQuery.isNotBlank(),
    filter.selectedCategories.isNotEmpty(),
    filter.selectedSources.isNotEmpty(),
    filter.objectTypeFilter != null,
    filter.maxDistanceNm != null,
    filter.minAltitudeFt != null,
    filter.maxAltitudeFt != null,
).count { it }

private fun distanceMatches(distanceMeters: Double?, maxDistanceNm: Float?): Boolean = when {
    maxDistanceNm == null -> true
    distanceMeters == null -> false
    else -> distanceMeters / 1852.0 <= maxDistanceNm
}
```

Use `distanceMatches` for live objects and History rows. When distance filtering is active, the Advanced sheet states `Rows without distance are excluded`. `Clear filters` assigns `FilterState()`; expanding/collapsing Advanced does not affect the count.

- [ ] **Step 4: Complete shared flat Material surfaces**

```kotlin
data class ListActions(
    val onQueryChanged: (String) -> Unit = {},
    val onOpenFilters: () -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onOpenPeek: (SkyObject) -> Unit = {},
)

data class MapActions(
    val onQueryChanged: (String) -> Unit = {},
    val onOpenFilters: () -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onOpenLegend: () -> Unit = {},
    val onRemoteSearch: () -> Unit = {},
    val onOpenAccessibleList: () -> Unit = {},
    val onRetryTiles: () -> Unit = {},
    val onRetryRemoteSearch: () -> Unit = {},
    val onOpenPeek: (MapTarget) -> Unit = {},
)

data class HistoryActions(
    val onQueryChanged: (String) -> Unit = {},
    val onOpenFilters: () -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onOpenRow: (Long) -> Unit = {},
    val onRequestDelete: (HistoryEntity) -> Unit = {},
    val onRequestClearAll: () -> Unit = {},
    val onRetry: () -> Unit = {},
)
```

Reuse `CollectionBodyState` created in Task 2; do not define a second hierarchy here.

Keep the Task 2 signatures for `FofScreenHeader`, `FofLoadingState`, `FofNoMatchesState`, and `FofFailureState`; add `FofStaleBanner`, `FofSectionStrip`, and a shared confirmation dialog beside them. All actions use `heightIn(min = 48.dp)`, status/severity includes text, and primary/evidence text wraps. Keep surfaces flat with 8–12dp shapes and 4/8/12/16dp spacing; do not add metric cards, gradients, or decorative art. Preserve `Theme.kt` token values exactly.

- [ ] **Step 5: Make List result-first with complete states**

```kotlin
sealed interface ListBodyState {
    data object Loading : ListBodyState
    data class Results(val rows: List<SkyObject>) : ListBodyState
    data class StaleResults(val rows: List<SkyObject>, val ageMs: Long, val message: String) : ListBodyState
    data object NoDetections : ListBodyState
    data class NoMatches(val activeFilterCount: Int) : ListBodyState
    data class Failed(val message: String) : ListBodyState
}

data class ListUiState(
    val filter: FilterState = FilterState(),
    val activeFilterCount: Int = 0,
    val body: ListBodyState = ListBodyState.Loading,
)

fun reduceListBody(
    raw: List<SkyObject>,
    visible: List<SkyObject>,
    resolved: Boolean,
    failure: String?,
    cacheAgeMs: Long? = null,
    activeFilterCount: Int = 0,
): ListBodyState = when {
    !resolved -> ListBodyState.Loading
    failure != null && visible.isNotEmpty() -> ListBodyState.StaleResults(
        visible, cacheAgeMs ?: 0L, failure,
    )
    failure != null -> ListBodyState.Failed(failure)
    raw.isEmpty() -> ListBodyState.NoDetections
    visible.isEmpty() -> ListBodyState.NoMatches(activeFilterCount)
    else -> ListBodyState.Results(visible)
}
```

Define the local source contract in `SkySourceHealth.kt`:

```kotlin
enum class SkySourceKind(val label: String) {
    ADS_B("ADS-B"), BLE_REMOTE_ID("Remote ID · Bluetooth"),
    WIFI_REMOTE_ID("Remote ID · Wi-Fi"), PHONE_DERIVED("Phone"),
}

data class SkySourceSnapshot(
    val source: SkySourceKind,
    val enabled: Boolean,
    val resolved: Boolean,
    val lastSuccessElapsedMs: Long?,
    val failure: String?,
    val rows: List<SkyObject>,
)
```

`SkyObjectRepository` exposes `sourceSnapshots: StateFlow<Map<SkySourceKind, SkySourceSnapshot>>`; each local producer updates only its own entry, and its rows retain the real `DetectionSource` used for per-row labels. The configured sensor backend remains owned by the Task 4 ViewModel poll jobs and is **not** polled again in `SkyObjectRepository`; `MapViewModel` merges its existing typed `MapBackendSnapshot` as a separate presentation source labeled `Configured backend`. `ListViewModel` combines the enabled local entries, filtered union, and their resolution/error evidence. First resolution occurs after each enabled source has emitted success/failure once, or immediately when every source is disabled; navigation alone does not mark resolved. Any successful nonempty or empty source response updates that source's monotonic `lastSuccessElapsedMs`; recomposition does not. A failed source cannot erase valid rows from another source; it produces stale/source-health copy until a later success. Add reducer tests for one-live/one-failed, all-disabled, empty-success, and Task 4's late-cancelled-backend result. List and Map user positions are `Position?` and start null—neither invents `(0, 0)`. List contains no privacy scanning/actions and no badge repository dependency.

`ListViewScreen` renders title, compact search/chips/filter count, then rows immediately. Tapping a row opens shared Object Peek; Full details performs navigation. Multiline rows show accurate source/category, optional distance/altitude/age, and a textual attention label. Tests define complete `drone(...)`, `aircraft(...)`, and state fixtures and assert visible `List` title plus exact `ADS-B`, `Remote ID`, `Phone`, and configured-backend source labels.

- [ ] **Step 6: Make Map map-first and honest about unavailable location**

Change `MapViewModel.userPosition` to `StateFlow<Position?>` with initial `null`; never use `(0, 0)` or another region as if it were the user's location. Define the complete presentation contract in `MapPresentation.kt`:

```kotlin
data class MapTarget(
    val id: String,
    val title: String,
    val sourceLabel: String,
    val latitude: Double,
    val longitude: Double,
    val skyObject: SkyObject?,
)

sealed interface MapSurfaceState {
    data object AwaitingLocation : MapSurfaceState
    data object Ready : MapSurfaceState
    data object LocationDenied : MapSurfaceState
    data class TileFailed(val message: String) : MapSurfaceState
    data class NetworkLimited(val cachedAgeMs: Long) : MapSurfaceState
}

sealed interface RemoteSearchState {
    data object Idle : RemoteSearchState
    data class Loading(val target: Position) : RemoteSearchState
    data class Results(val target: Position, val rows: List<MapTarget>) : RemoteSearchState
    data class Empty(val target: Position) : RemoteSearchState
    data class Failed(val target: Position, val message: String, val canRetry: Boolean = true) : RemoteSearchState
}

data class MapTileHealth(
    val lastSuccessElapsedMs: Long? = null,
    val consecutiveFailures: Int = 0,
    val cachedTileVisible: Boolean = false,
)

sealed interface MapTileEvent {
    data class Succeeded(val cachedTileVisible: Boolean) : MapTileEvent
    data class Failed(val cachedTileVisible: Boolean) : MapTileEvent
}

data class MapTileReduction(
    val health: MapTileHealth,
    val surface: MapSurfaceState,
)

data class MapUiState(
    val filter: FilterState = FilterState(),
    val activeFilterCount: Int = 0,
    val targets: List<MapTarget> = emptyList(),
    val userPosition: Position? = null,
    val mapCenter: Position? = null,
    val surface: MapSurfaceState = MapSurfaceState.AwaitingLocation,
    val remoteSearch: RemoteSearchState = RemoteSearchState.Idle,
)
```

Provide adapters from both `SkyObject` and `LocatedDroneDto` to `MapTarget`. Object Peek accepts `MapTarget`: local targets use the embedded `SkyObject`, while remote-only targets use an evidence-limited target summary and can open full detail only when a real detail key exists.

The map overlay contains one rounded search/filter HUD with result count, active-filter count, and Clear. Advanced filters open a modal bottom sheet. Add visible 48dp `Legend`, `Remote search`, and `Accessible target list` actions. Remove hidden long-press entry. Marker taps open Object Peek. The accessible list uses the same filtered targets and provides title, source, bearing/distance when known, and the same action path.

```kotlin
@Composable
@OptIn(ExperimentalLayoutApi::class)
private fun MapHud(state: MapUiState, actions: MapActions) {
    Column(Modifier.fillMaxWidth().padding(12.dp)) {
        CompactSearchField(state.filter.searchQuery, actions.onQueryChanged)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            AssistChip(onClick = actions.onOpenFilters,
                label = { Text("Filters ${state.activeFilterCount}") },
                modifier = Modifier.heightIn(min = 48.dp))
            AssistChip(onClick = actions.onOpenLegend, label = { Text("Legend") },
                modifier = Modifier.heightIn(min = 48.dp))
            AssistChip(onClick = actions.onRemoteSearch, label = { Text("Remote search") },
                modifier = Modifier.heightIn(min = 48.dp))
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            IconButton(onClick = actions.onOpenAccessibleList,
                modifier = Modifier.size(48.dp)) {
                Icon(Icons.AutoMirrored.Filled.List, "Accessible target list")
            }
        }
    }
}
```

`Remote search` opens a sheet with an explicit `Search visible map center` action and the current center coordinates; if no map center exists it is disabled with an explanation. Its state is `Idle`, `Loading(target)`, `Results(target, rows)`, `Empty(target)`, or `Failed(target, message, canRetry=true)`. It never silently uses the device location. Retry repeats the same captured target.

Wire osmdroid tile callbacks into `reduceMapTileHealth(previous, event: MapTileEvent, nowElapsedMs): MapTileReduction`. One failure with cached content preserves `Ready`. Three consecutive failures before any tile success yield `MapSurfaceState.TileFailed`; failures after a success yield `MapSurfaceState.NetworkLimited(cachedAgeMs = now - lastSuccess)`. Any tile success clears the failure count. Use the injected monotonic clock and unit-test each transition in `MapPresentationTest`. `AwaitingLocation` preserves a usable world map and explains why nearby distance/bearing is unavailable; `LocationDenied` adds the Task 13 recovery action.

- [ ] **Step 7: Complete History loading/empty/filter/error presentation**

Reuse the single `HistoryUiState` created in Task 5. This task only projects its `body` into all shared `CollectionBodyState` variants and wires retry; it does not redefine the state or deletion model.

Wrap `historyStore.observeAll()` in a retryable trigger rather than a one-shot catch:

```kotlin
private val historyReloads = MutableSharedFlow<Unit>(replay = 1).apply { tryEmit(Unit) }

val historyRows = historyReloads.flatMapLatest {
    historyStore.observeAll()
        .map<List<HistoryEntity>, Result<List<HistoryEntity>>> { Result.success(it) }
        .catch { emit(Result.failure(it)) }
}

fun retryHistory() { historyReloads.tryEmit(Unit) }
```

An empty database renders `No saved detections`; a nonempty database filtered to zero renders `No matches` and `Clear filters`; a Room failure renders `Retry`, which re-subscribes through `retryHistory`. Keep the exact-row route/delete/clear behavior from Task 5. Rows wrap, use accurate local source/category labels, and the screen includes: `History may include observation and phone coordinates. Records stay on this device until you delete them, clear History, clear app data, or uninstall.` Do not add Export.

- [ ] **Step 8: Add core-destination Compose coverage**

```kotlin
@Test fun listExposesResultsWithoutBadgePanel() {
    compose.setContent { ListContent(listResultsState(), ListActions()) }
    compose.onNodeWithTag("list_results").assertIsDisplayed()
    compose.onNodeWithText("List").assertIsDisplayed()
    compose.onNodeWithText("Badge status", substring = true).assertDoesNotExist()
}

@Test fun mapExposesVisibleToolsAndAccessibleAlternative() {
    compose.setContent { MapContent(mapReadyState(), MapActions()) }
    compose.onNodeWithText("Map").assertIsDisplayed()
    compose.onNodeWithText("Legend").assertHasClickAction()
    compose.onNodeWithText("Remote search").assertHasClickAction()
    compose.onNodeWithContentDescription("Accessible target list").assertHasClickAction()
}

@Test fun historyEmptyHasNoExport() {
    compose.setContent { HistoryContent(historyEmptyState(), HistoryActions()) }
    compose.onNodeWithText("History").assertIsDisplayed()
    compose.onNodeWithText("No saved detections").assertIsDisplayed()
    compose.onNodeWithText("Export", substring = true).assertDoesNotExist()
}
```

Additional separate tests cover Map HUD at 360dp/1.3x without clipped actions, remote-search Loading/Empty/Failed/Retry, tile failure versus cached network-limited state, nullable user positions, List first resolution and mixed-source failure, History Retry resubscription, exact source/category labels, and Clear filters. Each test calls `setContent` once.

- [ ] **Step 9: Run filter, state, and screen tests**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*FilterEngineTest' --tests '*FilterSummaryTest' --tests '*ListUiStateTest' --tests '*SkySourceHealthTest' --tests '*MapPresentationTest' --tests '*History*Test'`

Expected: PASS.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.CoreDestinationCleanupTest`

Expected: PASS with List/Map/History state distinctions and route ownership.

- [ ] **Step 10: Commit the core destination cleanup**

```bash
git add android/app/src/main/java/com/friendorfoe/domain android/app/src/main/java/com/friendorfoe/data/repository/SkyObjectRepository.kt android/app/src/main/java/com/friendorfoe/data/repository/SkySourceHealth.kt android/app/src/main/java/com/friendorfoe/presentation/components android/app/src/main/java/com/friendorfoe/presentation/filter android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/map android/app/src/main/java/com/friendorfoe/presentation/history android/app/src/test/java/com/friendorfoe android/app/src/androidTest/java/com/friendorfoe/presentation/CoreDestinationCleanupTest.kt
git commit -m "android: clean list map and history states"
```

### Task 17: Finish AR, Detail, Reference, responsive layout, and semantics

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/RouteCodec.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArPresentation.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/CompassOverlay.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/detail/DetailPresentation.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/reference/ReferenceGuideScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/aircraft/AircraftReferenceScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/drones/DroneReferenceScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/theme/Dimens.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/components/AccessibilitySemantics.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/detail/DetailPresentationTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/reference/ReferenceRouteTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/CertifiedLayoutMatrixTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/ComposeAssertions.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/CertifiedRouteFixtures.kt`

**Interfaces:**
- Consumes: explicit Object Peek/Capture Review from Task 6, live and historical detail states from Task 5, and shared screen surfaces from Task 16.
- Produces: responsive AR chrome/camera recovery/target semantics, summary-first live/historical Detail, one encoded/saveable Reference route, 48dp semantic targets, and certified font/width layout tests.

- [ ] **Step 1: Write failing detail and route tests**

```kotlin
class DetailPresentationTest {
    @Test fun historicalSnapshotIsLabeledAndNeverLive() {
        val model = presentHistoricalDetail(history(id = 11, objectId = "abc"))
        assertEquals("Historical detection", model.statusLabel)
        assertFalse(model.isLive)
        assertTrue(model.identifiers.any { it.value == "abc" && it.copyable })
        assertFalse(model.rawExpandedByDefault)
    }

    @Test fun partialLiveDetailKeepsSummaryAndRetry() {
        val model = presentLiveDetail(aircraft(), remoteDetail = null,
            remoteFailure = "Aircraft details unavailable")
        assertTrue(model.summary.isNotEmpty())
        assertTrue(model.summary.any { it.first == "Source" })
        assertEquals("Retry details", model.retryLabel)
    }

    @Test fun droneDetailKeepsLocalIdentityAndSource() {
        val model = presentLiveDroneDetail(drone(droneId = "rid-7"))
        assertEquals("Live detection", model.statusLabel)
        assertTrue(model.identifiers.any { it.value == "rid-7" })
        assertTrue(model.summary.any { it.first == "Source" })
    }
}

class ReferenceRouteTest {
    @Test fun routeEncodesTabAndQueryExactlyOnce() {
        assertEquals(
            "reference_guide?tab=drones&query=DJI%20Mini%2F4",
            referenceGuideRoute(ReferenceTab.DRONES, "DJI Mini/4"),
        )
    }
}
```

- [ ] **Step 2: Run focused tests and confirm red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*DetailPresentationTest' --tests '*ReferenceRouteTest'`

Expected: compilation fails because presentation models and the one-route builder are missing.

- [ ] **Step 3: Consolidate AR chrome and camera failure state**

```kotlin
sealed interface ArCameraState {
    data object AwaitingPermission : ArCameraState
    data object Starting : ArCameraState
    data object Live : ArCameraState
    data class Failed(val message: String, val canOpenSettings: Boolean) : ArCameraState
}

data class ArChromeMetrics(
    val edgePadding: Dp,
    val topStatusMinHeight: Dp,
    val targetTouchSize: Dp,
    val bottomClearance: Dp,
)

fun arChromeMetrics(width: Dp, fontScale: Float) = ArChromeMetrics(
    edgePadding = if (width < 400.dp) 8.dp else 12.dp,
    topStatusMinHeight = 48.dp,
    targetTouchSize = 48.dp,
    bottomClearance = if (fontScale > 1.2f) 80.dp else 64.dp,
)
```

Use one wrapping top status surface for camera/location/compass/source text instead of overlapping independent labels. Apply `heightIn(min = topStatusMinHeight)`—never a fixed height—so 1.3x/2.0x copy can grow. Place controls with `WindowInsets.safeDrawing`, `BoxWithConstraints`, and these dp metrics. Keep target projection calculations in pixels but size interactive/label surfaces from dp/density. Camera initialization exceptions publish `ArCameraState.Failed`; `retryCamera()` cancels the failed bind and performs one new bind, while `openCameraSettings()` is exposed only for permanent permission denial. Render both actions inline instead of logging only.

- [ ] **Step 4: Give every AR target an accessible action**

```kotlin
@Composable
private fun AccessibleArTarget(
    target: ArTargetPresentation,
    onOpenPeek: () -> Unit,
) {
    Box(
        Modifier
            .offset { IntOffset(target.screenX.roundToInt(), target.screenY.roundToInt()) }
            .sizeIn(minWidth = 48.dp, minHeight = 48.dp)
            .clickable(
                onClickLabel = "Open object preview",
                role = Role.Button,
                onClick = onOpenPeek,
            )
            .semantics(mergeDescendants = true) {
                contentDescription = buildString {
                    append(target.title)
                    target.categoryLabel?.let { append(", $it") }
                    target.distanceLabel?.let { append(", $it") }
                    append(", Open object preview")
                }
                isInteractiveTarget = true
            },
    ) { ArTargetLabel(target) }
}
```

The physical `.clickable` owns the hit target. `ArTargetLabel` is visual-only and has no second click/role semantics, preventing duplicate actions and TalkBack announcements.

Opening Object Peek, Inspect/Zoom, Full details, Share, or Discard follows Task 6's zero-write contract. `Capture` creates one in-memory draft; only Review `Save` writes.

- [ ] **Step 5: Build a summary-first Detail model and responsive screen**

```kotlin
data class DetailIdentifier(val label: String, val value: String, val copyable: Boolean)

data class DetailPresentation(
    val title: String,
    val statusLabel: String,
    val isLive: Boolean,
    val summary: List<Pair<String, String>>,
    val identifiers: List<DetailIdentifier>,
    val advanced: List<Pair<String, String>>,
    val raw: List<Pair<String, String>>,
    val retryLabel: String?,
    val rawExpandedByDefault: Boolean = false,
)

fun presentHistoricalDetail(row: HistoryEntity): DetailPresentation = DetailPresentation(
    title = row.displayName,
    statusLabel = "Historical detection",
    isLive = false,
    summary = listOf(
        "Source" to row.detectionSource,
        "Category" to row.category,
        "Observed" to formatHistoryInstant(row.lastSeen),
    ),
    identifiers = listOf(DetailIdentifier("Object ID", row.objectId, copyable = true)),
    advanced = listOfNotNull(
        row.distanceMeters?.let { "Distance at observation" to formatDistance(it) },
        "Position at observation" to formatPosition(row.latitude, row.longitude),
    ),
    raw = historyRawFields(row),
    retryLabel = null,
)

fun presentLiveDetail(
    aircraft: Aircraft,
    remoteDetail: AircraftDetailDto?,
    remoteFailure: String?,
): DetailPresentation = DetailPresentation(
    title = aircraft.callsign ?: aircraft.registration ?: "Aircraft",
    statusLabel = "Live detection",
    isLive = true,
    summary = liveAircraftSummary(aircraft, remoteDetail),
    identifiers = liveAircraftIdentifiers(aircraft, remoteDetail),
    advanced = liveAircraftAdvanced(aircraft, remoteDetail),
    raw = liveAircraftRaw(aircraft, remoteDetail),
    retryLabel = remoteFailure?.let { "Retry details" },
)

fun presentLiveDroneDetail(drone: Drone): DetailPresentation = DetailPresentation(
    title = drone.displayLabel(),
    statusLabel = "Live detection",
    isLive = true,
    summary = liveDroneSummary(drone),
    identifiers = liveDroneIdentifiers(drone),
    advanced = liveDroneAdvanced(drone),
    raw = liveDroneRaw(drone),
    retryLabel = null,
)
```

Implement the named aircraft and drone formatting/list helpers as pure functions in `DetailPresentation.kt`; every nullable remote field is omitted rather than invented. `DetailState.AircraftLoaded` uses `presentLiveDetail`, `DetailState.DroneLoaded` uses `presentLiveDroneDetail`, and `DetailState.HistoricalLoaded` uses `presentHistoricalDetail`. `DetailViewModel.retryRemoteDetail()` repeats only the enrichment request for the already loaded live aircraft and preserves local summary while pending/failing. Historical and drone presentation never invoke it.

Render identity/category/source/freshness and important position facts first; show visible `Live detection` or `Historical detection`; retain local summary on partial remote-detail failure and place `Retry details` inline. Identifiers use `SelectionContainer` plus a labeled Copy action backed by `ClipboardManager.setPrimaryClip`, followed by a `Copied <label>` polite live-region confirmation. Advanced and Raw sections start collapsed with `rememberSaveable(historyId/objectId)` so rotation/process recreation restores the correct item's expansion state. Replace every fixed 40/60 split with weighted/wrapping `FlowRow` or stacked label/value content.

- [ ] **Step 6: Consolidate Reference into one scaffold and route**

```kotlin
enum class ReferenceTab(val wireValue: String) { AIRCRAFT("aircraft"), DRONES("drones") }

fun referenceGuideRoute(
    tab: ReferenceTab = ReferenceTab.AIRCRAFT,
    query: String = "",
): String = "reference_guide?tab=${encodeRouteSegment(tab.wireValue)}" +
    "&query=${encodeRouteSegment(query)}"

@Composable
fun ReferenceGuideScreen(
    state: ReferenceUiState,
    onTabChanged: (ReferenceTab) -> Unit,
    onQueryChanged: (String) -> Unit,
    onBack: () -> Unit,
) {
    ReferenceScaffold(
        tab = state.tab,
        query = state.query,
        onTabChanged = onTabChanged,
        onQueryChanged = onQueryChanged,
        onBack = onBack,
    )
}
```

`RouteCodec.kt` implements `encodeRouteSegment`/strict decode with `java.net.URLEncoder`/`URLDecoder` and `%20`, so the JVM builder test does not invoke `android.net.Uri`. Navigation Compose supplies already-decoded argument values; the destination consumes each argument directly and never calls `URLDecoder` a second time. The destination declares defaults `tab=aircraft` and `query=""`, rejects unknown tabs to Aircraft, and seeds a `ReferenceViewModel(SavedStateHandle)` once. `SavedStateHandle` is the sole tab/query owner; the screen contains no parallel `rememberSaveable` state.

Aircraft and Drone catalog functions provide data/content to this scaffold instead of owning separate navigation or app bars. Search with zero results renders `No reference matches`. Informational tags are static labels without selected/click semantics. Claims such as surveillance, military, law-enforcement, or privacy risk show a bundled-data provenance label when present; otherwise use cautious capability language. Add a real test `NavHost` round-trip for query values containing `%`, `%2F`, `+`, spaces, and `/`, and assert the ViewModel receives the original string exactly once before and after Activity recreation. Also test default arguments, malformed input, zero results, static tag semantics, cautious claims, Back, and `SavedStateHandle` restoration.

- [ ] **Step 7: Add reusable 48dp and overflow assertions**

Put the semantics key in production `AccessibilitySemantics.kt` so AR targets and other real controls can mark themselves; keep only assertion helpers in the Android-test source set:

```kotlin
val InteractiveTargetKey = SemanticsPropertyKey<Boolean>("InteractiveTarget")
var SemanticsPropertyReceiver.isInteractiveTarget by InteractiveTargetKey

// ComposeAssertions.kt below this line

fun SemanticsNodeInteraction.assertTouchTargetAtLeast(
    density: Density,
    minimum: Dp = 48.dp,
): SemanticsNodeInteraction = apply {
    val bounds = fetchSemanticsNode().boundsInRoot
    with(density) {
        check(bounds.width >= minimum.toPx() && bounds.height >= minimum.toPx()) {
            "Touch target was ${bounds.width}x${bounds.height}px; expected at least $minimum"
        }
    }
}

fun SemanticsNodeInteraction.assertInside(root: Rect): SemanticsNodeInteraction = apply {
    val bounds = fetchSemanticsNode().boundsInRoot
    check(bounds.left >= root.left && bounds.top >= root.top &&
        bounds.right <= root.right && bounds.bottom <= root.bottom) {
        "Interactive bounds $bounds escaped root $root"
    }
}

fun SemanticsNode.assertTouchTargetAtLeast(density: Density, minimum: Dp = 48.dp) {
    with(density) {
        check(boundsInRoot.width >= minimum.toPx() && boundsInRoot.height >= minimum.toPx())
    }
}

fun SemanticsNode.assertInside(root: Rect) {
    check(boundsInRoot.left >= root.left && boundsInRoot.top >= root.top &&
        boundsInRoot.right <= root.right && boundsInRoot.bottom <= root.bottom)
}
```

Give the seven nav actions, row actions, filter chips, Object Peek actions, Badge swatches/apply/recovery, confirmation buttons, and Advanced entries their specific `testTag` plus a custom Boolean `InteractiveTargetKey` semantics property. Production `FofNavigationSuite` also applies `navigation_rail`, `navigation_bar`, and a shared `nav_destination` tag (plus each destination's unique content description). The matrix queries `SemanticsMatcher.expectValue(InteractiveTargetKey, true)`, asserts the returned node set is nonempty for every fixture that declares interactions, and invokes `assertTouchTargetAtLeast`/`assertInside` for every returned node. Avoid duplicate TalkBack announcements by setting one merged description only when child text is intentionally hidden from accessibility.

- [ ] **Step 8: Implement the certified width/font matrix test**

```kotlin
data class CertifiedLayoutCase(
    val width: Dp,
    val height: Dp,
    val fontScale: Float,
    val routeFixture: CertifiedRouteFixture,
)

@OptIn(ExperimentalTestApi::class)
@Test fun certifiedRoutesFitOneRecomposableMatrixHost() {
    lateinit var active: MutableState<CertifiedLayoutCase>
    lateinit var testDensity: Density
    val cases = certifiedRouteFixtures().flatMap { route ->
        listOf(
            CertifiedLayoutCase(360.dp, 800.dp, 1f, route),
            CertifiedLayoutCase(360.dp, 800.dp, 1.3f, route),
            CertifiedLayoutCase(412.dp, 800.dp, 1f, route),
            CertifiedLayoutCase(412.dp, 800.dp, 1.3f, route),
        )
    } + listOf(
        CertifiedLayoutCase(360.dp, 800.dp, 2f, CertifiedRouteFixture.Shell),
        CertifiedLayoutCase(599.dp, 360.dp, 1.3f, CertifiedRouteFixture.Shell),
        CertifiedLayoutCase(600.dp, 360.dp, 1.3f, CertifiedRouteFixture.Shell),
        CertifiedLayoutCase(600.dp, 800.dp, 1.3f, CertifiedRouteFixture.Shell),
    )

    compose.setContent {
        testDensity = LocalDensity.current
        active = remember { mutableStateOf(cases.first()) }
        val value = active.value
        DeviceConfigurationOverride(DeviceConfigurationOverride.FontScale(value.fontScale)) {
            DeviceConfigurationOverride(
                DeviceConfigurationOverride.ForcedSize(DpSize(value.width, value.height))
            ) {
                Box(Modifier.fillMaxSize().testTag("matrix_root")) {
                    CertifiedRoutesFixture(value.routeFixture)
                }
            }
        }
    }

    cases.forEach { case ->
        compose.runOnUiThread { active.value = case }
        compose.waitForIdle()
        compose.onAllNodesWithTag("unreachable_action").assertCountEquals(0)
        val root = compose.onNodeWithTag("matrix_root").fetchSemanticsNode().boundsInRoot
        val interactive = compose.onAllNodes(
            SemanticsMatcher.expectValue(InteractiveTargetKey, true),
            useUnmergedTree = true,
        ).fetchSemanticsNodes()
        if (case.routeFixture.expectedInteractiveCount > 0) {
            check(interactive.isNotEmpty()) { "Fixture exposed no interactive semantics" }
        }
        interactive.forEach { node ->
                node.assertTouchTargetAtLeast(testDensity)
                node.assertInside(root)
            }
        if (case.routeFixture == CertifiedRouteFixture.Shell) {
            compose.onAllNodesWithTag("nav_destination").assertCountEquals(7)
            val landscapeRail = case.width >= 600.dp && case.width > case.height
            compose.onNodeWithTag("navigation_rail").apply {
                if (landscapeRail) assertIsDisplayed() else assertDoesNotExist()
            }
            compose.onNodeWithTag("navigation_bar").apply {
                if (landscapeRail) assertDoesNotExist() else assertIsDisplayed()
            }
        }
    }
}
```

`CertifiedRouteFixtures.kt` defines the complete enum/sealed fixtures named above, the expected root tag, expected action tags, and nonzero expected interactive count for each interactive route: seven top-level routes, Object Peek, Capture Review, Map filters, History detail, Reference, Ignored devices, Badge diagnostics/recovery, Calibration unavailable, Magnetic field, IR, permission recovery, and long confirmation. Every fixture uses complete production state factories. At 600x360 landscape the test asserts rail; at 599x360 and 600x800 portrait it asserts bottom bar. The forced-size override, not a parent-clamped `Modifier.width`, exercises the actual 599/600 breakpoint. The single host avoids Compose's one-`setContent` limitation. Add `@OptIn(ExperimentalLayoutApi::class)` to production/test call sites that use `FlowRow`.

- [ ] **Step 9: Run Detail/Reference/AR and layout suites**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*Detail*Test' --tests '*Reference*Test' --tests '*CaptureReviewViewModelTest'`

Expected: PASS.

Run with emulator: `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.CertifiedLayoutMatrixTest`

Expected: PASS across 360dp/412dp at 1.0x/1.3x, shell at 2.0x, and 599dp/600dp navigation breakpoints.

- [ ] **Step 10: Commit the secondary-flow and accessibility pass**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/ar android/app/src/main/java/com/friendorfoe/presentation/detail android/app/src/main/java/com/friendorfoe/presentation/reference android/app/src/main/java/com/friendorfoe/presentation/aircraft android/app/src/main/java/com/friendorfoe/presentation/drones android/app/src/main/java/com/friendorfoe/presentation/navigation android/app/src/main/java/com/friendorfoe/presentation/theme/Dimens.kt android/app/src/test/java/com/friendorfoe/presentation android/app/src/androidTest/java/com/friendorfoe/presentation
git commit -m "android: finish responsive secondary flows"
```

### Task 18: Prove the Android-only release on JVM, emulator, and supported badge transports

**Files:**
- Create: `android/app/src/test/java/com/friendorfoe/AndroidOverhaulAcceptanceTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/ClearAndroidStateRule.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/AndroidOverhaulJourneyTest.kt`
- Create: `docs/testing/android-interface-overhaul-qa.md`
- Create: `docs/testing/evidence/android-interface-overhaul/README.md`
- Modify only after complete physical proof: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeReleaseCertification.kt`

**Interfaces:**
- Consumes: all production/test work from Tasks 1–17, two API 35 emulator profiles, and an optional user-provided phone + physical badge.
- Produces: full unit/build/connected-test evidence, route/permission/layout walkthrough records, an Android-only scope proof, and a capability matrix in which untested hardware mutations remain disabled/unverified.

- [ ] **Step 1: Add one acceptance test for cross-feature invariants**

```kotlin
class AndroidOverhaulAcceptanceTest {
    @Test fun topLevelOrderAndFirmwareDefaultsRemainLocked() {
        assertEquals(
            listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info"),
            TopLevelDestination.entries.map { it.label },
        )
        assertEquals(0xC3AA2A8DL, BadgeTheme.firmwareDefaults().firmwareHash())
        assertEquals(0x0DAD6299L, BadgeDisplayPolicy.firmwareDefaults().firmwareHash())
    }

    @Test fun appleActivityCannotReachCriticalAlertPolicy() {
        val normalized = PrivacyFindingNormalizer.normalize(appleListeningFinding())
        val state = PrivacyCurrentReducer().reduce(
            sources = listOf(liveSnapshot(normalized)),
            ignoredKeys = emptySet(),
            nowElapsedMs = 1_000L,
        )
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals(FindingSeverity.INFO, normalized.severity)
        assertEquals(0, state.threatCount)
        assertTrue(state.alertEligible.isEmpty())
    }

    @Test fun bleAndUncertifiedUsbCannotMutateAndUploadHasNoContract() {
        val ble = liveBleEvidence()
        assertEquals(BadgeCapabilitySupport.SUPPORTED,
            badgeCapability(ble, BadgeCapability.READ_STATUS))
        assertEquals(BadgeCapabilitySupport.UNSUPPORTED,
            badgeCapability(ble, BadgeCapability.NETWORK_MODE))
        assertEquals(BadgeCapabilitySupport.UNSUPPORTED,
            badgeCapability(ble, BadgeCapability.THEME_V1))
        assertEquals(BadgeCapabilitySupport.UNSUPPORTED,
            badgeCapability(ble, BadgeCapability.DISPLAY_POLICY_V1))
        assertEquals(BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(ble, BadgeCapability.DISPLAY_NAV, payloadBytes = 37))

        val usb = liveUncertifiedUsbEvidence()
        listOf(
            BadgeCapability.DISPLAY_NAV,
            BadgeCapability.NETWORK_MODE,
            BadgeCapability.THEME_V1,
            BadgeCapability.DISPLAY_POLICY_V1,
            BadgeCapability.REBOOT,
            BadgeCapability.BOOTLOADER,
        ).forEach { capability ->
            val payloadBytes = if (capability == BadgeCapability.DISPLAY_NAV) 37 else null
            assertEquals(BadgeCapabilitySupport.UNKNOWN,
                badgeCapability(usb, capability, payloadBytes))
        }
        assertFalse(BadgeCapability.entries.any {
            it.name.contains("UPLOAD") || it.name.contains("FIRMWARE")
        })
    }

    @Test fun checkedInCertificationExactlyMatchesTheQaApprovedSet() {
        val qaApproved = BadgeReleaseCertification() // update only with a completed QA evidence row
        assertEquals(qaApproved, CheckedInBadgeReleaseCertification)
    }

    private fun appleListeningFinding() = PrivacyFinding(
        displayId = "apple-row",
        observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "row-7"),
        source = PrivacySourceKind.BACKEND,
        stableSourceId = "device-7",
        routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "row-7"),
        title = "Possible Remote Listening",
        evidence = "Apple activity flag",
        limitation = null,
        category = PrivacyCategory.REMOTE_LISTENING,
        severity = FindingSeverity.CRITICAL,
        ownership = Ownership.UNKNOWN,
        signalDbm = -45,
        firstSeenWallMs = 10L,
        lastSeenWallMs = 20L,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = 20_000L,
        hasLiveLocalSamples = false,
        appleEvidence = AppleListeningEvidence(
            appleFamilyEvidence = true,
            airPodsAssociationEvidence = true,
            listeningOrientedCategoryOrWording = true,
        ),
    )

    private fun liveSnapshot(finding: PrivacyFinding) = PrivacySourceSnapshot(
        health = PrivacySourceHealth(
            source = finding.source,
            state = SourceHealthState.LIVE,
            lastSuccessElapsedMs = 1_000L,
            lastSuccessWallMs = 20L,
            recoveryLabel = null,
            message = null,
        ),
        findings = listOf(finding),
        emittedAtElapsedMs = 1_000L,
    )

    private fun liveBleEvidence() = BadgeConnectionEvidence(
        transport = BadgeTransport.BLE_GATT,
        phase = BadgeConnectionPhase.LIVE,
        lastValidStatusAtElapsedMs = 1_000L,
        protocolVersion = "1",
        targetId = "badge-ble-1",
        usbCandidateCount = null,
        exactEspressifVendorMatch = false,
        serialInterfaceReadable = false,
        badgeApEndpoint = null,
        negotiatedBleMtu = 64,
        fofBleServicePresent = true,
        bleStatusCharacteristicPresent = true,
        bleControlCharacteristicPresent = true,
        bleBonded = true,
        bleEncrypted = true,
        debugBridgeSerialPort = null,
        debugPhysicalStatusAtElapsedMs = null,
        debugBridgeLastError = null,
        releaseCertifiedMutations = emptySet(),
    )

    private fun liveUncertifiedUsbEvidence() = BadgeConnectionEvidence(
        transport = BadgeTransport.USB_SERIAL,
        phase = BadgeConnectionPhase.LIVE,
        lastValidStatusAtElapsedMs = 1_000L,
        protocolVersion = "1",
        targetId = "badge-usb-1",
        usbCandidateCount = 1,
        exactEspressifVendorMatch = true,
        serialInterfaceReadable = true,
        badgeApEndpoint = null,
        negotiatedBleMtu = null,
        fofBleServicePresent = false,
        bleStatusCharacteristicPresent = false,
        bleControlCharacteristicPresent = false,
        bleBonded = false,
        bleEncrypted = false,
        debugBridgeSerialPort = null,
        debugPhysicalStatusAtElapsedMs = null,
        debugBridgeLastError = null,
        releaseCertifiedMutations = emptySet(),
    )
}
```

The static scan in Step 9 separately proves that no firmware upload command, capability, picker, or endpoint remains; the JVM test intentionally does not invent a `canUploadFirmware` API. Add three acceptance cases that feed raw local `GlassesDetection`, backend DTO, and same-entity Apple-marked `BadgeThreatEntity` through their real adapter mappers before normalization; each must become INFO/non-alerting. A badge listening label without same-entity Apple evidence must remain unrelated rather than being rewritten.

- [ ] **Step 2: Add a pre-Activity state reset and one deterministic end-to-end journey**

`ClearAndroidStateRule` clears through live storage APIs instead of deleting open files. It runs before `ActivityScenarioRule`, and this class has one test so DataStore and Room are reset exactly once:

```kotlin
class ClearAndroidStateRule : TestRule {
    override fun apply(base: Statement, description: Description) = object : Statement() {
        override fun evaluate() {
            val context = InstrumentationRegistry.getInstrumentation().targetContext
            runBlocking {
                AppPreferencesRepository(context).resetForInstrumentation()
                val database = FriendOrFoeDatabase.create(context)
                try {
                    database.clearAllTables()
                } finally {
                    database.close()
                }
            }
            check(context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)
                .edit().clear().commit())
            base.evaluate()
        }
    }
}
```

```kotlin
@RunWith(AndroidJUnit4::class)
class AndroidOverhaulJourneyTest {
    private val clearState = ClearAndroidStateRule()
    private val app = createAndroidComposeRule<MainActivity>()

    @get:Rule
    val rules: TestRule = RuleChain.outerRule(clearState).around(app)

    @Test fun onboardingSevenRoutesAndSecondaryOwnershipWork() {
        app.onNodeWithText("Continue").assertIsDisplayed().performClick()
        listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info")
            .forEach { label ->
                app.onNodeWithContentDescription(label).performClick()
                app.onNodeWithTag("screen_${label.lowercase()}").assertIsDisplayed()
            }
        app.onNodeWithText("Advanced").performScrollTo().assertIsDisplayed()
        app.onNodeWithText("Magnetic-field sweep").assertHasClickAction().performClick()
        app.onNodeWithTag("screen_magnetic_field").assertIsDisplayed()
        app.onAllNodesWithTag("nav_destination").assertCountEquals(0)
        app.onNodeWithContentDescription("Back").performClick()
        app.onNodeWithTag("screen_info").assertIsDisplayed()
        app.onAllNodesWithTag("nav_destination").assertCountEquals(7)
        app.onNodeWithText("IR-like light scan").performScrollTo().assertHasClickAction()
        app.onNodeWithTag("calibration_entry").performScrollTo().assertIsNotEnabled()

        app.onNodeWithContentDescription("Badge").performClick()
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        runBlocking {
            AppPreferencesRepository(context).launchState
                .filterIsInstance<AppLaunchState.Ready>()
                .first { it.startRoute == Screen.Badge.route }
        }
        app.activityRule.scenario.recreate()
        app.onNodeWithTag("screen_badge").assertIsDisplayed()
        app.onNodeWithText("Continue").assertDoesNotExist()
    }
}
```

The rule proves onboarding from clean state and last-route restoration after Activity recreation without relying on test order. Task 1's repository test proves values survive constructing a new repository over the same DataStore; the device walkthrough in Step 8 additionally force-stops and relaunches the real APK to cover process death.

- [ ] **Step 3: Create the QA record before recording results**

Create `docs/testing/android-interface-overhaul-qa.md` with these sections and tables: automated command results (`command`, `UTC time`, `commit`, `result`, `report path`); seven-route walkthrough (`profile`, `font`, `orientation`, `theme`, `route/state`, `result`, `evidence`); permissions; accessibility/large font; process restoration; and physical capability evidence (`transport`, `READ_STATUS`, `DISPLAY_NAV`, `THEME_V1`, `DISPLAY_POLICY_V1`, `NETWORK_MODE`, `REBOOT`, `BOOTLOADER`, `restored`, `evidence`). Initialize unexecuted cells to `Not run — unavailable controls remain disabled`, never to `Pass`.

Create `docs/testing/evidence/android-interface-overhaul/README.md` with the filename convention above, the exact command used for every generated artifact, the tested app commit, emulator/device build fingerprint, and a note that evidence files contain no credentials, Wi-Fi secrets, precise saved locations, or unrelated device identifiers.

- [ ] **Step 4: Run the complete JVM suite**

Run: `cd android && ./gradlew testDebugUnitTest`

Expected: `BUILD SUCCESSFUL`, including hash, parser, freshness, normalization, History, capture, permission, version, filter, Detail, and route suites.

- [ ] **Step 5: Run Android lint and build the debug APK**

Run: `cd android && ./gradlew lintDebug`

Expected: no new fatal lint errors; any baseline warning is recorded with file/rule and disposition in the QA document.

Run: `cd android && ./gradlew assembleDebug`

Expected: `BUILD SUCCESSFUL`; APK at `android/app/build/outputs/apk/debug/app-debug.apk`.

- [ ] **Step 6: Run all connected tests on API 35**

Run `adb devices -l`, select one exact target, and assign its printed serial to the task-specific shell variable `FOF_ANDROID_SERIAL`. Shut down/disconnect other Android targets for this connected-test run, then run: `cd android && ANDROID_SERIAL="$FOF_ANDROID_SERIAL" ./gradlew connectedDebugAndroidTest`

Expected: `BUILD SUCCESSFUL`. If a test is emulator-hardware limited, make the state injectable and test the denied/unavailable fixture; do not delete the assertion.

- [ ] **Step 7: Perform the two-width/font/rotation walkthrough**

Use the `test-android-apps:android-emulator-qa` workflow during execution. On API 35 profiles at 412dp and 360dp:

- walk all seven top-level routes at 1.0x and 1.3x;
- verify seven 48dp nav targets and icon-only accessibility at 2.0x;
- rotate below 600dp and at/above 600dp to verify bottom bar versus rail;
- cover loading, empty, no matches, stale, failed, denied, permanently denied, unsupported, confirmation, and expired-item fixtures;
- cover Object Peek, Capture Review, Map filters/list/legend/remote search, exact History detail, Reference zero results, Ignored devices, Badge diagnostics/recovery, Calibration unavailable, Magnetic field, and IR;
- check both system light and dark theme;
- capture screenshots and UI trees for AR, Map, List, Privacy, Badge, History, Info, and the long-dialog/large-font cases;
- inspect logcat after the journey for crashes, ANRs, leaked sensor/camera callbacks, and repeated polling/transport errors.

Record profile, font scale, orientation, route/state, result, screenshot filename, and defect/fix in `docs/testing/android-interface-overhaul-qa.md`. Store screenshots, UI XML, and logcat under `docs/testing/evidence/android-interface-overhaul/`; its README maps every file to profile, font scale, orientation, route/state, command, and result. Use deterministic names such as `pixel8-api35-360dp-1.3x-dark-privacy.png` and `pixel8-api35-600dp-landscape-2.0x-shell.xml`.

- [ ] **Step 8: Install and launch the built APK**

Run `adb devices -l`, set `FOF_ANDROID_SERIAL` to the exact intended target, and use it for every command below.

Run: `adb -s "$FOF_ANDROID_SERIAL" install -r android/app/build/outputs/apk/debug/app-debug.apk`

Run: `adb -s "$FOF_ANDROID_SERIAL" shell am force-stop com.friendorfoe`

Run: `adb -s "$FOF_ANDROID_SERIAL" shell monkey -p com.friendorfoe -c android.intent.category.LAUNCHER 1`

Expected: install succeeds, launch reaches Welcome or the persisted valid top-level route, and logcat has no startup crash.

Select Badge, wait until the route is persisted, then repeat force-stop/launch. Expected: Badge returns as the selected top-level route and onboarding does not replay.

- [ ] **Step 9: Verify Android-only scope and removed mutation paths**

Run: `git diff --name-only b6b1617...HEAD -- backend esp32`

Expected: no output.

Run: `git diff --name-only -- backend esp32`

Expected: no output from unstaged changes.

Run: `git diff --cached --name-only -- backend esp32`

Expected: no output from staged changes.

Run: `git ls-files --others --exclude-standard -- backend esp32`

Expected: no untracked backend or firmware files.

Run: `rg -n 'OpenDocument|fw_upload|/api/fw|flashScannerFirmware|relayScannerFirmware' android/app/src/main android/app/src/test android/app/src/androidTest`

Expected: no matches.

Run: `rg -n 'BadgeAppearanceSection|BadgeDisplayFiltersSection' android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy`

Expected: no matches.

Run: `rg -n 'Possible Remote Listening|eavesdropping|NOT in any aircraft database|EMF Sweep|IR Camera Scan|Camera detected' android/app/src/main/java/com/friendorfoe/presentation android/app/src/main/java/com/friendorfoe/detection`

Expected: no false Apple/AR/Advanced-tool claims in Android-visible code. An unrelated non-Apple domain enum may remain only when its UI wording is evidence-limited and its tests prove the Apple path cannot reach it.

- [ ] **Step 10: Start the optional physical debug-bridge validation gate**

Do this only when the user provides the phone and badge. Enumerate candidates with `find /dev -maxdepth 1 -name 'cu.usbmodem*' -print`. With multiple candidates, use `system_profiler SPUSBDataType` plus `ioreg -p IOUSB -l -w0` and the badge's printed USB serial/vendor metadata to resolve one exact device; zero or unresolved candidates stop the gate. Assign only that explicit path to `FOF_BADGE_PORT`, then start a separate terminal from the repository root:

```bash
python3 scripts/fof_badge_debug_bridge.py --port "$FOF_BADGE_PORT" --host 127.0.0.1 --http-port 8765
```

For an emulator, build with `-PbadgeDebugBridgeUrl=http://10.0.2.2:8765/`. For a USB-connected phone, run `adb -s "$FOF_ANDROID_SERIAL" reverse tcp:8765 tcp:8765` and build with `-PbadgeDebugBridgeUrl=http://127.0.0.1:8765/`. Release builds keep the bridge disabled regardless of this property. Before opening any Android control, run this read-only request twice with more than two seconds between samples:

```bash
curl -sS http://127.0.0.1:8765/api/badge/status
```

Both responses must contain the same nonblank protocol/version and the same nonblank `debug_bridge.serial_port`; Android derives the debug target identity from that physical serial port because shipped status need not contain a target-ID field. Both also require `debug_bridge.status_age_s` and empty `debug_bridge.last_error`. The second response must be fresh under Task 8's ten-second window. Record both responses after removing unrelated identifiers. A listening socket or HTTP 200 alone is not physical evidence. Do not call `/api/badge/control`, `/api/fw/upload`, `/api/badge/fw_upload`, `/api/fw/relay`, or `/api/badge/fw_relay` during this read-only gate.

- [ ] **Step 11: Run the optional physical phone + badge smoke test with reversible safeguards**

First prove `READ_STATUS` from a nonblank version and fresh transport-specific evidence. Then gate each reversible command class by the complete original state needed to restore that class: Display navigation requires an exact focus identity/title plus an independent deterministic readback path; Theme requires every original Theme field and a recomputed matching hash; Display Policy requires all 13 original rows/priorities and a recomputed matching hash; network mode requires a valid original `usb_only`, `local_ap`, or `backend` value plus an independent USB/debug-bridge restoration path. A class with absent, invalid, ambiguous, or unreadable original state remains `Unverified` even if another class can be tested. Do not flash firmware, select/upload a file, reboot, or enter bootloader. For each transport intended to ship as verified:

The certification gate must not be bypassed in production. After the complete-original gate passes, test exactly one transport/capability pair at a time with an **unstaged provisional** entry in `CheckedInBadgeReleaseCertification`. For example, the first debug-bridge Theme test build may contain only:

```kotlin
internal val CheckedInBadgeReleaseCertification = BadgeReleaseCertification(
    mutationsByTransport = mapOf(
        BadgeTransport.DEBUG_BRIDGE to setOf(BadgeCapability.THEME_V1),
    ),
)
```

Build and install that debug APK, perform the change/readback/restoration sequence below, and immediately return the entry to empty if any assertion or restoration fails. Do not stage the provisional entry. After a complete pass, its evidence may justify retaining that exact pair; then return to an unstaged one-pair patch for the next class. Before the release commit, compare the final checked-in set against the QA evidence row by row; no pair is inferred from another pair or transport.

- read valid FoF status over at least two freshness intervals;
- confirm transport-open alone never enables mutation;
- exercise navigation only when status exposes a stable `focus_total > 0`, exact original focus identity/index, `detail_mode=false`, and `detail_page=0`. Verify Next changes to the expected next focus, then send exactly `focus_total - 1` further Next commands and prove the original identity/index returned; verify Detail enters detail mode and Back returns to the same original focus. Never assume Back undoes Next. BLE navigation requires a simultaneous verified USB/debug status path to the sole physical badge (or another equally deterministic physical focus readback); BLE connection state alone is insufficient;
- on USB/AP/debug bridge, make one reversible Theme change only when that transport/command class is already enabled by an execution-only test certification. Preserve every field except intensity; set intensity to `original - 1` when above 25, otherwise to 26. Require the exact computed changed hash, restore the complete original Theme, and require the original hash;
- on USB/AP/debug bridge, make one reversible Policy change only under the same gate. Toggle only the `beacon` enabled state using the production typed helper while preserving the stored priority and every other class/field; require matching badge hash and scanner ACK hashes before labeling `Verified on scanners`, then restore the complete original Policy including the original `beacon` lane/proximity/priority and read back the original hash;
- test network mode only while an independent verified USB or debug-bridge restoration path remains connected; never change mode over the sole Local AP path, then restore and read back the exact original mode;
- on BLE, certify status/read only by default; test short navigation within negotiated MTU only when the independent focus/restoration gate above passes, and keep network/Theme/Policy/recovery unavailable;
- treat HTTP `ok:false`, USB error, timeout, hash mismatch, scanner mismatch, or lost status as failure/not verified;
- after every changed command class, restore it before testing the next class;
- confirm the final readback exactly matches every recorded original for each attempted command class; an untouched/unreadable class remains explicitly unverified. Store only a whitelisted evidence projection: UTC timestamp, app/build version, redacted transport name, protocol version, hashes/modes, focus index/total plus redacted stable focus token, ACK outcome, restoration outcome, and scrubbed filenames. Never store Wi-Fi credentials, precise coordinates, unrelated USB identifiers, raw entity feeds, or full status bodies.

The release `BadgeReleaseCertification` remains empty for every untested or incompletely restored transport/command pair. Move a pair into the checked-in certification only when the QA record contains its initial readback, acknowledged change, verified changed readback, exact restoration readback, timestamps, device/app versions, and evidence filenames. A partial run does not certify the class. Reboot/bootloader require separate explicit user approval, verified direct USB, and the matching dedicated firmware ACK; they are not needed to complete this release and remain unavailable on the debug bridge even if other bridge mutations pass.

- [ ] **Step 12: Request a final code review and resolve findings**

Use `superpowers:requesting-code-review` against commits after `b6b1617`, scoped to Android behavior, tests, and documentation. Reproduce each actionable finding, add a regression test, and fix it. After the review fixes and any final physically proven certification-map edit are assembled, update the acceptance test's exact `qaApproved` map and rerun Steps 4–8 in full against the final APK (JVM, lint, assemble, connected tests, explicit install, force-stop, and relaunch). Record these final-run results; earlier provisional-build results do not certify the release. Do not change backend or firmware to resolve an Android finding.

- [ ] **Step 13: Commit verification evidence**

```bash
git add android/app/src/test/java/com/friendorfoe/AndroidOverhaulAcceptanceTest.kt android/app/src/androidTest/java/com/friendorfoe/ClearAndroidStateRule.kt android/app/src/androidTest/java/com/friendorfoe/AndroidOverhaulJourneyTest.kt docs/testing/android-interface-overhaul-qa.md android/app/src/main/java/com/friendorfoe/data/badge/BadgeReleaseCertification.kt
git add -f docs/testing/evidence/android-interface-overhaul
git commit -m "android: verify interface overhaul"
```

Stage `BadgeReleaseCertification.kt` only if its final map differs from empty after complete physical proof; otherwise verify it remains unchanged/empty. Force-add only scrubbed evidence artifacts named in the QA record because repository image/XML/log ignores may otherwise omit them. Before committing, inspect the staged evidence for credentials, precise locations, and unrelated identifiers.

## Plan Completion Audit

Before declaring implementation complete, check every item below against code and fresh command output:

- [ ] Only `android/`, `docs/testing/android-interface-overhaul-qa.md`, `docs/testing/evidence/android-interface-overhaul/`, and this approved Android spec/plan changed after `b6b1617`; `backend/` and `esp32/` are untouched.
- [ ] Seven top-level destinations are exact and ordered; Welcome is a persisted gate; Back does not replay tabs; 600dp is the only bar/rail breakpoint.
- [ ] Privacy is the four-group current list, with source health/freshness/capabilities/source-aware Ignore and exact notification routes; no config/tool cards remain.
- [ ] Apple/AirPods activity is informational on local/backend/badge mappings and cannot count or notify as listening.
- [ ] Badge is the only configuration surface, uses exact firmware contracts/hashes/capability/ack evidence, shows codes rather than a simulator, and has no firmware upload.
- [ ] History exact-row routing/delete/clear/local-retention copy, explicit Capture > Save, contextual permissions, backend polling gate, Info truth copy, and Calibration session-health gate all pass regression tests.
- [ ] List, Map, AR, Detail, Reference, Magnetic-field, and IR-like-light screens meet their state, wording, touch, font, and responsive requirements.
- [ ] `testDebugUnitTest`, `lintDebug`, `assembleDebug`, and `connectedDebugAndroidTest` have fresh recorded results; APK installs and launches.
- [ ] Every physical transport labeled verified has recorded physical evidence and restored original state; untested operations remain unavailable.

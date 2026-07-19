# Android Dedicated Badge USB Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an Android-only top-level Badge USB workspace, remove badge controls from Privacy and List, and mark badge-origin List rows with a gold three-triangle icon while preserving the connected badge firmware unchanged.

**Architecture:** `BadgeUsbRepository` remains the single USB protocol owner and gains an idempotent app-foreground lifecycle plus a bounded parsed activity feed. A dedicated `BadgeControlViewModel` and `BadgeControlScreen` own every badge command and theme/filter editor. List consumes badge state read-only through a presentation-only union model, while Privacy becomes independent of the badge repository.

**Tech Stack:** Kotlin, Jetpack Compose Material 3, Navigation Compose, Hilt, Kotlin coroutines/StateFlow, Android USB Host API, Gson, JUnit 4, Gradle, adb/emulator tooling.

## Global Constraints

- This is an Android-only change. Do not edit, format, build, flash, or stage anything under `esp32/`.
- Pre-work ESP32 binary-diff hash: `82bb2e31817ca7ec3afefc0e403cf34c62b7b62bc446042b92d90f96179bb968`.
- Pre-work ESP32 status-list hash: `5824bde974b6c960cc16d5114309ccf92c6c53e05dae67c8d41fe56981b476ef`.
- Preserve the existing `FOF_*` firmware wire contract; do not add or alter a firmware command.
- Badge commands remain direct-USB-only. HTTP may provide stale/read-only status but never enables controls.
- Android firmware upload remains disabled. Scanner images continue to be staged from a laptop.
- The bottom bar stays at seven items. Badge replaces Cal beside Privacy; Calibration remains available from Info.
- No manual Connect button appears on Privacy, List, or Badge. Only `Grant USB access` may appear after denial.
- Reboot, bootloader, and per-slot scanner recovery each require an action-specific confirmation.
- The connected badge is a read/write control test fixture only. Never enter bootloader or relay firmware during theme verification.

---

## File map

### New files

- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbActivity.kt` — bounded activity and detection provenance helpers.
- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbLifecycleGate.kt` — idempotent foreground ownership.
- `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeMarkIcon.kt` — shared three-triangle vector and gold color.
- `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlAction.kt` — disruptive-action confirmation reducer.
- `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt` — dedicated command/state owner.
- `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt` — dedicated status/feed/remote/theme/filter/operations UI.
- `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeEntityInvestigation.kt` — badge-entity-to-investigation target mapping.
- `android/app/src/main/java/com/friendorfoe/presentation/list/ListFeedItem.kt` — presentation-only union of normal and badge rows.
- Matching JVM tests under `android/app/src/test/java/com/friendorfoe/...`.

### Modified files

- `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt` — auto-permission, activity feed, received timestamps, lifecycle gate.
- `android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt` — foreground USB ownership.
- `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt` — seven-item bar with Badge beside Privacy.
- `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt` — Badge and badge-focus routes.
- `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt` — dedicated route and List-to-Badge navigation.
- `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt` — read-only badge feed, no commands or profiles.
- `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt` — unified rows, no control panel.
- `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt` — phone/backend-only privacy state.
- `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt` — no badge UI or dialogs.

---

### Task 1: Add bounded USB activity and stable badge provenance

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbActivity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- Test: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbActivityTest.kt`

**Interfaces:**
- Produces: `BadgeUsbDetection.receivedAtElapsedMs: Long`
- Produces: `BadgeUsbDetection.stableKey: String`
- Produces: `BadgeUsbActivity(kind, key, title, detail, receivedAtElapsedMs)`
- Produces: `pushBadgeUsbActivity(current, next, limit): List<BadgeUsbActivity>`
- Produces: `BadgeUsbState.activity: List<BadgeUsbActivity>`

- [ ] **Step 1: Write failing bounded-feed and stable-key tests**

```kotlin
class BadgeUsbActivityTest {
    @Test fun `stable key prefers entity then id`() {
        assertEquals("entity:drone:abc", detection(entity = "drone:abc").stableKey)
        assertEquals("id:rid-7", detection(id = "rid-7").stableKey)
    }

    @Test fun `activity is newest first deduplicated and bounded`() {
        val result = (1L..70L).fold(emptyList<BadgeUsbActivity>()) { current, n ->
            pushBadgeUsbActivity(
                current,
                BadgeUsbActivity(BadgeUsbActivityKind.DETECTION, "event:$n", "Drone $n", "", n),
                limit = 64,
            )
        }
        assertEquals(64, result.size)
        assertEquals(70L, result.first().receivedAtElapsedMs)
        assertEquals(7L, result.last().receivedAtElapsedMs)

        val replaced = pushBadgeUsbActivity(
            result,
            result.first().copy(title = "Updated"),
            limit = 64,
        )
        assertEquals(64, replaced.size)
        assertEquals("Updated", replaced.first().title)
        assertEquals(1, replaced.count { it.key == "event:70" })
    }

    private fun detection(
        id: String = "",
        entity: String = "",
    ) = BadgeUsbDetection(
        id = id,
        manufacturer = "DJI",
        badgeEntityKey = entity,
        source = 0,
        confidence = 0.9f,
        rssi = -45,
        receivedAtElapsedMs = 1L,
    )
}
```

- [ ] **Step 2: Run the focused test and verify red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeUsbActivityTest'`

Expected: compilation fails because `BadgeUsbActivity` and `stableKey` do not exist.

- [ ] **Step 3: Add the pure activity model and helpers**

```kotlin
enum class BadgeUsbActivityKind { DETECTION, COMMAND, FIRMWARE, STATUS, ERROR }

data class BadgeUsbActivity(
    val kind: BadgeUsbActivityKind,
    val key: String,
    val title: String,
    val detail: String,
    val receivedAtElapsedMs: Long,
)

internal const val MAX_BADGE_USB_ACTIVITY = 64

val BadgeUsbDetection.stableKey: String
    get() = when {
        badgeEntityKey.isNotBlank() -> "entity:$badgeEntityKey"
        id.isNotBlank() -> "id:$id"
        else -> "source:$source:${badgeClass}:${manufacturer}:${rssi}"
    }

internal fun pushBadgeUsbActivity(
    current: List<BadgeUsbActivity>,
    next: BadgeUsbActivity,
    limit: Int = MAX_BADGE_USB_ACTIVITY,
): List<BadgeUsbActivity> = buildList {
    add(next)
    current.asSequence().filterNot { it.key == next.key }.take((limit - 1).coerceAtLeast(0)).forEach(::add)
}
```

Add `receivedAtElapsedMs: Long = elapsedRealtimeMs()` to `BadgeUsbDetection`, add `activity` to `BadgeUsbState`, and update `handleLine` to record only `FOF_DET`, `FOF_CTL_OK`, `FOF_CTL_ERROR`, `FOF_FW_*`, accepted `FOF_STATUS`, and investigation results. Continue truncating `lastLine`; do not retain arbitrary ESP-IDF log lines.

- [ ] **Step 4: Run focused repository tests green**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeUsbActivityTest' --tests '*BadgeUsbIdentityHandshakeTest' --tests '*BadgeUsbLineFramerTest'`

Expected: all selected tests pass.

- [ ] **Step 5: Verify firmware invariant and commit**

Run: `shasum -a 256 <(git diff --binary -- esp32)`

Expected: `82bb2e31817ca7ec3afefc0e403cf34c62b7b62bc446042b92d90f96179bb968`.

Commit only Android files:

```bash
git add android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbActivity.kt android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbActivityTest.kt
git commit -m "android: model live badge USB activity"
```

---

### Task 2: Make USB attachment app-owned and automatic

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbLifecycleGate.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Test: `android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbLifecycleGateTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/BadgeUsbApplicationLifecycleContractTest.kt`

**Interfaces:**
- Produces: `BadgeUsbLifecycleGate.begin(): Boolean`
- Produces: `BadgeUsbLifecycleGate.end(): Boolean`
- Changes: `BadgeUsbRepository.start()` automatically requests permission/connects exactly once per foreground session.

- [ ] **Step 1: Write failing gate and application-contract tests**

```kotlin
class BadgeUsbLifecycleGateTest {
    @Test fun `begin and end are idempotent`() {
        val gate = BadgeUsbLifecycleGate()
        assertTrue(gate.begin())
        assertFalse(gate.begin())
        assertTrue(gate.end())
        assertFalse(gate.end())
        assertTrue(gate.begin())
    }
}
```

The application contract test reads `FriendOrFoeApplication.kt` and asserts injection plus `onStart -> badgeUsbRepository.start()` and `onStop -> badgeUsbRepository.stop()`. It also reads List and Privacy sources and asserts neither contains `startBadgeUsb()` nor `stopBadgeUsb()`.

- [ ] **Step 2: Run the focused tests and verify red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeUsbLifecycleGateTest' --tests '*BadgeUsbApplicationLifecycleContractTest'`

Expected: failure because the gate and application ownership are absent.

- [ ] **Step 3: Implement idempotent auto-permission lifecycle**

```kotlin
internal class BadgeUsbLifecycleGate {
    private var active = false
    @Synchronized fun begin(): Boolean = if (active) false else { active = true; true }
    @Synchronized fun end(): Boolean = if (!active) false else { active = false; true }
}
```

In `BadgeUsbRepository`:

```kotlin
private val lifecycleGate = BadgeUsbLifecycleGate()

fun start() {
    if (!lifecycleGate.begin()) return
    registerReceiverIfNeeded()
    requestConnection()
    if (BadgeControlTransportPolicy.allowsReadOnlyHttpStatus()) {
        startApPoller()
        startDebugBridgePoller()
    }
}

fun stop() {
    if (!lifecycleGate.end()) return
    // existing disconnect and poller cleanup
}
```

Update permission copy from `Tap Connect` to `Waiting for USB permission` or `USB access required`. Inject the repository into `FriendOrFoeApplication`, start it in process `onStart`, and stop it in `onStop`. Remove only per-screen badge start/stop lifecycle calls; leave unrelated location and privacy lifecycle behavior intact.

- [ ] **Step 4: Run lifecycle and transport tests green**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeUsbLifecycleGateTest' --tests '*BadgeUsbApplicationLifecycleContractTest' --tests '*BadgeControlTransportPolicyTest' --tests '*BadgeUsbIdentityHandshakeTest'`

Expected: all selected tests pass and only verified `CONNECTED` enables commands.

- [ ] **Step 5: Verify firmware invariant and commit**

```bash
git add android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbLifecycleGate.kt android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt android/app/src/main/java/com/friendorfoe/FriendOrFoeApplication.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt android/app/src/test/java/com/friendorfoe/data/badge/BadgeUsbLifecycleGateTest.kt android/app/src/test/java/com/friendorfoe/BadgeUsbApplicationLifecycleContractTest.kt
git commit -m "android: auto-connect badge USB in app lifecycle"
```

---

### Task 3: Add the Badge icon, navigation, and dedicated control page

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeMarkIcon.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlAction.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeMarkIconTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeControlActionTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeNavigationContractTest.kt`

**Interfaces:**
- Produces: `BadgeMarkIcon: ImageVector`
- Produces: `BadgeMarkGold = Color(0xFFFFC107)`
- Produces: `BadgeDangerAction` and `reduceBadgeDangerConfirmation(current, event)`
- Produces: `Screen.Badge` and `Screen.BadgeFocus.createRoute(stableKey)`
- Produces: `BadgeControlScreen(initialFocusKey: String?, viewModel: BadgeControlViewModel)`

- [ ] **Step 1: Write failing icon, confirmation, and route tests**

```kotlin
class BadgeControlActionTest {
    @Test fun `each dangerous action requires exact confirmation`() {
        BadgeDangerAction.entries.forEach { action ->
            val armed = reduceBadgeDangerConfirmation(null, BadgeDangerEvent.Request(action))
            assertEquals(action, armed.pending)
            assertNull(reduceBadgeDangerConfirmation(armed.pending, BadgeDangerEvent.Cancel).confirmed)
            assertEquals(action, reduceBadgeDangerConfirmation(armed.pending, BadgeDangerEvent.Confirm).confirmed)
        }
    }
}
```

The reducer under test is exact and side-effect free:

```kotlin
enum class BadgeDangerAction { REBOOT, BOOTLOADER, RECOVER_SLOT_0, RECOVER_SLOT_1 }
sealed interface BadgeDangerEvent {
    data class Request(val action: BadgeDangerAction) : BadgeDangerEvent
    data object Confirm : BadgeDangerEvent
    data object Cancel : BadgeDangerEvent
}
data class BadgeDangerTransition(
    val pending: BadgeDangerAction? = null,
    val confirmed: BadgeDangerAction? = null,
)
internal fun reduceBadgeDangerConfirmation(
    pending: BadgeDangerAction?,
    event: BadgeDangerEvent,
) = when (event) {
    is BadgeDangerEvent.Request -> BadgeDangerTransition(pending = event.action)
    BadgeDangerEvent.Cancel -> BadgeDangerTransition()
    BadgeDangerEvent.Confirm -> BadgeDangerTransition(confirmed = pending)
}
```

The navigation contract test asserts the bottom-nav source contains seven `BottomNavItem` entries in the exact order `AR, Map, List, Privacy, Badge, History, Info`, references `BadgeMarkIcon`, omits Cal, and the nav graph contains both Badge routes.

- [ ] **Step 2: Run focused tests and verify red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeMarkIconTest' --tests '*BadgeControlActionTest' --tests '*BadgeNavigationContractTest'`

Expected: compile/source-contract failures for missing Badge types and route.

- [ ] **Step 3: Implement the shared three-triangle vector**

```kotlin
internal data class BadgeTriangle(val topX: Float, val topY: Float, val leftX: Float, val baseY: Float, val rightX: Float)

internal val BadgeMarkTriangles = listOf(
    BadgeTriangle(12f, 2f, 7f, 10f, 17f),
    BadgeTriangle(6.5f, 11f, 1.5f, 19f, 11.5f),
    BadgeTriangle(17.5f, 11f, 12.5f, 19f, 22.5f),
)

val BadgeMarkGold = Color(0xFFFFC107)

val BadgeMarkIcon: ImageVector by lazy {
    ImageVector.Builder("BadgeMark", 24.dp, 24.dp, 24f, 24f).apply {
        BadgeMarkTriangles.forEach { triangle ->
            path(fill = SolidColor(Color.Black)) {
                moveTo(triangle.topX, triangle.topY)
                lineTo(triangle.leftX, triangle.baseY)
                lineTo(triangle.rightX, triangle.baseY)
                close()
            }
        }
    }.build()
}
```

- [ ] **Step 4: Implement dedicated ViewModel commands and confirmations**

`BadgeControlViewModel` exposes repository state, theme profiles, refresh/grant-permission, LCD navigation, mode, theme, display policy, scanner recovery, reboot, and bootloader methods. `BadgeDangerAction` has `REBOOT`, `BOOTLOADER`, `RECOVER_SLOT_0`, and `RECOVER_SLOT_1`; only a confirmed action calls the repository. Badge investigation is added with its mapper in Task 5 so this task compiles without a half-moved Privacy dependency.

```kotlin
@HiltViewModel
class BadgeControlViewModel @Inject constructor(
    private val repository: BadgeUsbRepository,
    private val profiles: BadgeThemeProfileStore,
) : ViewModel() {
    val badgeState = repository.state
    val themeProfiles = profiles.profiles
    fun grantUsbAccess() = repository.requestConnection()
    fun refresh() = repository.requestStatus()
    fun displayNav(action: String) = repository.displayNav(action)
    fun setMode(mode: String) = repository.setMode(mode)
    fun applyTheme(theme: BadgeTheme) = repository.applyBadgeTheme(theme)
    fun resetTheme() = repository.resetBadgeTheme()
    fun applyDisplayPolicy(policy: BadgeDisplayPolicy) = repository.applyDisplayPolicy(policy)
    fun resetDisplayPolicy() = repository.resetDisplayPolicy()
    fun createProfile(name: String, theme: BadgeTheme) = profiles.create(name, theme)
    fun renameProfile(id: String, name: String) = profiles.rename(id, name)
    fun replaceProfile(id: String, theme: BadgeTheme) = profiles.replace(id, theme)
    fun deleteProfile(id: String) = profiles.delete(id)
    fun execute(action: BadgeDangerAction) = when (action) {
        BadgeDangerAction.REBOOT -> repository.rebootBadge()
        BadgeDangerAction.BOOTLOADER -> repository.enterBootloader()
        BadgeDangerAction.RECOVER_SLOT_0 -> repository.relayScannerFirmware("ble")
        BadgeDangerAction.RECOVER_SLOT_1 -> repository.relayScannerFirmware("wifi")
    }
}
```

- [ ] **Step 5: Build the dedicated screen and routes**

Use a `LazyColumn` with tagged sections `badge_status`, `badge_live_feed`, `badge_lcd_remote`, `badge_appearance`, `badge_filters`, and `badge_operations`. Reuse `BadgeAppearanceSection` and `BadgeDisplayFiltersSection`. Show `Grant USB access` only for `PERMISSION_NEEDED`; never show Connect. Disable all command buttons unless `BadgeControlTransportPolicy.allowsCommandSurface(state.status)`.

The screen root follows this complete state wiring; each named section is a
focused private composable in the same file:

```kotlin
@Composable
fun BadgeControlScreen(
    initialFocusKey: String? = null,
    viewModel: BadgeControlViewModel = hiltViewModel(),
) {
    val state by viewModel.badgeState.collectAsStateWithLifecycle()
    val profiles by viewModel.themeProfiles.collectAsStateWithLifecycle()
    var draftTheme by remember(state.controlStatus?.themeHash) {
        mutableStateOf(state.controlStatus?.theme ?: defaultBadgeTheme())
    }
    var draftPolicy by remember(state.controlStatus?.displayPolicyHash) {
        mutableStateOf(state.controlStatus?.displayPolicy ?: defaultBadgeDisplayPolicy())
    }
    var appearanceExpanded by remember { mutableStateOf(false) }
    var filtersExpanded by remember { mutableStateOf(false) }
    var pendingDanger by remember { mutableStateOf<BadgeDangerAction?>(null) }
    val commandsEnabled = BadgeControlTransportPolicy.allowsCommandSurface(state.status)

    LazyColumn(Modifier.fillMaxSize()) {
        item { BadgeStatusSection(state, viewModel::grantUsbAccess, viewModel::refresh) }
        item { BadgeLiveFeedSection(state, initialFocusKey) }
        item { BadgeLcdRemoteSection(state.controlStatus?.displayState, commandsEnabled, viewModel::displayNav) }
        item {
            BadgeAppearanceSection(
                expanded = appearanceExpanded,
                onExpandedChange = { appearanceExpanded = it },
                theme = draftTheme,
                appliedTheme = state.controlStatus?.theme ?: defaultBadgeTheme(),
                themeHash = state.controlStatus?.themeHash ?: 0,
                profiles = profiles,
                onThemeChange = { draftTheme = it },
                onCreateProfile = viewModel::createProfile,
                onRenameProfile = viewModel::renameProfile,
                onReplaceProfile = viewModel::replaceProfile,
                onDeleteProfile = viewModel::deleteProfile,
                onApply = viewModel::applyTheme,
                onRefresh = viewModel::refresh,
            )
        }
        item {
            BadgeDisplayFiltersSection(
                expanded = filtersExpanded,
                onExpandedChange = { filtersExpanded = it },
                policy = draftPolicy,
                displayPolicyHash = state.controlStatus?.displayPolicyHash ?: 0,
                filteredCounts = state.controlStatus?.filteredCounts.orEmpty(),
                onPolicyChange = { draftPolicy = it },
                onApply = { viewModel.applyDisplayPolicy(draftPolicy) },
                onReset = {
                    draftPolicy = defaultBadgeDisplayPolicy()
                    viewModel.resetDisplayPolicy()
                },
                onRefresh = viewModel::refresh,
            )
        }
        item { BadgeOperationsSection(state, commandsEnabled, onDanger = { pendingDanger = it }, onSetMode = viewModel::setMode) }
    }
    pendingDanger?.let { action ->
        BadgeDangerConfirmationDialog(
            action = action,
            onConfirm = { pendingDanger = null; viewModel.execute(action) },
            onDismiss = { pendingDanger = null },
        )
    }
}
```

Add:

```kotlin
data object Badge : Screen("badge")
data object BadgeFocus : Screen("badge/{focusKey}") {
    fun createRoute(focusKey: String) = "badge/${Uri.encode(focusKey)}"
}
```

Register both routes, pass `focusKey` to the screen, and select the Badge bottom item for either route. Replace Cal with Badge directly after Privacy. Do not remove the existing Info-to-Calibration navigation.

- [ ] **Step 6: Run focused tests and compile the Android app**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*BadgeMarkIconTest' --tests '*BadgeControlActionTest' --tests '*BadgeNavigationContractTest' assembleDebug`

Expected: selected tests and debug assembly pass.

- [ ] **Step 7: Verify firmware invariant and commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeMarkIcon.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlAction.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt android/app/src/main/java/com/friendorfoe/presentation/MainActivity.kt android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeMarkIconTest.kt android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeControlActionTest.kt android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeNavigationContractTest.kt
git commit -m "android: add dedicated badge USB workspace"
```

---

### Task 4: Put badge detections into List with badge provenance

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/list/ListFeedItem.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/list/ListFeedItemTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/list/BadgeListSeparationContractTest.kt`

**Interfaces:**
- Produces: `sealed interface ListFeedItem`
- Produces: `mergeListFeed(skyObjects, badgeDetections): List<ListFeedItem>`
- Changes: `ListViewScreen` accepts `onBadgeDetectionTapped: (String) -> Unit`.

- [ ] **Step 1: Write failing feed/provenance tests**

```kotlin
class ListFeedItemTest {
    @Test fun `badge rows are deduplicated newest first and keep badge provenance`() {
        val old = badgeDetection(id = "d1", receivedAt = 10)
        val updated = old.copy(rssi = -41, receivedAtElapsedMs = 20)
        val feed = mergeListFeed(emptyList(), listOf(old, updated))
        assertEquals(1, feed.size)
        val row = feed.single() as ListFeedItem.Badge
        assertEquals(-41, row.detection.rssi)
        assertEquals(ListSourceMarker.BADGE, row.sourceMarker)
    }

    private fun badgeDetection(id: String, receivedAt: Long) = BadgeUsbDetection(
        id = id,
        manufacturer = "DJI",
        source = 0,
        confidence = 0.9f,
        rssi = -50,
        receivedAtElapsedMs = receivedAt,
    )
}
```

- [ ] **Step 2: Run focused tests and verify red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*ListFeedItemTest' --tests '*BadgeListSeparationContractTest'`

Expected: missing union model and old `BadgeUsbPanel` contract failure.

- [ ] **Step 3: Add the presentation-only union model**

```kotlin
enum class ListSourceMarker { BADGE, ADS_B, REMOTE_ID, WIFI }

sealed interface ListFeedItem {
    val key: String
    val sourceMarker: ListSourceMarker

    data class Sky(val value: SkyObject) : ListFeedItem {
        override val key = "sky:${value.id}"
        override val sourceMarker = when (value.source) {
            DetectionSource.ADS_B -> ListSourceMarker.ADS_B
            DetectionSource.REMOTE_ID -> ListSourceMarker.REMOTE_ID
            DetectionSource.WIFI_NAN, DetectionSource.WIFI_BEACON, DetectionSource.WIFI -> ListSourceMarker.WIFI
        }
    }

    data class Badge(val detection: BadgeUsbDetection) : ListFeedItem {
        override val key = "badge:${detection.stableKey}"
        override val sourceMarker = ListSourceMarker.BADGE
    }
}

internal fun mergeListFeed(
    skyObjects: List<SkyObject>,
    badgeDetections: List<BadgeUsbDetection>,
): List<ListFeedItem> = badgeDetections
    .sortedByDescending { it.receivedAtElapsedMs }
    .distinctBy { it.stableKey }
    .map { ListFeedItem.Badge(it) } + skyObjects.map { ListFeedItem.Sky(it) }
```

- [ ] **Step 4: Remove List controls and render badge rows**

Delete `BadgeUsbPanel` and all badge command/theme/profile methods from `ListViewModel`. Keep only `val badgeUsbState = badgeUsbRepository.state`. Combine `skyObjects` and `badgeUsbState.detections` into the presentation feed.

Render `ListFeedItem.Badge` with best label/manufacturer, class/transport evidence, RSSI, confidence, `--` altitude/distance, and `Icon(BadgeMarkIcon, tint = BadgeMarkGold)`. Ordinary rows keep existing icons. Tapping a badge row calls `Screen.BadgeFocus.createRoute(stableKey)`.

- [ ] **Step 5: Run focused and existing List tests green**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*ListFeedItemTest' --tests '*BadgeListSeparationContractTest' --tests '*ListSurfacePresentationTest'`

Expected: all selected tests pass and `BadgeUsbPanel`/`Connect` are absent from List source.

- [ ] **Step 6: Verify firmware invariant and commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/list/ListFeedItem.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/list/ListViewScreen.kt android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt android/app/src/test/java/com/friendorfoe/presentation/list/ListFeedItemTest.kt android/app/src/test/java/com/friendorfoe/presentation/list/BadgeListSeparationContractTest.kt
git commit -m "android: mark badge-origin detections in List"
```

---

### Task 5: Remove badge state from Privacy and preserve badge investigation on Badge

**Files:**
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeEntityInvestigation.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyBadgeSeparationContractTest.kt`
- Test: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeControlInvestigationTest.kt`

**Interfaces:**
- Privacy produces only phone-local + backend + Wi-Fi anomaly detections.
- Badge page consumes `BadgeUsbRepository.investigation` and calls `investigateBle`/`cancelBleInvestigation`.

- [ ] **Step 1: Write failing separation and investigation tests**

The separation test reads both Privacy files and rejects `BadgeUsbRepository`, `BadgeUsbState`, `BadgeDetailPanel`, `BadgeUsbStatusRow`, `badgeUsbState`, and `toPrivacyDetections()` usage. The investigation test constructs a fresh BLE entity and asserts the dedicated badge target mapper produces `BleInvestigationRoute.BADGE` with GATT mode and the entity BSSID.

- [ ] **Step 2: Run tests and verify red**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*PrivacyBadgeSeparationContractTest' --tests '*BadgeControlInvestigationTest'`

Expected: Privacy still owns badge state and the dedicated mapper is absent.

- [ ] **Step 3: Remove badge dependencies from Privacy**

Remove badge repository/theme-store constructor dependencies, state flows, lifecycle/connection/command methods, badge merge input, badge entity dialog, and badge-routed investigation code from `PrivacyViewModel` and `PrivacyScreen`. Preserve phone investigation, phone/backend privacy detections, Wi-Fi anomaly handling, and unrelated sweep tools.

The privacy detection merge becomes:

```kotlin
val privacyDetections = combine(
    skyObjectRepository.glassesDetections,
    _backendPrivacyDetections,
    _wifiAnomalies,
) { local, backend, wifiAnomalies ->
    mergePrivacyDetections(local, backend + wifiAnomalies.map { it.toPrivacyDetection() })
}
```

- [ ] **Step 4: Preserve badge-only BLE investigation on the dedicated page**

Move the pure `BadgeThreatEntity` target mapping needed for GATT/passive capture into the badge presentation package. `BadgeControlViewModel.investigate(entity)` creates a bounded request ID, forces `BleInvestigationRoute.BADGE`, calls `repository.investigateBle`, observes `repository.investigation`, and supports cancel. The Badge entity detail dialog shows Investigate only when a target is available.

```kotlin
internal fun BadgeThreatEntity.badgeInvestigationTarget(nowElapsedMs: Long): BleInvestigationTarget? {
    if (stale) return null
    val entityKey = "badge:${threatClass}:${code}:${badgeEntityStableId()}"
    val observedAt = badgeEntityObservedAtElapsedMs(
        snapshotAtElapsedMs.takeIf { it >= 0 } ?: nowElapsedMs,
        lastSeenSeconds,
    )
    val pairingSpam = listOf(code, category, label, detail).any {
        it.trim().replace('-', '_').replace(' ', '_').uppercase() in setOf("PAIRING_SPAM", "BLE_SPAM")
    }
    return when {
        pairingSpam -> BleInvestigationTarget(BleInvestigationMode.PASSIVE_CAPTURE, null, entityKey, observedAt, PrivacyDetectionOrigin.BADGE)
        threatClass.equals("ble", true) && bssid.isNotBlank() -> BleInvestigationTarget(BleInvestigationMode.GATT, bssid, entityKey, observedAt, PrivacyDetectionOrigin.BADGE)
        else -> null
    }
}

private fun BadgeThreatEntity.badgeEntityStableId(): String = bssid.ifBlank {
    displayId.ifBlank { operatorId ?: detail.ifBlank { label } }
}

internal fun badgeEntityObservedAtElapsedMs(snapshotAtElapsedMs: Long, lastSeenSeconds: Int): Long {
    val ageMs = lastSeenSeconds.coerceAtLeast(0).toLong() * 1_000L
    return (snapshotAtElapsedMs.coerceAtLeast(0L) - ageMs).coerceAtLeast(0L)
}
```

Extend the ViewModel with the repository-owned investigation state and exact
badge-only request path:

```kotlin
val investigation = repository.investigation
private var activeInvestigationRequestId: String? = null

fun investigate(entity: BadgeThreatEntity) {
    val now = elapsedRealtimeMs()
    val target = entity.badgeInvestigationTarget(now) ?: return
    val requestId = "badge-${now.toString(36)}".take(48)
    val request = BleInvestigationRequest(
        requestId = requestId,
        target = target,
        route = BleInvestigationRoute.BADGE,
    )
    if (repository.investigateBle(request)) activeInvestigationRequestId = requestId
}

fun cancelInvestigation() {
    activeInvestigationRequestId?.let(repository::cancelBleInvestigation)
    activeInvestigationRequestId = null
}
```

Update `BadgeLiveFeedSection` to accept the collected result plus these two
callbacks and render the existing investigation progress/result fields.

- [ ] **Step 5: Run privacy, investigation, and mapping tests green**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*PrivacyBadgeSeparationContractTest' --tests '*BadgeControlInvestigationTest' --tests '*BleInvestigationRoutingTest' --tests '*PrivacyCategoryMappingTest' --tests '*BadgePrivacyMapperTest'`

Expected: all selected tests pass; existing pure mapping/routing coverage remains green even though Privacy no longer consumes badge state.

- [ ] **Step 6: Verify firmware invariant and commit**

```bash
git add android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyScreen.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlViewModel.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeEntityInvestigation.kt android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyBadgeSeparationContractTest.kt android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeControlInvestigationTest.kt
git commit -m "android: isolate badge USB from Privacy"
```

---

### Task 6: Full Android, emulator, and connected-badge verification

**Files:**
- Modify only Android tests if verification exposes an Android defect.
- Do not modify any firmware file.

**Interfaces:**
- Consumes all prior tasks.
- Produces a debug APK and captured verification evidence.

- [ ] **Step 1: Run formatting/static diff checks**

Run:

```bash
git diff --check -- android
rg -n "BadgeUsbPanel|Tap Connect|Text\(\"Connect\"\)|startBadgeUsb|stopBadgeUsb" android/app/src/main/java/com/friendorfoe/presentation/list android/app/src/main/java/com/friendorfoe/presentation/privacy
```

Expected: no diff errors and no matches for the removed control/lifecycle surface.

- [ ] **Step 2: Run the entire Android JVM suite and build APK**

Run: `cd android && ./gradlew testDebugUnitTest assembleDebug`

Expected: `BUILD SUCCESSFUL`; debug APK at `android/app/build/outputs/apk/debug/app-debug.apk`.

- [ ] **Step 3: Start an emulator and validate navigation visually**

Use the Android emulator QA workflow to install the debug APK, launch it, grant ordinary app permissions, and capture screenshots proving:

- Badge is directly beside Privacy and uses the three-triangle icon.
- Privacy has no badge panel.
- List has no Connect/control panel.
- Badge page shows disconnected/permission state without a Connect button.
- Badge page exposes status, live feed, LCD remote, appearance, filters, and operations sections.

- [ ] **Step 4: Test emulator USB passthrough without mutating the badge**

Inspect `adb devices`, `adb shell dumpsys usb`, and host USB ownership. If the badge cannot be attached to the emulator, record that as an emulator limitation and do not alter firmware or force the badge into bootloader. If passthrough works, grant USB permission, verify the exact uplink identity, refresh status, apply two preset themes and one custom palette, and restore the original theme.

- [ ] **Step 5: Use Mac USB plus camera as the existing-wire fallback**

When emulator passthrough is unavailable, first use JVM tests to capture the exact `FOF_CTL` JSON produced by Android for the chosen themes. Send those same existing frames over the Mac serial port to the already-identified uplink. Use camera stills before/after each theme to verify the LCD changed, then restore the initial `BadgeTheme` from the pre-test `FOF_STATUS`. Do not send reboot, bootloader, firmware upload, or relay commands.

- [ ] **Step 6: Prove firmware remained untouched**

Run:

```bash
shasum -a 256 <(git diff --binary -- esp32)
git status --short -- esp32 | shasum -a 256
```

Expected hashes:

- diff: `82bb2e31817ca7ec3afefc0e403cf34c62b7b62bc446042b92d90f96179bb968`
- status: `5824bde974b6c960cc16d5114309ccf92c6c53e05dae67c8d41fe56981b476ef`

- [ ] **Step 7: Commit Android verification fixes if any and record final evidence**

If verification required Android-only corrections, stage exact Android paths and commit:

```bash
git commit -m "android: verify dedicated badge USB workspace"
```

Do not stage `.camera-before-zoom.jpg`, emulator screenshots outside the intended artifact directory, generated APKs, or any ESP32 path.

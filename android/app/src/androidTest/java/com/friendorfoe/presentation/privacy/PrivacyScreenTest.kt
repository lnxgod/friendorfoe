package com.friendorfoe.presentation.privacy

import android.graphics.Bitmap
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performClick
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class PrivacyScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun liveBluetoothUpdatesKeepRowPositionsAndTapTargetsStable() {
        val first = finding(FindingSeverity.NEARBY, "a", category = PrivacyCategory.SMART_SPEAKER)
        val second = finding(FindingSeverity.NEARBY, "b", category = PrivacyCategory.SMART_SPEAKER)
        fun project(rows: List<PrivacyFinding>) = projectPrivacyUiState(
            PrivacyCurrentReducer().reduce(
                listOf(PrivacySourceSnapshot(
                    health = health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE),
                    findings = rows,
                    emittedAtElapsedMs = 2_000L,
                )), emptySet(), 2_000L,
            ),
        )
        val state = mutableStateOf(project(listOf(first, second)))
        compose.setContent {
            FriendOrFoeTheme { PrivacyContent(state.value, PrivacyActions()) }
        }
        val firstBounds = compose.onNodeWithTag("finding_a").fetchSemanticsNode().boundsInRoot
        val secondBounds = compose.onNodeWithTag("finding_b").fetchSemanticsNode().boundsInRoot
        repeat(10) { index ->
            compose.runOnIdle {
                state.value = project(if (index % 2 == 0) {
                    listOf(second.copy(lastObservedElapsedMs = 2_000, signalDbm = -35), first)
                } else {
                    listOf(first.copy(lastObservedElapsedMs = 2_000, signalDbm = -35), second)
                })
            }
            compose.onNodeWithTag("finding_a").assertIsDisplayed()
            compose.onNodeWithTag("finding_b").assertIsDisplayed()
            assertEquals(firstBounds, compose.onNodeWithTag("finding_a").fetchSemanticsNode().boundsInRoot)
            assertEquals(secondBounds, compose.onNodeWithTag("finding_b").fetchSemanticsNode().boundsInRoot)
        }
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, "privacy-stable.png")
            .outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    @Test
    fun currentFindingsKeepFourClearGroupsAndCapabilityBackedActions() {
        val actions = RecordingActions()
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state = stateWithAllSeverities(),
                    actions = actions.asActions(),
                )
            }
        }

        listOf("THREATS", "AWARENESS", "NEARBY", "INFO").forEach {
            compose.onNodeWithTag("privacy_content").performScrollToNode(hasText(it))
            compose.onNodeWithText(it).assertIsDisplayed()
        }
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_critical_ignore"))
        compose.onNodeWithTag("finding_critical_ignore").assertHasClickAction()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_critical_track"))
        compose.onNodeWithTag("finding_critical_track").assertHasClickAction()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_critical_details"))
        compose.onNodeWithTag("finding_critical_details").assertHasClickAction()
        compose.onAllNodesWithText("Track", substring = true).fetchSemanticsNodes()

        listOf(
            "Calibration",
            "Sweep tools",
            "Drone alerts",
            "Military alerts",
            "Firmware upload",
            "Badge configuration",
        ).forEach { forbidden ->
            assertEquals(
                forbidden,
                0,
                compose.onAllNodesWithText(forbidden, substring = true, ignoreCase = true)
                    .fetchSemanticsNodes().size,
            )
        }
    }

    @Test
    fun sourceStatusIsCompactAndFailureDoesNotHideGoodRows() {
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(
                    health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE),
                    health(
                        PrivacySourceKind.BACKEND,
                        SourceHealthState.FAILED,
                        message = "Backend timed out",
                        recoveryLabel = "Retry",
                    ),
                    health(PrivacySourceKind.BADGE_USB, SourceHealthState.LIVE),
                    health(PrivacySourceKind.WIFI_ANALYSIS, SourceHealthState.LIVE),
                ),
                findings = listOf(finding(FindingSeverity.NEARBY, "phone", category = PrivacyCategory.SMART_SPEAKER)),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme { PrivacyContent(state, PrivacyActions()) }
        }

        listOf("Phone", "Backend", "Badge", "Wi-Fi").forEach {
            compose.onNodeWithText(it).assertIsDisplayed()
        }
        compose.onNodeWithText("Backend timed out").assertIsDisplayed()
        compose.onNodeWithTag("finding_phone").performScrollTo().assertIsDisplayed()
        compose.onNodeWithText("Privacy sources unavailable").assertDoesNotExist()
    }

    @Test
    fun pausedPhoneSourceOffersOneTapActivationWithoutBecomingASettingsScreen() {
        var activations = 0
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(
                    health(
                        PrivacySourceKind.PHONE_BLE,
                        SourceHealthState.PAUSED,
                        message = "Paused in detection settings",
                    ),
                ),
                findings = emptyList(),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state = state,
                    actions = PrivacyActions(
                        onEnablePhoneScan = { activations++ },
                    ),
                )
            }
        }

        compose.onNodeWithTag("privacy_source_status").performClick()
        compose.onNodeWithText("Turn on").assertHasClickAction().performClick()
        compose.runOnIdle { assertEquals(1, activations) }
        compose.onNodeWithText("Phone privacy scan").assertDoesNotExist()
        compose.onNodeWithText("BLE Remote ID").assertDoesNotExist()
    }

    @Test
    fun promptablePhonePermissionOffersGrantAccessInsteadOfDeadEndSettings() {
        var permissionRequests = 0
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(
                    health(
                        PrivacySourceKind.PHONE_BLE,
                        SourceHealthState.PERMISSION_BLOCKED,
                        message = "Nearby-device access is required",
                        recoveryLabel = "Grant permission",
                    ),
                    health(PrivacySourceKind.BACKEND, SourceHealthState.LIVE),
                ),
                findings = emptyList(),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state = state,
                    actions = PrivacyActions(
                        onResolveSourcePermission = { permissionRequests++ },
                    ),
                )
            }
        }

        compose.onNodeWithText("Grant access").assertHasClickAction().performClick()
        compose.runOnIdle { assertEquals(1, permissionRequests) }
        compose.onNodeWithText("Open settings").assertDoesNotExist()
    }

    @Test
    fun bluetoothRadioOffUsesPlatformRecoveryInsteadOfRetryingADeadScanner() {
        var platformRecoveries = 0
        var scannerRetries = 0
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(
                    health(
                        PrivacySourceKind.PHONE_BLE,
                        SourceHealthState.FAILED,
                        message = "Bluetooth is turned off",
                        recoveryLabel = "Turn on Bluetooth",
                    ),
                    health(PrivacySourceKind.BACKEND, SourceHealthState.LIVE),
                ),
                findings = emptyList(),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state = state,
                    actions = PrivacyActions(
                        onRecoverSource = { scannerRetries++ },
                        onTurnOnBluetooth = { platformRecoveries++ },
                    ),
                )
            }
        }

        compose.onNodeWithText("Turn on Bluetooth").assertHasClickAction().performClick()
        compose.runOnIdle {
            assertEquals(1, platformRecoveries)
            assertEquals(0, scannerRetries)
        }
    }

    @Test
    fun badgePermissionRecoveryReconnectsBadgeWithoutRequestingPhoneRadioAccess() {
        var badgeRecoveries = 0
        var phonePermissionRequests = 0
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(
                    health(
                        PrivacySourceKind.BADGE_USB,
                        SourceHealthState.PERMISSION_BLOCKED,
                        message = "Badge permission is required",
                        recoveryLabel = "Connect badge",
                    ),
                    health(PrivacySourceKind.BACKEND, SourceHealthState.LIVE),
                ),
                findings = emptyList(),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state = state,
                    actions = PrivacyActions(
                        onRecoverSource = { source ->
                            if (source == PrivacySourceKind.BADGE_USB) badgeRecoveries++
                        },
                        onResolveSourcePermission = { phonePermissionRequests++ },
                    ),
                )
            }
        }

        compose.onNodeWithText("Connect badge").assertHasClickAction().performClick()
        compose.runOnIdle {
            assertEquals(1, badgeRecoveries)
            assertEquals(0, phonePermissionRequests)
        }
    }

    @Test
    fun headerQualifiesZeroFindingsWhileSourcesAreStillResolving() {
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LOADING)),
                findings = emptyList(),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = false,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme { PrivacyContent(state, PrivacyActions()) }
        }

        compose.onNodeWithText("0 findings so far · sources still resolving").assertIsDisplayed()
        compose.onNodeWithText("0 current findings").assertDoesNotExist()
    }

    @Test
    fun noMatchesExplainsFiltersAndOffersARealReset() {
        var cleared = 0
        val state = projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE)),
                findings = listOf(finding(FindingSeverity.NEARBY, "phone", category = PrivacyCategory.SMART_SPEAKER)),
                threatCount = 0,
                alertEligible = emptyList(),
                initialResolutionComplete = true,
            ),
            filters = PrivacyFilterState(query = "camera"),
        )
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(
                    state,
                    PrivacyActions(onClearFilters = { cleared++ }),
                )
            }
        }

        compose.onNodeWithText("No matches for 1 active filters").assertIsDisplayed()
        compose.onNodeWithText("Clear filters").assertHasClickAction().performClick()
        compose.runOnIdle { assertEquals(1, cleared) }
    }

    @Test
    fun venueBeaconsStartCollapsedAndExpandWithWorkingDeviceActions() {
        val beacon = finding(FindingSeverity.INFO, "beacon", fullActions = true, category = PrivacyCategory.VENUE_BEACON).copy(title = "iBeacon Lobby")
        val tracker = finding(FindingSeverity.NEARBY, "findhub").copy(
            title = "Find Hub accessory", evidence = "Google Find Hub advertisement · normal advertising mode",
            limitation = "This broadcast does not establish ownership or following.",
        )
        val security = finding(FindingSeverity.AWARENESS, "wifi", category = PrivacyCategory.WIFI_SECURITY).copy(
            title = "Weak Wi-Fi security", evidence = "Old router advertises TKIP", limitation = "Advertised settings do not confirm an attack.",
            source = PrivacySourceKind.WIFI_ANALYSIS,
            observationKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, "wifi"),
            routableKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, "wifi"),
        )
        val initial = projectPrivacyUiState(PrivacyCurrentState(
            sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE)),
            findings = listOf(security, tracker, beacon), threatCount = 1, alertEligible = emptyList(), initialResolutionComplete = true,
        ))
        val state = mutableStateOf(initial)
        var ignored: PrivacyFinding? = null
        var tracked: PrivacyFinding? = null
        var opened: PrivacyFindingKey? = null
        compose.setContent {
            FriendOrFoeTheme {
                PrivacyContent(state.value, PrivacyActions(
                    onIgnore = { ignored = it }, onTrack = { tracked = it }, onOpenDetails = { opened = it },
                ))
            }
        }
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_beacons"))
        compose.onNodeWithText("Venue beacons · 1").assertIsDisplayed()
        compose.onNodeWithTag("finding_beacon").assertDoesNotExist()
        saveBeaconScreenshot("beacons-collapsed.png")
        compose.onNodeWithTag("privacy_tree_beacons").performClick()
        compose.onNodeWithTag("finding_beacon").assertDoesNotExist()
        clickBranch("ibeacon")
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_beacon_ignore"))
        compose.onNodeWithTag("finding_beacon_ignore").performClick()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_beacon_track"))
        compose.onNodeWithTag("finding_beacon_track").performClick()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_beacon_details"))
        compose.onNodeWithTag("finding_beacon_details").performClick()
        compose.runOnIdle {
            assertEquals(beacon, ignored)
            assertEquals(beacon, tracked)
            assertEquals(beacon.routableKey, opened)
            // A normal scan update must not collapse an expanded group.
            state.value = initial.copy(visibleFindings = initial.visibleFindings.map { it.copy(signalDbm = -44) })
        }
        compose.onNodeWithTag("finding_beacon").assertIsDisplayed()
        saveBeaconScreenshot("beacons-expanded.png")
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_beacons"))
        compose.onNodeWithTag("privacy_tree_beacons").performClick()
        compose.onNodeWithTag("finding_beacon").assertDoesNotExist()
    }

    @Test
    fun beaconSearchShowsMatchingGroupAndClearsWhenIgnored() {
        val beacon = finding(FindingSeverity.INFO, "beacon", category = PrivacyCategory.VENUE_BEACON).copy(title = "iBeacon Lobby")
        val current = PrivacyCurrentState(emptyList(), listOf(beacon), 0, emptyList(), true)
        val state = mutableStateOf(projectPrivacyUiState(current, PrivacyFilterState(query = "lobby")))
        compose.setContent { FriendOrFoeTheme { PrivacyContent(state.value, PrivacyActions()) } }
        // Matching branches open automatically while searching.
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasText("iBeacon Lobby"))
        compose.onNodeWithText("iBeacon Lobby").assertIsDisplayed()
        compose.runOnIdle { state.value = projectPrivacyUiState(current.copy(findings = emptyList())) }
        compose.onNodeWithTag("privacy_tree_beacons").assertDoesNotExist()
    }

    @Test
    fun trackerFamiliesAreSeparateAndAttentionStaysVisibleAboveTheTree() {
        val rows = listOf(
            finding(FindingSeverity.AWARENESS, "attention").copy(title = "Repeated tracker encounter"),
            finding(FindingSeverity.INFO, "airtag", category = PrivacyCategory.FINDMY).copy(title = "AirTag (Near Owner)"),
            finding(FindingSeverity.NEARBY, "tile").copy(title = "BLE Tracker", evidence = "Tile • uuid:feed"),
            finding(FindingSeverity.INFO, "beacon", category = PrivacyCategory.VENUE_BEACON).copy(title = "iBeacon Lobby"),
            finding(FindingSeverity.INFO, "eddy", category = PrivacyCategory.VENUE_BEACON).copy(title = "Eddystone Beacon"),
        )
        val state = projectPrivacyUiState(PrivacyCurrentState(emptyList(), rows, 1, emptyList(), true))
        compose.setContent { FriendOrFoeTheme { PrivacyContent(state, PrivacyActions()) } }
        compose.onNodeWithTag("finding_attention").assertIsDisplayed()
        compose.onNodeWithTag("finding_airtag").assertDoesNotExist()
        saveBeaconScreenshot("device-tree-collapsed.png")
        clickBranch("trackers")
        clickBranch("beacons")
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_trackers"))
        saveBeaconScreenshot("device-tree-families.png")
        compose.onNodeWithTag("finding_tile").assertDoesNotExist()
        clickBranch("find_my")
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_airtag"))
        compose.onNodeWithTag("finding_airtag").assertIsDisplayed()
        compose.onNodeWithTag("finding_tile").assertDoesNotExist()
        clickBranch("find_my")
        compose.onNodeWithTag("finding_airtag").assertDoesNotExist()
    }

    @Test
    fun expandedTrackerBranchesSurviveUpdatesAndStateRestoration() {
        val restorer = StateRestorationTester(compose)
        val tracker = finding(FindingSeverity.NEARBY, "tracker").copy(title = "Find Hub accessory")
        val current = PrivacyCurrentState(emptyList(), listOf(tracker), 0, emptyList(), true)
        val state = mutableStateOf(projectPrivacyUiState(current))
        restorer.setContent { FriendOrFoeTheme { PrivacyContent(state.value, PrivacyActions()) } }
        clickBranch("trackers")
        clickBranch("find_hub")
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_tracker"))
        val bounds = compose.onNodeWithTag("finding_tracker").fetchSemanticsNode().boundsInRoot
        compose.runOnIdle { state.value = projectPrivacyUiState(current.copy(findings = listOf(tracker.copy(signalDbm = -40)))) }
        assertEquals(bounds, compose.onNodeWithTag("finding_tracker").fetchSemanticsNode().boundsInRoot)
        restorer.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("finding_tracker").assertIsDisplayed()
    }

    @Test
    fun expandAllSupportsLongListsAndCollapseAllHidesTheirLeaves() {
        val rows = (1..250).map { finding(FindingSeverity.NEARBY, "tile-$it").copy(title = "Tile $it") }
        var opened: PrivacyFindingKey? = null
        val state = projectPrivacyUiState(PrivacyCurrentState(emptyList(), rows, 0, emptyList(), true))
        compose.setContent { FriendOrFoeTheme { PrivacyContent(state, PrivacyActions(onOpenDetails = { opened = it })) } }
        compose.onNodeWithTag("privacy_tree_toggle_all").performClick()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_tile-250_details"))
        compose.onNodeWithTag("finding_tile-250_details").performClick()
        compose.runOnIdle { assertEquals(rows.last().routableKey, opened) }
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_toggle_all"))
        compose.onNodeWithText("Collapse all").performClick()
        compose.onNodeWithTag("privacy_tree_tile").assertDoesNotExist()
        compose.onNodeWithTag("finding_tile-1").assertDoesNotExist()
        compose.onNodeWithText("Trackers · 250").assertIsDisplayed()
    }

    @Test
    fun searchingOpensTrackerFamilyAndClearingSearchRestoresCompactTree() {
        val tracker = finding(FindingSeverity.NEARBY, "tracker").copy(title = "BLE Tracker", evidence = "Samsung • uuid:fd5a")
        val beacon = finding(FindingSeverity.INFO, "beacon", category = PrivacyCategory.VENUE_BEACON).copy(title = "iBeacon")
        val current = PrivacyCurrentState(emptyList(), listOf(tracker, beacon), 0, emptyList(), true)
        val state = mutableStateOf(projectPrivacyUiState(current))
        compose.setContent { FriendOrFoeTheme { PrivacyContent(state.value, PrivacyActions()) } }
        compose.runOnIdle { state.value = projectPrivacyUiState(current, PrivacyFilterState(query = "SmartTag")) }
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("finding_tracker"))
        compose.onNodeWithTag("finding_tracker").assertIsDisplayed()
        compose.onNodeWithTag("privacy_tree_beacons").assertDoesNotExist()
        compose.runOnIdle { state.value = projectPrivacyUiState(current) }
        compose.onNodeWithTag("finding_tracker").assertDoesNotExist()
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_beacons"))
        compose.onNodeWithTag("privacy_tree_beacons").assertIsDisplayed()
    }

    private fun clickBranch(key: String) {
        compose.onNodeWithTag("privacy_content").performScrollToNode(hasTestTag("privacy_tree_$key"))
        compose.onNodeWithTag("privacy_tree_$key").performClick()
    }

    private fun saveBeaconScreenshot(name: String) {
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        File(InstrumentationRegistry.getInstrumentation().targetContext.filesDir, name)
            .outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun stateWithAllSeverities(): PrivacyUiState {
        val rows = listOf(
            finding(FindingSeverity.CRITICAL, "critical", fullActions = true),
            finding(FindingSeverity.AWARENESS, "awareness"),
            finding(FindingSeverity.NEARBY, "nearby", category = PrivacyCategory.SMART_SPEAKER),
            finding(FindingSeverity.INFO, "info", category = PrivacyCategory.APPLE_CONTINUITY),
        )
        return projectPrivacyUiState(
            PrivacyCurrentState(
                sources = listOf(health(PrivacySourceKind.PHONE_BLE, SourceHealthState.LIVE)),
                findings = rows,
                threatCount = 2,
                alertEligible = listOf(rows.first()),
                initialResolutionComplete = true,
            ),
        )
    }

    private fun finding(
        severity: FindingSeverity,
        id: String,
        fullActions: Boolean = false,
        category: PrivacyCategory = PrivacyCategory.BLE_TRACKER,
    ) = PrivacyFinding(
        displayId = id,
        observationKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "observation:$id"),
        source = PrivacySourceKind.PHONE_BLE,
        stableSourceId = "stable:$id",
        routableKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "mac:$id"),
        title = if (category == PrivacyCategory.APPLE_CONTINUITY) {
            "AirPods connection/activity nearby"
        } else {
            "Finding $id"
        },
        evidence = "Observed by this phone",
        limitation = if (category == PrivacyCategory.APPLE_CONTINUITY) {
            "Live Listen and microphone use cannot be determined from BLE."
        } else {
            null
        },
        category = category,
        severity = severity,
        ownership = Ownership.UNKNOWN,
        signalDbm = -61,
        firstSeenWallMs = null,
        lastSeenWallMs = 1_000L,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = true,
        capabilities = PrivacyCapabilities(
            canIgnore = true,
            canTrack = fullActions,
            canOpenDirectionSweep = fullActions,
        ),
    )

    private fun health(
        source: PrivacySourceKind,
        state: SourceHealthState,
        message: String? = null,
        recoveryLabel: String? = null,
    ) = PrivacySourceHealth(
        source = source,
        state = state,
        lastSuccessElapsedMs = 1_000L,
        lastSuccessWallMs = 1_000L,
        recoveryLabel = recoveryLabel,
        message = message,
    )
}

private class RecordingActions {
    fun asActions() = PrivacyActions()
}

package com.friendorfoe.presentation.badge

import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.hasAnyDescendant
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import com.friendorfoe.data.badge.BadgeBleControlStatus
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeConfigReadback
import com.friendorfoe.data.badge.BadgeConnectionEvidence
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeControlAcknowledgement
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeDebugBridgeEvidence
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeDisplayLane
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeMinimumProximity
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeNetworkModeReadback
import com.friendorfoe.data.badge.BadgeReportingStatus
import com.friendorfoe.data.badge.BadgeRuntimeNetworkMode
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThreatCounts
import com.friendorfoe.data.badge.BadgeTransport
import com.friendorfoe.data.badge.firmwareHash
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class BadgeScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun showsExactThemeCodesAndNoSimulationOrInactiveEditors() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(state = editableBadgeState(), actions = noOpBadgeActions())
            }
        }

        mapOf(
            "drone" to ("Drone" to "0xFEA0 · 65184"),
            "meta" to ("Meta" to "0xF833 · 63539"),
            "tracker" to ("Tracker" to "0xF81F · 63519"),
            "flock" to ("Flock" to "0xA81F · 43039"),
            "wifi_attack" to ("Wi-Fi Attack" to "0x07FF · 2047"),
            "clear" to ("Clear" to "0x2F65 · 12133"),
        ).forEach { (key, labelAndCode) ->
            compose.onNode(
                hasTestTag("badge_accent_$key") and
                    hasAnyDescendant(hasText(labelAndCode.first)) and
                    hasAnyDescendant(hasText(key)) and
                    hasAnyDescendant(hasText(labelAndCode.second)),
                useUnmergedTree = true,
            ).assertExists()
        }
        listOf("Palette", "Brightness", "Priority", "LCD preview", "Badge simulation")
            .forEach { forbidden ->
                compose.onAllNodesWithText(
                    forbidden,
                    substring = true,
                    ignoreCase = true,
                    useUnmergedTree = true,
                ).assertCountEquals(0)
            }
        compose.onNodeWithContentDescription("Drone Ice color, 0x07FF").assertExists()
        compose.onNodeWithContentDescription("Color intensity").assertExists()
    }

    @Test
    fun swatchesBackgroundAndIntensityEditOnlyExactThemeFields() {
        val initial = BadgeTheme.firmwareDefaults()
        val actions = RecordingBadgeActions(initialTheme = initial)
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(editableBadgeState(), actions.asActions())
            }
        }

        compose.onNodeWithTag("badge_accent_drone_swatch_f800")
            .performScrollTo()
            .performClick()
        compose.onNodeWithTag("badge_backgrounds").performScrollTo()
        compose.onNodeWithTag("badge_background_dim")
            .performScrollTo()
            .assertIsDisplayed()
            .performClick()
        compose.onNodeWithTag("badge_intensity")
            .performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { setProgress ->
                assertTrue(setProgress(25f))
            }

        val edited = actions.currentTheme
        assertEquals(0xF800, edited.accents.getValue("drone"))
        assertEquals(initial.accents.getValue("meta"), edited.accents.getValue("meta"))
        assertEquals("dim", edited.background)
        assertEquals(25, edited.intensity)
        compose.onNodeWithText("Black — dark — 0x0000").assertExists()
        compose.onNodeWithText("Dim — dim — 0x1082").assertExists()
        compose.onNodeWithText("Blue-black — scanline — 0x0108").assertExists()
        compose.onNodeWithText(
            "Color intensity changes RGB565 output; the badge backlight is fixed.",
        ).assertExists()
    }

    @Test
    fun networkChoicesKeepPersistedUsbOnlyDistinctFromRuntimeOff() {
        val actions = RecordingBadgeActions()
        val state = editableBadgeState().copy(
            appliedNetworkMode = BadgeNetworkMode.USB_ONLY,
            draftNetworkMode = BadgeNetworkMode.USB_ONLY,
            applyState = BadgeApplyState(
                network = BadgeSectionApplyResult(
                    section = BadgeConfigSection.NETWORK_MODE,
                    phase = BadgeApplyPhase.ACKNOWLEDGED,
                    message = "Badge accepted usb_only",
                ),
            ),
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, actions.asActions()) }
        }

        compose.onNodeWithText("USB only — usb_only").assertExists()
        compose.onNodeWithText("Local AP — local_ap").assertExists()
        compose.onNodeWithText("Backend — backend").assertExists()
        compose.onNodeWithText("Applied persisted mode: usb_only").assertExists()
        compose.onNodeWithText("Device runtime mode: off").assertExists()
        compose.onNodeWithTag("badge_network_backend").performScrollTo().performClick()

        assertEquals(listOf(BadgeNetworkMode.BACKEND), actions.networkModes)
    }

    @Test
    fun rendersAllThirteenFirmwareRulesWithoutExposingPriority() {
        val actions = RecordingBadgeActions(initialPolicy = BadgeDisplayPolicy.firmwareDefaults())
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(editableBadgeState(), actions.asActions())
            }
        }

        BadgeDisplayPolicy.classOrder.forEach { key ->
            compose.onNodeWithTag("badge_rule_$key", useUnmergedTree = true).assertExists()
            compose.onNode(
                hasTestTag("badge_rule_$key") and hasAnyDescendant(hasText(key)),
                useUnmergedTree = true,
            ).assertExists()
        }
        compose.onNodeWithTag("badge_rule_camera_toggle")
            .performScrollTo()
            .performClick()

        val camera = actions.currentPolicy.classes.getValue("camera")
        assertEquals(false, camera.enabled)
        assertEquals(BadgeDisplayLane.OFF, camera.lane)
        assertEquals(65, camera.priority)
        compose.onAllNodesWithText("Priority", substring = true, ignoreCase = true)
            .assertCountEquals(0)
        listOf("Firmware upload", "Choose firmware", "Hardware preview", "LCD simulation")
            .forEach { compose.onNodeWithText(it, substring = true, ignoreCase = true).assertDoesNotExist() }
    }

    @Test
    fun rapidPolicyEditsUseLatestDraftAndPreserveInvisiblePriorities() {
        val actions = RecordingBadgeActions(initialPolicy = BadgeDisplayPolicy.firmwareDefaults())
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(editableBadgeState(), actions.asActions())
            }
        }

        compose.onNodeWithTag("badge_rule_camera_toggle").performScrollTo().performClick()
        compose.onNodeWithTag("badge_rule_tracker").performScrollTo()
        compose.onNodeWithTag("badge_rule_tracker_lane_top").performScrollTo().performClick()
        compose.onNodeWithTag("badge_rule_tracker_proximity_close")
            .performScrollTo()
            .performClick()

        val camera = actions.currentPolicy.classes.getValue("camera")
        val tracker = actions.currentPolicy.classes.getValue("tracker")
        assertEquals(false, camera.enabled)
        assertEquals(BadgeDisplayLane.OFF, camera.lane)
        assertEquals(65, camera.priority)
        assertEquals(BadgeDisplayLane.TOP, tracker.lane)
        assertEquals(BadgeMinimumProximity.CLOSE, tracker.minProximity)
        assertEquals(70, tracker.priority)
    }

    @Test
    fun applyInFlightLocksEveryEditorAndRuleTogglesHaveSpecificNames() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(
                    editableBadgeState().copy(applyInFlight = true),
                    noOpBadgeActions(),
                )
            }
        }

        compose.onNodeWithTag("badge_accent_drone_swatch_f800").assertIsNotEnabled()
        compose.onNodeWithTag("badge_background_dim").assertIsNotEnabled()
        compose.onNodeWithTag("badge_intensity").assertIsNotEnabled()
        compose.onNodeWithTag("badge_rule_drone_toggle").assertIsNotEnabled()
        compose.onNodeWithTag("badge_network_backend").assertIsNotEnabled()
        compose.onNodeWithTag("badge_apply").assertIsNotEnabled()
        compose.onNodeWithTag("badge_open_recovery").assertIsNotEnabled()
        compose.onNodeWithText("Finish applying changes before opening Recovery.").assertExists()
        compose.onNodeWithContentDescription("Drone display rule").assertExists()
    }

    @Test
    fun pendingRecoveryLocksConfigurationAndApplyAfterLeavingRecoveryScreen() {
        val base = editableBadgeState()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(
                    base.copy(
                        draftTheme = requireNotNull(base.draftTheme).copy(intensity = 75),
                        recovery = BadgeRecoveryState(
                            action = BadgeRecoveryAction.REBOOT,
                            targetId = "badge-7",
                            targetTransportGeneration = 7,
                            phase = BadgeRecoveryPhase.PENDING,
                        ),
                    ),
                    noOpBadgeActions(),
                )
            }
        }

        compose.onNodeWithTag("badge_accent_drone_swatch_f800").assertIsNotEnabled()
        compose.onNodeWithTag("badge_background_dim").assertIsNotEnabled()
        compose.onNodeWithTag("badge_intensity").assertIsNotEnabled()
        compose.onNodeWithTag("badge_rule_drone_toggle").assertIsNotEnabled()
        compose.onNodeWithTag("badge_network_backend").assertIsNotEnabled()
        compose.onNodeWithTag("badge_nav_next").assertIsNotEnabled()
        compose.onNodeWithText("Use firmware defaults").assertIsNotEnabled()
        compose.onNodeWithText("Revert draft").assertIsNotEnabled()
        compose.onNodeWithTag("badge_apply").assertIsNotEnabled()
        compose.onNodeWithText(
            "A Recovery command is pending. Wait for its result before applying changes.",
        ).assertExists()
    }

    @Test
    fun anOffRuleHidesLaneAndProximityEditors() {
        val current = BadgeDisplayPolicy.firmwareDefaults()
        val policy = current.copy(
            classes = current.classes + (
                "hid" to current.classes.getValue("hid").copy(
                    enabled = false,
                    lane = BadgeDisplayLane.OFF,
                )
                ),
        )
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(
                    editableBadgeState().copy(appliedPolicy = policy, draftPolicy = policy),
                    noOpBadgeActions(),
                )
            }
        }

        compose.onNodeWithTag("badge_rule_hid_lanes").assertDoesNotExist()
        compose.onNodeWithTag("badge_rule_hid_proximity").assertDoesNotExist()
        compose.onNodeWithText(
            "Off is not an absolute suppression guarantee; firmware safety rules may still show high-confidence evidence.",
        ).assertExists()
        compose.onNodeWithText(
            "Close ≥ -60 dBm · Near ≥ -76 dBm · Present < -76 dBm",
        ).assertExists()
    }

    @Test
    fun oneSharedApplyAreaShowsDirtyFieldsAndIndependentResults() {
        val actions = RecordingBadgeActions()
        val applied = BadgeTheme.firmwareDefaults()
        val draft = applied.copy(intensity = 75)
        val state = editableBadgeState().copy(
            appliedTheme = applied,
            draftTheme = draft,
            applyState = BadgeApplyState(
                theme = BadgeSectionApplyResult(
                    BadgeConfigSection.THEME,
                    BadgeApplyPhase.VERIFIED,
                    "Readback matches",
                ),
                policy = BadgeSectionApplyResult(
                    BadgeConfigSection.DISPLAY_POLICY,
                    BadgeApplyPhase.FAILED,
                    "Policy command failed",
                ),
                network = BadgeSectionApplyResult(
                    BadgeConfigSection.NETWORK_MODE,
                    BadgeApplyPhase.APPLIED_ON_BADGE,
                    "Waiting for scanners",
                ),
            ),
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, actions.asActions()) }
        }

        compose.onAllNodesWithTag("badge_apply").assertCountEquals(1)
        compose.onNodeWithText("Unsaved changes: LCD accent colors").assertExists()
        compose.onNodeWithTag("badge_result_theme")
            .assert(hasAnyDescendant(hasText("Verified")))
        compose.onNodeWithTag("badge_result_policy")
            .assert(hasAnyDescendant(hasText("Failed")))
        compose.onNodeWithTag("badge_result_network")
            .assert(hasAnyDescendant(hasText("Applied on badge")))
        compose.onNodeWithText("Use firmware defaults").performScrollTo().performClick()
        compose.onNodeWithText("Revert draft")
            .performScrollTo()
            .assertIsEnabled()
            .performClick()
        compose.onNodeWithTag("badge_apply")
            .performScrollTo()
            .assertIsEnabled()
            .performClick()
        compose.waitForIdle()

        assertEquals(1, actions.defaultsCount)
        assertEquals(1, actions.revertCount)
        assertEquals(1, actions.applyCount)
    }

    @Test
    fun themeOnlyReadbackIsPresentedAsEditableWithoutImplyingOtherSectionsMatch() {
        val base = editableBadgeState()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(
                    base.copy(
                        appliedPolicy = null,
                        draftPolicy = null,
                        appliedNetworkMode = null,
                        draftNetworkMode = null,
                    ),
                    noOpBadgeActions(),
                )
            }
        }

        compose.onNodeWithText("Loaded configuration: LCD accent colors. No unsaved changes.")
            .assertExists()
        compose.onNodeWithText("No editable draft yet", substring = true).assertDoesNotExist()
        compose.onNodeWithText("Draft matches the badge.").assertDoesNotExist()
    }

    @Test
    fun disconnectedApplyReasonRequestsConnectionInsteadOfAChange() {
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(BadgeUiState(), noOpBadgeActions()) }
        }

        compose.onNodeWithText("Connect and refresh badge status before applying.")
            .assertExists()
        compose.onNodeWithText("Make a change to enable Apply changes.").assertDoesNotExist()
    }

    @Test
    fun staleUnknownAndUncertifiedConnectionsExplainDisabledControls() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeContent(staleUncertifiedBadgeState(), noOpBadgeActions())
            }
        }

        compose.onNodeWithText("Status is stale").assertIsDisplayed()
        compose.onNodeWithText("Theme readback is unavailable").assertExists()
        compose.onNodeWithText("Display policy readback is unavailable").assertExists()
        compose.onNodeWithText("Verified direct USB is required").assertExists()
        compose.onNodeWithTag("badge_apply").assertIsNotEnabled()
    }

    @Test
    fun liveBuildWithoutMutationCertificationDoesNotSuggestARefreshLoop() {
        val base = editableBadgeState()
        val unknownSupport = BadgeCapability.entries.associateWith {
            BadgeCapabilitySupport.UNKNOWN
        }
        val state = base.copy(
            connection = base.connection.copy(releaseCertifiedMutations = emptySet()),
            capabilities = unknownSupport,
            displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
                BadgeCapabilitySupport.UNKNOWN
            },
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, noOpBadgeActions()) }
        }

        compose.onNodeWithText(
            "Theme changes are not release-verified for USB serial in this app build.",
        ).assertExists()
        compose.onNodeWithText(
            "Display policy changes are not release-verified for USB serial in this app build.",
        ).assertExists()
        compose.onNodeWithText(
            "Network mode changes are not release-verified for USB serial in this app build.",
        ).assertExists()
        compose.onNodeWithText(
            "Next is not release-verified for USB serial in this app build.",
        ).assertExists()
        compose.onNodeWithTag("badge_apply").assertIsNotEnabled()
        compose.onNodeWithText("No changes to apply.").assertExists()
        compose.onNodeWithText("Make a change to enable Apply changes.").assertDoesNotExist()
        compose.onAllNodesWithText(
            "Refresh verified badge status before changing",
            substring = true,
        ).assertCountEquals(0)
    }

    @Test
    fun lcdButtonsSerializeAllThreeExactLowercaseActions() {
        val actions = RecordingBadgeActions()
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(editableBadgeState(), actions.asActions()) }
        }

        listOf("next", "detail", "back").forEach { wireValue ->
            compose.onNodeWithTag("badge_nav_$wireValue").performScrollTo().performClick()
        }

        assertEquals(
            listOf("next", "detail", "back"),
            actions.displayActions.map(BadgeDisplayAction::wireValue),
        )
    }

    @Test
    fun bleMtuSupportEnablesNextAndBackButTruthfullyDisablesDetail() {
        val state = editableBadgeState().copy(
            connection = editableBadgeState().connection.copy(
                transport = BadgeTransport.BLE_GATT,
                targetId = "badge-ble",
                negotiatedBleMtu = 41,
                usbCandidateCount = null,
                exactEspressifVendorMatch = false,
                serialInterfaceReadable = false,
                fofBleServicePresent = true,
                bleStatusCharacteristicPresent = true,
                bleControlCharacteristicPresent = true,
                bleBonded = true,
                bleEncrypted = true,
            ),
            displayNavigationSupport = mapOf(
                BadgeDisplayAction.NEXT to BadgeCapabilitySupport.SUPPORTED,
                BadgeDisplayAction.DETAIL to BadgeCapabilitySupport.UNSUPPORTED,
                BadgeDisplayAction.BACK to BadgeCapabilitySupport.SUPPORTED,
            ),
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, noOpBadgeActions()) }
        }

        compose.onNodeWithTag("badge_nav_next").performScrollTo().assertIsEnabled()
        compose.onNodeWithTag("badge_nav_detail").assertIsNotEnabled()
        compose.onNodeWithTag("badge_nav_back").assertIsEnabled()
        compose.onNodeWithText("Detail needs a larger BLE MTU (connected at 41)")
            .assertExists()
    }

    @Test
    fun unbondedBleNavigationNamesTheMissingBondInsteadOfSuggestingRefresh() {
        val base = editableBadgeState()
        val state = base.copy(
            connection = completeBleConnection(base, bonded = false, encrypted = false),
            displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
                BadgeCapabilitySupport.UNKNOWN
            },
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, noOpBadgeActions()) }
        }

        compose.onNodeWithText("Next requires a bonded BLE connection.").assertExists()
    }

    @Test
    fun unencryptedBleNavigationNamesEncryptionInsteadOfSuggestingRefresh() {
        val base = editableBadgeState()
        val state = base.copy(
            connection = completeBleConnection(base, bonded = true, encrypted = false),
            displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
                BadgeCapabilitySupport.UNKNOWN
            },
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, noOpBadgeActions()) }
        }

        compose.onNodeWithText("Next requires an encrypted BLE connection.").assertExists()
    }

    @Test
    fun debugBridgeMutationErrorIsReportedInsteadOfSuggestingRefresh() {
        val base = editableBadgeState()
        val unknownSupport = BadgeCapability.entries.associateWith {
            BadgeCapabilitySupport.UNKNOWN
        }
        val state = base.copy(
            connection = base.connection.copy(
                transport = BadgeTransport.DEBUG_BRIDGE,
                usbCandidateCount = null,
                exactEspressifVendorMatch = false,
                serialInterfaceReadable = false,
                debugBridgeSerialPort = "/dev/cu.usbmodem1",
                debugPhysicalStatusAtElapsedMs = 1_000,
                debugBridgeLastError = "serial write failed",
            ),
            capabilities = unknownSupport,
            displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
                BadgeCapabilitySupport.UNKNOWN
            },
        )
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(state, noOpBadgeActions()) }
        }

        compose.onNodeWithText(
            "Theme changes are blocked because the debug bridge reports: serial write failed.",
        ).assertExists()
        compose.onNodeWithText(
            "Next is blocked because the debug bridge reports: serial write failed.",
        ).assertExists()
    }

    @Test
    fun diagnosticsAreReadOnlyAndExposeDeviceReportedEvidence() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeDiagnosticsContent(
                    state = editableBadgeState(),
                    onBack = {},
                    onRefresh = {},
                )
            }
        }

        listOf(
            "Firmware 0.64.65",
            "Transport USB serial",
            "Theme hash 0x1234ABCD",
            "Policy hash 0x89ABCDEF",
            "Scanner 0 · healthy",
            "Stack uplink: 8192 B",
            "Internal heap free: 120000 B",
            "Target scanner-s3-combo",
            "OTA idle",
            "Last firmware error: none",
        ).forEach { value -> compose.onNodeWithText(value).assertExists() }
        compose.onNodeWithText("Upload", substring = true, ignoreCase = true).assertDoesNotExist()
    }

    @Test
    fun disconnectedStateIsAnHonestUsableConnectionCardNotEndlessLoading() {
        compose.setContent {
            FriendOrFoeTheme { BadgeContent(BadgeUiState(), noOpBadgeActions()) }
        }

        compose.onNodeWithText("No verified badge connected").assertIsDisplayed()
        compose.onNodeWithText("Reconnect").assertIsDisplayed()
        compose.onNodeWithText("Refresh status").assertIsDisplayed()
        compose.onNodeWithText("Checking for a verified", substring = true).assertDoesNotExist()
        compose.onNodeWithTag("badge_connection_progress").assertDoesNotExist()
    }

    @Test
    fun largeTextStacksPairedActionsInsteadOfClipping() {
        compose.setContent {
            FriendOrFoeTheme {
                CompositionLocalProvider(LocalDensity provides Density(3f, 2f)) {
                    BadgeContent(BadgeUiState(), noOpBadgeActions())
                }
            }
        }

        val reconnect = compose.onNodeWithText("Reconnect").fetchSemanticsNode().boundsInRoot
        val refresh = compose.onNodeWithText("Refresh status").fetchSemanticsNode().boundsInRoot
        assertTrue(refresh.top > reconnect.bottom)
    }
}

internal class RecordingBadgeActions(
    initialTheme: BadgeTheme = BadgeTheme.firmwareDefaults(),
    initialPolicy: BadgeDisplayPolicy = BadgeDisplayPolicy.firmwareDefaults(),
) {
    var currentTheme: BadgeTheme = initialTheme
    var currentPolicy: BadgeDisplayPolicy = initialPolicy
    val networkModes = mutableListOf<BadgeNetworkMode>()
    val displayActions = mutableListOf<BadgeDisplayAction>()
    val recoveryActions = mutableListOf<BadgeRecoveryAction>()
    var refreshCount = 0
    var reconnectCount = 0
    var defaultsCount = 0
    var revertCount = 0
    var applyCount = 0
    var diagnosticsCount = 0
    var recoveryCount = 0

    fun asActions() = BadgeActions(
        refresh = { refreshCount++ },
        reconnect = { reconnectCount++ },
        updateTheme = { transform -> currentTheme = transform(currentTheme) },
        updatePolicy = { transform -> currentPolicy = transform(currentPolicy) },
        updateNetworkMode = { networkModes += it },
        useDefaults = { defaultsCount++ },
        revert = { revertCount++ },
        apply = { applyCount++ },
        navigateDisplay = { displayActions += it },
        requestRecovery = { recoveryActions += it },
        openDiagnostics = { diagnosticsCount++ },
        openRecovery = { recoveryCount++ },
    )
}

internal fun noOpBadgeActions() = BadgeActions(
    refresh = {},
    reconnect = {},
    updateTheme = {},
    updatePolicy = {},
    updateNetworkMode = {},
    useDefaults = {},
    revert = {},
    apply = {},
    navigateDisplay = {},
    requestRecovery = {},
    openDiagnostics = {},
    openRecovery = {},
)

internal fun editableBadgeState(): BadgeUiState {
    val theme = BadgeTheme.firmwareDefaults()
    val policy = BadgeDisplayPolicy.firmwareDefaults()
    val receipt = 1_000L
    val status = BadgeControlStatus(
        version = "0.64.65",
        receivedAtElapsedMs = receipt,
        receivedAtWallClock = Instant.EPOCH,
        themeReadback = BadgeConfigReadback(theme, 0x1234_ABCD, null),
        policyReadback = BadgeConfigReadback(policy, 0x89AB_CDEF, null),
        networkModeReadback = BadgeNetworkModeReadback(BadgeNetworkMode.USB_ONLY, null),
        entities = emptyList(),
        scanners = listOf(
            BadgeScannerStatus(
                slot = 0,
                connected = true,
                health = "healthy",
                expectedScanProfile = "privacy",
                scanProfile = "privacy",
                displayPolicyHash = policy.firmwareHash(),
                displayPolicyAckHash = policy.firmwareHash(),
                firmwareState = "ready",
                targetVersion = "scanner-s3-combo",
                otaState = "idle",
                lastFirmwareError = "",
            ),
        ),
        displayState = BadgeDisplayState(
            active = true,
            title = "Nearby drone",
            detail = "Remote ID",
            focusIndex = 0,
            focusTotal = 2,
        ),
        debugBridge = BadgeDebugBridgeEvidence(null, null, null),
        reporting = BadgeReportingStatus(networkMode = "off", standalone = true),
        counts = BadgeThreatCounts(drone = 1, tracker = 1),
        bleControl = BadgeBleControlStatus(),
        safeMode = false,
        safeReason = "",
        resetReason = "power_on",
        crashCount = 0,
        recoveryMode = "normal",
        stackFreeBytes = linkedMapOf("uplink" to 8192),
        heapInternalFreeBytes = 120_000,
        heapInternalMinimumFreeBytes = 90_000,
        psramFreeBytes = 2_000_000,
    )
    val capabilities = BadgeCapability.entries.associateWith { BadgeCapabilitySupport.SUPPORTED }
    return BadgeUiState(
        connection = BadgeConnectionEvidence(
            transport = BadgeTransport.USB_SERIAL,
            transportGeneration = 7,
            phase = BadgeConnectionPhase.LIVE,
            lastValidStatusAtElapsedMs = receipt,
            protocolVersion = "1",
            targetId = "badge-7",
            usbCandidateCount = 1,
            exactEspressifVendorMatch = true,
            serialInterfaceReadable = true,
            releaseCertifiedMutations = BadgeCapability.entries.toSet(),
        ),
        capabilities = capabilities,
        displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
            BadgeCapabilitySupport.SUPPORTED
        },
        appliedTheme = theme,
        draftTheme = theme,
        appliedPolicy = policy,
        draftPolicy = policy,
        appliedNetworkMode = BadgeNetworkMode.USB_ONLY,
        draftNetworkMode = BadgeNetworkMode.USB_ONLY,
        controlStatus = status,
        recoveryAvailability = BadgeRecoveryAction.entries.associateWith {
            BadgeRecoveryAvailability(true, "Available over verified direct USB")
        },
    )
}

private fun completeBleConnection(
    base: BadgeUiState,
    bonded: Boolean,
    encrypted: Boolean,
): BadgeConnectionEvidence = base.connection.copy(
    transport = BadgeTransport.BLE_GATT,
    targetId = "badge-ble",
    usbCandidateCount = null,
    exactEspressifVendorMatch = false,
    serialInterfaceReadable = false,
    negotiatedBleMtu = 64,
    fofBleServicePresent = true,
    bleStatusCharacteristicPresent = true,
    bleControlCharacteristicPresent = true,
    bleBonded = bonded,
    bleEncrypted = encrypted,
)

private fun staleUncertifiedBadgeState(): BadgeUiState {
    val base = editableBadgeState()
    val draftTheme = BadgeTheme.firmwareDefaults().copy(intensity = 75)
    return base.copy(
        connection = base.connection.copy(
            phase = BadgeConnectionPhase.STALE,
            releaseCertifiedMutations = emptySet(),
        ),
        capabilities = BadgeCapability.entries.associateWith { BadgeCapabilitySupport.UNKNOWN },
        displayNavigationSupport = BadgeDisplayAction.entries.associateWith {
            BadgeCapabilitySupport.UNKNOWN
        },
        appliedTheme = null,
        draftTheme = draftTheme,
        appliedPolicy = null,
        draftPolicy = base.draftPolicy,
        appliedNetworkMode = null,
        draftNetworkMode = base.draftNetworkMode,
        controlStatus = base.controlStatus?.copy(
            themeReadback = BadgeConfigReadback(null, null, "Theme readback is unavailable"),
            policyReadback = BadgeConfigReadback(
                null,
                null,
                "Display policy readback is unavailable",
            ),
            networkModeReadback = BadgeNetworkModeReadback(
                null,
                "Network mode readback is unavailable",
            ),
        ),
        recoveryAvailability = BadgeRecoveryAction.entries.associateWith {
            BadgeRecoveryAvailability(false, "Verified direct USB is required")
        },
    )
}

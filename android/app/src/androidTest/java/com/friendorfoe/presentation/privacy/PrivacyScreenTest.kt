package com.friendorfoe.presentation.privacy

import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
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
            compose.onNodeWithText(it).performScrollTo().assertIsDisplayed()
        }
        compose.onNodeWithTag("finding_critical_ignore")
            .performScrollTo()
            .assertHasClickAction()
        compose.onNodeWithTag("finding_critical_track")
            .performScrollTo()
            .assertHasClickAction()
        compose.onNodeWithTag("finding_critical_details")
            .performScrollTo()
            .assertHasClickAction()
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
                findings = listOf(finding(FindingSeverity.NEARBY, "phone")),
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
                findings = listOf(finding(FindingSeverity.NEARBY, "phone")),
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

    private fun stateWithAllSeverities(): PrivacyUiState {
        val rows = listOf(
            finding(FindingSeverity.CRITICAL, "critical", fullActions = true),
            finding(FindingSeverity.AWARENESS, "awareness"),
            finding(FindingSeverity.NEARBY, "nearby"),
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

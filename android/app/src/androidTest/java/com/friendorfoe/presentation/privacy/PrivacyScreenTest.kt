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

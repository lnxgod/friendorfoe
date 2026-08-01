package com.friendorfoe.presentation.badge

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import com.friendorfoe.presentation.theme.FriendOrFoeTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class BadgeRecoveryScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun unsupportedRecoveryIsExplainedAndCannotSend() {
        val actions = RecordingRecoveryActions()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    state = recoveryState(rebootEnabled = false, bootloaderEnabled = false),
                    actions = actions.asActions(),
                )
            }
        }

        compose.onNodeWithText("Verified direct USB is required").assertIsDisplayed()
        compose.onNodeWithTag("recovery_reboot").assertIsNotEnabled()
        compose.onNodeWithTag("recovery_bootloader").assertIsNotEnabled()
        assertTrue(actions.confirmed.isEmpty())
        assertTrue(actions.requested.isEmpty())
    }

    @Test
    fun confirmationNamesTargetAndCancelSendsNothing() {
        val actions = RecordingRecoveryActions()
        var state by mutableStateOf(
            recoveryState(targetId = "badge-7", rebootEnabled = true),
        )
        val recorderActions = actions.asActions()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    state,
                    recorderActions.copy(
                        request = { action ->
                            recorderActions.request(action)
                            state = state.copy(
                                recovery = BadgeRecoveryState(
                                    action = action,
                                    targetId = "badge-7",
                                    targetTransportGeneration = 7,
                                    phase = BadgeRecoveryPhase.CONFIRMING,
                                ),
                            )
                        },
                    ),
                )
            }
        }

        compose.onNodeWithTag("recovery_reboot").performClick()
        compose.onNodeWithText("Reboot badge-7?").assertIsDisplayed()
        compose.onNodeWithText("Cancel").performClick()

        assertEquals(listOf(BadgeRecoveryAction.REBOOT), actions.requested)
        assertTrue(actions.confirmed.isEmpty())
        assertEquals(1, actions.cancelCount)
    }

    @Test
    fun reducerRejectionNeverShowsAnOptimisticRecoveryConfirmation() {
        val actions = RecordingRecoveryActions()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(targetId = "badge-7", rebootEnabled = true),
                    actions.asActions(),
                )
            }
        }

        compose.onNodeWithTag("recovery_reboot").performClick()

        assertEquals(listOf(BadgeRecoveryAction.REBOOT), actions.requested)
        compose.onNodeWithText("Reboot badge-7?").assertDoesNotExist()
        compose.onNodeWithText("Confirm").assertDoesNotExist()
    }

    @Test
    fun confirmingBootloaderNamesTheSamePhysicalTargetAndConfirmsOnce() {
        val actions = RecordingRecoveryActions()
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(
                        targetId = "badge-7",
                        bootloaderEnabled = true,
                        recovery = BadgeRecoveryState(
                            action = BadgeRecoveryAction.ENTER_BOOTLOADER,
                            targetId = "badge-7",
                            targetTransportGeneration = 7,
                            phase = BadgeRecoveryPhase.CONFIRMING,
                        ),
                    ),
                    actions.asActions(),
                )
            }
        }

        compose.onNodeWithText("Enter bootloader on badge-7?").assertIsDisplayed()
        compose.onNodeWithText("Confirm").performClick()

        assertEquals(listOf(Unit), actions.confirmed)
    }

    @Test
    fun pendingDisablesBothRecoveryControls() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(recoveryPendingState(targetId = "badge-7"), noOpRecoveryActions())
            }
        }

        compose.onNodeWithTag("recovery_reboot").assertIsNotEnabled()
        compose.onNodeWithTag("recovery_bootloader").assertIsNotEnabled()
        compose.onNodeWithText("Waiting for badge acknowledgement…").assertIsDisplayed()
    }

    @Test
    fun acknowledgedRecoveryShowsReconnectGuidance() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryAcknowledgedState(targetId = "badge-7"),
                    noOpRecoveryActions(),
                )
            }
        }

        compose.onNodeWithText("Reboot acknowledged").assertIsDisplayed()
        compose.onNodeWithText("Reconnect and refresh badge status").assertIsDisplayed()
    }

    @Test
    fun unverifiedRecoveryShowsTimeoutAndReconnectGuidance() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(
                        targetId = "badge-7",
                        rebootEnabled = true,
                        recovery = BadgeRecoveryState(
                            action = BadgeRecoveryAction.REBOOT,
                            targetId = "badge-7",
                            targetTransportGeneration = 7,
                            phase = BadgeRecoveryPhase.NOT_VERIFIED,
                        ),
                    ),
                    noOpRecoveryActions(),
                )
            }
        }

        compose.onNodeWithText("Badge acknowledgement timed out").assertIsDisplayed()
        compose.onNodeWithText("Reconnect and refresh badge status").assertIsDisplayed()
    }

    @Test
    fun failedRecoveryShowsFailureAndReconnectGuidance() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(
                        targetId = "badge-7",
                        rebootEnabled = true,
                        recovery = BadgeRecoveryState(
                            action = BadgeRecoveryAction.REBOOT,
                            targetId = "badge-7",
                            targetTransportGeneration = 7,
                            phase = BadgeRecoveryPhase.FAILED,
                            message = "USB command failed",
                        ),
                    ),
                    noOpRecoveryActions(),
                )
            }
        }

        compose.onNodeWithText("USB command failed").assertIsDisplayed()
        compose.onNodeWithText("Reconnect and refresh badge status").assertIsDisplayed()
    }

    @Test
    fun ambiguousUsbTargetHasASeparateTruthfulReason() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(
                        targetId = null,
                        rebootEnabled = false,
                        bootloaderEnabled = false,
                        unavailableReason = "Connect exactly one badge over USB before recovery",
                    ),
                    noOpRecoveryActions(),
                )
            }
        }

        compose.onNodeWithText("Connect exactly one badge over USB before recovery")
            .assertIsDisplayed()
        compose.onNodeWithTag("recovery_reboot").assertIsNotEnabled()
        compose.onNodeWithTag("recovery_bootloader").assertIsNotEnabled()
    }

    @Test
    fun availableDirectUsbRecoveryControlsRemainExplicitAndEnabled() {
        compose.setContent {
            FriendOrFoeTheme {
                BadgeRecoveryContent(
                    recoveryState(
                        targetId = "badge-7",
                        rebootEnabled = true,
                        bootloaderEnabled = true,
                    ),
                    noOpRecoveryActions(),
                )
            }
        }

        compose.onNodeWithTag("recovery_reboot").assertIsEnabled()
        compose.onNodeWithTag("recovery_bootloader").assertIsEnabled()
        compose.onNodeWithText("This sends a command only; it does not upload firmware.")
            .assertIsDisplayed()
    }
}

private class RecordingRecoveryActions {
    val requested = mutableListOf<BadgeRecoveryAction>()
    val confirmed = mutableListOf<Unit>()
    var cancelCount = 0
    var refreshCount = 0

    fun asActions() = BadgeRecoveryActions(
        request = { requested += it },
        confirm = { confirmed += Unit },
        cancel = { cancelCount++ },
        refresh = { refreshCount++ },
    )
}

private fun noOpRecoveryActions() = BadgeRecoveryActions(
    request = {},
    confirm = {},
    cancel = {},
    refresh = {},
)

private fun recoveryState(
    targetId: String? = "badge-7",
    rebootEnabled: Boolean = false,
    bootloaderEnabled: Boolean = false,
    unavailableReason: String = "Verified direct USB is required",
    recovery: BadgeRecoveryState = BadgeRecoveryState(),
): BadgeUiState {
    val base = editableBadgeState()
    return base.copy(
        connection = base.connection.copy(targetId = targetId),
        recoveryAvailability = mapOf(
            BadgeRecoveryAction.REBOOT to BadgeRecoveryAvailability(
                enabled = rebootEnabled,
                reason = if (rebootEnabled) "Available over verified direct USB" else unavailableReason,
            ),
            BadgeRecoveryAction.ENTER_BOOTLOADER to BadgeRecoveryAvailability(
                enabled = bootloaderEnabled,
                reason = if (bootloaderEnabled) {
                    "Available over verified direct USB"
                } else {
                    unavailableReason
                },
            ),
        ),
        recovery = recovery,
    )
}

private fun recoveryPendingState(targetId: String) = recoveryState(
    targetId = targetId,
    rebootEnabled = true,
    bootloaderEnabled = true,
    recovery = BadgeRecoveryState(
        action = BadgeRecoveryAction.REBOOT,
        targetId = targetId,
        targetTransportGeneration = 7,
        phase = BadgeRecoveryPhase.PENDING,
        message = "Waiting for badge acknowledgement…",
    ),
)

private fun recoveryAcknowledgedState(targetId: String) = recoveryState(
    targetId = targetId,
    rebootEnabled = true,
    bootloaderEnabled = true,
    recovery = BadgeRecoveryState(
        action = BadgeRecoveryAction.REBOOT,
        targetId = targetId,
        targetTransportGeneration = 7,
        phase = BadgeRecoveryPhase.ACKNOWLEDGED,
        message = "Reboot acknowledged",
        reconnectGuidance = "Reconnect and refresh badge status",
    ),
)

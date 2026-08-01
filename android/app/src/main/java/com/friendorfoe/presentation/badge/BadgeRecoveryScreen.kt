package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

data class BadgeRecoveryActions(
    val request: (BadgeRecoveryAction) -> Unit,
    val confirm: () -> Unit,
    val cancel: () -> Unit,
    val refresh: () -> Unit,
)

@Composable
fun BadgeRecoveryRoute(
    onBack: () -> Unit,
    viewModel: BadgeViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack, modifier = Modifier.heightIn(min = 48.dp)) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
            }
            Text(
                "Badge recovery",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.SemiBold,
            )
        }
        BadgeRecoveryContent(
            state = state,
            actions = BadgeRecoveryActions(
                request = viewModel::requestRecovery,
                confirm = viewModel::confirmRecovery,
                cancel = viewModel::cancelRecovery,
                refresh = viewModel::refresh,
            ),
            modifier = Modifier.weight(1f),
        )
    }
}

@Composable
fun BadgeRecoveryContent(
    state: BadgeUiState,
    actions: BadgeRecoveryActions,
    modifier: Modifier = Modifier,
) {
    val confirmationAction = state.recovery.action.takeIf {
        state.recovery.phase == BadgeRecoveryPhase.CONFIRMING
    }
    val confirmationTarget = state.recovery.targetId.takeIf {
        state.recovery.phase == BadgeRecoveryPhase.CONFIRMING
    }
    val controlsBusy = state.mutationLocked

    Column(
        modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp)
            .testTag("screen_badge_recovery"),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            "Direct USB commands",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Recovery is intentionally narrow and bound to one verified physical USB target.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "This sends a command only; it does not upload firmware.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        state.connection.targetId?.let { target ->
            Text(
                "Physical target: $target",
                style = MaterialTheme.typography.labelLarge,
            )
        }

        BadgeSectionCard {
            Text("Reboot", style = MaterialTheme.typography.titleMedium)
            Text(
                "Restart the badge without changing its installed firmware.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val availability = state.recoveryAvailability.getValue(BadgeRecoveryAction.REBOOT)
            Button(
                onClick = {
                    actions.request(BadgeRecoveryAction.REBOOT)
                },
                enabled = availability.enabled && !controlsBusy,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp)
                    .testTag("recovery_reboot"),
            ) { Text("Reboot badge") }
        }

        BadgeSectionCard {
            Text("Bootloader", style = MaterialTheme.typography.titleMedium)
            Text(
                "Restart into the badge bootloader for a separate service workflow.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val availability = state.recoveryAvailability.getValue(
                BadgeRecoveryAction.ENTER_BOOTLOADER,
            )
            OutlinedButton(
                onClick = {
                    actions.request(BadgeRecoveryAction.ENTER_BOOTLOADER)
                },
                enabled = availability.enabled && !controlsBusy,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp)
                    .testTag("recovery_bootloader"),
            ) { Text("Enter bootloader") }
        }

        state.recoveryAvailability.values
            .filterNot(BadgeRecoveryAvailability::enabled)
            .map(BadgeRecoveryAvailability::reason)
            .filter(String::isNotBlank)
            .distinct()
            .forEach { reason ->
                Text(
                    reason,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
            }

        recoveryMessage(state.recovery)?.let { message ->
            Text(
                message,
                style = MaterialTheme.typography.bodyMedium,
                color = when (state.recovery.phase) {
                    BadgeRecoveryPhase.FAILED,
                    BadgeRecoveryPhase.NOT_VERIFIED,
                    -> MaterialTheme.colorScheme.error
                    BadgeRecoveryPhase.ACKNOWLEDGED -> MaterialTheme.colorScheme.secondary
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
        recoveryGuidance(state.recovery)?.let { guidance ->
            Text(
                guidance,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        OutlinedButton(
            onClick = actions.refresh,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
        ) { Text("Refresh badge status") }
    }

    if (confirmationAction != null && confirmationTarget != null) {
        AlertDialog(
            onDismissRequest = {
                actions.cancel()
            },
            title = {
                Text(confirmationTitle(confirmationAction, confirmationTarget))
            },
            text = {
                Text(
                    "Confirm the physical target before sending this direct USB command.",
                )
            },
            dismissButton = {
                TextButton(
                    onClick = {
                        actions.cancel()
                    },
                    modifier = Modifier.heightIn(min = 48.dp),
                ) { Text("Cancel") }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        actions.confirm()
                    },
                    modifier = Modifier.heightIn(min = 48.dp),
                ) { Text("Confirm") }
            },
        )
    }
}

private fun confirmationTitle(action: BadgeRecoveryAction, target: String): String = when (action) {
    BadgeRecoveryAction.REBOOT -> "Reboot $target?"
    BadgeRecoveryAction.ENTER_BOOTLOADER -> "Enter bootloader on $target?"
}

private fun recoveryMessage(recovery: BadgeRecoveryState): String? = when (recovery.phase) {
    BadgeRecoveryPhase.IDLE,
    BadgeRecoveryPhase.CONFIRMING,
    -> recovery.message
    BadgeRecoveryPhase.PENDING -> recovery.message ?: "Waiting for badge acknowledgement…"
    BadgeRecoveryPhase.ACKNOWLEDGED -> recovery.message ?: "Command acknowledged"
    BadgeRecoveryPhase.NOT_VERIFIED -> recovery.message ?: "Badge acknowledgement timed out"
    BadgeRecoveryPhase.FAILED -> recovery.message ?: "Recovery command failed"
}

private fun recoveryGuidance(recovery: BadgeRecoveryState): String? =
    recovery.reconnectGuidance ?: when (recovery.phase) {
        BadgeRecoveryPhase.ACKNOWLEDGED,
        BadgeRecoveryPhase.NOT_VERIFIED,
        BadgeRecoveryPhase.FAILED,
        -> "Reconnect and refresh badge status"
        else -> null
    }

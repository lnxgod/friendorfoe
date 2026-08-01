package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeCommand
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeTransport
import com.friendorfoe.data.badge.payloadSizeOrNull

@Composable
fun BadgeDeviceStatusSection(
    state: BadgeUiState,
    onNavigate: (BadgeDisplayAction) -> Unit,
) {
    BadgeSectionCard {
        Text(
            "Device-reported status",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        val status = state.controlStatus
        if (status == null) {
            Text(
                "No fresh device status is available.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            StatusValue("Firmware", status.version)
            StatusValue(
                "Display",
                status.displayState?.let { display ->
                    if (display.active) {
                        "Active · ${display.title.ifBlank { "untitled item" }}"
                    } else {
                        "Idle"
                    }
                } ?: "Not reported",
            )
            StatusValue(
                "Scanner health",
                if (status.scanners.isEmpty()) {
                    "No scanners reported"
                } else {
                    "${status.scanners.count { it.connected }}/${status.scanners.size} connected"
                },
            )
            StatusValue("Runtime network", status.reporting.networkMode.ifBlank { "not reported" })
        }

        Text(
            "Badge display controls",
            style = MaterialTheme.typography.labelLarge,
        )
        Text(
            "These send textual next, detail, and back commands. They do not mirror the badge screen.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            BadgeDisplayAction.entries.forEach { action ->
                val support = state.displayNavigationSupport[action]
                    ?: BadgeCapabilitySupport.UNKNOWN
                OutlinedButton(
                    onClick = { onNavigate(action) },
                    enabled = support == BadgeCapabilitySupport.SUPPORTED &&
                        !state.mutationLocked,
                    modifier = Modifier
                        .weight(1f)
                        .heightIn(min = 48.dp)
                        .testTag("badge_nav_${action.wireValue}"),
                ) {
                    Text(actionLabel(action))
                }
            }
        }
        BadgeDisplayAction.entries.forEach { action ->
            val support = state.displayNavigationSupport[action]
                ?: BadgeCapabilitySupport.UNKNOWN
            if (support != BadgeCapabilitySupport.SUPPORTED) {
                Text(
                    navigationReason(state, action, support),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        badgeMutationLockedReason(
            state = state,
            operation = "using badge display controls",
            applyInFlightReason =
                "Badge display controls are locked while Apply changes is pending.",
        )?.let { reason ->
            Text(
                reason,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        state.displayNavigationResult?.let { outcome ->
            Text(
                navigationOutcome(outcome),
                style = MaterialTheme.typography.bodyMedium,
                color = when (outcome) {
                    is BadgeCommandOutcome.Failed,
                    is BadgeCommandOutcome.Unsupported,
                    BadgeCommandOutcome.TimedOut,
                    -> MaterialTheme.colorScheme.error
                    else -> MaterialTheme.colorScheme.secondary
                },
            )
        }
    }
}

@Composable
private fun StatusValue(label: String, value: String) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.Top,
    ) {
        Text(label, style = MaterialTheme.typography.bodySmall)
        Text(
            value,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun actionLabel(action: BadgeDisplayAction): String = when (action) {
    BadgeDisplayAction.NEXT -> "Next"
    BadgeDisplayAction.DETAIL -> "Detail"
    BadgeDisplayAction.BACK -> "Back"
}

private fun navigationReason(
    state: BadgeUiState,
    action: BadgeDisplayAction,
    support: BadgeCapabilitySupport,
): String {
    val actionName = actionLabel(action)
    if (
        support == BadgeCapabilitySupport.UNSUPPORTED &&
        state.connection.transport == BadgeTransport.BLE_GATT
    ) {
        val mtu = state.connection.negotiatedBleMtu
        if (mtu != null) return "$actionName needs a larger BLE MTU (connected at $mtu)"
    }
    return when (support) {
        BadgeCapabilitySupport.UNSUPPORTED -> "$actionName is not supported by this connection"
        BadgeCapabilitySupport.UNKNOWN -> capabilityEvidenceReason(
            state = state,
            capability = BadgeCapability.DISPLAY_NAV,
            featureLabel = actionName,
            payloadBytes = BadgeCommand.NavigateDisplay(action).payloadSizeOrNull(),
        )
        BadgeCapabilitySupport.SUPPORTED -> ""
    }
}

private fun navigationOutcome(outcome: BadgeCommandOutcome): String = when (outcome) {
    is BadgeCommandOutcome.Accepted -> "Accepted · ${outcome.message}"
    is BadgeCommandOutcome.Acknowledged -> "Acknowledged · ${outcome.acknowledgement.message}"
    is BadgeCommandOutcome.Failed -> "Failed · ${outcome.message}"
    is BadgeCommandOutcome.Unsupported -> "Not supported · ${outcome.reason}"
    BadgeCommandOutcome.TimedOut -> "Not verified · acknowledgement timed out"
}

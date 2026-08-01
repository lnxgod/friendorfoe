package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.heightIn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeTransport

@Composable
fun BadgeConnectionSection(
    state: BadgeUiState,
    onRefresh: () -> Unit,
    onReconnect: () -> Unit,
) {
    BadgeSectionCard {
        Text(
            "Connection",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            connectionHeadline(state),
            style = MaterialTheme.typography.bodyLarge,
            fontWeight = FontWeight.Medium,
        )
        Text(
            connectionDetail(state),
            style = MaterialTheme.typography.bodySmall,
            color = when (state.connection.phase) {
                BadgeConnectionPhase.LIVE -> MaterialTheme.colorScheme.secondary
                BadgeConnectionPhase.STALE,
                BadgeConnectionPhase.EXPIRED,
                BadgeConnectionPhase.ERROR,
                -> MaterialTheme.colorScheme.error
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
        )

        state.recoveryAvailability.values
            .filterNot(BadgeRecoveryAvailability::enabled)
            .map(BadgeRecoveryAvailability::reason)
            .filter(String::isNotBlank)
            .distinct()
            .forEach { reason ->
                Text(
                    reason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

        BadgeResponsiveActionPair(
            first = { modifier ->
                Button(
                    onClick = onReconnect,
                    modifier = modifier.heightIn(min = 48.dp),
                ) {
                    Icon(Icons.Default.Sync, contentDescription = null)
                    Text("Reconnect")
                }
            },
            second = { modifier ->
                OutlinedButton(
                    onClick = onRefresh,
                    modifier = modifier.heightIn(min = 48.dp),
                ) {
                    Icon(Icons.Default.Refresh, contentDescription = null)
                    Text("Refresh status")
                }
            },
        )
    }
}

private fun connectionHeadline(state: BadgeUiState): String = when (state.connection.phase) {
    BadgeConnectionPhase.LIVE -> state.connection.targetId
        ?.let { "Connected to $it" }
        ?: "Verified badge connected"
    BadgeConnectionPhase.STALE -> "Status is stale"
    BadgeConnectionPhase.EXPIRED -> "Badge status expired"
    BadgeConnectionPhase.PERMISSION_NEEDED -> "USB permission is needed"
    BadgeConnectionPhase.CONNECTING,
    BadgeConnectionPhase.TRANSPORT_OPEN,
    -> "Connecting to badge"
    BadgeConnectionPhase.ERROR -> "Badge connection error"
    BadgeConnectionPhase.DISCONNECTED -> "No verified badge connected"
}

private fun connectionDetail(state: BadgeUiState): String {
    val transport = transportLabel(state.connection.transport)
    val phase = state.connection.phase.name.lowercase().replace('_', ' ')
    val protocol = state.connection.protocolVersion?.let { " · protocol $it" }.orEmpty()
    return if (state.connection.transport == null) {
        "Connect one badge over USB-C, local AP, or encrypted BLE."
    } else {
        "$transport · $phase$protocol"
    }
}

internal fun transportLabel(transport: BadgeTransport?): String = when (transport) {
    BadgeTransport.USB_SERIAL -> "USB serial"
    BadgeTransport.LOCAL_AP_HTTP -> "Local AP"
    BadgeTransport.BLE_GATT -> "Encrypted BLE"
    BadgeTransport.DEBUG_BRIDGE -> "Debug bridge"
    null -> "Not connected"
}

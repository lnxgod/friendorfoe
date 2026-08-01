package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.Build
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeNetworkMode

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

@Composable
fun BadgeContent(
    state: BadgeUiState,
    actions: BadgeActions,
) {
    val connectionIsLive = state.connection.phase == BadgeConnectionPhase.LIVE
    val themeEnabled = connectionIsLive && !state.mutationLocked &&
        state.appliedTheme != null && state.draftTheme != null &&
        state.capabilities[BadgeCapability.THEME_V1] == BadgeCapabilitySupport.SUPPORTED
    val policyEnabled = connectionIsLive && !state.mutationLocked &&
        state.appliedPolicy != null && state.draftPolicy != null &&
        state.capabilities[BadgeCapability.DISPLAY_POLICY_V1] == BadgeCapabilitySupport.SUPPORTED

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp)
            .testTag("badge_content"),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            "Badge",
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Configure exactly what the badge firmware stores—then verify what came back.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        BadgeConnectionSection(
            state = state,
            onRefresh = actions.refresh,
            onReconnect = actions.reconnect,
        )
        BadgeAppearanceSection(
            theme = state.draftTheme,
            themeHash = state.controlStatus?.themeReadback?.hash,
            enabled = themeEnabled,
            unavailableReason = themeUnavailableReason(state),
            onThemeChange = actions.updateTheme,
        )
        BadgeDisplayFiltersSection(
            policy = state.draftPolicy,
            policyHash = state.controlStatus?.policyReadback?.hash,
            enabled = policyEnabled,
            unavailableReason = policyUnavailableReason(state),
            onPolicyChange = actions.updatePolicy,
        )
        BadgeNetworkModeSection(
            state = state,
            onModeChange = actions.updateNetworkMode,
        )
        BadgeApplySection(
            state = state,
            onDefaults = actions.useDefaults,
            onRevert = actions.revert,
            onApply = actions.apply,
        )
        BadgeDeviceStatusSection(
            state = state,
            onNavigate = actions.navigateDisplay,
        )
        BadgeAdvancedSection(
            recoveryEnabled = !state.applyInFlight,
            onDiagnostics = actions.openDiagnostics,
            onRecovery = actions.openRecovery,
        )
    }
}

@Composable
private fun BadgeNetworkModeSection(
    state: BadgeUiState,
    onModeChange: (BadgeNetworkMode) -> Unit,
) {
    val support = state.capabilities[BadgeCapability.NETWORK_MODE]
        ?: BadgeCapabilitySupport.UNKNOWN
    val enabled = state.connection.phase == BadgeConnectionPhase.LIVE &&
        !state.mutationLocked &&
        state.appliedNetworkMode != null &&
        state.draftNetworkMode != null &&
        support == BadgeCapabilitySupport.SUPPORTED
    BadgeSectionCard {
        Text(
            "Network mode",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "This changes the persisted startup mode. Runtime off is the expected live state for usb_only.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            networkChoices.forEach { choice ->
                FilterChip(
                    selected = state.draftNetworkMode == choice.mode,
                    onClick = { onModeChange(choice.mode) },
                    enabled = enabled,
                    label = { Text("${choice.label} — ${choice.mode.wireValue}") },
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 48.dp)
                        .testTag("badge_network_${choice.testKey}"),
                )
            }
        }
        Text(
            "Applied persisted mode: ${state.appliedNetworkMode?.wireValue ?: "unavailable"}",
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
        )
        Text(
            "Draft persisted mode: ${state.draftNetworkMode?.wireValue ?: "unavailable"}",
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
        )
        Text(
            "Device runtime mode: ${state.controlStatus?.reporting?.networkMode?.ifBlank { "unavailable" } ?: "unavailable"}",
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
        )
        Text(
            if (enabled) {
                "Network mode changes are supported by this connection."
            } else {
                networkUnavailableReason(state, support)
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Network result: ${phaseLabel(state.applyState.network.phase)}",
            style = MaterialTheme.typography.labelMedium,
            color = when (state.applyState.network.phase) {
                BadgeApplyPhase.FAILED,
                BadgeApplyPhase.NOT_VERIFIED,
                BadgeApplyPhase.UNSUPPORTED,
                -> MaterialTheme.colorScheme.error
                BadgeApplyPhase.VERIFIED -> MaterialTheme.colorScheme.secondary
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
        )
        state.applyState.network.message?.let { message ->
            Text(
                message,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private data class NetworkChoice(
    val mode: BadgeNetworkMode,
    val label: String,
    val testKey: String,
)

private val networkChoices = listOf(
    NetworkChoice(BadgeNetworkMode.USB_ONLY, "USB only", "usb_only"),
    NetworkChoice(BadgeNetworkMode.LOCAL_AP, "Local AP", "local_ap"),
    NetworkChoice(BadgeNetworkMode.BACKEND, "Backend", "backend"),
)

@Composable
private fun BadgeAdvancedSection(
    recoveryEnabled: Boolean,
    onDiagnostics: () -> Unit,
    onRecovery: () -> Unit,
) {
    BadgeSectionCard {
        Text(
            "Advanced",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Read device evidence or use guarded direct-USB recovery commands.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        BadgeResponsiveActionPair(
            first = { modifier ->
                OutlinedButton(
                    onClick = onDiagnostics,
                    modifier = modifier.heightIn(min = 48.dp),
                ) {
                    Icon(Icons.Default.BugReport, contentDescription = null)
                    Text("Diagnostics")
                }
            },
            second = { modifier ->
                OutlinedButton(
                    onClick = onRecovery,
                    enabled = recoveryEnabled,
                    modifier = modifier
                        .heightIn(min = 48.dp)
                        .testTag("badge_open_recovery"),
                ) {
                    Icon(Icons.Default.Build, contentDescription = null)
                    Text("Recovery")
                }
            },
        )
        if (!recoveryEnabled) {
            Text(
                "Finish applying changes before opening Recovery.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun themeUnavailableReason(state: BadgeUiState): String? =
    badgeMutationLockedReason(
        state = state,
        operation = "editing the theme",
        applyInFlightReason = "Theme controls are locked while Apply changes is pending.",
    ) ?: when {
        state.appliedTheme == null || state.draftTheme == null ->
            state.controlStatus?.themeReadback?.issue ?: "Theme readback is unavailable"
        state.connection.phase == BadgeConnectionPhase.STALE -> "Status is stale"
        state.connection.phase != BadgeConnectionPhase.LIVE ->
            "A fresh verified badge connection is required."
        else -> capabilityReason(state, BadgeCapability.THEME_V1, "theme")
    }

private fun policyUnavailableReason(state: BadgeUiState): String? =
    badgeMutationLockedReason(
        state = state,
        operation = "editing display rules",
        applyInFlightReason =
            "Display rule controls are locked while Apply changes is pending.",
    ) ?: when {
        state.appliedPolicy == null || state.draftPolicy == null ->
            state.controlStatus?.policyReadback?.issue ?: "Display policy readback is unavailable"
        state.connection.phase == BadgeConnectionPhase.STALE -> "Status is stale"
        state.connection.phase != BadgeConnectionPhase.LIVE ->
            "A fresh verified badge connection is required."
        else -> capabilityReason(state, BadgeCapability.DISPLAY_POLICY_V1, "display policy")
    }

private fun networkUnavailableReason(
    state: BadgeUiState,
    support: BadgeCapabilitySupport,
): String = badgeMutationLockedReason(
    state = state,
    operation = "editing the network mode",
    applyInFlightReason = "Network mode controls are locked while Apply changes is pending.",
) ?: when {
    state.appliedNetworkMode == null || state.draftNetworkMode == null ->
        state.controlStatus?.networkModeReadback?.issue ?: "Network mode readback is unavailable"
    state.connection.phase == BadgeConnectionPhase.STALE -> "Status is stale"
    state.connection.phase != BadgeConnectionPhase.LIVE ->
        "A fresh verified badge connection is required."
    support == BadgeCapabilitySupport.UNSUPPORTED ->
        "This connection does not support network mode changes."
    support == BadgeCapabilitySupport.UNKNOWN -> capabilityEvidenceReason(
        state = state,
        capability = BadgeCapability.NETWORK_MODE,
        featureLabel = "Network mode changes",
    )
    else -> "Network mode changes are unavailable from the current readback."
}

internal fun badgeMutationLockedReason(
    state: BadgeUiState,
    operation: String,
    applyInFlightReason: String,
): String? = when {
    state.applyInFlight -> applyInFlightReason
    state.recovery.phase == BadgeRecoveryPhase.CONFIRMING ->
        "Finish or cancel Recovery before $operation."
    state.recovery.phase == BadgeRecoveryPhase.PENDING ->
        "A Recovery command is pending. Wait for its result before $operation."
    state.recoveryRequiresReconnect ->
        "Reconnect and refresh badge status after Recovery before $operation."
    else -> null
}

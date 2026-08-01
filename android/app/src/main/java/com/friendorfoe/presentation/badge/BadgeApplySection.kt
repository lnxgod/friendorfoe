package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeTransport

@Composable
fun BadgeApplySection(
    state: BadgeUiState,
    onDefaults: () -> Unit,
    onRevert: () -> Unit,
    onApply: () -> Unit,
) {
    BadgeSectionCard {
        Text(
            "Review and apply",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        val dirtyLabels = buildList {
            if (state.themeDirty) add("LCD accent colors")
            if (state.policyDirty) add("Display rules")
            if (state.networkDirty) add("Network mode")
        }
        val editableLabels = buildList {
            if (state.appliedTheme != null && state.draftTheme != null) {
                add("LCD accent colors")
            }
            if (state.appliedPolicy != null && state.draftPolicy != null) {
                add("Display rules")
            }
            if (state.appliedNetworkMode != null && state.draftNetworkMode != null) {
                add("Network mode")
            }
        }
        Text(
            when {
                dirtyLabels.isNotEmpty() -> "Unsaved changes: ${dirtyLabels.joinToString()}"
                editableLabels.isNotEmpty() ->
                    "Loaded configuration: ${editableLabels.joinToString()}. No unsaved changes."
                else -> "No editable draft yet. Connect and refresh badge status."
            },
            style = MaterialTheme.typography.bodyMedium,
        )
        Text(
            "Defaults and Revert change this draft only. Nothing is sent until Apply changes.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        BadgeResponsiveActionPair(
            first = { modifier ->
                OutlinedButton(
                    onClick = onDefaults,
                    enabled = state.canUseFirmwareDefaults,
                    modifier = modifier.heightIn(min = 48.dp),
                ) { Text("Use firmware defaults") }
            },
            second = { modifier ->
                OutlinedButton(
                    onClick = onRevert,
                    enabled = state.canRevertDraft,
                    modifier = modifier.heightIn(min = 48.dp),
                ) { Text("Revert draft") }
            },
        )
        Button(
            onClick = onApply,
            enabled = state.canApply,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("badge_apply"),
        ) {
            Text(if (state.applyInFlight) "Applying changes…" else "Apply changes")
        }
        applyDisabledReason(state)?.let { reason ->
            Text(
                reason,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Text("Results", style = MaterialTheme.typography.labelLarge)
        ApplyResultRow("Theme", state.applyState.theme, "badge_result_theme")
        ApplyResultRow("Display policy", state.applyState.policy, "badge_result_policy")
        ApplyResultRow("Network mode", state.applyState.network, "badge_result_network")
    }
}

@Composable
private fun ApplyResultRow(
    label: String,
    result: BadgeSectionApplyResult,
    testTag: String,
) {
    Column(Modifier.fillMaxWidth().testTag(testTag)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(
                phaseLabel(result.phase),
                style = MaterialTheme.typography.labelMedium,
                color = when (result.phase) {
                    BadgeApplyPhase.FAILED,
                    BadgeApplyPhase.NOT_VERIFIED,
                    BadgeApplyPhase.UNSUPPORTED,
                    -> MaterialTheme.colorScheme.error
                    BadgeApplyPhase.VERIFIED,
                    BadgeApplyPhase.VERIFIED_ON_SCANNERS,
                    -> MaterialTheme.colorScheme.secondary
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
        result.message?.takeIf(String::isNotBlank)?.let { message ->
            Text(
                message,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

internal fun phaseLabel(phase: BadgeApplyPhase): String = when (phase) {
    BadgeApplyPhase.CLEAN -> "No pending result"
    BadgeApplyPhase.DIRTY -> "Draft changed"
    BadgeApplyPhase.PENDING -> "Pending"
    BadgeApplyPhase.ACCEPTED -> "Accepted"
    BadgeApplyPhase.ACKNOWLEDGED -> "Acknowledged"
    BadgeApplyPhase.APPLIED_ON_BADGE -> "Applied on badge"
    BadgeApplyPhase.VERIFIED -> "Verified"
    BadgeApplyPhase.VERIFIED_ON_SCANNERS -> "Verified on scanners"
    BadgeApplyPhase.NOT_VERIFIED -> "Not verified"
    BadgeApplyPhase.FAILED -> "Failed"
    BadgeApplyPhase.UNSUPPORTED -> "Unsupported"
}

private fun applyDisabledReason(state: BadgeUiState): String? {
    badgeMutationLockedReason(
        state = state,
        operation = "applying changes",
        applyInFlightReason = "Applying this exact draft to the connected badge.",
    )?.let { return it }
    if (state.connection.phase == BadgeConnectionPhase.STALE) {
        return "Refresh stale badge status before applying."
    }
    if (state.connection.phase != BadgeConnectionPhase.LIVE) {
        return "Connect and refresh badge status before applying."
    }
    if (state.themeDirty) {
        if (state.appliedTheme == null || state.draftTheme == null) {
            return "Theme readback is unavailable"
        }
    }
    if (state.policyDirty) {
        if (state.appliedPolicy == null || state.draftPolicy == null) {
            return "Display policy readback is unavailable"
        }
    }
    if (state.networkDirty) {
        if (state.appliedNetworkMode == null || state.draftNetworkMode == null) {
            return "Network mode readback is unavailable"
        }
    }
    val hasAnyEditableDraft = (state.appliedTheme != null && state.draftTheme != null) ||
        (state.appliedPolicy != null && state.draftPolicy != null) ||
        (state.appliedNetworkMode != null && state.draftNetworkMode != null)
    if (!hasAnyEditableDraft) return "No verified configuration readback is available yet."
    if (!state.isDirty) return "No changes to apply."
    if (state.themeDirty) {
        capabilityReason(state, BadgeCapability.THEME_V1, "theme")?.let { return it }
    }
    if (state.policyDirty) {
        capabilityReason(state, BadgeCapability.DISPLAY_POLICY_V1, "display policy")?.let {
            return it
        }
    }
    if (state.networkDirty) {
        capabilityReason(state, BadgeCapability.NETWORK_MODE, "network mode")?.let { return it }
    }
    return null
}

internal fun capabilityReason(
    state: BadgeUiState,
    capability: BadgeCapability,
    featureName: String,
): String? = when (state.capabilities[capability] ?: BadgeCapabilitySupport.UNKNOWN) {
    BadgeCapabilitySupport.SUPPORTED -> null
    BadgeCapabilitySupport.UNSUPPORTED ->
        "This connection does not support $featureName changes."
    BadgeCapabilitySupport.UNKNOWN ->
        capabilityEvidenceReason(
            state = state,
            capability = capability,
            featureLabel = "${featureName.replaceFirstChar(Char::uppercase)} changes",
        )
}

internal fun capabilityEvidenceReason(
    state: BadgeUiState,
    capability: BadgeCapability,
    featureLabel: String,
    payloadBytes: Int? = null,
): String {
    val evidence = state.connection
    val plural = featureLabel.endsWith("changes")
    val requires = if (plural) "require" else "requires"
    val be = if (plural) "are" else "is"
    val transport = evidence.transport
        ?: return "$featureLabel $requires a verified badge connection."
    if (evidence.phase != BadgeConnectionPhase.LIVE) {
        return "$featureLabel $requires fresh verified badge status."
    }
    if (evidence.protocolVersion.isNullOrBlank()) {
        return "$featureLabel $requires a verified badge protocol version."
    }

    when (transport) {
        BadgeTransport.USB_SERIAL -> when {
            evidence.usbCandidateCount != 1 ->
                return "$featureLabel $requires exactly one connected USB badge."
            !evidence.exactEspressifVendorMatch ->
                return "$featureLabel $requires a verified Espressif USB badge."
            !evidence.serialInterfaceReadable ->
                return "$featureLabel $requires a readable USB serial interface."
        }
        BadgeTransport.LOCAL_AP_HTTP -> {
            if (evidence.badgeApEndpoint != "http://192.168.4.1") {
                return "$featureLabel $requires the verified badge local AP endpoint."
            }
        }
        BadgeTransport.BLE_GATT -> when {
            !evidence.fofBleServicePresent ->
                return "$featureLabel $requires the Friend or Foe BLE service."
            !evidence.bleStatusCharacteristicPresent ->
                return "$featureLabel $requires the badge BLE status characteristic."
            !evidence.bleControlCharacteristicPresent ->
                return "$featureLabel $requires the badge BLE control characteristic."
            payloadBytes != null && evidence.negotiatedBleMtu == null ->
                return "$featureLabel $requires a negotiated BLE MTU."
            payloadBytes != null && payloadBytes > requireNotNull(evidence.negotiatedBleMtu) - 3 ->
                return "$featureLabel needs a larger BLE MTU " +
                    "(connected at ${evidence.negotiatedBleMtu})."
            !evidence.bleBonded ->
                return "$featureLabel $requires a bonded BLE connection."
            !evidence.bleEncrypted ->
                return "$featureLabel $requires an encrypted BLE connection."
        }
        BadgeTransport.DEBUG_BRIDGE -> when {
            evidence.debugBridgeSerialPort.isNullOrBlank() ->
                return "$featureLabel $requires a connected debug-bridge serial port."
            evidence.debugPhysicalStatusAtElapsedMs == null ->
                return "$featureLabel $requires a current physical badge report."
            evidence.debugBridgeLastError == null ->
                return "$featureLabel $requires a successful debug-bridge mutation report."
            evidence.debugBridgeLastError.isNotBlank() -> {
                val report = evidence.debugBridgeLastError.trim().trimEnd('.')
                return "$featureLabel $be blocked because the debug bridge reports: $report."
            }
        }
    }

    if (capability !in evidence.releaseCertifiedMutations) {
        val transportName = when (transport) {
            BadgeTransport.USB_SERIAL -> "USB serial"
            BadgeTransport.LOCAL_AP_HTTP -> "local AP"
            BadgeTransport.BLE_GATT -> "encrypted BLE"
            BadgeTransport.DEBUG_BRIDGE -> "debug bridge"
        }
        return "$featureLabel $be not release-verified for $transportName in this app build."
    }
    return "$featureLabel cannot be verified from the current connection evidence."
}

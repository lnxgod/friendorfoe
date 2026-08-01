package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeCommand
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeConnectionEvidence
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeControlStatus

enum class BadgeApplyPhase {
    CLEAN,
    DIRTY,
    PENDING,
    ACCEPTED,
    ACKNOWLEDGED,
    APPLIED_ON_BADGE,
    VERIFIED,
    VERIFIED_ON_SCANNERS,
    NOT_VERIFIED,
    FAILED,
    UNSUPPORTED,
}

enum class BadgeConfigSection {
    THEME,
    DISPLAY_POLICY,
    NETWORK_MODE,
}

data class BadgeSectionApplyResult(
    val section: BadgeConfigSection,
    val phase: BadgeApplyPhase = BadgeApplyPhase.CLEAN,
    val message: String? = null,
    val expectedHash: Long? = null,
    val acknowledgementHash: Long? = null,
    val readbackHash: Long? = null,
)

data class BadgeApplyState(
    val theme: BadgeSectionApplyResult = BadgeSectionApplyResult(BadgeConfigSection.THEME),
    val policy: BadgeSectionApplyResult = BadgeSectionApplyResult(
        BadgeConfigSection.DISPLAY_POLICY,
    ),
    val network: BadgeSectionApplyResult = BadgeSectionApplyResult(
        BadgeConfigSection.NETWORK_MODE,
    ),
) {
    val activeResults: List<BadgeSectionApplyResult>
        get() = listOf(theme, policy, network).filter { it.phase != BadgeApplyPhase.CLEAN }
}

enum class BadgeRecoveryAction(
    val capability: BadgeCapability,
    val command: BadgeCommand,
) {
    REBOOT(BadgeCapability.REBOOT, BadgeCommand.Reboot),
    ENTER_BOOTLOADER(BadgeCapability.BOOTLOADER, BadgeCommand.EnterBootloader),
}

enum class BadgeRecoveryPhase {
    IDLE,
    CONFIRMING,
    PENDING,
    ACKNOWLEDGED,
    NOT_VERIFIED,
    FAILED,
}

data class BadgeRecoveryAvailability(
    val enabled: Boolean,
    val reason: String,
)

data class BadgeRecoveryState(
    val action: BadgeRecoveryAction? = null,
    val targetId: String? = null,
    val targetTransportGeneration: Long? = null,
    val phase: BadgeRecoveryPhase = BadgeRecoveryPhase.IDLE,
    val message: String? = null,
    val reconnectGuidance: String? = null,
)

data class BadgeUiState(
    val connection: BadgeConnectionEvidence = BadgeConnectionEvidence(),
    val capabilities: Map<BadgeCapability, BadgeCapabilitySupport> =
        BadgeCapability.entries.associateWith { BadgeCapabilitySupport.UNKNOWN },
    val displayNavigationSupport: Map<BadgeDisplayAction, BadgeCapabilitySupport> =
        BadgeDisplayAction.entries.associateWith { BadgeCapabilitySupport.UNKNOWN },
    val appliedTheme: BadgeTheme? = null,
    val draftTheme: BadgeTheme? = null,
    val appliedPolicy: BadgeDisplayPolicy? = null,
    val draftPolicy: BadgeDisplayPolicy? = null,
    val appliedNetworkMode: BadgeNetworkMode? = null,
    val draftNetworkMode: BadgeNetworkMode? = null,
    val controlStatus: BadgeControlStatus? = null,
    val applyState: BadgeApplyState = BadgeApplyState(),
    val applyInFlight: Boolean = false,
    val recoveryAvailability: Map<BadgeRecoveryAction, BadgeRecoveryAvailability> =
        BadgeRecoveryAction.entries.associateWith {
            BadgeRecoveryAvailability(false, "Verified direct USB is required")
        },
    val displayNavigationResult: BadgeCommandOutcome? = null,
    val recovery: BadgeRecoveryState = BadgeRecoveryState(),
) {
    val themeDirty: Boolean
        get() = draftTheme != appliedTheme
    val policyDirty: Boolean
        get() = draftPolicy != appliedPolicy
    val networkDirty: Boolean
        get() = draftNetworkMode != appliedNetworkMode
    val isDirty: Boolean
        get() = themeDirty || policyDirty || networkDirty
    val recoveryCommandActive: Boolean
        get() = recovery.phase == BadgeRecoveryPhase.CONFIRMING ||
            recovery.phase == BadgeRecoveryPhase.PENDING
    val recoveryRequiresReconnect: Boolean
        get() = recovery.phase == BadgeRecoveryPhase.PENDING ||
            recovery.phase == BadgeRecoveryPhase.ACKNOWLEDGED ||
            recovery.phase == BadgeRecoveryPhase.NOT_VERIFIED ||
            recovery.phase == BadgeRecoveryPhase.FAILED
    val mutationLocked: Boolean
        get() = applyInFlight || recovery.phase != BadgeRecoveryPhase.IDLE
    val canApply: Boolean
        get() = isDirty && !mutationLocked &&
            (!themeDirty || (
                appliedTheme != null &&
                    draftTheme != null &&
                    capabilities[BadgeCapability.THEME_V1] == BadgeCapabilitySupport.SUPPORTED
                )) &&
            (!policyDirty || (
                appliedPolicy != null &&
                    draftPolicy != null &&
                    capabilities[BadgeCapability.DISPLAY_POLICY_V1] ==
                    BadgeCapabilitySupport.SUPPORTED
                )) &&
            (!networkDirty || (
                appliedNetworkMode != null &&
                    draftNetworkMode != null &&
                    capabilities[BadgeCapability.NETWORK_MODE] == BadgeCapabilitySupport.SUPPORTED
                ))
    val canUseFirmwareDefaults: Boolean
        get() = !mutationLocked && (
            firmwareDefaultThemeDraftOrNull()?.let { it != draftTheme } == true ||
                firmwareDefaultPolicyDraftOrNull()?.let { it != draftPolicy } == true
            )
    val canRevertDraft: Boolean
        get() = !mutationLocked && (
            (themeDirty && appliedTheme != null && draftTheme != null) ||
                (policyDirty && appliedPolicy != null && draftPolicy != null) ||
                (networkDirty && appliedNetworkMode != null && draftNetworkMode != null)
            )
}

internal fun BadgeUiState.firmwareDefaultThemeDraftOrNull(): BadgeTheme? {
    val applied = appliedTheme ?: return null
    if (draftTheme == null ||
        capabilities[BadgeCapability.THEME_V1] != BadgeCapabilitySupport.SUPPORTED
    ) {
        return null
    }
    val readback = controlStatus?.themeReadback
    if (readback?.isEditable != true || readback.value != applied) return null
    return BadgeTheme.firmwareDefaults().copy(palette = applied.palette)
}

internal fun BadgeUiState.firmwareDefaultPolicyDraftOrNull(): BadgeDisplayPolicy? {
    val applied = appliedPolicy ?: return null
    if (draftPolicy == null ||
        capabilities[BadgeCapability.DISPLAY_POLICY_V1] != BadgeCapabilitySupport.SUPPORTED
    ) {
        return null
    }
    val readback = controlStatus?.policyReadback
    if (readback?.isEditable != true || readback.value != applied) return null
    val defaults = BadgeDisplayPolicy.firmwareDefaults()
    return defaults.copy(
        classes = BadgeDisplayPolicy.classOrder.associateWithTo(linkedMapOf()) { key ->
            defaults.classes.getValue(key).copy(
                priority = applied.classes.getValue(key).priority,
            )
        },
    )
}

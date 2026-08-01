package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.detection.PrivacyCategory

enum class PrivacySourceKind(val preferenceId: String) {
    PHONE_BLE("phone_ble"),
    PHONE_ULTRASONIC("phone_ultrasonic"),
    BACKEND("backend"),
    BADGE("badge"),
    BADGE_USB("badge_usb"),
    BADGE_AP("badge_ap"),
    BADGE_BLE("badge_ble"),
    BADGE_DEBUG_BRIDGE("badge_debug_bridge"),
    WIFI_ANALYSIS("wifi_analysis"),
}

enum class SourceHealthState {
    LOADING,
    LIVE,
    STALE,
    PAUSED,
    PERMISSION_BLOCKED,
    UNSUPPORTED,
    FAILED,
}

enum class FindingFreshness {
    LIVE,
    STALE,
    PAUSED_CACHED,
    EXPIRED,
}

enum class FindingSeverity(val rank: Int) {
    INFO(0),
    NEARBY(1),
    AWARENESS(2),
    CRITICAL(3),
}

enum class Ownership {
    UNKNOWN,
    OWNED,
}

data class PrivacyFindingKey(
    val source: PrivacySourceKind,
    val sourceRecordId: String,
) {
    init {
        require(sourceRecordId.isNotBlank()) { "A Privacy finding key requires a source record ID" }
    }

    val encoded: String = "${source.preferenceId}\u001F$sourceRecordId"
}

data class PrivacyCapabilities(
    val canIgnore: Boolean = false,
    val canTrack: Boolean = false,
    val canOpenDirectionSweep: Boolean = false,
)

data class PrivacyAppleListeningEvidence(
    val appleFamilyEvidence: Boolean,
    val airPodsAssociationEvidence: Boolean,
    val listeningOrientedCategoryOrWording: Boolean,
)

data class PrivacyFinding(
    val displayId: String,
    val observationKey: PrivacyFindingKey,
    val source: PrivacySourceKind,
    val stableSourceId: String?,
    val routableKey: PrivacyFindingKey?,
    val title: String,
    val evidence: String?,
    val limitation: String?,
    val category: PrivacyCategory,
    val severity: FindingSeverity,
    val ownership: Ownership,
    val signalDbm: Int?,
    val firstSeenWallMs: Long?,
    val lastSeenWallMs: Long?,
    val lastObservedElapsedMs: Long,
    val protocolTtlMs: Long?,
    val hasLiveLocalSamples: Boolean,
    val appleEvidence: PrivacyAppleListeningEvidence? = null,
    val capabilities: PrivacyCapabilities = PrivacyCapabilities(),
    val freshness: FindingFreshness = FindingFreshness.LIVE,
) {
    init {
        require(observationKey.source == source) {
            "Observation key source ${observationKey.source} does not match finding source $source"
        }
        require(routableKey == null || routableKey.source == source) {
            "Routable key source ${routableKey?.source} does not match finding source $source"
        }
    }

    val ignoreKey: FindingPreferenceKey?
        get() = stableSourceId?.let { FindingPreferenceKey.create(source.preferenceId, it) }
}

data class PrivacySourceHealth(
    val source: PrivacySourceKind,
    val state: SourceHealthState,
    val lastSuccessElapsedMs: Long?,
    val lastSuccessWallMs: Long?,
    val recoveryLabel: String?,
    val message: String?,
)

data class PrivacySourceSnapshot(
    val health: PrivacySourceHealth,
    val findings: List<PrivacyFinding>,
    val emittedAtElapsedMs: Long,
) {
    init {
        require(findings.all { it.source == health.source }) {
            "Snapshot ${health.source} contains a finding from another source"
        }
    }
}

internal fun PrivacySourceSnapshot.preserveLoadingStartFrom(
    previous: PrivacySourceSnapshot?,
): PrivacySourceSnapshot = if (
    previous?.health?.source == health.source &&
    previous.health.state == SourceHealthState.LOADING &&
    health.state == SourceHealthState.LOADING
) {
    copy(emittedAtElapsedMs = previous.emittedAtElapsedMs)
} else {
    this
}

data class PrivacyCurrentState(
    val sources: List<PrivacySourceHealth>,
    val findings: List<PrivacyFinding>,
    val threatCount: Int,
    val alertEligible: List<PrivacyFinding>,
    val initialResolutionComplete: Boolean = false,
)

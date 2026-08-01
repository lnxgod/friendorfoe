package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory

enum class PrivacySection(val label: String) {
    THREATS("THREATS"),
    AWARENESS("AWARENESS"),
    NEARBY("NEARBY"),
    INFO("INFO"),
}

fun PrivacyFinding.section(): PrivacySection = when (severity) {
    FindingSeverity.CRITICAL -> PrivacySection.THREATS
    FindingSeverity.AWARENESS -> PrivacySection.AWARENESS
    FindingSeverity.NEARBY -> PrivacySection.NEARBY
    FindingSeverity.INFO -> PrivacySection.INFO
}

data class PrivacyFilterState(
    val query: String = "",
    val categories: Set<PrivacyCategory> = emptySet(),
    val sources: Set<PrivacySourceKind> = emptySet(),
) {
    val activeFilterCount: Int
        get() = (if (query.isBlank()) 0 else 1) +
            (if (categories.isEmpty()) 0 else 1) +
            (if (sources.isEmpty()) 0 else 1)
}

sealed interface PrivacyBodyState {
    data object Loading : PrivacyBodyState
    data object Content : PrivacyBodyState
    data object Empty : PrivacyBodyState
    data class NoMatches(val activeFilterCount: Int) : PrivacyBodyState
    data class RetryableFailure(val message: String) : PrivacyBodyState
    data class PermissionBlocked(val message: String) : PrivacyBodyState
    data class Unsupported(val message: String) : PrivacyBodyState
    data class Stale(val message: String) : PrivacyBodyState
}

enum class PrivacySourceGroup(val label: String) {
    PHONE("Phone"),
    BACKEND("Backend"),
    BADGE("Badge"),
    WIFI("Wi-Fi"),
}

data class PrivacySourceSummary(
    val group: PrivacySourceGroup,
    val state: SourceHealthState,
    val representedSources: List<PrivacySourceKind>,
    val recoverySource: PrivacySourceKind,
    val message: String?,
    val recoveryLabel: String?,
)

data class PrivacyUiState(
    val sourceHealth: List<PrivacySourceHealth> = emptyList(),
    val sourceSummaries: List<PrivacySourceSummary> = emptyList(),
    val availableCategories: List<PrivacyCategory> = emptyList(),
    val availableSources: List<PrivacySourceKind> = emptyList(),
    val visibleFindings: List<PrivacyFinding> = emptyList(),
    val totalCurrentCount: Int = 0,
    val threatCount: Int = 0,
    val filters: PrivacyFilterState = PrivacyFilterState(),
    val body: PrivacyBodyState = PrivacyBodyState.Loading,
    val lastUpdatedWallMs: Long? = null,
    val partialFailureCount: Int = 0,
    val initialResolutionComplete: Boolean = false,
    val focusedKey: PrivacyFindingKey? = null,
    val focusedFinding: PrivacyFinding? = null,
    val focusedFindingExpired: Boolean = false,
) {
    val findingCountLabel: String
        get() = privacyFindingCountLabel(totalCurrentCount, initialResolutionComplete)
}

fun projectPrivacyUiState(
    current: PrivacyCurrentState,
    filters: PrivacyFilterState = PrivacyFilterState(),
    focusedKey: PrivacyFindingKey? = null,
): PrivacyUiState {
    val visible = current.findings.filter { it.matches(filters) }
    val focused = focusedKey?.let { key ->
        current.findings.singleOrNull { it.routableKey == key }
    }
    return PrivacyUiState(
        sourceHealth = current.sources,
        sourceSummaries = summarizePrivacySources(current.sources),
        availableCategories = current.findings.map(PrivacyFinding::category)
            .distinct()
            .sortedBy(PrivacyCategory::label),
        availableSources = (current.sources.map(PrivacySourceHealth::source) +
            current.findings.map(PrivacyFinding::source))
            .distinct()
            .sortedBy(PrivacySourceKind::preferenceId),
        visibleFindings = visible,
        totalCurrentCount = current.findings.size,
        threatCount = current.threatCount,
        filters = filters,
        body = privacyBodyState(current, visible, filters),
        lastUpdatedWallMs = current.sources.mapNotNull(PrivacySourceHealth::lastSuccessWallMs)
            .maxOrNull(),
        partialFailureCount = current.sources.count { it.state == SourceHealthState.FAILED },
        initialResolutionComplete = current.initialResolutionComplete,
        focusedKey = focusedKey,
        focusedFinding = focused,
        focusedFindingExpired = focusedKey != null &&
            current.initialResolutionComplete &&
            focused == null,
    )
}

private fun privacyFindingCountLabel(
    count: Int,
    initialResolutionComplete: Boolean,
): String = when {
    initialResolutionComplete ->
        "$count current finding${if (count == 1) "" else "s"}"
    count == 0 -> "0 findings so far · sources still resolving"
    else -> "$count finding${if (count == 1) "" else "s"} so far · some sources still resolving"
}

fun summarizePrivacySources(
    sources: List<PrivacySourceHealth>,
): List<PrivacySourceSummary> = PrivacySourceGroup.entries.mapNotNull { group ->
    val members = sources.filter { it.source.group() == group }
    if (members.isEmpty()) return@mapNotNull null

    val pausedCorePhoneScan = members.firstOrNull {
        group == PrivacySourceGroup.PHONE &&
            it.source == PrivacySourceKind.PHONE_BLE &&
            it.state == SourceHealthState.PAUSED
    }
    if (pausedCorePhoneScan != null) {
        return@mapNotNull PrivacySourceSummary(
            group = group,
            state = SourceHealthState.PAUSED,
            representedSources = members.map(PrivacySourceHealth::source)
                .sortedBy(PrivacySourceKind::preferenceId),
            recoverySource = pausedCorePhoneScan.source,
            message = pausedCorePhoneScan.message,
            recoveryLabel = pausedCorePhoneScan.recoveryLabel,
        )
    }

    val effective = members.filterNot {
        it.state == SourceHealthState.PAUSED || it.state == SourceHealthState.UNSUPPORTED
    }.ifEmpty { members }
    val selected = effective.maxWithOrNull(
        compareBy<PrivacySourceHealth> { it.state.rollupPriority() }
            .thenBy { it.source.preferenceId },
    ) ?: return@mapNotNull null
    val state = if (
        effective === members &&
        members.none { it.state != SourceHealthState.PAUSED }
    ) {
        SourceHealthState.PAUSED
    } else {
        selected.state
    }
    val messageSource = effective.firstOrNull { it.state == state } ?: selected
    PrivacySourceSummary(
        group = group,
        state = state,
        representedSources = members.map(PrivacySourceHealth::source).sortedBy { it.preferenceId },
        recoverySource = messageSource.source,
        message = messageSource.message,
        recoveryLabel = messageSource.recoveryLabel,
    )
}

private fun PrivacyFinding.matches(filters: PrivacyFilterState): Boolean {
    if (filters.categories.isNotEmpty() && category !in filters.categories) return false
    if (filters.sources.isNotEmpty() && source !in filters.sources) return false
    val query = filters.query.trim()
    if (query.isEmpty()) return true
    return listOfNotNull(
        title,
        evidence,
        limitation,
        category.label,
        source.userLabel(),
    ).any { it.contains(query, ignoreCase = true) }
}

private fun privacyBodyState(
    current: PrivacyCurrentState,
    visible: List<PrivacyFinding>,
    filters: PrivacyFilterState,
): PrivacyBodyState {
    if (current.findings.isNotEmpty()) {
        return if (visible.isNotEmpty()) {
            PrivacyBodyState.Content
        } else {
            PrivacyBodyState.NoMatches(filters.activeFilterCount)
        }
    }
    if (!current.initialResolutionComplete || current.sources.isEmpty()) {
        return PrivacyBodyState.Loading
    }
    if (current.sources.any { it.state == SourceHealthState.LIVE }) {
        return PrivacyBodyState.Empty
    }
    current.sources.firstOrNull { it.state == SourceHealthState.PERMISSION_BLOCKED }?.let {
        return PrivacyBodyState.PermissionBlocked(
            it.message ?: "Permission is needed for this privacy source.",
        )
    }
    current.sources.firstOrNull { it.state == SourceHealthState.FAILED }?.let {
        return PrivacyBodyState.RetryableFailure(
            it.message ?: "A privacy source could not be reached.",
        )
    }
    if (current.sources.any { it.state == SourceHealthState.LOADING }) {
        return PrivacyBodyState.Loading
    }
    if (current.sources.any { it.state == SourceHealthState.STALE }) {
        return PrivacyBodyState.Stale(
            current.sources.firstNotNullOfOrNull(PrivacySourceHealth::message)
                ?: "No privacy source has reported fresh results.",
        )
    }
    if (current.sources.all {
            it.state == SourceHealthState.PAUSED || it.state == SourceHealthState.UNSUPPORTED
        }
    ) {
        val message = current.sources.firstNotNullOfOrNull(PrivacySourceHealth::message)
            ?: "Privacy sources are off or unavailable on this device."
        return PrivacyBodyState.Unsupported(message)
    }
    return PrivacyBodyState.Empty
}

fun PrivacySourceKind.userLabel(): String = when (this) {
    PrivacySourceKind.PHONE_BLE -> "Phone Bluetooth"
    PrivacySourceKind.PHONE_ULTRASONIC -> "Phone ultrasonic"
    PrivacySourceKind.BACKEND -> "Backend"
    PrivacySourceKind.BADGE -> "Badge"
    PrivacySourceKind.BADGE_USB -> "Badge USB"
    PrivacySourceKind.BADGE_AP -> "Badge local Wi-Fi"
    PrivacySourceKind.BADGE_BLE -> "Badge Bluetooth"
    PrivacySourceKind.BADGE_DEBUG_BRIDGE -> "Badge debug bridge"
    PrivacySourceKind.WIFI_ANALYSIS -> "Wi-Fi analysis"
}

private fun PrivacySourceKind.group(): PrivacySourceGroup = when (this) {
    PrivacySourceKind.PHONE_BLE,
    PrivacySourceKind.PHONE_ULTRASONIC -> PrivacySourceGroup.PHONE

    PrivacySourceKind.BACKEND -> PrivacySourceGroup.BACKEND
    PrivacySourceKind.BADGE,
    PrivacySourceKind.BADGE_USB,
    PrivacySourceKind.BADGE_AP,
    PrivacySourceKind.BADGE_BLE,
    PrivacySourceKind.BADGE_DEBUG_BRIDGE -> PrivacySourceGroup.BADGE

    PrivacySourceKind.WIFI_ANALYSIS -> PrivacySourceGroup.WIFI
}

private fun SourceHealthState.rollupPriority(): Int = when (this) {
    SourceHealthState.FAILED -> 7
    SourceHealthState.PERMISSION_BLOCKED -> 6
    SourceHealthState.STALE -> 5
    SourceHealthState.LOADING -> 4
    SourceHealthState.LIVE -> 3
    SourceHealthState.UNSUPPORTED -> 2
    SourceHealthState.PAUSED -> 1
}

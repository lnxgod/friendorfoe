package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.badge.isBackendLite
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationRequest
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyDetectionOrigin
import com.friendorfoe.detection.isBleInvestigationTargetFresh
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

private val BADGE_GATT_MAC = Regex(
    pattern = "^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$",
    option = RegexOption.IGNORE_CASE,
)

internal val BADGE_INVESTIGATION_ACTIVE_STATES = setOf(
    BleInvestigationState.IDLE,
    BleInvestigationState.QUEUED,
    BleInvestigationState.SCANNING,
    BleInvestigationState.CONNECTING,
    BleInvestigationState.DISCOVERING,
    BleInvestigationState.READING,
)

internal fun BadgeUsbState.badgeInvestigationAvailable(): Boolean =
    status == BadgeUsbStatus.CONNECTED &&
        controlStatus?.isBackendLite() != true &&
        controlStatus?.scanners?.any { it.slot == 0 && it.connected } == true

internal fun BadgeThreatEntity.badgeInvestigationTarget(
    nowElapsedMs: Long,
): BleInvestigationTarget? {
    if (stale) return null
    val observedAt = badgeEntityObservedAtElapsedMs(
        snapshotAtElapsedMs = snapshotAtElapsedMs.takeIf { it >= 0L } ?: nowElapsedMs,
        lastSeenSeconds = lastSeenSeconds,
    )
    if (!isBleInvestigationTargetFresh(observedAt, nowElapsedMs)) return null

    val entityKey = "badge:${threatClass}:${code}:${badgeEntityStableId()}"
    val pairingSpam = listOf(code, category, label, detail).any { value ->
        value.trim()
            .replace('-', '_')
            .replace(' ', '_')
            .uppercase(Locale.ROOT) in setOf("PAIRING_SPAM", "BLE_SPAM")
    }
    val targetMac = bssid.trim()
    return when {
        pairingSpam -> BleInvestigationTarget(
            mode = BleInvestigationMode.PASSIVE_CAPTURE,
            mac = null,
            entityKey = entityKey,
            observedAtElapsedMs = observedAt,
            origin = PrivacyDetectionOrigin.BADGE,
        )
        threatClass.equals("ble", ignoreCase = true) && BADGE_GATT_MAC.matches(targetMac) ->
            BleInvestigationTarget(
                mode = BleInvestigationMode.GATT,
                mac = targetMac,
                entityKey = entityKey,
                observedAtElapsedMs = observedAt,
                origin = PrivacyDetectionOrigin.BADGE,
            )
        else -> null
    }
}

internal fun BadgeThreatEntity.badgeInvestigationRequest(
    nowElapsedMs: Long,
    requestId: String,
): BleInvestigationRequest? {
    if (requestId.length !in 1..32 || requestId.any { it.code !in 0x21..0x7E }) return null
    return badgeInvestigationTarget(nowElapsedMs)?.let { target ->
        BleInvestigationRequest(
            requestId = requestId,
            target = target,
            route = BleInvestigationRoute.BADGE,
        )
    }
}

private fun BadgeThreatEntity.badgeEntityStableId(): String = bssid.ifBlank {
    displayId.ifBlank { operatorId ?: detail.ifBlank { label } }
}

internal fun badgeEntityObservedAtElapsedMs(
    snapshotAtElapsedMs: Long,
    lastSeenSeconds: Int,
): Long {
    val ageMs = lastSeenSeconds.coerceAtLeast(0).toLong() * 1_000L
    return (snapshotAtElapsedMs.coerceAtLeast(0L) - ageMs).coerceAtLeast(0L)
}

internal fun badgeInvestigationStartAllowed(
    state: BadgeUsbState,
    repositoryResult: BleInvestigationResult?,
): Boolean = state.badgeInvestigationAvailable() &&
    repositoryResult?.state !in BADGE_INVESTIGATION_ACTIVE_STATES

/**
 * Process-wide presentation ownership for the repository's process-wide investigation.
 * It intentionally outlives individual Badge and BadgeFocus ViewModels.
 */
internal class BadgeInvestigationSessionTracker {
    private val requestSequence = AtomicLong(0L)
    private val _currentRequestId = MutableStateFlow<String?>(null)
    val currentRequestId: StateFlow<String?> = _currentRequestId.asStateFlow()

    fun nextRequestId(nowElapsedMs: Long): String {
        val sequence = requestSequence.incrementAndGet().toULong().toString(36)
        return "bdg-$sequence-${nowElapsedMs.coerceAtLeast(0L).toString(36)}"
    }

    @Synchronized
    fun recordRepositoryResult(
        requestId: String,
        repositoryResult: BleInvestigationResult?,
    ): Boolean {
        if (repositoryResult?.requestId != requestId) return false
        _currentRequestId.value = requestId
        return true
    }

    fun visibleResult(result: BleInvestigationResult?): BleInvestigationResult? = result
        ?.takeIf { it.requestId == _currentRequestId.value }

    fun activeCancelRequestId(result: BleInvestigationResult?): String? = visibleResult(result)
        ?.takeIf { it.state in BADGE_INVESTIGATION_ACTIVE_STATES }
        ?.requestId
}

internal val sharedBadgeInvestigationSession = BadgeInvestigationSessionTracker()

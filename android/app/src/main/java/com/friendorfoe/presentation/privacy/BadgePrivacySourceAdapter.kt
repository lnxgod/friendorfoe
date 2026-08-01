package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import com.friendorfoe.detection.PrivacyCategory
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

@Singleton
class BadgePrivacySourceAdapter internal constructor(
    private val state: StateFlow<BadgeUsbState>,
    private val clock: MonotonicClock,
    @ApplicationScope private val scope: CoroutineScope,
    private val requestConnection: () -> Unit,
    private val requestStatus: () -> Unit,
) : PrivacySourceAdapter {

    @Inject
    internal constructor(
        repository: BadgeUsbRepository,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        state = repository.state,
        clock = clock,
        scope = scope,
        requestConnection = repository::requestConnection,
        requestStatus = repository::requestStatus,
    )

    override val adapterId: String = "badge"
    override val representedSources: Set<PrivacySourceKind> = setOf(
        PrivacySourceKind.BADGE,
        PrivacySourceKind.BADGE_USB,
        PrivacySourceKind.BADGE_AP,
        PrivacySourceKind.BADGE_BLE,
        PrivacySourceKind.BADGE_DEBUG_BRIDGE,
    )

    private val _snapshots = MutableStateFlow<List<PrivacySourceSnapshot>>(emptyList())
    override val snapshots: StateFlow<List<PrivacySourceSnapshot>> = _snapshots.asStateFlow()

    init {
        _snapshots.value = mapState(state.value)
        scope.launch {
            state.collect { badgeState ->
                _snapshots.value = mapState(badgeState)
            }
        }
    }

    override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
        if (source !in representedSources) return PrivacyRecoveryResult.SourceUnavailable(source)
        resetLoadingDeadline(source)
        requestConnection()
        requestStatus()
        return PrivacyRecoveryResult.Recovered(source)
    }

    private fun mapState(state: BadgeUsbState): List<PrivacySourceSnapshot> {
        val source = state.sourceKindOrNull()
        if (source == null) {
            return listOf(
                PrivacySourceSnapshot(
                    health = PrivacySourceHealth(
                        source = PrivacySourceKind.BADGE,
                        state = SourceHealthState.UNSUPPORTED,
                        lastSuccessElapsedMs = null,
                        lastSuccessWallMs = null,
                        recoveryLabel = null,
                        message = "No badge connected",
                    ),
                    findings = emptyList(),
                    emittedAtElapsedMs = clock.nowElapsedMs(),
                ),
            )
        }
        val status = state.controlStatus
        val previous = _snapshots.value.singleOrNull()
            ?.takeIf { it.health.source == source }
        val healthState = state.status.sourceHealth(hasStatus = status != null)

        if (status == null) {
            return listOf(
                PrivacySourceSnapshot(
                    health = PrivacySourceHealth(
                        source = source,
                        state = healthState,
                        lastSuccessElapsedMs = previous?.health?.lastSuccessElapsedMs,
                        lastSuccessWallMs = previous?.health?.lastSuccessWallMs,
                        recoveryLabel = when (healthState) {
                            SourceHealthState.FAILED -> "Reconnect"
                            SourceHealthState.PERMISSION_BLOCKED -> "Connect badge"
                            else -> null
                        },
                        message = when (healthState) {
                            SourceHealthState.FAILED -> state.message.ifBlank { "Badge status is unavailable" }
                            SourceHealthState.PAUSED -> "No badge connected"
                            SourceHealthState.PERMISSION_BLOCKED -> "Badge permission is required"
                            else -> "Waiting for badge status"
                        },
                    ),
                    findings = previous?.findings.orEmpty(),
                    emittedAtElapsedMs = clock.nowElapsedMs(),
                ).preserveLoadingStartFrom(previous),
            )
        }

        val nowElapsed = clock.nowElapsedMs()
        val nowWall = clock.nowWallClock().toEpochMilli()
        val statusElapsed = status.entities.mapNotNull { entity ->
            entity.snapshotAtElapsedMs.takeIf { it >= 0L }
        }.maxOrNull() ?: nowElapsed
        val statusWall = subtractSaturated(nowWall, elapsedDelta(nowElapsed, statusElapsed))
        val rows = status.entities.mapIndexedNotNull { index, entity ->
            val snapshotElapsed = entity.snapshotAtElapsedMs.takeIf { it >= 0L } ?: statusElapsed
            val snapshotWall = subtractSaturated(nowWall, elapsedDelta(nowElapsed, snapshotElapsed))
            mapEntity(
                entity = entity,
                source = source,
                snapshotAtElapsedMs = snapshotElapsed,
                snapshotAtWallMs = snapshotWall,
                ephemeralRecordId = "ephemeral:${source.preferenceId}:$snapshotElapsed:$index",
            )
        }.sortedBy { it.observationKey.encoded }
        return listOf(
            PrivacySourceSnapshot(
                health = PrivacySourceHealth(
                    source = source,
                    state = healthState,
                    lastSuccessElapsedMs = statusElapsed,
                    lastSuccessWallMs = statusWall,
                    recoveryLabel = when (healthState) {
                        SourceHealthState.FAILED -> "Reconnect"
                        SourceHealthState.PERMISSION_BLOCKED -> "Connect badge"
                        else -> null
                    },
                    message = when (healthState) {
                        SourceHealthState.FAILED -> state.message.ifBlank { "Badge status failed" }
                        SourceHealthState.PERMISSION_BLOCKED -> "Badge permission is required"
                        SourceHealthState.PAUSED -> "No badge connected"
                        SourceHealthState.LOADING -> "Waiting for current badge status"
                        else -> null
                    },
                ),
                findings = rows,
                emittedAtElapsedMs = clock.nowElapsedMs(),
            ).preserveLoadingStartFrom(previous),
        )
    }

    private fun BadgeUsbState.sourceKindOrNull(): PrivacySourceKind? = when (status) {
        BadgeUsbStatus.AP_CONNECTED -> PrivacySourceKind.BADGE_AP
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED -> PrivacySourceKind.BADGE_DEBUG_BRIDGE
        BadgeUsbStatus.BLE_CONNECTED -> PrivacySourceKind.BADGE_BLE
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.PERMISSION_NEEDED,
        -> PrivacySourceKind.BADGE_USB
        BadgeUsbStatus.CONNECTING,
        BadgeUsbStatus.ERROR,
        BadgeUsbStatus.DISCONNECTED,
        -> when {
            transportLabel.contains("debug", ignoreCase = true) ->
                PrivacySourceKind.BADGE_DEBUG_BRIDGE
            transportLabel.contains("badge ap", ignoreCase = true) ||
                transportLabel.equals("ap", ignoreCase = true) -> PrivacySourceKind.BADGE_AP
            transportLabel.contains("ble", ignoreCase = true) -> PrivacySourceKind.BADGE_BLE
            transportLabel.contains("usb", ignoreCase = true) -> PrivacySourceKind.BADGE_USB
            else -> null
        }
    }

    private fun BadgeUsbStatus.sourceHealth(hasStatus: Boolean): SourceHealthState = when (this) {
        BadgeUsbStatus.DISCONNECTED -> SourceHealthState.PAUSED
        BadgeUsbStatus.PERMISSION_NEEDED -> SourceHealthState.PERMISSION_BLOCKED
        BadgeUsbStatus.CONNECTING -> SourceHealthState.LOADING
        BadgeUsbStatus.ERROR -> SourceHealthState.FAILED
        BadgeUsbStatus.AP_CONNECTED,
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
        BadgeUsbStatus.BLE_CONNECTED,
        BadgeUsbStatus.CONNECTED,
        -> if (hasStatus) SourceHealthState.LIVE else SourceHealthState.LOADING
    }

    private fun elapsedDelta(nowElapsedMs: Long, snapshotElapsedMs: Long): Long =
        if (snapshotElapsedMs >= nowElapsedMs) 0L else nowElapsedMs - snapshotElapsedMs

    private fun resetLoadingDeadline(source: PrivacySourceKind) {
        _snapshots.value = _snapshots.value.map { snapshot ->
            if (snapshot.health.source == source && snapshot.health.state == SourceHealthState.LOADING) {
                snapshot.copy(emittedAtElapsedMs = clock.nowElapsedMs())
            } else {
                snapshot
            }
        }
    }

    companion object {
        internal fun mapEntity(
            entity: BadgeThreatEntity,
            source: PrivacySourceKind,
            snapshotAtElapsedMs: Long,
            snapshotAtWallMs: Long,
            ephemeralRecordId: String,
        ): PrivacyFinding? {
            if (entity.stale) return null
            require(source in BADGE_SOURCES) { "$source is not a badge transport source" }
            val provenEntityId = if (entity.sourceId > 0 && entity.source.isNotBlank()) {
                "${entity.source}:id:${entity.sourceId}"
            } else {
                null
            }
            val record = provenEntityId?.let { "entity:$it" } ?: ephemeralRecordId
            val key = PrivacyFindingKey(source, record)
            val category = categoryFor(entity)
            val title = entity.label.takeIf { it.isNotBlank() }
                ?: entity.detail.takeIf { it.isNotBlank() }
                ?: "Badge privacy finding"
            val rowText = listOf(
                entity.label,
                entity.detail,
                entity.evidence,
                entity.threatClass,
                entity.category,
                entity.code,
            ).joinToString(" ")
            val tokens = Regex("[A-Za-z0-9]+").findAll(rowText)
                .map { it.value.lowercase() }
                .toSet()
            val appleFamily = tokens.any { it in APPLE_DEVICE_TOKENS }
            val airPods = "airpods" in tokens
            val listening = category == PrivacyCategory.REMOTE_LISTENING ||
                tokens.any { it in LISTENING_TOKENS }
            val plainAppleActivity = appleFamily &&
                !listening &&
                category == PrivacyCategory.INFORMATIONAL
            val effectiveCategory = if (plainAppleActivity) {
                PrivacyCategory.APPLE_CONTINUITY
            } else {
                category
            }
            val lastAgeMs = secondsToMillisSaturated(entity.lastSeenSeconds.coerceAtLeast(0))
            val firstAgeMs = secondsToMillisSaturated(entity.ageSeconds.coerceAtLeast(0))
            val lastObservedElapsed = subtractSaturated(snapshotAtElapsedMs, lastAgeMs)
            val lastWall = subtractSaturated(snapshotAtWallMs, lastAgeMs)
            val firstWall = subtractSaturated(snapshotAtWallMs, firstAgeMs)
            val signal = when {
                entity.rssi != 0 -> entity.rssi
                entity.bestRssi != 0 -> entity.bestRssi
                else -> null
            }
            val evidence = listOfNotNull(
                entity.detail.takeIf { it.isNotBlank() },
                entity.evidence.takeIf { it.isNotBlank() },
            ).joinToString(" • ").takeIf { it.isNotBlank() }

            val raw = PrivacyFinding(
                displayId = entity.displayId.takeIf { it.isNotBlank() } ?: record,
                observationKey = key,
                source = source,
                stableSourceId = provenEntityId,
                routableKey = provenEntityId?.let { key },
                title = title,
                evidence = evidence,
                limitation = null,
                category = effectiveCategory,
                severity = if (plainAppleActivity) {
                    FindingSeverity.INFO
                } else {
                    severityFor(effectiveCategory, entity.score)
                },
                ownership = Ownership.UNKNOWN,
                signalDbm = signal,
                firstSeenWallMs = firstWall,
                lastSeenWallMs = lastWall,
                lastObservedElapsedMs = lastObservedElapsed,
                protocolTtlMs = null,
                hasLiveLocalSamples = false,
                appleEvidence = PrivacyAppleListeningEvidence(
                    appleFamilyEvidence = appleFamily,
                    airPodsAssociationEvidence = airPods,
                    listeningOrientedCategoryOrWording = listening,
                ).takeIf { appleFamily || listening },
            )
            return PrivacyFindingNormalizer.normalize(raw)
        }

        private fun categoryFor(entity: BadgeThreatEntity): PrivacyCategory {
            val text = listOf(
                entity.threatClass,
                entity.category,
                entity.code,
                entity.label,
                entity.detail,
            ).joinToString(" ").lowercase()
            return when {
                "flock" in text || "alpr" in text -> PrivacyCategory.ALPR_CAMERA
                "evil twin" in text || "wifi_anomaly" in text || "attack" in text ->
                    PrivacyCategory.ATTACK_TOOL
                "remote_listening" in text || "possible listening" in text ->
                    PrivacyCategory.REMOTE_LISTENING
                "tracker" in text || "airtag" in text || "findmy" in text ->
                    PrivacyCategory.BLE_TRACKER
                "meta" in text || "smart glasses" in text -> PrivacyCategory.SMART_GLASSES
                "camera" in text -> PrivacyCategory.SURVEILLANCE_CAMERA
                "beacon" in text -> PrivacyCategory.VENUE_BEACON
                else -> PrivacyCategory.INFORMATIONAL
            }
        }

        private fun severityFor(category: PrivacyCategory, score: Int): FindingSeverity =
            when {
                category.threatLevel >= 3 || score >= 85 -> FindingSeverity.CRITICAL
                category.threatLevel == 2 || score >= 60 -> FindingSeverity.AWARENESS
                category.threatLevel == 1 || score > 0 -> FindingSeverity.NEARBY
                else -> FindingSeverity.INFO
            }

        private fun secondsToMillisSaturated(seconds: Int): Long =
            runCatching { Math.multiplyExact(seconds.toLong(), 1_000L) }
                .getOrDefault(Long.MAX_VALUE)

        private fun subtractSaturated(value: Long, amount: Long): Long =
            if (amount >= value) 0L else value - amount

        private val BADGE_SOURCES = setOf(
            PrivacySourceKind.BADGE_USB,
            PrivacySourceKind.BADGE_AP,
            PrivacySourceKind.BADGE_BLE,
            PrivacySourceKind.BADGE_DEBUG_BRIDGE,
        )
        private val APPLE_DEVICE_TOKENS = setOf("apple", "airpods", "iphone", "ipad", "macbook")
        private val LISTENING_TOKENS = setOf("listening", "listen", "eavesdrop", "microphone")
    }
}

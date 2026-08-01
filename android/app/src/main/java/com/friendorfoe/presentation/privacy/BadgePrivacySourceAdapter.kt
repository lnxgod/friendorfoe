package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeRepositoryState
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeTransport
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
class BadgePrivacySourceAdapter @Inject internal constructor(
    private val controlPort: BadgeControlPort,
    private val clock: MonotonicClock,
    @ApplicationScope private val scope: CoroutineScope,
) : PrivacySourceAdapter {

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
        _snapshots.value = mapState(controlPort.state.value)
        scope.launch {
            controlPort.state.collect { state ->
                _snapshots.value = mapState(state)
            }
        }
    }

    override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
        if (source !in representedSources) return PrivacyRecoveryResult.SourceUnavailable(source)
        resetLoadingDeadline(source)
        controlPort.requestConnection()
        controlPort.refreshStatus()
        return PrivacyRecoveryResult.Recovered(source)
    }

    private fun mapState(state: BadgeRepositoryState): List<PrivacySourceSnapshot> {
        val transport = state.connection.transport
        if (transport == null) {
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
        val source = transport.sourceKind()
        val status = state.controlStatus
        val previous = _snapshots.value.singleOrNull()
            ?.takeIf { it.health.source == source }

        if (status == null) {
            val healthState = state.connection.phase.healthWithoutStatus()
            return listOf(
                PrivacySourceSnapshot(
                    health = PrivacySourceHealth(
                        source = source,
                        state = healthState,
                        lastSuccessElapsedMs = previous?.health?.lastSuccessElapsedMs,
                        lastSuccessWallMs = previous?.health?.lastSuccessWallMs,
                        recoveryLabel = if (healthState == SourceHealthState.FAILED) "Reconnect" else null,
                        message = when (healthState) {
                            SourceHealthState.FAILED -> "Badge status is unavailable"
                            SourceHealthState.STALE -> "Badge status is stale"
                            SourceHealthState.PAUSED -> "No badge connected"
                            else -> "Waiting for badge status"
                        },
                    ),
                    findings = previous?.findings.orEmpty(),
                    emittedAtElapsedMs = clock.nowElapsedMs(),
                ).preserveLoadingStartFrom(previous),
            )
        }

        val generation = state.connection.transportGeneration ?: 0L
        val rows = status.entities.mapIndexedNotNull { index, entity ->
            mapEntity(
                entity = entity,
                source = source,
                status = status,
                ephemeralRecordId = "ephemeral:$generation:${status.receivedAtElapsedMs}:$index",
            )
        }.sortedBy { it.observationKey.encoded }
        val healthState = when (state.connection.phase) {
            BadgeConnectionPhase.LIVE -> SourceHealthState.LIVE
            BadgeConnectionPhase.STALE,
            BadgeConnectionPhase.EXPIRED,
            -> SourceHealthState.STALE
            BadgeConnectionPhase.ERROR -> SourceHealthState.FAILED
            BadgeConnectionPhase.PERMISSION_NEEDED -> SourceHealthState.PERMISSION_BLOCKED
            BadgeConnectionPhase.DISCONNECTED -> SourceHealthState.PAUSED
            BadgeConnectionPhase.CONNECTING,
            BadgeConnectionPhase.TRANSPORT_OPEN,
            -> SourceHealthState.LOADING
        }
        return listOf(
            PrivacySourceSnapshot(
                health = PrivacySourceHealth(
                    source = source,
                    state = healthState,
                    lastSuccessElapsedMs = status.receivedAtElapsedMs,
                    lastSuccessWallMs = status.receivedAtWallClock.toEpochMilli(),
                    recoveryLabel = if (healthState == SourceHealthState.FAILED) "Reconnect" else null,
                    message = when (healthState) {
                        SourceHealthState.STALE -> "Badge status is stale"
                        SourceHealthState.FAILED -> "Badge status failed"
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
            status: BadgeControlStatus,
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
            val receivedElapsed = status.receivedAtElapsedMs
            val receivedWall = status.receivedAtWallClock.toEpochMilli()
            val lastAgeMs = secondsToMillisSaturated(entity.lastSeenSeconds.coerceAtLeast(0))
            val firstAgeMs = secondsToMillisSaturated(entity.ageSeconds.coerceAtLeast(0))
            val lastObservedElapsed = subtractSaturated(receivedElapsed, lastAgeMs)
            val lastWall = subtractSaturated(receivedWall, lastAgeMs)
            val firstWall = subtractSaturated(receivedWall, firstAgeMs)
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

        private fun BadgeTransport.sourceKind(): PrivacySourceKind = when (this) {
            BadgeTransport.USB_SERIAL -> PrivacySourceKind.BADGE_USB
            BadgeTransport.LOCAL_AP_HTTP -> PrivacySourceKind.BADGE_AP
            BadgeTransport.BLE_GATT -> PrivacySourceKind.BADGE_BLE
            BadgeTransport.DEBUG_BRIDGE -> PrivacySourceKind.BADGE_DEBUG_BRIDGE
        }

        private fun BadgeConnectionPhase.healthWithoutStatus(): SourceHealthState = when (this) {
            BadgeConnectionPhase.PERMISSION_NEEDED -> SourceHealthState.PERMISSION_BLOCKED
            BadgeConnectionPhase.ERROR -> SourceHealthState.FAILED
            BadgeConnectionPhase.STALE,
            BadgeConnectionPhase.EXPIRED,
            -> SourceHealthState.STALE
            BadgeConnectionPhase.DISCONNECTED -> SourceHealthState.PAUSED
            BadgeConnectionPhase.CONNECTING,
            BadgeConnectionPhase.TRANSPORT_OPEN,
            BadgeConnectionPhase.LIVE,
            -> SourceHealthState.LOADING
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

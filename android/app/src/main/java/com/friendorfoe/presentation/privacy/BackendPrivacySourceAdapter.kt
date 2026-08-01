package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.remote.LivePrivacyDeviceDto
import com.friendorfoe.data.remote.LivePrivacyDevicesDto
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import com.friendorfoe.detection.PrivacyCategory
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

@Singleton
class BackendPrivacySourceAdapter internal constructor(
    private val settings: StateFlow<DetectionSettings>,
    private val fetch: suspend (endpoint: String) -> LivePrivacyDevicesDto,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
    private val pollIntervalMs: Long = 5_000L,
) : PrivacySourceAdapter {

    @Inject
    constructor(
        detectionPrefs: DetectionPrefs,
        api: SensorMapApiService,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        settings = detectionPrefs.settings,
        fetch = { api.getLivePrivacyDevices() },
        clock = clock,
        scope = scope,
    )

    override val adapterId: String = "backend"
    override val representedSources: Set<PrivacySourceKind> = setOf(PrivacySourceKind.BACKEND)

    private val responseSequence = AtomicLong(0L)
    private val retry = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    private var activeEndpoint: String? = null
    private var retainedRows: List<PrivacyFinding> = emptyList()

    private val _snapshots = MutableStateFlow(
        listOf(
            snapshot(
                state = if (settings.value.sensorBackendEnabled) {
                    SourceHealthState.LOADING
                } else {
                    SourceHealthState.PAUSED
                },
            ),
        ),
    )
    override val snapshots: StateFlow<List<PrivacySourceSnapshot>> = _snapshots.asStateFlow()

    init {
        scope.launch {
            val gates = settings.map(::gateFor).distinctUntilChanged()
            merge(gates, retry.map { gateFor(settings.value) }).collectLatest { gate ->
                if (!gate.enabled) {
                    publishState(
                        state = SourceHealthState.PAUSED,
                        message = "Backend privacy source is paused",
                        recoveryLabel = null,
                    )
                    return@collectLatest
                }

                val endpoint = gate.endpoint
                if (endpoint == null) {
                    activeEndpoint = null
                    retainedRows = emptyList()
                    _snapshots.value = listOf(
                        snapshot(
                            state = SourceHealthState.FAILED,
                            rows = emptyList(),
                            lastSuccessElapsedMs = null,
                            lastSuccessWallMs = null,
                        ).copy(
                            health = PrivacySourceHealth(
                                source = PrivacySourceKind.BACKEND,
                                state = SourceHealthState.FAILED,
                                lastSuccessElapsedMs = null,
                                lastSuccessWallMs = null,
                                recoveryLabel = "Fix backend URL",
                                message = gate.invalidMessage ?: "Configured backend URL is invalid",
                            ),
                        ),
                    )
                    return@collectLatest
                }

                if (activeEndpoint != endpoint) {
                    val previousEndpoint = activeEndpoint
                    activeEndpoint = endpoint
                    retainedRows = emptyList()
                    val next = snapshot(
                            state = SourceHealthState.LOADING,
                            rows = emptyList(),
                            lastSuccessElapsedMs = null,
                            lastSuccessWallMs = null,
                        )
                    _snapshots.update { current ->
                        listOf(
                            if (previousEndpoint == null) {
                                next.preserveLoadingStartFrom(current.single())
                            } else {
                                next
                            },
                        )
                    }
                } else {
                    publishState(
                        state = resumedHealth(retainedRows.isNotEmpty()),
                        message = if (retainedRows.isEmpty()) null else "Refreshing cached backend findings",
                        recoveryLabel = "Retry",
                    )
                }

                while (true) {
                    try {
                        val response = fetch(endpoint)
                        currentCoroutineContext().ensureActive()
                        if (activeEndpoint != endpoint) continue
                        val sequence = responseSequence.incrementAndGet()
                        val mapped = response.devices.mapIndexed { index, dto ->
                            mapDevice(
                                dto = dto,
                                endpointNamespace = endpoint,
                                responseSequence = sequence,
                                rowIndex = index,
                                clock = clock,
                            )
                        }.sortedBy { it.observationKey.encoded }
                        retainedRows = mapped
                        val nowElapsed = clock.nowElapsedMs()
                        val nowWall = clock.nowWallClock().toEpochMilli()
                        _snapshots.value = listOf(
                            snapshot(
                                state = SourceHealthState.LIVE,
                                rows = mapped,
                                lastSuccessElapsedMs = nowElapsed,
                                lastSuccessWallMs = nowWall,
                            ),
                        )
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        publishState(
                            state = SourceHealthState.FAILED,
                            message = failure.message ?: "Backend privacy request failed",
                            recoveryLabel = "Retry",
                        )
                    }
                    delay(pollIntervalMs)
                }
            }
        }
    }

    override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
        if (source != PrivacySourceKind.BACKEND) {
            return PrivacyRecoveryResult.SourceUnavailable(source)
        }
        resetLoadingDeadline()
        retry.emit(Unit)
        return PrivacyRecoveryResult.Recovered(source)
    }

    private fun publishState(
        state: SourceHealthState,
        message: String?,
        recoveryLabel: String?,
    ) {
        _snapshots.update { current ->
            val previous = current.single()
            listOf(
                previous.copy(
                    health = previous.health.copy(
                        state = state,
                        message = message,
                        recoveryLabel = recoveryLabel,
                    ),
                    findings = retainedRows,
                    emittedAtElapsedMs = clock.nowElapsedMs(),
                ).preserveLoadingStartFrom(previous),
            )
        }
    }

    private fun resetLoadingDeadline() {
        _snapshots.update { current ->
            val snapshot = current.single()
            listOf(
                if (snapshot.health.state == SourceHealthState.LOADING) {
                    snapshot.copy(emittedAtElapsedMs = clock.nowElapsedMs())
                } else {
                    snapshot
                },
            )
        }
    }

    private fun snapshot(
        state: SourceHealthState,
        rows: List<PrivacyFinding> = retainedRows,
        lastSuccessElapsedMs: Long? = null,
        lastSuccessWallMs: Long? = null,
    ) = PrivacySourceSnapshot(
        health = PrivacySourceHealth(
            source = PrivacySourceKind.BACKEND,
            state = state,
            lastSuccessElapsedMs = lastSuccessElapsedMs,
            lastSuccessWallMs = lastSuccessWallMs,
            recoveryLabel = if (state == SourceHealthState.FAILED) "Retry" else null,
            message = null,
        ),
        findings = rows,
        emittedAtElapsedMs = clock.nowElapsedMs(),
    )

    private fun gateFor(current: DetectionSettings): BackendGate {
        val parsed = BackendEndpoint.parse(current.backendUrl)
        return BackendGate(
            enabled = current.sensorBackendEnabled,
            endpoint = parsed.getOrNull()?.baseUrl,
            invalidMessage = parsed.exceptionOrNull()?.message,
        )
    }

    companion object {
        internal fun mapDevice(
            dto: LivePrivacyDeviceDto,
            endpointNamespace: String,
            responseSequence: Long,
            rowIndex: Int,
            clock: MonotonicClock,
        ): PrivacyFinding {
            val fingerprint = dto.fingerprint?.takeIf { it.isNotBlank() }
            val stableId = fingerprint?.let { "$endpointNamespace|fingerprint:$it" }
            val record = stableId ?: "$endpointNamespace|ephemeral:$responseSequence:$rowIndex"
            val key = PrivacyFindingKey(PrivacySourceKind.BACKEND, record)
            val nowWall = clock.nowWallClock().toEpochMilli()
            val nowElapsed = clock.nowElapsedMs()
            val firstWall = dto.firstSeen.toWallMsOrNull()?.coerceAtMost(nowWall)
            val lastWall = dto.lastSeen.toWallMsOrNull()?.coerceAtMost(nowWall)
            val lastObservedElapsed = lastWall?.let {
                elapsedTimestampForWall(nowElapsed, nowWall, it)
            } ?: nowElapsed
            val title = dto.displayLabel?.takeIf { it.isNotBlank() }
                ?: dto.deviceType?.takeIf { it.isNotBlank() }
                ?: dto.privacyKind?.takeIf { it.isNotBlank() }
                ?: "Backend privacy finding"
            val category = categoryFor(dto.privacyKind, dto.deviceType, title)
            val evidence = listOfNotNull(
                dto.displayDetail?.takeIf { it.isNotBlank() },
                dto.evidence?.filter(String::isNotBlank)?.joinToString(" • ")?.takeIf { it.isNotBlank() },
            ).joinToString(" • ").takeIf { it.isNotBlank() }
            val rowText = listOfNotNull(
                dto.manufacturer,
                dto.deviceType,
                dto.displayLabel,
                dto.displayDetail,
                dto.privacyKind,
                evidence,
            ).joinToString(" ")
            val tokens = Regex("[A-Za-z0-9]+").findAll(rowText)
                .map { it.value.lowercase() }
                .toSet()
            val appleFamily = dto.bleCompanyId == 0x004C ||
                dto.appleContinuity != null ||
                tokens.any { it in APPLE_DEVICE_TOKENS }
            val airPods = "airpods" in tokens
            val listening = category == PrivacyCategory.REMOTE_LISTENING ||
                tokens.any { it in LISTENING_TOKENS }
            val plainAppleActivity = appleFamily &&
                !listening &&
                category == PrivacyCategory.APPLE_CONTINUITY

            val raw = PrivacyFinding(
                displayId = fingerprint ?: "backend-$responseSequence-$rowIndex",
                observationKey = key,
                source = PrivacySourceKind.BACKEND,
                stableSourceId = stableId,
                routableKey = stableId?.let { key },
                title = title,
                evidence = evidence,
                limitation = null,
                category = category,
                severity = if (plainAppleActivity) {
                    FindingSeverity.INFO
                } else {
                    severityFor(dto.riskLevel, category)
                },
                ownership = Ownership.UNKNOWN,
                signalDbm = dto.currentRssi,
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

        private fun categoryFor(kind: String?, type: String?, title: String): PrivacyCategory =
            when (kind?.uppercase()) {
                "VENUE_BEACON" -> PrivacyCategory.VENUE_BEACON
                "EVENT_BADGE" -> PrivacyCategory.EVENT_BADGE
                "MOBILE_KEY_LOCK" -> PrivacyCategory.MOBILE_KEY_LOCK
                "BLE_HID" -> PrivacyCategory.BLE_HID
                "AURACAST" -> PrivacyCategory.AURACAST
                "APPLE_CONTINUITY" -> PrivacyCategory.APPLE_CONTINUITY
                "REMOTE_LISTENING" -> PrivacyCategory.REMOTE_LISTENING
                "FLOCK_ALPR" -> PrivacyCategory.ALPR_CAMERA
                "CAMERA_NEAR" -> PrivacyCategory.SURVEILLANCE_CAMERA
                "SKIMMER" -> PrivacyCategory.ATTACK_TOOL
                "PAYMENT_READER" -> PrivacyCategory.PAYMENT_READER
                "TRACKER_NEAR" -> PrivacyCategory.BLE_TRACKER
                "META_GLASSES" -> PrivacyCategory.SMART_GLASSES
                else -> GlassesDetectorCategoryBridge.category(type ?: title)
            }

        private fun severityFor(risk: String?, category: PrivacyCategory): FindingSeverity =
            when (risk?.lowercase()) {
                "critical", "high" -> FindingSeverity.CRITICAL
                "medium", "awareness" -> FindingSeverity.AWARENESS
                "low", "nearby" -> FindingSeverity.NEARBY
                else -> when (category.threatLevel) {
                    3 -> FindingSeverity.CRITICAL
                    2 -> FindingSeverity.AWARENESS
                    1 -> FindingSeverity.NEARBY
                    else -> FindingSeverity.INFO
                }
            }

        private fun Double?.toWallMsOrNull(): Long? {
            val value = this ?: return null
            if (!value.isFinite() || value < 0.0) return null
            val milliseconds = value * 1_000.0
            return if (milliseconds >= Long.MAX_VALUE.toDouble()) Long.MAX_VALUE else milliseconds.toLong()
        }

        private fun elapsedTimestampForWall(nowElapsed: Long, nowWall: Long, observedWall: Long): Long {
            if (observedWall >= nowWall) return nowElapsed
            val age = saturatingSubtract(nowWall, observedWall)
            return if (age >= nowElapsed) 0L else nowElapsed - age
        }

        private fun saturatingSubtract(later: Long, earlier: Long): Long {
            if (later <= earlier) return 0L
            val difference = later - earlier
            return if (difference < 0L) Long.MAX_VALUE else difference
        }

        private val APPLE_DEVICE_TOKENS = setOf("apple", "airpods", "iphone", "ipad", "macbook")
        private val LISTENING_TOKENS = setOf("listening", "listen", "eavesdrop", "microphone")
    }
}

private data class BackendGate(
    val enabled: Boolean,
    val endpoint: String?,
    val invalidMessage: String?,
)

/** Keeps backend mapping isolated without exposing detector internals as durable identity. */
private object GlassesDetectorCategoryBridge {
    fun category(type: String): PrivacyCategory =
        com.friendorfoe.detection.GlassesDetector.categorizeDeviceType(type)
}

package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.repository.LocalDetectionPermissionUpdates
import com.friendorfoe.data.repository.LocalDetectionPermissions
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.WifiAnomalyDetector
import com.friendorfoe.detection.WifiPrivacyScanner
import com.friendorfoe.detection.WifiScanBatch
import com.friendorfoe.detection.WifiScanCoordinator
import com.friendorfoe.detection.WifiScanEvent
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

@Singleton
class WifiPrivacySourceAdapter internal constructor(
    private val settings: StateFlow<DetectionSettings>,
    private val permissions: StateFlow<LocalDetectionPermissions>,
    private val wifiEvents: () -> Flow<WifiScanEvent>,
    private val signatureMapper: (WifiScanBatch) -> List<GlassesDetection>,
    private val anomalyMapper: (WifiScanBatch) -> List<WifiAnomalyDetector.WifiAnomaly>,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
) : PrivacySourceAdapter {
    @Inject
    constructor(
        detectionPrefs: DetectionPrefs,
        permissionUpdates: LocalDetectionPermissionUpdates,
        coordinator: WifiScanCoordinator,
        privacyScanner: WifiPrivacyScanner,
        anomalyDetector: WifiAnomalyDetector,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        settings = detectionPrefs.settings,
        permissions = permissionUpdates.current,
        wifiEvents = coordinator::scanEvents,
        signatureMapper = privacyScanner::mapBatch,
        anomalyMapper = anomalyDetector::analyzeBatch,
        clock = clock,
        scope = scope,
    )

    override val adapterId: String = "wifi"
    override val representedSources: Set<PrivacySourceKind> = setOf(
        PrivacySourceKind.WIFI_ANALYSIS,
    )

    private val signatureRows = linkedMapOf<PrivacyFindingKey, PrivacyFinding>()
    private val anomalyRows = linkedMapOf<PrivacyFindingKey, PrivacyFinding>()
    private val retry = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    private val _snapshots = MutableStateFlow(listOf(initialSnapshot()))
    override val snapshots: StateFlow<List<PrivacySourceSnapshot>> = _snapshots.asStateFlow()

    init {
        scope.launch {
            val gates = combine(
                settings.map(::WifiSettings).distinctUntilChanged(),
                permissions.map { it.wifiManagerScanResults }.distinctUntilChanged(),
                ::WifiGate,
            ).distinctUntilChanged()
            merge(
                gates,
                retry.map { currentGate() },
            ).collectLatest { gate ->
                removeDisabledFeatureRows(gate.settings)
                transitionForGate(gate)
                if (gate.canRun) collectWifi(gate.settings)
            }
        }
    }

    override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
        if (source != PrivacySourceKind.WIFI_ANALYSIS) {
            return PrivacyRecoveryResult.SourceUnavailable(source)
        }
        resetLoadingDeadline()
        retry.emit(Unit)
        return PrivacyRecoveryResult.Recovered(source)
    }

    private suspend fun collectWifi(activeSettings: WifiSettings) {
        var terminalEvent = false
        try {
            wifiEvents().collect { event ->
                when (event) {
                    is WifiScanEvent.Success -> {
                        terminalEvent = false
                        publishBatch(event.batch, activeSettings)
                    }
                    is WifiScanEvent.Failure -> {
                        terminalEvent = true
                        publishFailure(event.message, event.observedElapsedMs)
                    }
                    is WifiScanEvent.Unsupported -> {
                        terminalEvent = true
                        publishUnsupported(event.message, event.observedElapsedMs)
                    }
                }
            }
            if (!terminalEvent) {
                publishFailure("Wi-Fi scan stream stopped unexpectedly", clock.nowElapsedMs())
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            publishFailure(
                failure.message ?: "Wi-Fi privacy analysis failed",
                clock.nowElapsedMs(),
            )
        }
    }

    private fun publishBatch(batch: WifiScanBatch, activeSettings: WifiSettings) {
        if (activeSettings.signaturesEnabled) {
            signatureMapper(batch).forEach { detection ->
                val finding = mapSignature(
                    detection = detection,
                    batch = batch,
                    previous = signatureRows.values.firstOrNull {
                        it.stableSourceId == signatureStableId(detection)
                    },
                )
                signatureRows[finding.observationKey] = finding
            }
            bound(signatureRows)
        }
        if (activeSettings.anomaliesEnabled) {
            anomalyMapper(batch).forEach { anomaly ->
                val record = anomalyRecord(anomaly)
                val key = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, record)
                anomalyRows[key] = mapAnomaly(
                    anomaly = anomaly,
                    batch = batch,
                    previous = anomalyRows[key],
                )
            }
            bound(anomalyRows)
        }

        updateSnapshot { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.LIVE,
                    lastSuccessElapsedMs = batch.observedElapsedMs,
                    lastSuccessWallMs = batch.observedWallMs,
                    recoveryLabel = null,
                    message = null,
                ),
                findings = rows(),
                emittedAtElapsedMs = batch.observedElapsedMs,
            )
        }
    }

    private fun removeDisabledFeatureRows(activeSettings: WifiSettings) {
        if (!activeSettings.signaturesEnabled) signatureRows.clear()
        if (!activeSettings.anomaliesEnabled) anomalyRows.clear()
    }

    private fun transitionForGate(gate: WifiGate) {
        val state = when {
            !gate.enabled -> SourceHealthState.PAUSED
            !gate.permitted -> SourceHealthState.PERMISSION_BLOCKED
            else -> resumedHealth(rows().isNotEmpty())
        }
        updateSnapshot { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = state,
                    recoveryLabel = when (state) {
                        SourceHealthState.PERMISSION_BLOCKED -> "Grant permission"
                        SourceHealthState.PAUSED -> null
                        else -> "Retry"
                    },
                    message = when (state) {
                        SourceHealthState.PERMISSION_BLOCKED ->
                            "Location permission is required for Wi-Fi privacy analysis"
                        SourceHealthState.PAUSED -> "Paused in detection settings"
                        SourceHealthState.STALE -> "Restarting with cached findings"
                        else -> null
                    },
                ),
                findings = rows(),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun publishFailure(message: String, elapsed: Long) {
        updateSnapshot { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.FAILED,
                    recoveryLabel = "Retry",
                    message = message,
                ),
                findings = rows(),
                emittedAtElapsedMs = elapsed,
            )
        }
    }

    private fun publishUnsupported(message: String, elapsed: Long) {
        updateSnapshot { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.UNSUPPORTED,
                    recoveryLabel = null,
                    message = message,
                ),
                findings = rows(),
                emittedAtElapsedMs = elapsed,
            )
        }
    }

    private fun updateSnapshot(transform: (PrivacySourceSnapshot) -> PrivacySourceSnapshot) {
        _snapshots.update { current ->
            val previous = current.single()
            listOf(transform(previous).preserveLoadingStartFrom(previous))
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

    private fun rows(): List<PrivacyFinding> =
        (signatureRows.values + anomalyRows.values).sortedBy { it.observationKey.encoded }

    private fun currentGate(): WifiGate = WifiGate(
        settings = WifiSettings(settings.value),
        permitted = permissions.value.wifiManagerScanResults,
    )

    private fun initialSnapshot(): PrivacySourceSnapshot {
        val gate = currentGate()
        val state = when {
            !gate.enabled -> SourceHealthState.PAUSED
            !gate.permitted -> SourceHealthState.PERMISSION_BLOCKED
            else -> SourceHealthState.LOADING
        }
        return PrivacySourceSnapshot(
            health = PrivacySourceHealth(
                source = PrivacySourceKind.WIFI_ANALYSIS,
                state = state,
                lastSuccessElapsedMs = null,
                lastSuccessWallMs = null,
                recoveryLabel = if (state == SourceHealthState.PERMISSION_BLOCKED) {
                    "Grant permission"
                } else {
                    null
                },
                message = when (state) {
                    SourceHealthState.PAUSED -> "Paused in detection settings"
                    SourceHealthState.PERMISSION_BLOCKED ->
                        "Location permission is required for Wi-Fi privacy analysis"
                    else -> null
                },
            ),
            findings = emptyList(),
            emittedAtElapsedMs = clock.nowElapsedMs(),
        )
    }

    private fun bound(rows: LinkedHashMap<PrivacyFindingKey, PrivacyFinding>) {
        while (rows.size > MAX_ROWS_PER_ANALYZER) {
            val oldest = rows.minByOrNull { it.value.lastObservedElapsedMs } ?: return
            rows.remove(oldest.key)
        }
    }

    companion object {
        private const val MAX_ROWS_PER_ANALYZER = 200

        internal fun mapSignature(
            detection: GlassesDetection,
            batch: WifiScanBatch,
            previous: PrivacyFinding? = null,
        ): PrivacyFinding {
            val stableId = signatureStableId(detection)
            val key = PrivacyFindingKey(
                PrivacySourceKind.WIFI_ANALYSIS,
                "signature:$stableId",
            )
            val evidence = listOf(
                detection.manufacturer.takeIf { it.isNotBlank() },
                detection.matchReason.takeIf { it.isNotBlank() },
                detection.deviceName?.takeIf { it.isNotBlank() },
            ).filterNotNull().joinToString(" • ").takeIf { it.isNotBlank() }
            return PrivacyFinding(
                displayId = detection.mac.ifBlank { stableId },
                observationKey = key,
                source = PrivacySourceKind.WIFI_ANALYSIS,
                stableSourceId = stableId,
                routableKey = key,
                title = detection.deviceType.ifBlank { "Nearby Wi-Fi device" },
                evidence = evidence,
                limitation = null,
                category = detection.category,
                severity = severityFor(detection.category.threatLevel),
                ownership = Ownership.UNKNOWN,
                signalDbm = detection.rssi,
                firstSeenWallMs = previous?.firstSeenWallMs ?: batch.observedWallMs,
                lastSeenWallMs = batch.observedWallMs,
                lastObservedElapsedMs = batch.observedElapsedMs,
                protocolTtlMs = null,
                hasLiveLocalSamples = true,
            )
        }

        internal fun mapAnomaly(
            anomaly: WifiAnomalyDetector.WifiAnomaly,
            batch: WifiScanBatch,
            previous: PrivacyFinding? = null,
        ): PrivacyFinding {
            val record = anomalyRecord(anomaly)
            val key = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, record)
            return PrivacyFinding(
                displayId = record,
                observationKey = key,
                source = PrivacySourceKind.WIFI_ANALYSIS,
                stableSourceId = record,
                routableKey = key,
                title = when (anomaly.type.lowercase()) {
                    "evil_twin" -> "Possible evil-twin Wi-Fi network"
                    "karma_attack" -> "Possible Wi-Fi karma attack"
                    "pwnagotchi" -> "Pwnagotchi pen-test device"
                    else -> "Suspicious Wi-Fi activity"
                },
                evidence = anomaly.details,
                limitation = "Wi-Fi analysis identifies network behavior, not a person.",
                category = PrivacyCategory.ATTACK_TOOL,
                severity = severityFor(anomaly.threatLevel),
                ownership = Ownership.UNKNOWN,
                signalDbm = anomaly.evidence.maxOfOrNull { it.rssi },
                firstSeenWallMs = previous?.firstSeenWallMs ?: batch.observedWallMs,
                lastSeenWallMs = batch.observedWallMs,
                lastObservedElapsedMs = batch.observedElapsedMs,
                protocolTtlMs = null,
                hasLiveLocalSamples = true,
            )
        }

        private fun signatureStableId(detection: GlassesDetection): String =
            detection.fingerprintKey.takeIf { it.isNotBlank() }
                ?: detection.mac.takeIf { it.isNotBlank() }?.let { "mac:$it" }
                ?: "wifi-signature:${detection.manufacturer}:${detection.deviceType}"

        private fun anomalyRecord(anomaly: WifiAnomalyDetector.WifiAnomaly): String {
            val identity = anomaly.bssids
                .filter { it.isNotBlank() }
                .map { it.lowercase() }
                .sorted()
                .joinToString(",")
                .ifBlank { "ssid:${anomaly.ssid.lowercase()}" }
            return "anomaly:${anomaly.type.lowercase()}:$identity"
        }

        private fun severityFor(threatLevel: Int): FindingSeverity = when {
            threatLevel >= 3 -> FindingSeverity.CRITICAL
            threatLevel == 2 -> FindingSeverity.AWARENESS
            threatLevel == 1 -> FindingSeverity.NEARBY
            else -> FindingSeverity.INFO
        }
    }
}

private data class WifiSettings(
    val signaturesEnabled: Boolean,
    val anomaliesEnabled: Boolean,
    val backendOnly: Boolean,
) {
    constructor(settings: DetectionSettings) : this(
        signaturesEnabled = settings.phonePrivacyScanEnabled,
        anomaliesEnabled = settings.wifiAnomalyEnabled,
        backendOnly = settings.backendOnlyMode,
    )
}

private data class WifiGate(
    val settings: WifiSettings,
    val permitted: Boolean,
) {
    val enabled: Boolean = !settings.backendOnly &&
        (settings.signaturesEnabled || settings.anomaliesEnabled)
    val canRun: Boolean = enabled && permitted
}

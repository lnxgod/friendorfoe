package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.repository.LocalDetectionPermissionUpdates
import com.friendorfoe.data.repository.LocalDetectionPermissions
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.GlassesDetector
import com.friendorfoe.detection.GlassesScanEvent
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.UltrasonicDetector
import com.friendorfoe.detection.UltrasonicScanEvent
import com.friendorfoe.sensor.SensorFusionEngine
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

@Singleton
class PhonePrivacySourceAdapter internal constructor(
    private val settings: StateFlow<DetectionSettings>,
    private val permissions: StateFlow<LocalDetectionPermissions>,
    private val bleEvents: () -> Flow<GlassesScanEvent>,
    private val ultrasonicEvents: () -> Flow<UltrasonicScanEvent>,
    private val bleTracker: BleTracker,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
    private val compassBearing: () -> Float? = { null },
    private val followerPollIntervalMs: Long = 30_000L,
) : PrivacySourceAdapter, RssiSampleSource {

    @Inject
    constructor(
        detectionPrefs: DetectionPrefs,
        permissionUpdates: LocalDetectionPermissionUpdates,
        glassesDetector: GlassesDetector,
        ultrasonicDetector: UltrasonicDetector,
        bleTracker: BleTracker,
        sensorFusionEngine: SensorFusionEngine,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        settings = detectionPrefs.settings,
        permissions = permissionUpdates.current,
        bleEvents = glassesDetector::scanEvents,
        ultrasonicEvents = ultrasonicDetector::monitoringEvents,
        bleTracker = bleTracker,
        clock = clock,
        scope = scope,
        compassBearing = {
            if (sensorFusionEngine.orientationReady.value) {
                sensorFusionEngine.orientation.value.azimuthDegrees
            } else {
                null
            }
        },
    )

    override val adapterId: String = "phone"
    override val representedSources: Set<PrivacySourceKind> = setOf(
        PrivacySourceKind.PHONE_BLE,
        PrivacySourceKind.PHONE_ULTRASONIC,
    )

    private val bleRows = linkedMapOf<PrivacyFindingKey, PrivacyFinding>()
    private val ultrasonicRows = linkedMapOf<PrivacyFindingKey, PrivacyFinding>()
    private val ultrasonicRecordByBin = linkedMapOf<Int, String>()
    private val ephemeralBleRecordByMac = linkedMapOf<String, String>()
    private val bleObservationElapsedByMac = linkedMapOf<String, Long>()
    private val bleSequence = AtomicLong(0L)
    private val ultrasonicSequence = AtomicLong(0L)
    private val bleRetry = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    private val ultrasonicRetry = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    private val rssiSamples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 64)

    private val _snapshots = MutableStateFlow(
        listOf(
            initialSnapshot(PrivacySourceKind.PHONE_BLE),
            initialSnapshot(PrivacySourceKind.PHONE_ULTRASONIC),
        ),
    )
    override val snapshots: StateFlow<List<PrivacySourceSnapshot>> = _snapshots.asStateFlow()

    override fun samplesFor(key: PrivacyFindingKey): Flow<RssiSample> =
        rssiSamples.filter { it.findingKey == key }

    init {
        scope.launch {
            merge(
                bleGateChanges(),
                bleRetry.map { currentBleGate() },
            ).collectLatest { gate ->
                transitionForGate(
                    source = PrivacySourceKind.PHONE_BLE,
                    enabled = gate.enabled,
                    permitted = gate.permitted,
                )
                if (gate.canRun) {
                    collectBle()
                }
            }
        }
        scope.launch {
            followerGateChanges().collectLatest { gate ->
                if (!gate.stalkerEnabled) {
                    removeFollowerRows()
                } else if (gate.bleCanRun) {
                    collectFollowers()
                }
            }
        }
        scope.launch {
            merge(
                ultrasonicGateChanges(),
                ultrasonicRetry.map { currentUltrasonicGate() },
            ).collectLatest { gate ->
                transitionForGate(
                    source = PrivacySourceKind.PHONE_ULTRASONIC,
                    enabled = gate.enabled,
                    permitted = gate.permitted,
                )
                if (gate.canRun) {
                    collectUltrasonic()
                }
            }
        }
    }

    override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
        return when (source) {
            PrivacySourceKind.PHONE_BLE -> {
                resetLoadingDeadline(source)
                bleRetry.emit(Unit)
                PrivacyRecoveryResult.Recovered(source)
            }
            PrivacySourceKind.PHONE_ULTRASONIC -> {
                resetLoadingDeadline(source)
                ultrasonicRetry.emit(Unit)
                PrivacyRecoveryResult.Recovered(source)
            }
            else -> PrivacyRecoveryResult.SourceUnavailable(source)
        }
    }

    private fun bleGateChanges(): Flow<PhoneSourceGate> =
        combine(settings, permissions) { currentSettings, currentPermissions ->
            PhoneSourceGate(
                enabled = currentSettings.phonePrivacyScanEnabled &&
                    !currentSettings.backendOnlyMode,
                permitted = currentPermissions.bluetoothScan,
            )
        }.distinctUntilChanged()

    private fun ultrasonicGateChanges(): Flow<PhoneSourceGate> =
        combine(settings, permissions) { currentSettings, currentPermissions ->
            PhoneSourceGate(
                enabled = currentSettings.ultrasonicEnabled &&
                    !currentSettings.backendOnlyMode,
                permitted = currentPermissions.audioCapture,
            )
        }.distinctUntilChanged()

    private fun followerGateChanges(): Flow<FollowerGate> =
        combine(settings, permissions) { currentSettings, currentPermissions ->
            FollowerGate(
                bleCanRun = currentSettings.phonePrivacyScanEnabled &&
                    !currentSettings.backendOnlyMode &&
                    currentPermissions.bluetoothScan,
                stalkerEnabled = currentSettings.stalkerEnabled,
            )
        }.distinctUntilChanged()

    private fun currentBleGate(): PhoneSourceGate = PhoneSourceGate(
        enabled = settings.value.phonePrivacyScanEnabled && !settings.value.backendOnlyMode,
        permitted = permissions.value.bluetoothScan,
    )

    private fun currentUltrasonicGate(): PhoneSourceGate = PhoneSourceGate(
        enabled = settings.value.ultrasonicEnabled && !settings.value.backendOnlyMode,
        permitted = permissions.value.audioCapture,
    )

    private suspend fun collectBle() {
        var terminalEvent = false
        try {
            bleEvents().collect { event ->
                when (event) {
                    GlassesScanEvent.Ready -> {
                        terminalEvent = false
                        publishSuccess(
                            PrivacySourceKind.PHONE_BLE,
                            clock.nowElapsedMs(),
                            clock.nowWallClock().toEpochMilli(),
                        )
                    }
                    is GlassesScanEvent.Observation -> {
                        terminalEvent = false
                        val nowElapsed = clock.nowElapsedMs()
                        val nowWall = clock.nowWallClock().toEpochMilli()
                        val bearing = compassBearing()
                        if (settings.value.stalkerEnabled) {
                            bleTracker.recordPrivacyObservation(event.detection, bearing ?: 0f)
                        } else {
                            bleTracker.recordDirectionSample(
                                mac = event.detection.mac,
                                rssi = event.detection.rssi,
                                compassBearing = bearing ?: 0f,
                            )
                        }
                        val finding = mapBle(
                            detection = event.detection,
                            observedElapsedMs = nowElapsed,
                            observedWallMs = nowWall,
                            observationRecordId = observationRecordFor(event.detection),
                        )
                        synchronized(bleRows) {
                            bleObservationElapsedByMac[event.detection.mac] = nowElapsed
                            bleRows[finding.observationKey] = finding
                            boundBleCache()
                        }
                        if (bearing != null) {
                            rssiSamples.tryEmit(
                                RssiSample(
                                    findingKey = finding.observationKey,
                                    dbm = event.detection.rssi,
                                    azimuthDegrees = bearing,
                                    observedAtElapsedMs = nowElapsed,
                                ),
                            )
                        }
                        publishSuccess(PrivacySourceKind.PHONE_BLE, nowElapsed, nowWall)
                    }
                    is GlassesScanEvent.Failure -> {
                        terminalEvent = true
                        publishFailure(PrivacySourceKind.PHONE_BLE, event.message)
                    }
                    is GlassesScanEvent.PermissionBlocked -> {
                        terminalEvent = true
                        publishPermissionBlocked(PrivacySourceKind.PHONE_BLE, event.message)
                    }
                    is GlassesScanEvent.Unsupported -> {
                        terminalEvent = true
                        publishUnsupported(PrivacySourceKind.PHONE_BLE, event.message)
                    }
                }
            }
            if (!terminalEvent) {
                publishFailure(PrivacySourceKind.PHONE_BLE, "BLE privacy scan stopped unexpectedly")
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            publishFailure(
                PrivacySourceKind.PHONE_BLE,
                failure.message ?: "BLE privacy scan failed",
            )
        }
    }

    private suspend fun collectFollowers() {
        while (true) {
            delay(followerPollIntervalMs)
            val nowElapsed = clock.nowElapsedMs()
            val nowWall = clock.nowWallClock().toEpochMilli()
            val alerts = bleTracker.checkForFollowers()
            if (alerts.isEmpty()) continue
            synchronized(bleRows) {
                alerts.forEach { alert ->
                    val key = PrivacyFindingKey(
                        PrivacySourceKind.PHONE_BLE,
                        "follower:${alert.device.mac}",
                    )
                    val wallAgeMs = (nowWall - alert.device.lastSeen.toEpochMilli())
                        .coerceAtLeast(0L)
                    val lastRealObservationElapsedMs =
                        bleObservationElapsedByMac[alert.device.mac]
                            ?: bleRows[key]?.lastObservedElapsedMs
                            ?: (nowElapsed - wallAgeMs).coerceAtLeast(0L)
                    val follower = mapFollower(
                        alert,
                        lastRealObservationElapsedMs,
                        nowWall,
                    )
                    bleRows[follower.observationKey] = follower
                }
                boundBleCache()
            }
            updateSnapshot(PrivacySourceKind.PHONE_BLE) { previous ->
                previous.copy(
                    findings = rowsFor(PrivacySourceKind.PHONE_BLE),
                    emittedAtElapsedMs = nowElapsed,
                )
            }
        }
    }

    private fun removeFollowerRows() {
        val removed = synchronized(bleRows) {
            val oldSize = bleRows.size
            bleRows.entries.removeAll { it.key.sourceRecordId.startsWith("follower:") }
            oldSize != bleRows.size
        }
        if (!removed) return
        updateSnapshot(PrivacySourceKind.PHONE_BLE) { previous ->
            previous.copy(
                findings = rowsFor(PrivacySourceKind.PHONE_BLE),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun observationRecordFor(detection: GlassesDetection): String {
        val canonical = detection.fingerprintKey.takeIf { it.isNotBlank() }
        if (canonical != null) return "observation:$canonical"
        return synchronized(ephemeralBleRecordByMac) {
            ephemeralBleRecordByMac.getOrPut(detection.mac) {
                "ephemeral:${bleSequence.incrementAndGet()}"
            }
        }
    }

    private fun boundBleCache() {
        while (bleRows.size > MAX_BLE_ROWS) {
            val oldest = bleRows.minByOrNull { it.value.lastObservedElapsedMs } ?: break
            bleRows.remove(oldest.key)
            val record = oldest.key.sourceRecordId
            if (record.startsWith("ephemeral:")) {
                synchronized(ephemeralBleRecordByMac) {
                    ephemeralBleRecordByMac.entries.removeAll { it.value == record }
                }
            }
        }
        while (bleObservationElapsedByMac.size > MAX_BLE_ROWS) {
            bleObservationElapsedByMac.remove(bleObservationElapsedByMac.keys.first())
        }
    }

    private fun ultrasonicRecordFor(frequencyHz: Float): String {
        val bin = (frequencyHz / ULTRASONIC_IDENTITY_BIN_HZ).roundToInt()
        return synchronized(ultrasonicRecordByBin) {
            ultrasonicRecordByBin.getOrPut(bin) {
                "ultrasonic:${ultrasonicSequence.incrementAndGet()}"
            }
        }
    }

    private fun boundUltrasonicCache() {
        while (ultrasonicRows.size > MAX_ULTRASONIC_ROWS) {
            val oldest = ultrasonicRows.minByOrNull { it.value.lastObservedElapsedMs } ?: break
            ultrasonicRows.remove(oldest.key)
            ultrasonicRecordByBin.entries.removeAll {
                it.value == oldest.key.sourceRecordId
            }
        }
    }

    private suspend fun collectUltrasonic() {
        var terminalEvent = false
        try {
            ultrasonicEvents().collect { event ->
                when (event) {
                    UltrasonicScanEvent.Ready -> {
                        terminalEvent = false
                        publishSuccess(
                            PrivacySourceKind.PHONE_ULTRASONIC,
                            clock.nowElapsedMs(),
                            clock.nowWallClock().toEpochMilli(),
                        )
                    }
                    is UltrasonicScanEvent.Observation -> {
                        terminalEvent = false
                        val nowElapsed = clock.nowElapsedMs()
                        val nowWall = clock.nowWallClock().toEpochMilli()
                        val finding = mapUltrasonic(
                            alert = event.alert,
                            observationRecordId = ultrasonicRecordFor(event.alert.frequencyHz),
                            observedElapsedMs = nowElapsed,
                            observedWallMs = nowWall,
                        )
                        synchronized(ultrasonicRows) {
                            ultrasonicRows[finding.observationKey] = finding
                            boundUltrasonicCache()
                        }
                        publishSuccess(PrivacySourceKind.PHONE_ULTRASONIC, nowElapsed, nowWall)
                    }
                    is UltrasonicScanEvent.Failure -> {
                        terminalEvent = true
                        publishFailure(PrivacySourceKind.PHONE_ULTRASONIC, event.message)
                    }
                    is UltrasonicScanEvent.PermissionBlocked -> {
                        terminalEvent = true
                        publishPermissionBlocked(
                            PrivacySourceKind.PHONE_ULTRASONIC,
                            event.message,
                        )
                    }
                    is UltrasonicScanEvent.Unsupported -> {
                        terminalEvent = true
                        publishUnsupported(PrivacySourceKind.PHONE_ULTRASONIC, event.message)
                    }
                }
            }
            if (!terminalEvent) {
                publishFailure(
                    PrivacySourceKind.PHONE_ULTRASONIC,
                    "Ultrasonic monitoring stopped unexpectedly",
                )
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            publishFailure(
                PrivacySourceKind.PHONE_ULTRASONIC,
                failure.message ?: "Ultrasonic monitoring failed",
            )
        }
    }

    private fun transitionForGate(
        source: PrivacySourceKind,
        enabled: Boolean,
        permitted: Boolean,
    ) {
        val state = when {
            !enabled -> SourceHealthState.PAUSED
            !permitted -> SourceHealthState.PERMISSION_BLOCKED
            else -> resumedHealth(rowsFor(source).isNotEmpty())
        }
        updateSnapshot(source) { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = state,
                    recoveryLabel = when (state) {
                        SourceHealthState.PERMISSION_BLOCKED -> "Grant permission"
                        SourceHealthState.PAUSED -> null
                        else -> "Retry"
                    },
                    message = when (state) {
                        SourceHealthState.PERMISSION_BLOCKED -> permissionMessage(source)
                        SourceHealthState.PAUSED -> "Paused in detection settings"
                        SourceHealthState.STALE -> "Restarting with cached findings"
                        else -> null
                    },
                ),
                findings = rowsFor(source),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun publishSuccess(source: PrivacySourceKind, elapsed: Long, wall: Long) {
        updateSnapshot(source) { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.LIVE,
                    lastSuccessElapsedMs = elapsed,
                    lastSuccessWallMs = wall,
                    recoveryLabel = null,
                    message = null,
                ),
                findings = rowsFor(source),
                emittedAtElapsedMs = elapsed,
            )
        }
    }

    private fun publishFailure(source: PrivacySourceKind, message: String) {
        updateSnapshot(source) { previous ->
            val bluetoothRadioOff = source == PrivacySourceKind.PHONE_BLE &&
                (message == BLUETOOTH_RADIO_OFF_MESSAGE ||
                    previous.health.recoveryLabel == TURN_ON_BLUETOOTH_RECOVERY)
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.FAILED,
                    recoveryLabel = if (bluetoothRadioOff) {
                        TURN_ON_BLUETOOTH_RECOVERY
                    } else {
                        "Retry"
                    },
                    message = if (bluetoothRadioOff) {
                        BLUETOOTH_RADIO_OFF_MESSAGE
                    } else {
                        message
                    },
                ),
                findings = rowsFor(source),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun publishUnsupported(source: PrivacySourceKind, message: String) {
        updateSnapshot(source) { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.UNSUPPORTED,
                    recoveryLabel = null,
                    message = message,
                ),
                findings = rowsFor(source),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun publishPermissionBlocked(source: PrivacySourceKind, message: String) {
        updateSnapshot(source) { previous ->
            previous.copy(
                health = previous.health.copy(
                    state = SourceHealthState.PERMISSION_BLOCKED,
                    recoveryLabel = "Grant permission",
                    message = message,
                ),
                findings = rowsFor(source),
                emittedAtElapsedMs = clock.nowElapsedMs(),
            )
        }
    }

    private fun updateSnapshot(
        source: PrivacySourceKind,
        transform: (PrivacySourceSnapshot) -> PrivacySourceSnapshot,
    ) {
        _snapshots.update { current ->
            current.map { snapshot ->
                if (snapshot.health.source == source) {
                    transform(snapshot).preserveLoadingStartFrom(snapshot)
                } else {
                    snapshot
                }
            }.sortedBy { it.health.source.preferenceId }
        }
    }

    private fun resetLoadingDeadline(source: PrivacySourceKind) {
        _snapshots.update { current ->
            current.map { snapshot ->
                if (snapshot.health.source == source && snapshot.health.state == SourceHealthState.LOADING) {
                    snapshot.copy(emittedAtElapsedMs = clock.nowElapsedMs())
                } else {
                    snapshot
                }
            }
        }
    }

    private fun rowsFor(source: PrivacySourceKind): List<PrivacyFinding> =
        when (source) {
            PrivacySourceKind.PHONE_BLE -> synchronized(bleRows) { bleRows.values.toList() }
            PrivacySourceKind.PHONE_ULTRASONIC -> synchronized(ultrasonicRows) {
                ultrasonicRows.values.toList()
            }
            else -> emptyList()
        }.sortedBy { it.observationKey.encoded }

    private fun initialSnapshot(source: PrivacySourceKind): PrivacySourceSnapshot {
        val enabled = when (source) {
            PrivacySourceKind.PHONE_BLE -> settings.value.phonePrivacyScanEnabled
            PrivacySourceKind.PHONE_ULTRASONIC -> settings.value.ultrasonicEnabled
            else -> false
        } && !settings.value.backendOnlyMode
        val permitted = when (source) {
            PrivacySourceKind.PHONE_BLE -> permissions.value.bluetoothScan
            PrivacySourceKind.PHONE_ULTRASONIC -> permissions.value.audioCapture
            else -> false
        }
        val state = when {
            !enabled -> SourceHealthState.PAUSED
            !permitted -> SourceHealthState.PERMISSION_BLOCKED
            else -> SourceHealthState.LOADING
        }
        return PrivacySourceSnapshot(
            health = PrivacySourceHealth(
                source = source,
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
                    SourceHealthState.PERMISSION_BLOCKED -> permissionMessage(source)
                    else -> null
                },
            ),
            findings = emptyList(),
            emittedAtElapsedMs = clock.nowElapsedMs(),
        )
    }

    private fun permissionMessage(source: PrivacySourceKind): String = when (source) {
        PrivacySourceKind.PHONE_BLE -> "Nearby devices permission is required"
        PrivacySourceKind.PHONE_ULTRASONIC -> "Microphone permission is required"
        else -> "Permission is required"
    }

    companion object {
        internal fun mapBle(
            detection: GlassesDetection,
            observedElapsedMs: Long,
            observedWallMs: Long,
            observationRecordId: String = detection.fingerprintKey
                .takeIf { it.isNotBlank() }
                ?.let { "observation:$it" }
                ?: "ephemeral:unassigned",
        ): PrivacyFinding {
            val canonical = detection.fingerprintKey.takeIf { it.isNotBlank() }
            val text = listOf(
                detection.manufacturer,
                detection.deviceType,
                detection.deviceName.orEmpty(),
                detection.matchReason,
                detection.details.values.joinToString(" "),
            ).joinToString(" ")
            val tokens = Regex("[A-Za-z0-9]+").findAll(text)
                .map { it.value.lowercase() }
                .toSet()
            val apple = tokens.any { it in APPLE_DEVICE_TOKENS } ||
                detection.bleCompanyId == 0x004C
            val airPods = "airpods" in tokens
            val listening = detection.category == PrivacyCategory.REMOTE_LISTENING ||
                listOf("listening", "live listen", "eavesdrop", "microphone").any {
                    text.contains(it, ignoreCase = true)
                }
            val key = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, observationRecordId)
            val knownEvidence = listOf(
                detection.manufacturer.takeIf { it.isNotBlank() },
                detection.matchReason.takeIf { it.isNotBlank() },
            ).filterNotNull().joinToString(" • ").takeIf { it.isNotBlank() }
            val raw = PrivacyFinding(
                displayId = detection.mac,
                observationKey = key,
                source = PrivacySourceKind.PHONE_BLE,
                stableSourceId = canonical,
                routableKey = key.takeIf { canonical != null },
                title = detection.deviceType.ifBlank { detection.deviceName ?: "Nearby BLE device" },
                evidence = knownEvidence,
                limitation = null,
                category = detection.category,
                severity = severityFor(detection.category),
                ownership = if (detection.isBonded ||
                    detection.matchReason.startsWith("own_device:")
                ) Ownership.OWNED else Ownership.UNKNOWN,
                signalDbm = detection.rssi,
                firstSeenWallMs = detection.firstSeen.toEpochMilli(),
                lastSeenWallMs = detection.lastSeen.toEpochMilli().coerceAtMost(observedWallMs),
                lastObservedElapsedMs = observedElapsedMs,
                protocolTtlMs = null,
                hasLiveLocalSamples = true,
                appleEvidence = PrivacyAppleListeningEvidence(
                    appleFamilyEvidence = apple,
                    airPodsAssociationEvidence = airPods,
                    listeningOrientedCategoryOrWording = listening,
                ).takeIf { apple || listening },
            )
            return PrivacyFindingNormalizer.normalize(raw)
        }

        internal fun mapUltrasonic(
            alert: UltrasonicDetector.UltrasonicAlert,
            observationRecordId: String,
            observedElapsedMs: Long,
            observedWallMs: Long,
        ): PrivacyFinding {
            val key = PrivacyFindingKey(PrivacySourceKind.PHONE_ULTRASONIC, observationRecordId)
            return PrivacyFinding(
                displayId = observationRecordId,
                observationKey = key,
                source = PrivacySourceKind.PHONE_ULTRASONIC,
                stableSourceId = null,
                routableKey = null,
                title = "Ultrasonic beacon near ${"%.1f".format(alert.frequencyHz / 1_000f)} kHz",
                evidence = "Signal is ${"%.1f".format(alert.snrDb)} dB above the local noise floor",
                limitation = "Ultrasonic audio identifies a signal, not the device emitting it.",
                category = PrivacyCategory.ULTRASONIC_BEACON,
                severity = FindingSeverity.CRITICAL,
                ownership = Ownership.UNKNOWN,
                signalDbm = null,
                firstSeenWallMs = observedWallMs,
                lastSeenWallMs = observedWallMs,
                lastObservedElapsedMs = observedElapsedMs,
                protocolTtlMs = null,
                hasLiveLocalSamples = false,
            )
        }

        internal fun mapFollower(
            alert: BleTracker.StalkerAlert,
            observedElapsedMs: Long,
            observedWallMs: Long,
        ): PrivacyFinding {
            val record = "follower:${alert.device.mac}"
            val key = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, record)
            return PrivacyFinding(
                displayId = alert.device.mac,
                observationKey = key,
                source = PrivacySourceKind.PHONE_BLE,
                stableSourceId = null,
                routableKey = key,
                title = alert.device.deviceType ?: "Nearby BLE device",
                evidence = "Repeated local sightings: ${alert.reason}",
                limitation = "Movement correlation is based on this phone's local samples.",
                category = PrivacyCategory.BLE_TRACKER,
                severity = when (alert.threatLevel) {
                    3 -> FindingSeverity.CRITICAL
                    2 -> FindingSeverity.AWARENESS
                    else -> FindingSeverity.NEARBY
                },
                ownership = Ownership.UNKNOWN,
                signalDbm = alert.device.peakRssi,
                firstSeenWallMs = alert.device.firstSeen.toEpochMilli(),
                lastSeenWallMs = minOf(alert.device.lastSeen.toEpochMilli(), observedWallMs),
                lastObservedElapsedMs = observedElapsedMs,
                protocolTtlMs = null,
                hasLiveLocalSamples = true,
            )
        }

        private fun severityFor(category: PrivacyCategory): FindingSeverity = when (category.threatLevel) {
            3 -> FindingSeverity.CRITICAL
            2 -> FindingSeverity.AWARENESS
            1 -> FindingSeverity.NEARBY
            else -> FindingSeverity.INFO
        }

        private const val MAX_BLE_ROWS = 200
        private const val MAX_ULTRASONIC_ROWS = 64
        private const val ULTRASONIC_IDENTITY_BIN_HZ = 100f
        private const val BLUETOOTH_RADIO_OFF_MESSAGE = "Bluetooth is turned off"
        private const val TURN_ON_BLUETOOTH_RECOVERY = "Turn on Bluetooth"
        private val APPLE_DEVICE_TOKENS = setOf(
            "apple",
            "airpods",
            "iphone",
            "ipad",
            "ipod",
            "macbook",
        )
    }
}

private data class PhoneSourceGate(
    val enabled: Boolean,
    val permitted: Boolean,
) {
    val canRun: Boolean get() = enabled && permitted
}

private data class FollowerGate(
    val bleCanRun: Boolean,
    val stalkerEnabled: Boolean,
)

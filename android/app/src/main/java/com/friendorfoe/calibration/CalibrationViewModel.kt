package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.location.Location
import android.location.LocationListener
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.gson.JsonObject
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * Orchestrates a calibration walk:
 *
 *  1. POST /walk/start → backend returns session_id + UUID to advertise
 *  2. Start BLE advertiser with that UUID
 *  3. Listen to GPS_PROVIDER, post each fix to /walk/sample
 *  4. Poll /walk/feedback every 2 s so the operator sees which sensors
 *     are hearing them, with live RSSI
 *  5. Operator walks up to each sensor and taps "I'm here" — the VM
 *     POSTs /walk/checkpoint, anchoring the OLS fit + getting back
 *     a sanity result (label-match, GPS-drift) per sensor
 *  6. POST /walk/end → backend runs the per-listener OLS fit and applies
 *
 * The screen consumes `state` as a single immutable snapshot. All state
 * mutations happen here so the Compose layer stays a pure render.
 */
@HiltViewModel
class CalibrationViewModel @Inject constructor(
    private val prefs: CalibrationSettingsStore,
    private val advertiser: CalibrationAdvertiser,
    private val api: CalibrationBackend,
    private val platform: CalibrationPlatform,
) : ViewModel() {
    private companion object {
        const val ANCHOR_PHONE_GPS = "phone_gps"
        const val ANCHOR_SENSOR_FALLBACK = "sensor_position_fallback"
        const val CHECKPOINT_HEARD_FRESH_S = 3.0
        const val CHECKPOINT_GOOD_GPS_ACCURACY_M = 10f
        const val CHECKPOINT_RETRY_AFTER_MS = 3500L
    }

    /** One sensor as known to the fleet — drives the "tap which sensor
     *  you're at" cards and the live RSSI feedback table. */
    data class SensorInfo(
        val deviceId: String,
        val name: String,
        val lat: Double,
        val lon: Double,
        val online: Boolean,
        val ageS: Double?,
        val modeState: String? = null,
    )

    data class TargetNode(
        val deviceId: String,
        val name: String,
        val lat: Double?,
        val lon: Double?,
        val modeState: String? = null,
    )

    /** Live snapshot of "what the fleet is hearing right now". */
    data class SensorReading(
        val sensorId: String,
        val rssi: Int?,
        val lastRssi: Int? = null,
        val lastHeardAgeS: Double? = null,
        val samplesInWindow: Int,
        val sampleCountTotal: Int = 0,
        val scannerSlotsSeen: Int? = null,
        val acceptedIntoFit: Boolean = false,
        val checkpointStatus: String? = null,
        val anchorSource: String? = null,
        val gpsDistanceM: Double?,
        // Closed-loop readiness — tells the operator when this sensor
        // has enough data to fit cleanly, so they know whether to keep
        // standing nearby or move on.
        val samplesCount: Int = 0,
        val samplesNeeded: Int = 20,
        val distanceRangeM: Double = 0.0,
        val hasCheckpoint: Boolean = false,
        val ready: Boolean = false,
        val hints: List<String> = emptyList(),
    )

    /** Overall session readiness — drives the "you can stop now" banner. */
    data class SessionReadiness(
        val sensorsReady: Int = 0,
        val sensorsTotal: Int = 0,
        val readyOverall: Boolean = false,
        val minRequired: Int = 4,
    )

    /** Real-time convergence state — phone GPS vs triangulated position,
     *  plus the smart "stand still until locked, then move on" signal. */
    data class MyPosition(
        val phoneLat: Double? = null,
        val phoneLon: Double? = null,
        val phoneAccuracyM: Float? = null,
        val triangulatedLat: Double? = null,
        val triangulatedLon: Double? = null,
        val triangulatedAccuracyM: Double? = null,
        val errorM: Double? = null,
        val sensorCount: Int = 0,
        val standingStill: Boolean = false,
        val stillS: Double = 0.0,
        val okToMove: Boolean = false,
        val status: String = "",
        val eligibleSensorCount: Int = 0,
        val eligibleSensorIds: List<String> = emptyList(),
        val heardSensorCount: Int = 0,
        val heardSensorIds: List<String> = emptyList(),
        val convergenceTargetM: Double = 10.0,
        val dwellTargetS: Double = 5.0,
        val minSensors: Int = 3,
    )

    /** Result of a "I'm here" checkpoint touch — green/yellow/red badge. */
    data class CheckpointResult(
        val sensorId: String,
        val severity: String,            // "ok" | "warn" | "error"
        val gpsDriftM: Double?,
        val rssiAtTouch: Int?,
        val strongestAtTouch: String?,
        val warnings: List<String>,
        val tsMs: Long,
        val anchorSource: String = ANCHOR_PHONE_GPS,
    )

    enum class CheckpointPhase {
        Idle,
        WaitingForNodeHearing,
        SyncingCheckpoint,
        AnchorLocked,
        CollectingRange,
        ReadyForNext,
        NeedsAttention,
    }

    data class ActiveCheckpointLock(
        val sensorId: String,
        val sensorName: String,
        val phase: CheckpointPhase = CheckpointPhase.WaitingForNodeHearing,
        val startedAtMs: Long = System.currentTimeMillis(),
        val updatedAtMs: Long = startedAtMs,
        val statusText: String = "Stand still at this node while the fleet hears your phone.",
        val detailText: String? = null,
        val anchorSource: String? = null,
        val gpsAccuracyM: Float? = null,
        val lastRssi: Int? = null,
        val lastHeardAgeS: Double? = null,
        val samplesCount: Int = 0,
        val samplesNeeded: Int = 20,
        val distanceRangeM: Double = 0.0,
        val backendSynced: Boolean = false,
    )

    private data class AnchorSelection(
        val lat: Double,
        val lon: Double,
        val accuracyM: Float?,
        val anchorSource: String,
        val detailText: String,
    )

    /** Backend reachability indicator shown next to the URL field. */
    enum class BackendStatus { Unknown, Ok, AuthFailed, Unreachable }

    /** A walk GPS sample waiting to be POSTed once the phone has a working
     *  WiFi route to the backend again. Property spans multiple APs so
     *  gaps during roaming are expected. */
    data class QueuedSample(
        val lat: Double, val lon: Double,
        val tsMs: Long, val accuracyM: Float?,
    )

    /** An "I'm here" checkpoint waiting to be POSTed. */
    data class QueuedCheckpoint(
        val sensorId: String,
        val lat: Double, val lon: Double,
        val tsMs: Long, val accuracyM: Float?,
        val anchorSource: String,
    )

    data class State(
        val backendUrl: String = "",
        val token: String = "",
        val operatorLabel: String = "",
        val isWalking: Boolean = false,
        val beaconOnAir: Boolean = false,
        val sessionId: String? = null,
        val advertiseUuid: String? = null,
        val fleetModeState: String = "inactive",
        val walkStartedAtMs: Long? = null,
        val tracePoints: Int = 0,
        val samplesTotal: Int = 0,
        val eligibleSensorCount: Int = 0,
        val eligibleSensorIds: List<String> = emptyList(),
        val heardSensorCount: Int = 0,
        val heardSensorIds: List<String> = emptyList(),
        val targetNodes: List<TargetNode> = emptyList(),
        val sensorsHearingMe: List<SensorReading> = emptyList(),
        val availableSensors: List<SensorInfo> = emptyList(),
        val checkpointResults: Map<String, CheckpointResult> = emptyMap(),
        val activeCheckpoint: ActiveCheckpointLock? = null,
        val sessionReadiness: SessionReadiness = SessionReadiness(),
        /** Live "phone GPS vs fleet's triangulated position" + convergence
         *  telemetry. Updated by a ~1 Hz poll during the walk. */
        val myPosition: MyPosition = MyPosition(),
        val phoneLat: Double? = null,
        val phoneLon: Double? = null,
        val gpsAccuracyM: Float? = null,
        val errorMessage: String? = null,
        val infoMessage: String? = null,
        val provisionalFitResult: JsonObject? = null,
        val verifiedFitResult: JsonObject? = null,
        /** Count of samples + checkpoints buffered locally because the
         *  backend couldn't be reached. Drives the amber "N queued" UI
         *  indicator so the operator knows the walk is still being
         *  recorded even when one AP's coverage has dropped. */
        val queuedCount: Int = 0,
        /** SSID the phone is currently associated with — surfaces in the
         *  UI next to the "Switch WiFi" button so operators can see which
         *  network they're on while roaming across the property. */
        val currentSsid: String? = null,
        val networkTransport: NetworkTransportState = NetworkTransportState.Offline,
        val backendReachability: BackendReachabilityState = BackendReachabilityState.Unknown,
        val calibrationAuthState: CalibrationAuthState = CalibrationAuthState.Unknown,
        val sensorLoadState: SensorLoadState = SensorLoadState.Unknown,
        val backendVersion: String? = null,
        val lastPreflightFailurePhase: PreflightPhase? = null,
        val lastPreflightFailureDetail: String? = null,
        val fitApplied: Boolean? = null,
        val applyReason: String? = null,
        val bluetoothEnabled: Boolean = true,
        val backendStatus: BackendStatus = BackendStatus.Unknown,
    )

    private val _state = MutableStateFlow(State(
        backendUrl = prefs.backendUrl,
        token = prefs.calibrationToken,
        operatorLabel = prefs.operatorLabel,
        bluetoothEnabled = platform.isBluetoothEnabled(),
        currentSsid = platform.currentNetworkSnapshot().ssid,
        networkTransport = platform.currentNetworkSnapshot().transport,
    ))
    val state: StateFlow<State> = _state.asStateFlow()

    // ── Offline queue for wifi roam ─────────────────────────────────
    // Property routinely spans more than one AP's coverage, so operators
    // will walk into a dead zone during calibration. Rather than drop
    // the samples or pause the walk, queue everything locally and flush
    // as soon as backend connectivity returns. FIFO; sizes capped to keep
    // memory bounded if connectivity stays out for a long time.
    private val pendingSamples = ArrayDeque<QueuedSample>()
    private val pendingCheckpoints = ArrayDeque<QueuedCheckpoint>()
    private val queueLock = Any()
    private val MAX_QUEUED_SAMPLES = 600      // 10 min of 1 Hz GPS
    private val MAX_QUEUED_CHECKPOINTS = 50
    private var flushJob: Job? = null

    private fun enqueueSample(s: QueuedSample) {
        synchronized(queueLock) {
            pendingSamples.addLast(s)
            while (pendingSamples.size > MAX_QUEUED_SAMPLES) {
                pendingSamples.removeFirst()    // drop oldest if truly stuck
            }
            updateQueuedCountLocked()
        }
    }

    private fun enqueueCheckpoint(c: QueuedCheckpoint) {
        synchronized(queueLock) {
            pendingCheckpoints.addLast(c)
            while (pendingCheckpoints.size > MAX_QUEUED_CHECKPOINTS) {
                pendingCheckpoints.removeFirst()
            }
            updateQueuedCountLocked()
        }
    }

    /** Must be called while holding queueLock. */
    private fun updateQueuedCountLocked() {
        val total = pendingSamples.size + pendingCheckpoints.size
        _state.value = _state.value.copy(queuedCount = total)
    }

    private fun startFlushLoop() {
        flushJob?.cancel()
        flushJob = viewModelScope.launch {
            while (true) {
                val s = _state.value
                if (!s.isWalking) break
                val sid = s.sessionId
                if (sid != null && s.backendUrl.isNotBlank() && s.token.isNotBlank()) {
                    flushOnce(sid, s.backendUrl, s.token)
                }
                refreshNetworkState()
                delay(4000)
            }
        }
    }

    private suspend fun flushOnce(sessionId: String, baseUrl: String, token: String) {
        // Samples: send in order, stop on first failure (likely still offline).
        while (true) {
            val next = synchronized(queueLock) {
                pendingSamples.firstOrNull()
            } ?: break
            val res = api.walkSample(baseUrl, token, sessionId,
                                     lat = next.lat, lon = next.lon,
                                     tsMs = next.tsMs, accuracyM = next.accuracyM)
            if (res.isFailure) return
            synchronized(queueLock) {
                if (pendingSamples.firstOrNull() === next) pendingSamples.removeFirst()
                updateQueuedCountLocked()
            }
        }
        // Checkpoints: same idea. Each flush re-queries its sanity result
        // so the operator still gets the green/yellow/red feedback.
        while (true) {
            val next = synchronized(queueLock) {
                pendingCheckpoints.firstOrNull()
            } ?: break
            val res = api.walkCheckpoint(baseUrl, token, sessionId,
                                         next.sensorId, next.lat, next.lon,
                                         next.accuracyM, next.tsMs,
                                         next.anchorSource)
            if (res.isFailure) return
            val body = res.getOrNull()
            if (body != null) {
                val cr = parseCheckpointResult(body, next.sensorId, next.tsMs, next.anchorSource)
                _state.value = _state.value.copy(
                    checkpointResults = _state.value.checkpointResults + (next.sensorId to cr),
                )
            }
            synchronized(queueLock) {
                if (pendingCheckpoints.firstOrNull() === next) pendingCheckpoints.removeFirst()
                updateQueuedCountLocked()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun refreshNetworkState() {
        val snapshot = try {
            platform.currentNetworkSnapshot()
        } catch (_: Exception) {
            CalibrationNetworkSnapshot()
        }
        _state.value = _state.value.copy(
            currentSsid = snapshot.ssid,
            networkTransport = snapshot.transport,
        )
    }

    private fun parseSensorInfoArray(body: JsonObject, key: String): List<SensorInfo> {
        val sensors = mutableListOf<SensorInfo>()
        body.getAsJsonArray(key)?.forEach { el ->
            val obj = el.asJsonObject
            val deviceId = obj.get("device_id")?.takeIf { !it.isJsonNull }?.asString ?: return@forEach
            sensors.add(
                SensorInfo(
                    deviceId = deviceId,
                    name = obj.get("name")?.takeIf { !it.isJsonNull }?.asString ?: deviceId,
                    lat = obj.get("lat")?.takeIf { !it.isJsonNull }?.asDouble ?: 0.0,
                    lon = obj.get("lon")?.takeIf { !it.isJsonNull }?.asDouble ?: 0.0,
                    online = obj.get("online")?.takeIf { !it.isJsonNull }?.asBoolean ?: true,
                    ageS = obj.get("age_s")?.takeIf { !it.isJsonNull }?.asDouble,
                    modeState = obj.get("mode_state")?.takeIf { !it.isJsonNull }?.asString,
                )
            )
        }
        return sensors
    }

    private fun parseTargetNodes(body: JsonObject): List<TargetNode> {
        val targets = mutableListOf<TargetNode>()
        body.getAsJsonArray("target_nodes")?.forEach { el ->
            val obj = el.asJsonObject
            val deviceId = obj.get("device_id")?.takeIf { !it.isJsonNull }?.asString ?: return@forEach
            targets.add(
                TargetNode(
                    deviceId = deviceId,
                    name = obj.get("name")?.takeIf { !it.isJsonNull }?.asString ?: deviceId,
                    lat = obj.get("lat")?.takeIf { !it.isJsonNull }?.asDouble,
                    lon = obj.get("lon")?.takeIf { !it.isJsonNull }?.asDouble,
                    modeState = obj.get("mode_state")?.takeIf { !it.isJsonNull }?.asString,
                )
            )
        }
        return targets
    }

    private fun buildProvisionalFitSnapshot(): JsonObject {
        val s = _state.value
        return JsonObject().apply {
            addProperty("ok", s.samplesTotal > 0 && s.tracePoints > 0)
            addProperty("source", "android_walk_provisional")
            addProperty("trace_points", s.tracePoints)
            addProperty("samples_total", s.samplesTotal)
            addProperty("checkpointed_sensor_count", s.checkpointResults.size)
            addProperty("ready_sensor_count", s.sessionReadiness.sensorsReady)
            addProperty("target_sensor_count", s.eligibleSensorCount)
            addProperty("heard_sensor_count", s.heardSensorCount)
            addProperty("ready_overall", s.sessionReadiness.readyOverall)
            addProperty("reason", if (s.samplesTotal > 0) "local_walk_summary" else "no_samples_collected")
        }
    }

    private fun parseCheckpointResult(
        body: JsonObject,
        sensorId: String,
        tsMs: Long,
        requestedAnchorSource: String,
    ): CheckpointResult {
        val warnings = body.getAsJsonArray("warnings")?.map { it.asString } ?: emptyList()
        return CheckpointResult(
            sensorId = sensorId,
            severity = body.get("severity")?.asString ?: "warn",
            gpsDriftM = body.get("gps_drift_m")?.takeIf { !it.isJsonNull }?.asDouble,
            rssiAtTouch = body.get("rssi_at_touch")?.takeIf { !it.isJsonNull }?.asInt,
            strongestAtTouch = body.get("strongest_sensor_at_touch")
                ?.takeIf { !it.isJsonNull }?.asString,
            warnings = warnings,
            tsMs = tsMs,
            anchorSource = body.get("anchor_source")
                ?.takeIf { !it.isJsonNull }
                ?.asString
                ?: requestedAnchorSource,
        )
    }

    private fun selectCheckpointAnchor(sensor: SensorInfo, state: State): AnchorSelection {
        val phoneLat = state.phoneLat
        val phoneLon = state.phoneLon
        val gpsAccuracy = state.gpsAccuracyM
        val hasGoodGps = state.phoneLat != null &&
            state.phoneLon != null &&
            gpsAccuracy != null &&
            gpsAccuracy <= CHECKPOINT_GOOD_GPS_ACCURACY_M

        return if (hasGoodGps) {
            AnchorSelection(
                lat = phoneLat!!,
                lon = phoneLon!!,
                accuracyM = gpsAccuracy,
                anchorSource = ANCHOR_PHONE_GPS,
                detailText = "Phone GPS accepted (±${"%.0f".format(gpsAccuracy)} m).",
            )
        } else {
            val gpsDetail = when {
                phoneLat == null || phoneLon == null -> "Phone GPS unavailable."
                gpsAccuracy == null -> "Phone GPS accuracy unavailable."
                else -> "Phone GPS is ±${"%.0f".format(gpsAccuracy)} m, above the ${CHECKPOINT_GOOD_GPS_ACCURACY_M.toInt()} m lock gate."
            }
            AnchorSelection(
                lat = sensor.lat,
                lon = sensor.lon,
                accuracyM = null,
                anchorSource = ANCHOR_SENSOR_FALLBACK,
                detailText = "$gpsDetail Using saved coordinates for ${sensor.name}.",
            )
        }
    }

    private fun freshHearing(reading: SensorReading?): Boolean {
        if (reading == null) return false
        val rssi = reading.lastRssi ?: reading.rssi
        val age = reading.lastHeardAgeS ?: if (reading.samplesInWindow > 0) 0.0 else 999.0
        return rssi != null && age <= CHECKPOINT_HEARD_FRESH_S
    }

    private fun activeCheckpointReading(state: State): SensorReading? {
        val lock = state.activeCheckpoint ?: return null
        return state.sensorsHearingMe.firstOrNull { it.sensorId == lock.sensorId }
    }

    private fun activeCheckpointSensor(state: State): SensorInfo? {
        val lock = state.activeCheckpoint ?: return null
        return state.availableSensors.firstOrNull { it.deviceId == lock.sensorId }
    }

    private fun lockWithReading(
        lock: ActiveCheckpointLock,
        reading: SensorReading?,
    ): ActiveCheckpointLock {
        return lock.copy(
            lastRssi = reading?.lastRssi ?: reading?.rssi ?: lock.lastRssi,
            lastHeardAgeS = reading?.lastHeardAgeS ?: lock.lastHeardAgeS,
            samplesCount = reading?.samplesCount ?: lock.samplesCount,
            samplesNeeded = reading?.samplesNeeded ?: lock.samplesNeeded,
            distanceRangeM = reading?.distanceRangeM ?: lock.distanceRangeM,
        )
    }

    private fun updateActiveCheckpointFromFeedback() {
        val state = _state.value
        val lock = state.activeCheckpoint ?: return
        val reading = activeCheckpointReading(state)
        val result = state.checkpointResults[lock.sensorId]
        val withReading = lockWithReading(lock, reading)
        val nowMs = System.currentTimeMillis()

        val next = when {
            reading?.ready == true && lock.phase != CheckpointPhase.NeedsAttention -> withReading.copy(
                phase = CheckpointPhase.ReadyForNext,
                updatedAtMs = nowMs,
                statusText = "This node is ready. Move to the next sensor.",
                detailText = "Checkpoint synced, ${reading.samplesCount}/${reading.samplesNeeded} samples collected, ${"%.0f".format(reading.distanceRangeM)} m range.",
                backendSynced = true,
            )
            result?.rssiAtTouch != null &&
                withReading.phase in setOf(
                    CheckpointPhase.AnchorLocked,
                    CheckpointPhase.CollectingRange,
                    CheckpointPhase.WaitingForNodeHearing,
                    CheckpointPhase.SyncingCheckpoint,
                ) -> withReading.copy(
                    phase = CheckpointPhase.CollectingRange,
                    updatedAtMs = nowMs,
                    statusText = "Anchor locked. Move away slowly from this node.",
                    detailText = "Collecting range: ${reading?.samplesCount ?: withReading.samplesCount}/${reading?.samplesNeeded ?: withReading.samplesNeeded} samples, ${"%.0f".format(reading?.distanceRangeM ?: withReading.distanceRangeM)} m range.",
                    anchorSource = result.anchorSource,
                    gpsAccuracyM = state.gpsAccuracyM,
                    backendSynced = true,
                )
            else -> withReading
        }

        if (next != lock) {
            _state.value = state.copy(activeCheckpoint = next)
        }
    }

    private suspend fun submitActiveCheckpointIfReady() {
        val state = _state.value
        val lock = state.activeCheckpoint ?: return
        if (lock.phase !in setOf(CheckpointPhase.WaitingForNodeHearing, CheckpointPhase.SyncingCheckpoint)) {
            return
        }
        val nowMs = System.currentTimeMillis()
        if (lock.phase == CheckpointPhase.SyncingCheckpoint &&
            nowMs - lock.updatedAtMs < CHECKPOINT_RETRY_AFTER_MS) {
            return
        }
        val sensor = activeCheckpointSensor(state) ?: return
        val sessionId = state.sessionId ?: return
        val reading = activeCheckpointReading(state)

        if (!state.isWalking || !state.beaconOnAir || state.fleetModeState != "active") {
            _state.value = state.copy(
                activeCheckpoint = lockWithReading(lock, reading).copy(
                    phase = CheckpointPhase.WaitingForNodeHearing,
                    updatedAtMs = nowMs,
                    statusText = "Waiting for active fleet calibration mode.",
                    detailText = "Fleet=${state.fleetModeState}, beacon=${if (state.beaconOnAir) "on" else "off"}.",
                )
            )
            return
        }

        if (!freshHearing(reading)) {
            val age = reading?.lastHeardAgeS
            val detail = if (reading?.lastRssi != null || reading?.rssi != null) {
                "Last RSSI ${reading.lastRssi ?: reading.rssi} dBm" +
                    (age?.let { ", ${"%.1f".format(it)} s ago" } ?: "")
            } else {
                "No packet from ${sensor.name} yet."
            }
            _state.value = state.copy(
                activeCheckpoint = lockWithReading(lock, reading).copy(
                    phase = CheckpointPhase.WaitingForNodeHearing,
                    updatedAtMs = nowMs,
                    statusText = "Stand still at ${sensor.name}; waiting for this exact node to hear you.",
                    detailText = "$detail Need a packet within ${CHECKPOINT_HEARD_FRESH_S.toInt()} s before syncing.",
                )
            )
            return
        }

        val anchor = selectCheckpointAnchor(sensor, state)
        _state.value = state.copy(
            activeCheckpoint = lockWithReading(lock, reading).copy(
                phase = CheckpointPhase.SyncingCheckpoint,
                updatedAtMs = nowMs,
                statusText = "Packets heard. Syncing checkpoint with backend…",
                detailText = anchor.detailText,
                anchorSource = anchor.anchorSource,
                gpsAccuracyM = state.gpsAccuracyM,
            )
        )

        val res = api.walkCheckpoint(
            baseUrl = state.backendUrl,
            token = state.token,
            sessionId = sessionId,
            sensorId = sensor.deviceId,
            lat = anchor.lat,
            lon = anchor.lon,
            accuracyM = anchor.accuracyM,
            tsMs = nowMs,
            anchorSource = anchor.anchorSource,
        )

        if (res.isFailure) {
            val current = _state.value.activeCheckpoint ?: return
            _state.value = _state.value.copy(
                activeCheckpoint = lockWithReading(current, activeCheckpointReading(_state.value)).copy(
                    phase = CheckpointPhase.SyncingCheckpoint,
                    updatedAtMs = System.currentTimeMillis(),
                    statusText = "Waiting to sync with backend. Stay at this node.",
                    detailText = res.exceptionOrNull()?.message ?: "Backend checkpoint call failed.",
                    anchorSource = anchor.anchorSource,
                    gpsAccuracyM = state.gpsAccuracyM,
                )
            )
            return
        }

        val body = res.getOrNull() ?: return
        val cr = parseCheckpointResult(body, sensor.deviceId, nowMs, anchor.anchorSource)
        val nextPhase = if (cr.rssiAtTouch == null || cr.severity == "error") {
            CheckpointPhase.NeedsAttention
        } else {
            CheckpointPhase.AnchorLocked
        }
        val status = if (nextPhase == CheckpointPhase.NeedsAttention) {
            "Checkpoint failed. Retry here before moving."
        } else {
            "Anchor locked. Move away slowly from this node."
        }
        _state.value = _state.value.copy(
            checkpointResults = _state.value.checkpointResults + (sensor.deviceId to cr),
            activeCheckpoint = lockWithReading(
                _state.value.activeCheckpoint ?: lock,
                activeCheckpointReading(_state.value),
            ).copy(
                phase = nextPhase,
                updatedAtMs = System.currentTimeMillis(),
                statusText = status,
                detailText = if (nextPhase == CheckpointPhase.NeedsAttention) {
                    cr.warnings.joinToString("; ").ifBlank { "Backend did not accept the anchor." }
                } else {
                    "RSSI@touch ${cr.rssiAtTouch} dBm. Now widen range until this sensor is ready."
                },
                anchorSource = cr.anchorSource,
                gpsAccuracyM = state.gpsAccuracyM,
                backendSynced = true,
            ),
        )
        updateActiveCheckpointFromFeedback()
    }

    private suspend fun abortRemoteSession(
        baseUrl: String,
        token: String,
        sessionId: String,
        reason: String,
    ) {
        api.walkAbort(baseUrl, token, sessionId, reason)
    }

    private var feedbackJob: Job? = null
    private var myPositionJob: Job? = null
    private var lastSentTraceMs: Long = 0
    private val gpsListener = object : LocationListener {
        override fun onLocationChanged(loc: Location) { onLocation(loc) }
        @Deprecated("API 29 removed status, but interface still requires override on older")
        override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) {}
        override fun onProviderEnabled(provider: String) {}
        override fun onProviderDisabled(provider: String) {
            _state.value = _state.value.copy(errorMessage = "GPS disabled — turn it on")
        }
    }

    fun setBackendUrl(value: String) {
        prefs.backendUrl = value
        _state.value = _state.value.copy(
            backendUrl = value,
            backendStatus = BackendStatus.Unknown,
            backendReachability = BackendReachabilityState.Unknown,
            calibrationAuthState = CalibrationAuthState.Unknown,
            sensorLoadState = SensorLoadState.Unknown,
            backendVersion = null,
        )
    }

    fun setToken(value: String) {
        prefs.calibrationToken = value
        _state.value = _state.value.copy(
            token = value,
            backendStatus = BackendStatus.Unknown,
            calibrationAuthState = CalibrationAuthState.Unknown,
            sensorLoadState = SensorLoadState.Unknown,
        )
    }

    fun setOperatorLabel(value: String) {
        prefs.operatorLabel = value
        _state.value = _state.value.copy(operatorLabel = value)
    }

    fun clearMessages() {
        _state.value = _state.value.copy(errorMessage = null, infoMessage = null)
    }

    private fun clearPreflightFailure() {
        _state.value = _state.value.copy(
            lastPreflightFailurePhase = null,
            lastPreflightFailureDetail = null,
        )
    }

    private fun setPreflightFailure(
        phase: PreflightPhase,
        detail: String,
        userMessage: String,
        backendStatus: BackendStatus,
        backendReachability: BackendReachabilityState,
        calibrationAuthState: CalibrationAuthState,
        sensorLoadState: SensorLoadState,
        backendVersion: String? = _state.value.backendVersion,
        availableSensors: List<SensorInfo> = _state.value.availableSensors,
    ) {
        _state.value = _state.value.copy(
            backendStatus = backendStatus,
            backendReachability = backendReachability,
            calibrationAuthState = calibrationAuthState,
            sensorLoadState = sensorLoadState,
            backendVersion = backendVersion,
            availableSensors = availableSensors,
            lastPreflightFailurePhase = phase,
            lastPreflightFailureDetail = detail,
            errorMessage = userMessage,
        )
    }

    /** Re-check Bluetooth state, e.g. after the user toggles it in
     *  Settings and returns to the screen. */
    fun refreshBluetoothState() {
        _state.value = _state.value.copy(
            bluetoothEnabled = platform.isBluetoothEnabled(),
        )
    }

    /** Reset the saved token back to the backend's default (`chompchomp`).
     *  Manual escape hatch if auto-recovery doesn't fire for some reason
     *  (e.g., backend briefly unreachable when auto-recovery tried). */
    fun resetTokenToDefault() {
        val default = CalibrationApi.DEFAULT_TOKEN
        prefs.calibrationToken = default
        _state.value = _state.value.copy(
            token = default,
            backendStatus = BackendStatus.Unknown,
            backendReachability = BackendReachabilityState.Unknown,
            calibrationAuthState = CalibrationAuthState.Unknown,
            sensorLoadState = SensorLoadState.Unknown,
            lastPreflightFailurePhase = null,
            lastPreflightFailureDetail = null,
            infoMessage = "Token reset to default. Testing…",
        )
        refreshConnectivity()
    }

    /** Preflight + sensor list load. Called on screen entry and after
     *  the operator edits backend URL/token. Distinguishes auth failure
     *  (red dot, "check token") from unreachable backend (gray dot).
     *
     *  Auto-recovery: if the saved token 401s but isn't the default, try
     *  the default silently and persist it on success. Covers the "old
     *  APK had a random dev token in prefs that's now stale" upgrade
     *  path where SharedPreferences survive across installs. */
    fun refreshConnectivity() {
        refreshNetworkState()
        val s = _state.value
        if (s.backendUrl.isBlank() || s.token.isBlank()) {
            _state.value = s.copy(
                backendStatus = BackendStatus.Unknown,
                backendReachability = BackendReachabilityState.Unknown,
                calibrationAuthState = CalibrationAuthState.Unknown,
                sensorLoadState = SensorLoadState.Unknown,
                backendVersion = null,
                lastPreflightFailurePhase = null,
                lastPreflightFailureDetail = null,
            )
            return
        }
        viewModelScope.launch {
            clearPreflightFailure()
            refreshNetworkState()
            val live = _state.value
            val healthRes = api.health(live.backendUrl)
            if (healthRes.isFailure) {
                val msg = healthRes.exceptionOrNull()?.message.orEmpty()
                setPreflightFailure(
                    phase = PreflightPhase.Health,
                    detail = msg.ifBlank { "unknown health failure" },
                    userMessage = "Backend unreachable: $msg",
                    backendStatus = BackendStatus.Unreachable,
                    backendReachability = BackendReachabilityState.Unreachable,
                    calibrationAuthState = CalibrationAuthState.Unknown,
                    sensorLoadState = SensorLoadState.Unknown,
                    backendVersion = null,
                    availableSensors = emptyList(),
                )
                return@launch
            }
            val healthBody = healthRes.getOrNull() ?: JsonObject()
            val healthVersion = healthBody.get("version")
                ?.takeIf { !it.isJsonNull }
                ?.asString
            _state.value = _state.value.copy(
                backendStatus = BackendStatus.Ok,
                backendReachability = BackendReachabilityState.Reachable,
                backendVersion = healthVersion,
                errorMessage = null,
                lastPreflightFailurePhase = null,
                lastPreflightFailureDetail = null,
            )

            var sensorsRes = api.walkSensors(live.backendUrl, live.token)
            if (sensorsRes.isFailure) {
                val firstMsg = sensorsRes.exceptionOrNull()?.message.orEmpty()
                val is401 = "401" in firstMsg
                if (is401 && live.token != CalibrationApi.DEFAULT_TOKEN) {
                    val recovered = api.walkSensors(live.backendUrl, CalibrationApi.DEFAULT_TOKEN)
                    if (recovered.isSuccess) {
                        prefs.calibrationToken = CalibrationApi.DEFAULT_TOKEN
                        _state.value = _state.value.copy(
                            token = CalibrationApi.DEFAULT_TOKEN,
                            infoMessage = "Stored token was stale — reset to default.",
                        )
                        sensorsRes = recovered
                    }
                }
            }
            if (sensorsRes.isFailure) {
                val msg = sensorsRes.exceptionOrNull()?.message.orEmpty()
                val is401 = "401" in msg
                setPreflightFailure(
                    phase = PreflightPhase.Sensors,
                    detail = msg.ifBlank { "unknown sensor preflight failure" },
                    userMessage = if (is401) {
                        "Calibration token rejected — update X-Cal-Token."
                    } else {
                        "Sensor list unavailable: $msg"
                    },
                    backendStatus = if (is401) BackendStatus.AuthFailed else BackendStatus.Ok,
                    backendReachability = BackendReachabilityState.Reachable,
                    calibrationAuthState = if (is401) CalibrationAuthState.Invalid else CalibrationAuthState.Valid,
                    sensorLoadState = if (is401) SensorLoadState.Unknown else SensorLoadState.Unavailable,
                    backendVersion = healthVersion,
                    availableSensors = if (is401) emptyList() else _state.value.availableSensors,
                )
                return@launch
            }
            val body = sensorsRes.getOrNull() ?: return@launch
            val sensors = parseSensorInfoArray(body, "sensors")
            _state.value = _state.value.copy(
                availableSensors = sensors,
                backendStatus = BackendStatus.Ok,
                backendReachability = BackendReachabilityState.Reachable,
                calibrationAuthState = CalibrationAuthState.Valid,
                sensorLoadState = if (sensors.isEmpty()) SensorLoadState.Empty else SensorLoadState.Ready,
                backendVersion = healthVersion,
                lastPreflightFailurePhase = null,
                lastPreflightFailureDetail = null,
                errorMessage = null,
            )
        }
    }

    /** Returns the list of permissions still needing runtime grant.
     *  Empty list → the screen can call startWalk(). */
    fun missingPermissions(granted: Set<String>): List<String> {
        val needed = mutableListOf(
            android.Manifest.permission.ACCESS_FINE_LOCATION,
        )
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            needed += listOf(
                android.Manifest.permission.BLUETOOTH_ADVERTISE,
                android.Manifest.permission.BLUETOOTH_SCAN,
                android.Manifest.permission.BLUETOOTH_CONNECT,
            )
        }
        return needed.filter { it !in granted }
    }

    @SuppressLint("MissingPermission")
    fun startWalk() {
        refreshNetworkState()
        refreshBluetoothState()
        val s = _state.value
        if (s.isWalking) return
        if (s.backendUrl.isBlank() || s.token.isBlank()) {
            _state.value = s.copy(errorMessage = "Backend URL and token are required.")
            return
        }
        if (!s.bluetoothEnabled) {
            _state.value = s.copy(
                bluetoothEnabled = false,
                errorMessage = "Bluetooth is off — enable it before starting a walk.",
            )
            return
        }
        val preflightFailure = s.preflightFailureReason()
        if (preflightFailure != null) {
            _state.value = s.copy(errorMessage = preflightFailure)
            return
        }
        viewModelScope.launch {
            val res = api.walkStart(s.backendUrl, s.token,
                                    operatorLabel = s.operatorLabel.ifBlank { "phone" })
            if (res.isFailure) {
                val msg = res.exceptionOrNull()?.message.orEmpty()
                setPreflightFailure(
                    phase = PreflightPhase.StartWalk,
                    detail = msg.ifBlank { "unknown walk-start failure" },
                    userMessage = when {
                        "401" in msg -> "Calibration token rejected — update X-Cal-Token."
                        "gps" in msg.lowercase() -> "GPS unavailable — check location services."
                        else -> "Walk-start failed: $msg"
                    },
                    backendStatus = if ("401" in msg) BackendStatus.AuthFailed else _state.value.backendStatus,
                    backendReachability = if ("401" in msg) BackendReachabilityState.Reachable
                        else _state.value.backendReachability,
                    calibrationAuthState = if ("401" in msg) CalibrationAuthState.Invalid
                        else _state.value.calibrationAuthState,
                    sensorLoadState = _state.value.sensorLoadState,
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val sid = body.get("session_id")?.asString ?: return@launch
            val uuid = body.get("advertise_uuid")?.asString ?: return@launch
            val modeState = body.get("mode_state")?.takeIf { !it.isJsonNull }?.asString ?: "inactive"
            val targetNodes = parseTargetNodes(body)
            val targetSensorIds = targetNodes.map { it.deviceId }
            val targetSensorCount = body.get("target_sensor_count")?.takeIf { !it.isJsonNull }?.asInt
                ?: targetSensorIds.size
            if (modeState != "active") {
                abortRemoteSession(
                    baseUrl = s.backendUrl,
                    token = s.token,
                    sessionId = sid,
                    reason = "fleet_mode_not_active_after_start",
                )
                setPreflightFailure(
                    phase = PreflightPhase.StartWalk,
                    detail = "backend returned mode_state=$modeState",
                    userMessage = "Fleet did not enter calibration mode — retry the walk start.",
                    backendStatus = BackendStatus.Ok,
                    backendReachability = BackendReachabilityState.Reachable,
                    calibrationAuthState = CalibrationAuthState.Valid,
                    sensorLoadState = _state.value.sensorLoadState,
                )
                return@launch
            }

            // Start BLE peripheral
            val advertiseRes = advertiser.start(uuid)
            if (advertiseRes.isFailure) {
                abortRemoteSession(
                    baseUrl = s.backendUrl,
                    token = s.token,
                    sessionId = sid,
                    reason = "advertiser_start_failed",
                )
                val msg = advertiseRes.exceptionOrNull()?.message
                    ?: "BLE advertise failed."
                _state.value = _state.value.copy(
                    isWalking = false,
                    beaconOnAir = false,
                    sessionId = null,
                    advertiseUuid = null,
                    fleetModeState = "inactive",
                    errorMessage = msg,
                )
                return@launch
            }

            // Reset local queue state for the new session
            synchronized(queueLock) {
                pendingSamples.clear()
                pendingCheckpoints.clear()
            }
            var gpsWarning: String? = null
            try {
                // GPS is useful trace data, but it must not block the RF
                // calibration session after the fleet and BLE beacon are live.
                platform.requestLocationUpdates(gpsListener)
            } catch (e: SecurityException) {
                gpsWarning = "Walk is active, but Android denied location. Use 'I'm here' at each sensor; anchors will use saved sensor coordinates until GPS is fixed."
            } catch (e: Exception) {
                gpsWarning = "Walk is active, but GPS did not start (${e.message ?: "unknown error"}). Use 'I'm here' at each sensor; anchors will use saved sensor coordinates until GPS is fixed."
            }
            _state.value = _state.value.copy(
                isWalking = true,
                beaconOnAir = true,
                sessionId = sid,
                advertiseUuid = uuid,
                fleetModeState = modeState,
                walkStartedAtMs = System.currentTimeMillis(),
                tracePoints = 0,
                samplesTotal = 0,
                eligibleSensorCount = targetSensorCount,
                eligibleSensorIds = targetSensorIds,
                heardSensorCount = 0,
                heardSensorIds = emptyList(),
                targetNodes = targetNodes,
                sensorsHearingMe = emptyList(),
                checkpointResults = emptyMap(),
                activeCheckpoint = null,
                myPosition = MyPosition(),
                provisionalFitResult = null,
                verifiedFitResult = null,
                fitApplied = null,
                applyReason = null,
                backendStatus = BackendStatus.Ok,
                backendReachability = BackendReachabilityState.Reachable,
                calibrationAuthState = CalibrationAuthState.Valid,
                queuedCount = 0,
                errorMessage = null,
                infoMessage = gpsWarning
                    ?: "Fleet calibration mode is active and the beacon is on air — walk near each live node and tap 'I'm here' to anchor the fit.",
            )
            refreshNetworkState()
            startFeedbackPolling(sid)
            startFlushLoop()
            startMyPositionPolling(sid)
        }
    }

    private fun startMyPositionPolling(sessionId: String) {
        myPositionJob?.cancel()
        myPositionJob = viewModelScope.launch {
            while (true) {
                val s = _state.value
                if (!s.isWalking) break
                val res = api.walkMyPosition(s.backendUrl, s.token, sessionId)
                if (res.isSuccess) {
                    val body = res.getOrNull()
                    if (body != null && !body.has("error")) {
                        val eligibleSensorIds = body.getAsJsonArray("eligible_sensor_ids")
                            ?.mapNotNull { element -> element.takeIf { !it.isJsonNull }?.asString }
                            ?: _state.value.eligibleSensorIds
                        val heardSensorIds = body.getAsJsonArray("heard_sensor_ids")
                            ?.mapNotNull { element -> element.takeIf { !it.isJsonNull }?.asString }
                            ?: _state.value.heardSensorIds
                        _state.value = _state.value.copy(
                            eligibleSensorCount = body.get("eligible_sensor_count")?.asInt
                                ?: eligibleSensorIds.size,
                            eligibleSensorIds = eligibleSensorIds,
                            heardSensorCount = body.get("heard_sensor_count")?.asInt
                                ?: heardSensorIds.size,
                            heardSensorIds = heardSensorIds,
                            fleetModeState = body.get("fleet_mode_state")
                                ?.takeIf { !it.isJsonNull }
                                ?.asString
                                ?: _state.value.fleetModeState,
                            myPosition = MyPosition(
                                phoneLat = body.get("phone_lat")?.takeIf { !it.isJsonNull }?.asDouble,
                                phoneLon = body.get("phone_lon")?.takeIf { !it.isJsonNull }?.asDouble,
                                phoneAccuracyM = body.get("phone_accuracy_m")
                                    ?.takeIf { !it.isJsonNull }?.asFloat,
                                triangulatedLat = body.get("triangulated_lat")
                                    ?.takeIf { !it.isJsonNull }?.asDouble,
                                triangulatedLon = body.get("triangulated_lon")
                                    ?.takeIf { !it.isJsonNull }?.asDouble,
                                triangulatedAccuracyM = body.get("triangulated_accuracy_m")
                                    ?.takeIf { !it.isJsonNull }?.asDouble,
                                errorM = body.get("error_m")?.takeIf { !it.isJsonNull }?.asDouble,
                                sensorCount = body.get("sensor_count")?.asInt ?: 0,
                                standingStill = body.get("standing_still")?.asBoolean ?: false,
                                stillS = body.get("still_s")?.asDouble ?: 0.0,
                                okToMove = body.get("ok_to_move")?.asBoolean ?: false,
                                status = body.get("status")?.asString ?: "",
                                eligibleSensorCount = body.get("eligible_sensor_count")?.asInt
                                    ?: eligibleSensorIds.size,
                                eligibleSensorIds = eligibleSensorIds,
                                heardSensorCount = body.get("heard_sensor_count")?.asInt
                                    ?: heardSensorIds.size,
                                heardSensorIds = heardSensorIds,
                                convergenceTargetM = body.get("convergence_target_m")?.asDouble ?: 10.0,
                                dwellTargetS = body.get("dwell_target_s")?.asDouble ?: 5.0,
                                minSensors = body.get("min_sensors")?.asInt ?: 3,
                            ),
                        )
                    }
                }
                delay(1000)
            }
        }
    }

    /** Operator walked up to a sensor and pressed its "I'm here" button. */
    fun markAtSensor(sensor: SensorInfo) {
        val s = _state.value
        s.sessionId ?: run {
            _state.value = s.copy(errorMessage = "Start the walk first.")
            return
        }
        val active = s.activeCheckpoint
        if (active != null &&
            active.sensorId != sensor.deviceId &&
            active.phase !in setOf(CheckpointPhase.ReadyForNext, CheckpointPhase.Idle)) {
            _state.value = s.copy(
                errorMessage = "Finish ${active.sensorName} before moving to another sensor.",
            )
            return
        }
        val startedAt = System.currentTimeMillis()
        val reading = s.sensorsHearingMe.firstOrNull { it.sensorId == sensor.deviceId }
        val anchor = selectCheckpointAnchor(sensor, s)
        _state.value = s.copy(
            errorMessage = null,
            infoMessage = null,
            activeCheckpoint = lockWithReading(
                ActiveCheckpointLock(
                    sensorId = sensor.deviceId,
                    sensorName = sensor.name,
                    phase = CheckpointPhase.WaitingForNodeHearing,
                    startedAtMs = startedAt,
                    updatedAtMs = startedAt,
                    statusText = "Stand still at ${sensor.name}; waiting for this exact node to hear you.",
                    detailText = anchor.detailText,
                    anchorSource = anchor.anchorSource,
                    gpsAccuracyM = s.gpsAccuracyM,
                ),
                reading,
            ),
        )
        viewModelScope.launch {
            submitActiveCheckpointIfReady()
        }
    }

    fun retryActiveCheckpoint() {
        val s = _state.value
        val lock = s.activeCheckpoint ?: return
        _state.value = s.copy(
            errorMessage = null,
            activeCheckpoint = lock.copy(
                phase = CheckpointPhase.WaitingForNodeHearing,
                updatedAtMs = System.currentTimeMillis(),
                statusText = "Retrying ${lock.sensorName}. Stand still at this node.",
                detailText = null,
            )
        )
        viewModelScope.launch {
            submitActiveCheckpointIfReady()
        }
    }

    @SuppressLint("MissingPermission")
    fun endWalk() {
        val s = _state.value
        val sid = s.sessionId ?: return
        feedbackJob?.cancel()
        feedbackJob = null
        flushJob?.cancel()
        flushJob = null
        myPositionJob?.cancel()
        myPositionJob = null
        try { platform.removeLocationUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        viewModelScope.launch {
            // Last-ditch drain of anything still queued before we end the
            // session — avoids "you walked past sensor X and got a great
            // checkpoint, but then lost WiFi, so the fit never anchored".
            flushOnce(sid, s.backendUrl, s.token)
            val provisionalFit = buildProvisionalFitSnapshot()
            val res = api.walkEnd(
                s.backendUrl,
                s.token,
                sid,
                provisionalFit = provisionalFit,
                applyRequested = true,
            )
            if (res.isFailure) {
                _state.value = _state.value.copy(
                    isWalking = false,
                    beaconOnAir = false,
                    fleetModeState = "inactive",
                    errorMessage = "Walk-end failed: ${res.exceptionOrNull()?.message}"
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val verifiedFit = body.getAsJsonObject("verified_fit")
            val returnedProvisionalFit = body.getAsJsonObject("provisional_fit") ?: provisionalFit
            val applied = body.get("applied")?.asBoolean ?: false
            val applyReason = body.get("apply_reason")?.takeIf { !it.isJsonNull }?.asString
            _state.value = _state.value.copy(
                isWalking = false,
                beaconOnAir = false,
                fleetModeState = "inactive",
                walkStartedAtMs = null,
                heardSensorCount = 0,
                heardSensorIds = emptyList(),
                activeCheckpoint = null,
                provisionalFitResult = returnedProvisionalFit,
                verifiedFitResult = verifiedFit,
                fitApplied = applied,
                applyReason = applyReason,
                infoMessage = if (applied) {
                    "Calibration applied to backend."
                } else {
                    "Walk ended — backend kept the current live model (${applyReason ?: "not applied"})."
                },
            )
        }
    }

    fun abortWalk(
        reason: String = "client_abort",
        userMessage: String = "Walk aborted.",
    ) {
        val s = _state.value
        val sid = s.sessionId ?: return
        feedbackJob?.cancel()
        feedbackJob = null
        flushJob?.cancel()
        flushJob = null
        myPositionJob?.cancel()
        myPositionJob = null
        try { platform.removeLocationUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        viewModelScope.launch {
            abortRemoteSession(s.backendUrl, s.token, sid, reason)
            _state.value = _state.value.copy(
                isWalking = false,
                beaconOnAir = false,
                fleetModeState = "inactive",
                walkStartedAtMs = null,
                heardSensorCount = 0,
                heardSensorIds = emptyList(),
                activeCheckpoint = null,
                infoMessage = userMessage,
            )
        }
    }

    private fun startFeedbackPolling(sessionId: String) {
        feedbackJob?.cancel()
        feedbackJob = viewModelScope.launch {
            while (true) {
                val s = _state.value
                if (!s.isWalking) break
                val res = api.walkFeedback(s.backendUrl, s.token, sessionId)
                if (res.isSuccess) {
                    val body = res.getOrNull()
                    if (body != null) {
                        val sensors = mutableListOf<SensorReading>()
                        body.getAsJsonArray("sensors")?.forEach { el ->
                            val obj = el.asJsonObject
                            val r = obj.getAsJsonObject("readiness")
                            sensors.add(SensorReading(
                                sensorId = obj.get("sensor_id")?.asString ?: "?",
                                rssi = obj.get("current_rssi")
                                    ?.takeIf { !it.isJsonNull }?.asInt,
                                lastRssi = obj.get("last_rssi")
                                    ?.takeIf { !it.isJsonNull }?.asInt,
                                lastHeardAgeS = obj.get("last_heard_age_s")
                                    ?.takeIf { !it.isJsonNull }?.asDouble,
                                samplesInWindow = obj.get("samples_in_window")?.asInt ?: 0,
                                sampleCountTotal = obj.get("sample_count_total")?.asInt ?: 0,
                                scannerSlotsSeen = obj.get("scanner_slots_seen")
                                    ?.takeIf { !it.isJsonNull }?.asInt,
                                acceptedIntoFit = obj.get("accepted_into_fit")?.asBoolean ?: false,
                                checkpointStatus = obj.get("checkpoint_status")
                                    ?.takeIf { !it.isJsonNull }?.asString,
                                anchorSource = obj.get("anchor_source")
                                    ?.takeIf { !it.isJsonNull }?.asString,
                                gpsDistanceM = obj.get("distance_m_estimated_from_phone_gps")
                                    ?.takeIf { !it.isJsonNull }?.asDouble,
                                samplesCount = r?.get("samples_count")?.asInt ?: 0,
                                samplesNeeded = r?.get("samples_needed")?.asInt ?: 20,
                                distanceRangeM = r?.get("distance_range_m")
                                    ?.takeIf { !it.isJsonNull }?.asDouble ?: 0.0,
                                hasCheckpoint = r?.get("has_checkpoint")?.asBoolean ?: false,
                                ready = r?.get("ready")?.asBoolean ?: false,
                                hints = r?.getAsJsonArray("hints")
                                    ?.mapNotNull { it.asString } ?: emptyList(),
                            ))
                        }
                        val sr = body.getAsJsonObject("session_readiness")
                        val eligibleSensorIds = body.getAsJsonArray("eligible_sensor_ids")
                            ?.mapNotNull { element -> element.takeIf { !it.isJsonNull }?.asString }
                            ?: s.availableSensors.map { it.deviceId }
                        val heardSensorIds = body.getAsJsonArray("heard_sensor_ids")
                            ?.mapNotNull { element -> element.takeIf { !it.isJsonNull }?.asString }
                            ?: sensors.filter { it.samplesInWindow > 0 }.map { it.sensorId }
                        val readiness = if (sr != null) SessionReadiness(
                            sensorsReady = sr.get("sensors_ready")?.asInt ?: 0,
                            sensorsTotal = sr.get("sensors_total")?.asInt ?: 0,
                            readyOverall = sr.get("ready_overall")?.asBoolean ?: false,
                            minRequired = sr.get("min_required")?.asInt ?: 4,
                        ) else SessionReadiness()
                        _state.value = _state.value.copy(
                            tracePoints = body.get("trace_points")?.asInt ?: s.tracePoints,
                            samplesTotal = body.get("samples_total")?.asInt ?: s.samplesTotal,
                            eligibleSensorCount = body.get("eligible_sensor_count")?.asInt
                                ?: eligibleSensorIds.size,
                            eligibleSensorIds = eligibleSensorIds,
                            heardSensorCount = body.get("heard_sensor_count")?.asInt
                                ?: heardSensorIds.size,
                            heardSensorIds = heardSensorIds,
                            fleetModeState = body.get("fleet_mode_state")
                                ?.takeIf { !it.isJsonNull }
                                ?.asString
                                ?: s.fleetModeState,
                            sensorsHearingMe = sensors,
                            sessionReadiness = readiness,
                        )
                        updateActiveCheckpointFromFeedback()
                        submitActiveCheckpointIfReady()
                    }
                }
                delay(2000)
            }
        }
    }

    private fun onLocation(loc: Location) {
        val s = _state.value
        if (!s.isWalking) return
        val sid = s.sessionId ?: return
        val nowMs = System.currentTimeMillis()
        // Throttle posts to ~1 Hz so the backend trace stays bounded.
        if (nowMs - lastSentTraceMs < 800) {
            _state.value = s.copy(
                phoneLat = loc.latitude, phoneLon = loc.longitude,
                gpsAccuracyM = loc.accuracy,
            )
            return
        }
        lastSentTraceMs = nowMs
        val accuracyM = if (loc.hasAccuracy()) loc.accuracy else null
        viewModelScope.launch {
            val res = api.walkSample(s.backendUrl, s.token, sid,
                                     lat = loc.latitude, lon = loc.longitude,
                                     tsMs = nowMs,
                                     accuracyM = accuracyM)
            if (res.isFailure) {
                // Backend unreachable — queue locally, the flush loop
                // will drain when the phone reassociates with a nearer AP.
                enqueueSample(QueuedSample(
                    lat = loc.latitude, lon = loc.longitude,
                    tsMs = nowMs, accuracyM = accuracyM,
                ))
            }
        }
        _state.value = s.copy(
            phoneLat = loc.latitude, phoneLon = loc.longitude,
            gpsAccuracyM = loc.accuracy,
        )
    }

    override fun onCleared() {
        feedbackJob?.cancel()
        flushJob?.cancel()
        myPositionJob?.cancel()
        try { platform.removeLocationUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        super.onCleared()
    }

    internal fun clearForTest() {
        viewModelScope.cancel()
        onCleared()
    }
}

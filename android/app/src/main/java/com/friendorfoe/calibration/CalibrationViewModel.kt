package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.wifi.WifiManager
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.DetectionPrefs
import com.google.gson.JsonObject
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
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
    private val prefs: DetectionPrefs,
    private val advertiser: BleCalibrationAdvertiser,
    private val api: CalibrationApi,
    private val locationManager: LocationManager,
    private val bluetoothManager: BluetoothManager,
    private val wifiManager: WifiManager,
) : ViewModel() {

    /** One sensor as known to the fleet — drives the "tap which sensor
     *  you're at" cards and the live RSSI feedback table. */
    data class SensorInfo(
        val deviceId: String,
        val name: String,
        val lat: Double,
        val lon: Double,
        val online: Boolean,
        val ageS: Double?,
    )

    /** Live snapshot of "what the fleet is hearing right now". */
    data class SensorReading(
        val sensorId: String,
        val rssi: Int?,
        val samplesInWindow: Int,
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
    )

    data class State(
        val backendUrl: String = "",
        val token: String = "",
        val operatorLabel: String = "",
        val isWalking: Boolean = false,
        val sessionId: String? = null,
        val advertiseUuid: String? = null,
        val tracePoints: Int = 0,
        val samplesTotal: Int = 0,
        val sensorsHearingMe: List<SensorReading> = emptyList(),
        val availableSensors: List<SensorInfo> = emptyList(),
        val checkpointResults: Map<String, CheckpointResult> = emptyMap(),
        val sessionReadiness: SessionReadiness = SessionReadiness(),
        /** Live "phone GPS vs fleet's triangulated position" + convergence
         *  telemetry. Updated by a ~1 Hz poll during the walk. */
        val myPosition: MyPosition = MyPosition(),
        val phoneLat: Double? = null,
        val phoneLon: Double? = null,
        val gpsAccuracyM: Float? = null,
        val errorMessage: String? = null,
        val infoMessage: String? = null,
        val fitResult: JsonObject? = null,
        /** Count of samples + checkpoints buffered locally because the
         *  backend couldn't be reached. Drives the amber "N queued" UI
         *  indicator so the operator knows the walk is still being
         *  recorded even when one AP's coverage has dropped. */
        val queuedCount: Int = 0,
        /** SSID the phone is currently associated with — surfaces in the
         *  UI next to the "Switch WiFi" button so operators can see which
         *  network they're on while roaming across the property. */
        val currentSsid: String? = null,
        val fitApplied: Boolean? = null,
        val bluetoothEnabled: Boolean = true,
        val backendStatus: BackendStatus = BackendStatus.Unknown,
    )

    private val _state = MutableStateFlow(State(
        backendUrl = prefs.backendUrl,
        token = prefs.calibrationToken,
        operatorLabel = prefs.operatorLabel,
        bluetoothEnabled = bluetoothManager.adapter?.isEnabled == true,
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
                refreshCurrentSsid()
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
                                         next.accuracyM, next.tsMs)
            if (res.isFailure) return
            val body = res.getOrNull()
            if (body != null) {
                val warnings = body.getAsJsonArray("warnings")?.map { it.asString } ?: emptyList()
                val cr = CheckpointResult(
                    sensorId = next.sensorId,
                    severity = body.get("severity")?.asString ?: "warn",
                    gpsDriftM = body.get("gps_drift_m")?.takeIf { !it.isJsonNull }?.asDouble,
                    rssiAtTouch = body.get("rssi_at_touch")?.takeIf { !it.isJsonNull }?.asInt,
                    strongestAtTouch = body.get("strongest_sensor_at_touch")
                        ?.takeIf { !it.isJsonNull }?.asString,
                    warnings = warnings,
                    tsMs = next.tsMs,
                )
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
    private fun refreshCurrentSsid() {
        try {
            val info = wifiManager.connectionInfo ?: return
            val raw = info.ssid ?: return
            // wifiManager returns "<unknown ssid>" when permission is denied
            // and quotes real SSIDs as "Name". Strip quotes for display.
            val ssid = raw.removePrefix("\"").removeSuffix("\"")
                .takeIf { it.isNotBlank() && it != "<unknown ssid>" }
            if (ssid != _state.value.currentSsid) {
                _state.value = _state.value.copy(currentSsid = ssid)
            }
        } catch (_: Exception) { /* permission / roaming glitch */ }
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
        _state.value = _state.value.copy(backendUrl = value, backendStatus = BackendStatus.Unknown)
    }

    fun setToken(value: String) {
        prefs.calibrationToken = value
        _state.value = _state.value.copy(token = value, backendStatus = BackendStatus.Unknown)
    }

    fun setOperatorLabel(value: String) {
        prefs.operatorLabel = value
        _state.value = _state.value.copy(operatorLabel = value)
    }

    fun clearMessages() {
        _state.value = _state.value.copy(errorMessage = null, infoMessage = null)
    }

    /** Re-check Bluetooth state, e.g. after the user toggles it in
     *  Settings and returns to the screen. */
    fun refreshBluetoothState() {
        _state.value = _state.value.copy(
            bluetoothEnabled = bluetoothManager.adapter?.isEnabled == true,
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
        val s = _state.value
        if (s.backendUrl.isBlank() || s.token.isBlank()) {
            _state.value = s.copy(backendStatus = BackendStatus.Unknown)
            return
        }
        viewModelScope.launch {
            var res = api.walkSensors(s.backendUrl, s.token)
            if (res.isFailure) {
                val firstMsg = res.exceptionOrNull()?.message.orEmpty()
                val is401 = "401" in firstMsg
                // Silent retry with the default token if a) the current one
                // got a 401 AND b) it's not already the default.
                if (is401 && s.token != CalibrationApi.DEFAULT_TOKEN) {
                    val recovered = api.walkSensors(s.backendUrl, CalibrationApi.DEFAULT_TOKEN)
                    if (recovered.isSuccess) {
                        prefs.calibrationToken = CalibrationApi.DEFAULT_TOKEN
                        _state.value = _state.value.copy(
                            token = CalibrationApi.DEFAULT_TOKEN,
                            infoMessage = "Stored token was stale — reset to default.",
                        )
                        res = recovered
                    }
                }
            }
            if (res.isFailure) {
                val msg = res.exceptionOrNull()?.message.orEmpty()
                _state.value = _state.value.copy(
                    backendStatus = if ("401" in msg) BackendStatus.AuthFailed
                                    else BackendStatus.Unreachable,
                    errorMessage = "Backend check failed: $msg",
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val sensors = mutableListOf<SensorInfo>()
            body.getAsJsonArray("sensors")?.forEach { el ->
                val obj = el.asJsonObject
                sensors.add(SensorInfo(
                    deviceId = obj.get("device_id")?.asString ?: return@forEach,
                    name = obj.get("name")?.asString ?: "?",
                    lat = obj.get("lat")?.asDouble ?: 0.0,
                    lon = obj.get("lon")?.asDouble ?: 0.0,
                    online = obj.get("online")?.asBoolean ?: false,
                    ageS = obj.get("age_s")?.takeIf { !it.isJsonNull }?.asDouble,
                ))
            }
            _state.value = _state.value.copy(
                availableSensors = sensors,
                backendStatus = BackendStatus.Ok,
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
        val s = _state.value
        if (s.isWalking) return
        if (s.backendUrl.isBlank() || s.token.isBlank()) {
            _state.value = s.copy(errorMessage = "Backend URL and token are required.")
            return
        }
        if (bluetoothManager.adapter?.isEnabled != true) {
            _state.value = s.copy(
                bluetoothEnabled = false,
                errorMessage = "Bluetooth is off — enable it before starting a walk.",
            )
            return
        }
        viewModelScope.launch {
            val res = api.walkStart(s.backendUrl, s.token,
                                    operatorLabel = s.operatorLabel.ifBlank { "phone" })
            if (res.isFailure) {
                val msg = res.exceptionOrNull()?.message.orEmpty()
                _state.value = _state.value.copy(
                    errorMessage = "Walk-start failed: $msg",
                    backendStatus = if ("401" in msg) BackendStatus.AuthFailed
                                    else BackendStatus.Unreachable,
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val sid = body.get("session_id")?.asString ?: return@launch
            val uuid = body.get("advertise_uuid")?.asString ?: return@launch

            // Start BLE peripheral
            val ok = advertiser.start(uuid) { msg ->
                _state.value = _state.value.copy(errorMessage = msg, isWalking = false)
            }
            if (!ok) return@launch

            // Subscribe to GPS — high-accuracy fixes at ~1 Hz
            try {
                locationManager.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER, 1000L, 0.5f, gpsListener
                )
                if (locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                    locationManager.requestLocationUpdates(
                        LocationManager.NETWORK_PROVIDER, 2000L, 1.0f, gpsListener
                    )
                }
            } catch (e: SecurityException) {
                advertiser.stop()
                _state.value = _state.value.copy(errorMessage = "Location permission missing.")
                return@launch
            } catch (e: Exception) {
                advertiser.stop()
                _state.value = _state.value.copy(errorMessage = "GPS unavailable: ${e.message}")
                return@launch
            }

            // Reset local queue state for the new session
            synchronized(queueLock) {
                pendingSamples.clear()
                pendingCheckpoints.clear()
            }
            _state.value = _state.value.copy(
                isWalking = true,
                sessionId = sid,
                advertiseUuid = uuid,
                tracePoints = 0,
                samplesTotal = 0,
                sensorsHearingMe = emptyList(),
                checkpointResults = emptyMap(),
                fitResult = null,
                fitApplied = null,
                backendStatus = BackendStatus.Ok,
                queuedCount = 0,
                infoMessage = "Walking — visit each sensor and tap its 'I'm here' button " +
                              "to anchor the fit + verify the sensor's coordinates.",
            )
            refreshCurrentSsid()
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
                        _state.value = _state.value.copy(myPosition = MyPosition(
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
                            convergenceTargetM = body.get("convergence_target_m")?.asDouble ?: 10.0,
                            dwellTargetS = body.get("dwell_target_s")?.asDouble ?: 5.0,
                            minSensors = body.get("min_sensors")?.asInt ?: 3,
                        ))
                    }
                }
                delay(1000)
            }
        }
    }

    /** Operator walked up to a sensor and pressed its "I'm here" button. */
    fun markAtSensor(sensor: SensorInfo) {
        val s = _state.value
        val sid = s.sessionId ?: run {
            _state.value = s.copy(errorMessage = "Start the walk first.")
            return
        }
        val lat = s.phoneLat
        val lon = s.phoneLon
        if (lat == null || lon == null) {
            _state.value = s.copy(errorMessage = "No GPS fix yet — wait for the phone to lock on.")
            return
        }
        val nowMs = System.currentTimeMillis()
        val accM = s.gpsAccuracyM
        viewModelScope.launch {
            val res = api.walkCheckpoint(
                baseUrl = s.backendUrl, token = s.token,
                sessionId = sid, sensorId = sensor.deviceId,
                lat = lat, lon = lon,
                accuracyM = accM, tsMs = nowMs,
            )
            if (res.isFailure) {
                // Offline — queue the checkpoint so the fit anchor + sanity
                // result still lands once connectivity returns. Show an
                // optimistic "queued" card in the UI so the operator can
                // keep walking without waiting.
                enqueueCheckpoint(QueuedCheckpoint(
                    sensorId = sensor.deviceId,
                    lat = lat, lon = lon,
                    tsMs = nowMs, accuracyM = accM,
                ))
                _state.value = _state.value.copy(
                    checkpointResults = _state.value.checkpointResults + (sensor.deviceId to
                        CheckpointResult(
                            sensorId = sensor.deviceId,
                            severity = "warn",
                            gpsDriftM = null,
                            rssiAtTouch = null,
                            strongestAtTouch = null,
                            warnings = listOf("queued_offline_will_sync_when_backend_reachable"),
                            tsMs = nowMs,
                        )),
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val warnings = body.getAsJsonArray("warnings")?.map { it.asString } ?: emptyList()
            val cr = CheckpointResult(
                sensorId = sensor.deviceId,
                severity = body.get("severity")?.asString ?: "warn",
                gpsDriftM = body.get("gps_drift_m")?.takeIf { !it.isJsonNull }?.asDouble,
                rssiAtTouch = body.get("rssi_at_touch")?.takeIf { !it.isJsonNull }?.asInt,
                strongestAtTouch = body.get("strongest_sensor_at_touch")
                    ?.takeIf { !it.isJsonNull }?.asString,
                warnings = warnings,
                tsMs = nowMs,
            )
            _state.value = _state.value.copy(
                checkpointResults = _state.value.checkpointResults + (sensor.deviceId to cr),
            )
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
        try { locationManager.removeUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        viewModelScope.launch {
            // Last-ditch drain of anything still queued before we end the
            // session — avoids "you walked past sensor X and got a great
            // checkpoint, but then lost WiFi, so the fit never anchored".
            flushOnce(sid, s.backendUrl, s.token)
            val res = api.walkEnd(s.backendUrl, s.token, sid)
            if (res.isFailure) {
                _state.value = _state.value.copy(
                    isWalking = false,
                    errorMessage = "Walk-end failed: ${res.exceptionOrNull()?.message}"
                )
                return@launch
            }
            val body = res.getOrNull() ?: return@launch
            val fit = body.getAsJsonObject("fit")
            val applied = body.get("applied")?.asBoolean ?: false
            _state.value = _state.value.copy(
                isWalking = false,
                fitResult = fit,
                fitApplied = applied,
                infoMessage = if (applied) "Calibration applied to backend."
                              else "Walk ended — fit did not meet quality threshold (R² < 0.4).",
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
                                samplesInWindow = obj.get("samples_in_window")?.asInt ?: 0,
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
                        val readiness = if (sr != null) SessionReadiness(
                            sensorsReady = sr.get("sensors_ready")?.asInt ?: 0,
                            sensorsTotal = sr.get("sensors_total")?.asInt ?: 0,
                            readyOverall = sr.get("ready_overall")?.asBoolean ?: false,
                            minRequired = sr.get("min_required")?.asInt ?: 4,
                        ) else SessionReadiness()
                        _state.value = _state.value.copy(
                            tracePoints = body.get("trace_points")?.asInt ?: s.tracePoints,
                            samplesTotal = body.get("samples_total")?.asInt ?: s.samplesTotal,
                            sensorsHearingMe = sensors,
                            sessionReadiness = readiness,
                        )
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
        try { locationManager.removeUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        super.onCleared()
    }
}

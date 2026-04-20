package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
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
        val phoneLat: Double? = null,
        val phoneLon: Double? = null,
        val gpsAccuracyM: Float? = null,
        val errorMessage: String? = null,
        val infoMessage: String? = null,
        val fitResult: JsonObject? = null,
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

    private var feedbackJob: Job? = null
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

    /** Preflight + sensor list load. Called on screen entry and after
     *  the operator edits backend URL/token. Distinguishes auth failure
     *  (red dot, "check token") from unreachable backend (gray dot). */
    fun refreshConnectivity() {
        val s = _state.value
        if (s.backendUrl.isBlank() || s.token.isBlank()) {
            _state.value = s.copy(backendStatus = BackendStatus.Unknown)
            return
        }
        viewModelScope.launch {
            val res = api.walkSensors(s.backendUrl, s.token)
            if (res.isFailure) {
                val msg = res.exceptionOrNull()?.message.orEmpty()
                _state.value = _state.value.copy(
                    backendStatus = if ("401" in msg) BackendStatus.AuthFailed
                                    else BackendStatus.Unreachable,
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
                infoMessage = "Walking — visit each sensor and tap its 'I'm here' button " +
                              "to anchor the fit + verify the sensor's coordinates.",
            )
            startFeedbackPolling(sid)
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
        viewModelScope.launch {
            val res = api.walkCheckpoint(
                baseUrl = s.backendUrl, token = s.token,
                sessionId = sid, sensorId = sensor.deviceId,
                lat = lat, lon = lon,
                accuracyM = s.gpsAccuracyM, tsMs = nowMs,
            )
            if (res.isFailure) {
                _state.value = _state.value.copy(
                    errorMessage = "Checkpoint failed: ${res.exceptionOrNull()?.message}"
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
        try { locationManager.removeUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        viewModelScope.launch {
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
        viewModelScope.launch {
            api.walkSample(s.backendUrl, s.token, sid,
                           lat = loc.latitude, lon = loc.longitude,
                           tsMs = nowMs,
                           accuracyM = if (loc.hasAccuracy()) loc.accuracy else null)
        }
        _state.value = s.copy(
            phoneLat = loc.latitude, phoneLon = loc.longitude,
            gpsAccuracyM = loc.accuracy,
        )
    }

    override fun onCleared() {
        feedbackJob?.cancel()
        try { locationManager.removeUpdates(gpsListener) } catch (_: Exception) {}
        advertiser.stop()
        super.onCleared()
    }
}

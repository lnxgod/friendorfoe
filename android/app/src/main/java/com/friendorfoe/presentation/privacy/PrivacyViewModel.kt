package com.friendorfoe.presentation.privacy

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import com.friendorfoe.data.remote.LivePrivacyDeviceDto
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationCoordinator
import com.friendorfoe.detection.BleInvestigationRequest
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyDetectionOrigin
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.WifiAnomalyDetector
import com.friendorfoe.detection.elapsedRealtimeMs
import com.friendorfoe.detection.isBleInvestigationTargetFresh
import com.friendorfoe.presentation.alerts.SkyAlertCandidate
import com.friendorfoe.presentation.alerts.SkyAlertPolicy
import com.friendorfoe.presentation.alerts.SkyAlertSettings
import com.friendorfoe.sensor.SensorFusionEngine
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import javax.inject.Inject

internal fun phoneInvestigationError(
    origin: PrivacyDetectionOrigin,
    target: BleInvestigationTarget,
    phoneAvailable: Boolean,
    nowElapsedMs: Long,
): String? {
    if (origin != target.origin) return "origin_mismatch"
    if (!phoneAvailable) return "phone_unavailable"
    if (target.mode != BleInvestigationMode.GATT) return "phone_requires_gatt"
    return validateGattTarget(target, nowElapsedMs)
}

private fun validateGattTarget(target: BleInvestigationTarget, nowElapsedMs: Long): String? {
    val mac = target.mac
    if (mac == null || !BLE_MAC_REGEX.matches(mac)) return "invalid_target"
    if (!isBleInvestigationTargetFresh(target.observedAtElapsedMs, nowElapsedMs)) return "stale_target"
    return null
}

private val BLE_MAC_REGEX = Regex("^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$")
private val INVESTIGATION_TERMINAL_STATES = setOf(
    BleInvestigationState.COMPLETE,
    BleInvestigationState.FAILED,
    BleInvestigationState.CANCELLED,
)

internal fun shouldRejectConcurrentInvestigationStart(
    hasActive: Boolean,
    currentState: BleInvestigationState?,
): Boolean = hasActive && currentState !in INVESTIGATION_TERMINAL_STATES

private fun boundedInvestigationRequestId(generation: Long, elapsedMs: Long): String =
    "inv-${generation.coerceAtLeast(0).toString(36)}-${elapsedMs.coerceAtLeast(0).toString(36)}"
        .take(32)

private fun investigationRouteErrorSummary(error: String?): String = when (error) {
    "phone_unavailable" -> "Phone BLE investigation is unavailable"
    "phone_requires_gatt" -> "Phone investigation requires a GATT target"
    "invalid_target" -> "The BLE target address is invalid"
    "stale_target" -> "The BLE target observation is stale"
    "origin_mismatch" -> "The investigation target origin is invalid"
    else -> "BLE investigation route is unavailable"
}

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val skyObjectRepository: SkyObjectRepository,
    val sensorFusionEngine: SensorFusionEngine,
    private val wifiAnomalyDetector: WifiAnomalyDetector,
    private val sensorMapApiService: SensorMapApiService,
    private val bleInvestigationCoordinator: BleInvestigationCoordinator,
    private val privacyAlertNotifier: PrivacyAlertNotifier,
) : ViewModel() {

    private data class ActiveInvestigation(
        val generation: Long,
        val request: BleInvestigationRequest,
    )

    private val _investigationResult = MutableStateFlow<BleInvestigationResult?>(null)
    val investigationResult: StateFlow<BleInvestigationResult?> = _investigationResult.asStateFlow()
    private var investigationGeneration = 0L
    private var activeInvestigation: ActiveInvestigation? = null
    private var investigationObserverJob: kotlinx.coroutines.Job? = null

    private val _backendOnlyMode = MutableStateFlow(skyObjectRepository.prefs.backendOnlyMode)
    val backendOnlyMode: StateFlow<Boolean> = _backendOnlyMode.asStateFlow()

    private val skyAlertSettings = flow {
        while (true) {
            emit(currentSkyAlertSettings())
            delay(2_000)
        }
    }.distinctUntilChanged()

    val skyAlertCandidates: StateFlow<List<SkyAlertCandidate>> = combine(
        skyObjectRepository.skyObjects,
        skyAlertSettings
    ) { objects, settings ->
        SkyAlertPolicy.candidatesFor(objects, settings)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    init {
        skyObjectRepository.ensureStarted(0.0, 0.0)
        // Poll the WifiAnomalyDetector every 15 s. Surfaces Pwnagotchi beacons
        // (attack tool), evil-twin APs, karma attacks — all computed off the
        // existing WiFi scan-results cache that other scanners already populate.
        viewModelScope.launch {
            while (true) {
                try {
                    if (!skyObjectRepository.prefs.wifiAnomalyEnabled ||
                        skyObjectRepository.prefs.backendOnlyMode
                    ) {
                        _wifiAnomalies.value = emptyList()
                        delay(15_000)
                        continue
                    }
                    val anomalies = wifiAnomalyDetector.analyze()
                    if (anomalies.isNotEmpty()) {
                        _wifiAnomalies.value = anomalies
                        anomalies.forEach(privacyAlertNotifier::notifyWifiAnomaly)
                    } else if (_wifiAnomalies.value.isNotEmpty()) {
                        // decay stale alerts after 60 s of no new hits
                        val latest = _wifiAnomalies.value.maxOfOrNull { it.timestamp.toEpochMilli() } ?: 0L
                        if (System.currentTimeMillis() - latest > 60_000) {
                            _wifiAnomalies.value = emptyList()
                        }
                    }
                } catch (_: Throwable) { /* sleep and retry */ }
                delay(15_000)
            }
        }
        viewModelScope.launch {
            while (true) {
                try {
                    val response = sensorMapApiService.getLivePrivacyDevices()
                    _backendPrivacyDetections.value = response.devices.mapNotNull {
                        it.toGlassesDetection()
                    }
                } catch (_: Throwable) {
                    // Backend privacy view is an enhancement; keep local phone
                    // detection working if the backend is unreachable.
                }
                delay(5_000)
            }
        }
    }

    private val _wifiAnomalies = MutableStateFlow<List<WifiAnomalyDetector.WifiAnomaly>>(emptyList())
    val wifiAnomalies: StateFlow<List<WifiAnomalyDetector.WifiAnomaly>> = _wifiAnomalies.asStateFlow()
    private val _backendPrivacyDetections = MutableStateFlow<List<GlassesDetection>>(emptyList())
    val privacyDetections: StateFlow<List<GlassesDetection>> = combine(
        skyObjectRepository.glassesDetections,
        _backendPrivacyDetections,
        _wifiAnomalies,
    ) { local, backend, wifiAnomalies ->
        mergePrivacyDetections(
            local,
            backend + wifiAnomalies.map { it.toPrivacyDetection() }
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    /** All privacy detections, grouped by category */
    val categorizedDetections: StateFlow<Map<PrivacyCategory, List<GlassesDetection>>> =
        privacyDetections.map { detections ->
            detections.groupBy { it.category }
                .toSortedMap(compareByDescending<PrivacyCategory> { it.threatLevel }.thenBy { it.name })
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5000),
            initialValue = emptyMap()
        )

    /** Total device count */
    val totalCount: StateFlow<Int> = privacyDetections.map { it.size }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    /** High threat count (threat level >= 2) */
    val threatCount: StateFlow<Int> = privacyDetections.map { detections ->
        detections.count { it.category.threatLevel >= 2 }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    /** Stalker alerts */
    val stalkerAlerts = skyObjectRepository.stalkerAlerts

    /** Ultrasonic tracking beacon alerts */
    val ultrasonicAlerts = skyObjectRepository.ultrasonicAlerts

    /** BLE tracker for direction finding */
    val bleTracker: BleTracker = skyObjectRepository.bleTracker

    init {
        viewModelScope.launch {
            privacyDetections.collect { detections ->
                detections.forEach(privacyAlertNotifier::notifyDetection)
            }
        }
        viewModelScope.launch {
            stalkerAlerts.collect { alerts ->
                alerts.forEach(privacyAlertNotifier::notifyStalker)
            }
        }
        viewModelScope.launch {
            ultrasonicAlerts.collect { alerts ->
                alerts.forEach(privacyAlertNotifier::notifyUltrasonic)
            }
        }
    }

    /** Live RSSI for the device being tracked (updated every BLE advertisement) */
    fun getTrackedDeviceRssi(mac: String): Int? {
        return privacyDetections.value
            .find { it.mac == mac }?.rssi
    }

    fun ignoreDevice(mac: String) {
        skyObjectRepository.ignorePrivacyDevice(mac)
    }

    /** Clear all detections and rescan fresh */
    fun refreshDetections() {
        syncBackendMode()
        skyObjectRepository.refreshPrivacyDetections()
    }

    fun investigate(detection: GlassesDetection) {
        val target = detection.investigationTarget ?: return
        startInvestigation(detection.origin, target)
    }

    fun cancelInvestigation() {
        if (activeInvestigation == null) return
        val current = _investigationResult.value
        if (current?.state in INVESTIGATION_TERMINAL_STATES) return
        viewModelScope.launch {
            bleInvestigationCoordinator.cancel()
        }
    }

    fun clearInvestigation() {
        val active = activeInvestigation
        if (_investigationResult.value?.state !in INVESTIGATION_TERMINAL_STATES && active != null) {
            viewModelScope.launch {
                bleInvestigationCoordinator.cancel()
            }
        }
        investigationGeneration++
        activeInvestigation = null
        investigationObserverJob?.cancel()
        investigationObserverJob = null
        _investigationResult.value = null
    }

    private fun startInvestigation(
        origin: PrivacyDetectionOrigin,
        target: BleInvestigationTarget,
    ) {
        if (shouldRejectConcurrentInvestigationStart(
                hasActive = activeInvestigation != null,
                currentState = _investigationResult.value?.state,
            )
        ) return
        investigationGeneration++
        investigationObserverJob?.cancel()
        val generation = investigationGeneration
        val requestId = boundedInvestigationRequestId(generation, elapsedRealtimeMs())
        val error = phoneInvestigationError(
            origin = origin,
            target = target,
            phoneAvailable = phoneInvestigationAvailable(),
            nowElapsedMs = elapsedRealtimeMs(),
        )
        if (error != null) {
            activeInvestigation = null
            _investigationResult.value = BleInvestigationResult(
                requestId = requestId,
                transport = "unavailable",
                mode = target.mode,
                targetMac = target.mac,
                state = BleInvestigationState.FAILED,
                connectable = null,
                services = emptyList(),
                characteristics = emptyList(),
                reads = emptyMap(),
                bonded = false,
                encrypted = false,
                authenticationRequired = false,
                summary = investigationRouteErrorSummary(error),
                error = error,
                truncated = false,
            )
            return
        }

        val request = BleInvestigationRequest(
            requestId = requestId,
            target = target,
            route = BleInvestigationRoute.PHONE,
        )
        activeInvestigation = ActiveInvestigation(generation, request)
        _investigationResult.value = BleInvestigationResult(
            requestId = requestId,
            transport = "phone",
            mode = target.mode,
            targetMac = target.mac,
            state = BleInvestigationState.QUEUED,
            connectable = null,
            services = emptyList(),
            characteristics = emptyList(),
            reads = emptyMap(),
            bonded = false,
            encrypted = false,
            authenticationRequired = false,
            summary = "Investigation queued",
            error = null,
            truncated = false,
        )
        investigationObserverJob = observePhoneInvestigation(generation, request)
    }

    private fun observePhoneInvestigation(
        generation: Long,
        request: BleInvestigationRequest,
    ) = viewModelScope.launch {
        val progress = launch {
            bleInvestigationCoordinator.state.filterNotNull().collect { result ->
                publishInvestigationGeneration(generation, request.requestId, result)
            }
        }
        try {
            val result = bleInvestigationCoordinator.investigatePhone(request)
            publishInvestigationGeneration(generation, request.requestId, result)
        } finally {
            progress.cancel()
        }
    }

    private fun publishInvestigationGeneration(
        generation: Long,
        requestId: String,
        result: BleInvestigationResult,
    ) {
        val active = activeInvestigation ?: return
        if (active.generation != generation || active.request.requestId != requestId ||
            result.requestId != requestId
        ) return
        if (_investigationResult.value?.state in INVESTIGATION_TERMINAL_STATES) return
        _investigationResult.value = result
    }

    private fun phoneInvestigationAvailable(): Boolean {
        if (!context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) return false
        val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)
            ?.adapter
            ?: return false
        if (!adapter.isEnabled) return false
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    }

    fun enablePhonePrivacyScanning() {
        if (skyObjectRepository.prefs.backendOnlyMode) {
            skyObjectRepository.prefs.backendOnlyMode = false
            skyObjectRepository.restartDetectionSources()
        }
        _backendOnlyMode.value = false
    }

    private fun currentSkyAlertSettings(): SkyAlertSettings =
        SkyAlertSettings(
            droneAlertsEnabled = skyObjectRepository.prefs.droneAlertsEnabled,
            helicopterAlertsEnabled = skyObjectRepository.prefs.helicopterAlertsEnabled,
            militaryAlertsEnabled = skyObjectRepository.prefs.militaryAlertsEnabled,
            policeAlertsEnabled = skyObjectRepository.prefs.policeAlertsEnabled
        )

    fun startDirectionScan(mac: String) {
        bleTracker.startDirectionScan(mac)
    }

    fun finishDirectionScan(): BleTracker.DirectionResult? {
        return bleTracker.finishDirectionScan()
    }

    private fun syncBackendMode() {
        _backendOnlyMode.value = skyObjectRepository.prefs.backendOnlyMode
    }

    private fun LivePrivacyDeviceDto.toGlassesDetection(): GlassesDetection? {
        val key = stablePrivacyKey()
        val type = displayLabel?.ifBlank { null }
            ?: deviceType
            ?: privacyKind
            ?: "Privacy Signal"
        val category = categoryForPrivacyKind(privacyKind, type)
        val now = Instant.now()
        val first = firstSeen?.toEpochInstant() ?: now
        val last = lastSeen?.toEpochInstant() ?: now
        val detailMap = buildMap {
            privacyKind?.let { put("privacy_kind", it) }
            riskLevel?.let { put("risk", it) }
            displayDetail?.let { put("detail", it) }
            source?.let { put("source", it) }
            if (sensorCount > 0) put("sensors", sensorCount.toString())
            if (macRotations > 0) put("mac_rotations", macRotations.toString())
            bleActivity?.let { put("apple_activity", it.toString()) }
            if (privacyEvidence.isNotEmpty()) put("evidence", privacyEvidence.joinToString("; ") { it.toString() })
            appleContinuity?.let { put("apple_continuity", it.toString()) }
        }
        return GlassesDetection(
            mac = lastBssid ?: fingerprint ?: key,
            deviceName = displayDetail ?: deviceType,
            deviceType = type,
            manufacturer = manufacturer ?: "Unknown",
            hasCamera = type.contains("camera", ignoreCase = true) ||
                category in setOf(
                    PrivacyCategory.HIDDEN_CAMERA,
                    PrivacyCategory.SURVEILLANCE_CAMERA,
                    PrivacyCategory.ALPR_CAMERA,
                    PrivacyCategory.BODY_CAMERA,
                    PrivacyCategory.VEHICLE_CAMERA,
                ),
            rssi = currentRssi ?: -100,
            confidence = confidence,
            matchReason = "backend:${privacyKind ?: deviceType ?: "privacy"}",
            firstSeen = first,
            lastSeen = last,
            details = detailMap,
            category = category,
            fingerprintKey = key,
            seenMacs = setOfNotNull(lastBssid),
            bleCompanyId = bleCompanyId,
            bleAppleType = bleAppleType,
            bleAppleFlags = bleAppleFlags,
            origin = PrivacyDetectionOrigin.BACKEND,
        )
    }

    private fun LivePrivacyDeviceDto.stablePrivacyKey(): String {
        val fp = fingerprint?.takeIf { it.isNotBlank() }
        val ja3 = bleJa3?.takeIf { it.isNotBlank() }
        return when {
            ja3 != null -> "fp:$ja3"
            fp != null -> "fp:$fp"
            !lastBssid.isNullOrBlank() -> "mac:$lastBssid"
            else -> "backend:${displayLabel ?: deviceType ?: privacyKind ?: "privacy"}"
        }
    }

    private fun WifiAnomalyDetector.WifiAnomaly.toPrivacyDetection(): GlassesDetection {
        val title = when (type) {
            "pwnagotchi" -> "Pwnagotchi"
            "evil_twin" -> "Evil Twin"
            "karma_attack" -> "Karma Attack"
            "rogue_ap" -> "Rogue AP"
            else -> "WiFi Anomaly"
        }
        val detailMap = buildMap {
            put("source", "android_wifi_anomaly")
            put("type", type)
            put("ssid", ssid)
            put("detail", details)
            if (bssids.isNotEmpty()) put("bssids", bssids.joinToString(", "))
            evidence.forEachIndexed { index, item ->
                put(
                    "ap_${index + 1}",
                    "${item.bssid} ${item.security}, ${item.rssi}dBm" +
                        if (item.frequencyMhz > 0) ", ${item.frequencyMhz}MHz" else ""
                )
            }
        }
        val primaryBssid = bssids.firstOrNull() ?: "wifi:$type:$ssid"
        val key = "wifi_anomaly:$type:$ssid:${bssids.sorted().joinToString(",")}"
        return GlassesDetection(
            mac = primaryBssid,
            deviceName = ssid,
            deviceType = title,
            manufacturer = "WiFi",
            hasCamera = false,
            rssi = evidence.maxByOrNull { it.rssi }?.rssi ?: -100,
            confidence = when (threatLevel) {
                3 -> 0.95f
                2 -> 0.80f
                else -> 0.65f
            },
            matchReason = "wifi_anomaly:$type",
            firstSeen = timestamp,
            lastSeen = timestamp,
            details = detailMap,
            category = PrivacyCategory.ATTACK_TOOL,
            fingerprintKey = key,
            seenMacs = bssids.toSet().ifEmpty { setOf(primaryBssid) },
            origin = PrivacyDetectionOrigin.WIFI,
        )
    }

    private fun categoryForPrivacyKind(kind: String?, fallbackType: String): PrivacyCategory = when (kind) {
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
        else -> com.friendorfoe.detection.GlassesDetector.categorizeDeviceType(fallbackType)
    }

    private fun mergePrivacyDetections(
        local: List<GlassesDetection>,
        backend: List<GlassesDetection>,
    ): List<GlassesDetection> {
        val merged = linkedMapOf<String, GlassesDetection>()
        (local + backend).forEach { detection ->
            val key = detection.fingerprintKey.ifBlank { "mac:${detection.mac}" }.lowercase()
            val existing = merged[key]
            if (existing == null ||
                detection.lastSeen.isAfter(existing.lastSeen) ||
                detection.category.threatLevel > existing.category.threatLevel) {
                merged[key] = detection
            }
        }
        return merged.values.sortedWith(
            compareByDescending<GlassesDetection> { it.category.threatLevel }
                .thenByDescending { it.rssi }
                .thenByDescending { it.lastSeen }
        )
    }

    private fun Double.toEpochInstant(): Instant {
        val seconds = this.toLong()
        val nanos = ((this - seconds.toDouble()) * 1_000_000_000.0).toLong()
        return Instant.ofEpochSecond(seconds, nanos)
    }
}

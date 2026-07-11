package com.friendorfoe.presentation.privacy

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.data.badge.BadgeUsbState
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
import kotlinx.coroutines.flow.takeWhile
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import javax.inject.Inject

internal data class BadgeInvestigationAvailability(
    val scannerSlotZeroConnected: Boolean,
    val usbAvailable: Boolean,
    val bleAvailable: Boolean,
    val httpAvailable: Boolean,
) {
    val badgeAvailable: Boolean
        get() = scannerSlotZeroConnected && (usbAvailable || bleAvailable || httpAvailable)
}

internal data class BleInvestigationRouteDecision(
    val route: BleInvestigationRoute?,
    val error: String? = null,
)

internal fun deriveBadgeInvestigationAvailability(
    badgeState: BadgeUsbState,
): BadgeInvestigationAvailability {
    val scannerSlotZeroConnected = badgeState.controlStatus?.scanners
        ?.any { it.slot == 0 && it.connected }
        ?: false
    val ble = badgeState.controlStatus?.bleControl
    return BadgeInvestigationAvailability(
        scannerSlotZeroConnected = scannerSlotZeroConnected,
        usbAvailable = badgeState.status == com.friendorfoe.data.badge.BadgeUsbStatus.CONNECTED,
        bleAvailable = badgeState.status == com.friendorfoe.data.badge.BadgeUsbStatus.BLE_CONNECTED &&
            ble?.connected == true && ble.bonded && ble.encrypted,
        httpAvailable = badgeState.status in setOf(
            com.friendorfoe.data.badge.BadgeUsbStatus.AP_CONNECTED,
            com.friendorfoe.data.badge.BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
        ),
    )
}

internal fun selectInvestigationRoute(
    origin: PrivacyDetectionOrigin,
    target: BleInvestigationTarget,
    badgeAvailable: Boolean,
    requestedRoute: BleInvestigationRoute,
    phoneAvailable: Boolean,
    nowElapsedMs: Long,
): BleInvestigationRouteDecision {
    if (origin != target.origin) return BleInvestigationRouteDecision(null, "origin_mismatch")

    if (requestedRoute == BleInvestigationRoute.PHONE) {
        if (!phoneAvailable) return BleInvestigationRouteDecision(null, "phone_unavailable")
        validatePhoneTarget(target, nowElapsedMs)?.let {
            return BleInvestigationRouteDecision(null, it)
        }
        return BleInvestigationRouteDecision(BleInvestigationRoute.PHONE)
    }
    if (requestedRoute == BleInvestigationRoute.BADGE) {
        if (!badgeAvailable) return BleInvestigationRouteDecision(null, "badge_unavailable")
        validateBadgeTarget(target, nowElapsedMs)?.let {
            return BleInvestigationRouteDecision(null, it)
        }
        return BleInvestigationRouteDecision(BleInvestigationRoute.BADGE)
    }

    if (target.mode == BleInvestigationMode.PASSIVE_CAPTURE) {
        return if (badgeAvailable) {
            BleInvestigationRouteDecision(BleInvestigationRoute.BADGE)
        } else {
            BleInvestigationRouteDecision(null, "badge_unavailable")
        }
    }
    validateGattTarget(target, nowElapsedMs)?.let {
        return BleInvestigationRouteDecision(null, it)
    }

    val preferred = if (origin == PrivacyDetectionOrigin.BADGE) {
        listOf(BleInvestigationRoute.BADGE, BleInvestigationRoute.PHONE)
    } else {
        listOf(BleInvestigationRoute.PHONE, BleInvestigationRoute.BADGE)
    }
    preferred.forEach { route ->
        if (route == BleInvestigationRoute.PHONE && phoneAvailable) {
            return BleInvestigationRouteDecision(route)
        }
        if (route == BleInvestigationRoute.BADGE && badgeAvailable) {
            return BleInvestigationRouteDecision(route)
        }
    }
    return BleInvestigationRouteDecision(
        route = null,
        error = if (origin == PrivacyDetectionOrigin.BADGE) "badge_unavailable" else "phone_unavailable",
    )
}

private fun validatePhoneTarget(target: BleInvestigationTarget, nowElapsedMs: Long): String? {
    if (target.mode != BleInvestigationMode.GATT) return "phone_requires_gatt"
    return validateGattTarget(target, nowElapsedMs)
}

private fun validateBadgeTarget(target: BleInvestigationTarget, nowElapsedMs: Long): String? =
    if (target.mode == BleInvestigationMode.GATT) validateGattTarget(target, nowElapsedMs) else null

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

private fun boundedInvestigationRequestId(generation: Long, elapsedMs: Long): String =
    "inv-${generation.coerceAtLeast(0).toString(36)}-${elapsedMs.coerceAtLeast(0).toString(36)}"
        .take(32)

private fun investigationRouteErrorSummary(error: String?): String = when (error) {
    "phone_unavailable" -> "Phone BLE investigation is unavailable"
    "badge_unavailable" -> "Badge BLE scanner is unavailable"
    "phone_requires_gatt" -> "Passive capture requires the badge scanner"
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
    private val badgeUsbRepository: BadgeUsbRepository,
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
    val badgeUsbState = badgeUsbRepository.state

    val privacyDetections: StateFlow<List<GlassesDetection>> = combine(
        skyObjectRepository.glassesDetections,
        _backendPrivacyDetections,
        badgeUsbRepository.state,
        _wifiAnomalies,
    ) { local, backend, badge, wifiAnomalies ->
        mergePrivacyDetections(
            local,
            backend + badge.toPrivacyDetections() + wifiAnomalies.map { it.toPrivacyDetection() }
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
        badgeUsbRepository.requestStatus()
    }

    fun startBadgeUsb() {
        syncBackendMode()
        badgeUsbRepository.start()
    }

    fun stopBadgeUsb() {
        badgeUsbRepository.stop()
    }

    fun connectBadgeUsb() {
        badgeUsbRepository.requestConnection()
    }

    fun refreshBadgeStatus() {
        badgeUsbRepository.requestStatus()
    }

    fun investigate(detection: GlassesDetection, route: BleInvestigationRoute) {
        val target = detection.investigationTarget ?: return
        startInvestigation(detection.origin, target, route)
    }

    fun investigateBadgeEntity(entity: BadgeThreatEntity, route: BleInvestigationRoute) {
        val detection = entity.toPrivacyDetection(Instant.now()) ?: return
        val target = detection.investigationTarget ?: return
        startInvestigation(detection.origin, target, route)
    }

    fun cancelInvestigation() {
        val active = activeInvestigation ?: return
        val current = _investigationResult.value
        if (current?.state in INVESTIGATION_TERMINAL_STATES) return
        when (active.request.route) {
            BleInvestigationRoute.PHONE -> viewModelScope.launch {
                bleInvestigationCoordinator.cancel()
            }
            BleInvestigationRoute.BADGE ->
                badgeUsbRepository.cancelBleInvestigation(active.request.requestId)
            BleInvestigationRoute.AUTO -> Unit
        }
    }

    fun clearInvestigation() {
        val active = activeInvestigation
        if (_investigationResult.value?.state !in INVESTIGATION_TERMINAL_STATES && active != null) {
            when (active.request.route) {
                BleInvestigationRoute.PHONE -> viewModelScope.launch {
                    bleInvestigationCoordinator.cancel()
                }
                BleInvestigationRoute.BADGE ->
                    badgeUsbRepository.cancelBleInvestigation(active.request.requestId)
                BleInvestigationRoute.AUTO -> Unit
            }
        }
        investigationGeneration++
        activeInvestigation = null
        investigationObserverJob?.cancel()
        investigationObserverJob = null
        _investigationResult.value = null
    }

    internal fun investigationRouteDecision(
        origin: PrivacyDetectionOrigin,
        target: BleInvestigationTarget,
        route: BleInvestigationRoute,
    ): BleInvestigationRouteDecision = selectInvestigationRoute(
        origin = origin,
        target = target,
        badgeAvailable = badgeInvestigationAvailability().badgeAvailable,
        requestedRoute = route,
        phoneAvailable = phoneInvestigationAvailable(),
        nowElapsedMs = elapsedRealtimeMs(),
    )

    internal fun badgeInvestigationAvailability(): BadgeInvestigationAvailability {
        return deriveBadgeInvestigationAvailability(badgeUsbRepository.state.value)
    }

    private fun startInvestigation(
        origin: PrivacyDetectionOrigin,
        target: BleInvestigationTarget,
        requestedRoute: BleInvestigationRoute,
    ) {
        val decision = investigationRouteDecision(origin, target, requestedRoute)
        investigationGeneration++
        investigationObserverJob?.cancel()
        val generation = investigationGeneration
        val requestId = boundedInvestigationRequestId(generation, elapsedRealtimeMs())
        val selectedRoute = decision.route
        if (selectedRoute == null) {
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
                summary = investigationRouteErrorSummary(decision.error),
                error = decision.error ?: "route_unavailable",
                truncated = false,
            )
            return
        }

        val request = BleInvestigationRequest(
            requestId = requestId,
            target = target,
            route = selectedRoute,
        )
        activeInvestigation = ActiveInvestigation(generation, request)
        _investigationResult.value = BleInvestigationResult(
            requestId = requestId,
            transport = if (selectedRoute == BleInvestigationRoute.PHONE) "phone" else "badge",
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
        investigationObserverJob = when (selectedRoute) {
            BleInvestigationRoute.PHONE -> observePhoneInvestigation(generation, request)
            BleInvestigationRoute.BADGE -> observeBadgeInvestigation(generation, request)
            BleInvestigationRoute.AUTO -> null
        }
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

    private fun observeBadgeInvestigation(
        generation: Long,
        request: BleInvestigationRequest,
    ) = viewModelScope.launch {
        badgeUsbRepository.investigation
            .filterNotNull()
            .takeWhile { result ->
                if (result.requestId == request.requestId) {
                    publishInvestigationGeneration(generation, request.requestId, result)
                    result.state !in INVESTIGATION_TERMINAL_STATES
                } else {
                    true
                }
            }
            .collect { }
    }.also {
        badgeUsbRepository.investigateBle(request)
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

    fun setBadgeMode(mode: String) {
        badgeUsbRepository.setMode(mode)
    }

    fun rebootBadge() {
        badgeUsbRepository.rebootBadge()
    }

    fun badgeBootloader() {
        badgeUsbRepository.enterBootloader()
    }

    fun relayBadgeScannerFirmware(uart: String) {
        badgeUsbRepository.relayScannerFirmware(uart)
    }

    fun flashBadgeScannerFirmware(uart: String, name: String, firmware: ByteArray) {
        badgeUsbRepository.flashScannerFirmware(
            uart = uart,
            name = name,
            version = "android-upload",
            firmware = firmware
        )
    }

    fun enablePhonePrivacyScanning() {
        if (skyObjectRepository.prefs.backendOnlyMode) {
            skyObjectRepository.prefs.backendOnlyMode = false
            skyObjectRepository.restartDetectionSources()
        }
        _backendOnlyMode.value = false
    }

    fun badgeNextFocus() {
        badgeUsbRepository.displayNav("next")
    }

    fun badgeToggleDetail() {
        badgeUsbRepository.displayNav("detail")
    }

    fun badgeBackFromDetail() {
        badgeUsbRepository.displayNav("back")
    }

    fun applyBadgeDisplayPolicy(policy: BadgeDisplayPolicy) {
        badgeUsbRepository.applyDisplayPolicy(policy)
    }

    fun resetBadgeDisplayPolicy() {
        badgeUsbRepository.resetDisplayPolicy()
    }

    private fun currentSkyAlertSettings(): SkyAlertSettings =
        SkyAlertSettings(
            droneAlertsEnabled = skyObjectRepository.prefs.droneAlertsEnabled,
            helicopterAlertsEnabled = skyObjectRepository.prefs.helicopterAlertsEnabled,
            militaryAlertsEnabled = skyObjectRepository.prefs.militaryAlertsEnabled,
            policeAlertsEnabled = skyObjectRepository.prefs.policeAlertsEnabled
        )

    fun applyBadgeTheme(theme: BadgeTheme) {
        badgeUsbRepository.applyBadgeTheme(theme)
    }

    fun resetBadgeTheme() {
        badgeUsbRepository.resetBadgeTheme()
    }

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

internal fun BadgeUsbState.toPrivacyDetections(now: Instant = Instant.now()): List<GlassesDetection> {
    val status = controlStatus ?: return emptyList()
    return status.entities.mapNotNull { it.toPrivacyDetection(now) }
}

internal fun BadgeThreatEntity.toPrivacyDetection(now: Instant): GlassesDetection? {
    if (stale) return null
    val category = categoryForBadgeEntity()
    val title = badgeDeviceType()
    val stableId = bssid.ifBlank {
        displayId.ifBlank { operatorId ?: detail.ifBlank { label } }
    }
    val displayName = detail.ifBlank { displayId.ifBlank { operatorId.orEmpty() } }
    val key = "badge:${threatClass.ifBlank { "threat" }}:" +
        "${code.ifBlank { this@toPrivacyDetection.category }}:${stableId.ifBlank { title }}"
    val rssiNow = when {
        rssi != 0 -> rssi
        bestRssi != 0 -> bestRssi
        else -> -100
    }
    val detailMap = buildMap {
        put("source", "usb_badge")
        if (threatClass.isNotBlank()) put("class", threatClass)
        if (this@toPrivacyDetection.category.isNotBlank()) {
            put("category", this@toPrivacyDetection.category)
        }
        if (code.isNotBlank()) put("code", code)
        if (displayId.isNotBlank()) put("display_id", displayId)
        if (ssid.isNotBlank()) put("ssid", ssid)
        if (bssid.isNotBlank()) put("bssid", bssid)
        if (authMode >= 0) put("auth_m", authMode.toString())
        if (freqMhz > 0) put("freq_mhz", freqMhz.toString())
        if (detail.isNotBlank()) put("detail", detail)
        if (evidence.isNotBlank()) put("evidence", evidence)
        if (source.isNotBlank()) put("badge_source", source)
        if (sourceId != 0) put("badge_source_id", sourceId.toString())
        if (confidencePct > 0) put("confidence", "$confidencePct%")
        put("score", score.toString())
        put("age_s", ageSeconds.toString())
        put("events", events.toString())
        if (seenCount > 0) put("seen", seenCount.toString())
        if (groupCount > 1) put("group", groupCount.toString())
        operatorId?.let { put("operator_id", it) }
    }
    val isPairingSpam = listOf(code, this@toPrivacyDetection.category, label, detail).any { value ->
        value.trim().replace('-', '_').replace(' ', '_').uppercase() in setOf("PAIRING_SPAM", "BLE_SPAM")
    }
    val investigationTarget = when {
        isPairingSpam -> BleInvestigationTarget(
            mode = BleInvestigationMode.PASSIVE_CAPTURE,
            mac = null,
            entityKey = key,
            observedAtElapsedMs = elapsedRealtimeMs(),
            origin = PrivacyDetectionOrigin.BADGE,
        )
        threatClass.equals("ble", ignoreCase = true) && bssid.isNotBlank() -> BleInvestigationTarget(
            mode = BleInvestigationMode.GATT,
            mac = bssid,
            entityKey = key,
            observedAtElapsedMs = elapsedRealtimeMs(),
            origin = PrivacyDetectionOrigin.BADGE,
        )
        else -> null
    }
    return GlassesDetection(
        mac = key,
        deviceName = displayName.takeIf { it.isNotBlank() },
        deviceType = title,
        manufacturer = "FoF Badge",
        hasCamera = category in setOf(
            PrivacyCategory.HIDDEN_CAMERA,
            PrivacyCategory.SURVEILLANCE_CAMERA,
            PrivacyCategory.ALPR_CAMERA,
            PrivacyCategory.BODY_CAMERA,
            PrivacyCategory.VEHICLE_CAMERA,
        ),
        rssi = rssiNow,
        confidence = (score / 100f).coerceIn(0f, 1f),
        matchReason = "badge:${threatClass.ifBlank { this.category.ifBlank { "privacy" } }}",
        firstSeen = now.minusSeconds(ageSeconds.coerceAtLeast(0).toLong()),
        lastSeen = now.minusSeconds(lastSeenSeconds.coerceAtLeast(0).toLong()),
        details = detailMap,
        category = category,
        fingerprintKey = key,
        seenMacs = setOf(key),
        origin = PrivacyDetectionOrigin.BADGE,
        investigationTarget = investigationTarget,
    )
}

private fun BadgeThreatEntity.categoryForBadgeEntity(): PrivacyCategory {
    val cls = threatClass.lowercase()
    val cat = category.uppercase()
    val catCode = code.uppercase()
    return when {
        cls == "meta" || cat == "GLASS" || catCode == "GLS" -> PrivacyCategory.SMART_GLASSES
        cls == "tracker" || cat == "TAG" || catCode == "TAG" -> PrivacyCategory.BLE_TRACKER
        cls == "wifi_anomaly" || cat == "WIFI" || catCode == "WIFI" -> PrivacyCategory.ATTACK_TOOL
        cls == "drone" || cat == "DRONE" || cat == "SSID" ||
            catCode == "DRN" || catCode == "SSID" -> PrivacyCategory.DRONE_CONTROLLER
        cat == "FLOCK" || catCode == "FLK" -> PrivacyCategory.ALPR_CAMERA
        cat == "SKIM" || catCode == "SKIM" -> PrivacyCategory.ATTACK_TOOL
        cat == "CAMERA" || catCode == "CAM" -> PrivacyCategory.SURVEILLANCE_CAMERA
        cat == "BEACON" || catCode == "BCN" -> PrivacyCategory.VENUE_BEACON
        cat == "EVENT" || catCode == "EVT" -> PrivacyCategory.EVENT_BADGE
        cat == "LOCK" || catCode == "LOCK" -> PrivacyCategory.MOBILE_KEY_LOCK
        cat == "HID" || catCode == "HID" -> PrivacyCategory.BLE_HID
        cat == "LISTEN" || catCode == "LIS" -> PrivacyCategory.REMOTE_LISTENING
        cat == "AUDIO" || catCode == "AUD" -> PrivacyCategory.AURACAST
        else -> PrivacyCategory.INFORMATIONAL
    }
}

private fun BadgeThreatEntity.badgeDeviceType(): String {
    val cat = category.uppercase()
    val catCode = code.uppercase()
    return when {
        threatClass.equals("drone", ignoreCase = true) &&
            (cat == "SSID" || catCode == "SSID") -> "Drone SSID"
        threatClass.equals("drone", ignoreCase = true) -> "Remote ID Drone"
        cat == "FLOCK" || catCode == "FLK" -> "Flock / ALPR Camera"
        cat == "SKIM" || catCode == "SKIM" -> "Skimmer"
        cat == "CAMERA" || catCode == "CAM" -> "Camera Near"
        cat == "BEACON" || catCode == "BCN" -> "Venue Beacon"
        cat == "EVENT" || catCode == "EVT" -> "Event Badge"
        cat == "LOCK" || catCode == "LOCK" -> "Mobile Key Lock"
        cat == "HID" || catCode == "HID" -> "BLE Input Device"
        cat == "LISTEN" || catCode == "LIS" -> "Possible Listening"
        cat == "AUDIO" || catCode == "AUD" -> "Auracast / LE Audio"
        label.isNotBlank() -> label
        else -> "Badge Privacy Signal"
    }
}

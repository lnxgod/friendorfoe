package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.remote.LivePrivacyDeviceDto
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.WifiAnomalyDetector
import com.friendorfoe.presentation.collectBackendWhileEnabled
import com.friendorfoe.presentation.alerts.SkyAlertCandidate
import com.friendorfoe.presentation.alerts.SkyAlertPolicy
import com.friendorfoe.presentation.alerts.SkyAlertSettings
import com.friendorfoe.sensor.SensorFusionEngine
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import javax.inject.Inject

internal sealed interface PrivacyBackendPollState {
    data object Disabled : PrivacyBackendPollState
    data object Connected : PrivacyBackendPollState
    data class Failed(val message: String?) : PrivacyBackendPollState
}

internal class PrivacyBackendIntegrationState(
    val localDetections: StateFlow<List<GlassesDetection>>,
    val badgeState: StateFlow<BadgeUsbState>,
) {
    val wifiAnomalies = MutableStateFlow<List<WifiAnomalyDetector.WifiAnomaly>>(emptyList())
    val backendPrivacyDetections = MutableStateFlow<List<GlassesDetection>>(emptyList())
    val backendPollState =
        MutableStateFlow<PrivacyBackendPollState>(PrivacyBackendPollState.Disabled)
}

internal suspend fun collectPrivacyBackend(
    settings: Flow<DetectionSettings>,
    intervalMs: Long,
    state: PrivacyBackendIntegrationState,
    fetchDetections: suspend () -> List<GlassesDetection>,
) {
    collectBackendWhileEnabled(
        settings = settings,
        intervalMs = intervalMs,
        clear = {
            state.backendPrivacyDetections.value = emptyList()
            state.backendPollState.value = PrivacyBackendPollState.Disabled
        },
        fetch = fetchDetections,
        publish = { detections ->
            state.backendPrivacyDetections.value = detections
            state.backendPollState.value = PrivacyBackendPollState.Connected
        },
        onFailure = { failure ->
            state.backendPollState.value = PrivacyBackendPollState.Failed(failure.message)
        },
    )
}

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    val sensorFusionEngine: SensorFusionEngine,
    private val wifiAnomalyDetector: WifiAnomalyDetector,
    private val detectionPrefs: DetectionPrefs,
    private val sensorMapApiService: SensorMapApiService,
    private val badgeUsbRepository: BadgeUsbRepository,
    private val privacyAlertNotifier: PrivacyAlertNotifier,
) : ViewModel() {

    private val backendIntegrationState = PrivacyBackendIntegrationState(
        localDetections = skyObjectRepository.glassesDetections,
        badgeState = badgeUsbRepository.legacyState,
    )

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
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Throwable) { /* sleep and retry */ }
                delay(15_000)
            }
        }
    }

    private val _wifiAnomalies = backendIntegrationState.wifiAnomalies
    val wifiAnomalies: StateFlow<List<WifiAnomalyDetector.WifiAnomaly>> = _wifiAnomalies.asStateFlow()
    private val _backendPrivacyDetections = backendIntegrationState.backendPrivacyDetections
    private val _backendPollState = backendIntegrationState.backendPollState
    internal val backendPollState: StateFlow<PrivacyBackendPollState> = _backendPollState.asStateFlow()

    private val backendPollJob = viewModelScope.launch {
        collectPrivacyBackend(
            settings = detectionPrefs.settings,
            intervalMs = 5_000L,
            state = backendIntegrationState,
            fetchDetections = {
                sensorMapApiService.getLivePrivacyDevices().devices.mapNotNull {
                    it.toGlassesDetection()
                }.map(PrivacyFindingNormalizer::normalize)
            },
        )
    }
    val badgeUsbState = backendIntegrationState.badgeState

    val privacyDetections: StateFlow<List<GlassesDetection>> = combine(
        backendIntegrationState.localDetections,
        _backendPrivacyDetections,
        backendIntegrationState.badgeState,
        _wifiAnomalies,
    ) { local, backend, badge, wifiAnomalies ->
        val normalizedLocal = local.map(PrivacyFindingNormalizer::normalize)
        val normalizedRemote = (
            backend + badge.toPrivacyDetections() + wifiAnomalies.map { it.toPrivacyDetection() }
        ).map(PrivacyFindingNormalizer::normalize)
        mergePrivacyDetections(
            normalizedLocal,
            normalizedRemote,
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

    fun connectBadgeUsb() {
        badgeUsbRepository.requestConnection()
    }

    fun refreshBadgeStatus() {
        badgeUsbRepository.requestStatus()
    }

    fun setBadgeMode(mode: BadgeNetworkMode) {
        badgeUsbRepository.setMode(mode)
    }

    fun rebootBadge() {
        badgeUsbRepository.rebootBadge()
    }

    fun badgeBootloader() {
        badgeUsbRepository.enterBootloader()
    }

    fun enablePhonePrivacyScanning() {
        if (skyObjectRepository.prefs.backendOnlyMode) {
            skyObjectRepository.prefs.backendOnlyMode = false
            skyObjectRepository.restartDetectionSources()
        }
        _backendOnlyMode.value = false
    }

    fun badgeNextFocus() {
        badgeUsbRepository.displayNav(BadgeDisplayAction.NEXT)
    }

    fun badgeToggleDetail() {
        badgeUsbRepository.displayNav(BadgeDisplayAction.DETAIL)
    }

    fun badgeBackFromDetail() {
        badgeUsbRepository.displayNav(BadgeDisplayAction.BACK)
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
        return PrivacyFindingNormalizer.normalize(GlassesDetection(
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
        ))
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
            seenMacs = bssids.toSet().ifEmpty { setOf(primaryBssid) }
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

internal fun BadgeUsbState.toPrivacyDetections(): List<GlassesDetection> {
    val status = controlStatus ?: return emptyList()
    return status.entities.mapNotNull { it.toPrivacyDetection(status.receivedAtWallClock) }
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
    val appleListeningEvidence = appleListeningEvidence()
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
        appleListeningEvidence?.let { put("apple_badge_evidence", it.explicitAppleField) }
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
    return PrivacyFindingNormalizer.normalize(GlassesDetection(
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
        seenMacs = setOf(key)
    ))
}

private data class AppleListeningEvidence(
    val explicitAppleField: String,
)

private fun BadgeThreatEntity.appleListeningEvidence(): AppleListeningEvidence? {
    val fields = listOf(label, detail, evidence, category, code)
    val appleField = fields.firstOrNull { APPLE_BADGE_EVIDENCE.containsMatchIn(it) }
        ?: return null
    val hasListeningWording = fields.any { LISTENING_BADGE_EVIDENCE.containsMatchIn(it) } ||
        category.equals("LISTEN", ignoreCase = true) ||
        code.equals("LIS", ignoreCase = true)
    if (!hasListeningWording) return null
    return AppleListeningEvidence(explicitAppleField = appleField)
}

private val APPLE_BADGE_EVIDENCE = Regex(
    pattern = "(?<![A-Za-z0-9])(?:Apple|AirPods?)(?![A-Za-z0-9])",
    option = RegexOption.IGNORE_CASE,
)

private val LISTENING_BADGE_EVIDENCE = Regex(
    pattern = "(?<![A-Za-z0-9])(?:listen(?:ing)?|eavesdrop(?:ping)?)(?![A-Za-z0-9])",
    option = RegexOption.IGNORE_CASE,
)

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

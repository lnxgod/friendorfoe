package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
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
import com.friendorfoe.sensor.SensorFusionEngine
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import javax.inject.Inject

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    val sensorFusionEngine: SensorFusionEngine,
    private val wifiAnomalyDetector: WifiAnomalyDetector,
    private val sensorMapApiService: SensorMapApiService,
    private val badgeUsbRepository: BadgeUsbRepository,
) : ViewModel() {

    init {
        skyObjectRepository.ensureStarted(0.0, 0.0)
        // Poll the WifiAnomalyDetector every 15 s. Surfaces Pwnagotchi beacons
        // (attack tool), evil-twin APs, karma attacks — all computed off the
        // existing WiFi scan-results cache that other scanners already populate.
        viewModelScope.launch {
            while (true) {
                try {
                    val anomalies = wifiAnomalyDetector.analyze()
                    if (anomalies.isNotEmpty()) {
                        _wifiAnomalies.value = anomalies
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
    ) { local, backend, badge ->
        mergePrivacyDetections(local, backend + badge.toPrivacyDetections())
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
        skyObjectRepository.refreshPrivacyDetections()
        badgeUsbRepository.requestStatus()
    }

    fun startBadgeUsb() {
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

    fun badgeNextFocus() {
        badgeUsbRepository.displayNav("next")
    }

    fun badgeToggleDetail() {
        badgeUsbRepository.displayNav("detail")
    }

    fun badgeBackFromDetail() {
        badgeUsbRepository.displayNav("back")
    }

    fun startDirectionScan(mac: String) {
        bleTracker.startDirectionScan(mac)
    }

    fun finishDirectionScan(): BleTracker.DirectionResult? {
        return bleTracker.finishDirectionScan()
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

    private fun categoryForPrivacyKind(kind: String?, fallbackType: String): PrivacyCategory = when (kind) {
        "VENUE_BEACON" -> PrivacyCategory.VENUE_BEACON
        "EVENT_BADGE" -> PrivacyCategory.EVENT_BADGE
        "MOBILE_KEY_LOCK" -> PrivacyCategory.MOBILE_KEY_LOCK
        "BLE_HID" -> PrivacyCategory.BLE_HID
        "AURACAST" -> PrivacyCategory.AURACAST
        "APPLE_CONTINUITY" -> PrivacyCategory.APPLE_CONTINUITY
        "FLOCK_ALPR" -> PrivacyCategory.ALPR_CAMERA
        "CAMERA_NEAR" -> PrivacyCategory.SURVEILLANCE_CAMERA
        "SKIMMER" -> PrivacyCategory.ATTACK_TOOL
        "TRACKER_NEAR" -> PrivacyCategory.BLE_TRACKER
        "META_GLASSES" -> PrivacyCategory.SMART_GLASSES
        else -> com.friendorfoe.detection.GlassesDetector.categorizeDeviceType(fallbackType)
    }

    private fun BadgeUsbState.toPrivacyDetections(): List<GlassesDetection> {
        val status = controlStatus ?: return emptyList()
        val now = Instant.now()
        return status.entities.mapNotNull { it.toPrivacyDetection(now) }
    }

    private fun BadgeThreatEntity.toPrivacyDetection(now: Instant): GlassesDetection? {
        if (stale) return null
        val category = categoryForBadgeEntity()
        val title = badgeDeviceType()
        val stableId = displayId.ifBlank { operatorId ?: detail.ifBlank { label } }
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
            seenMacs = setOf(key)
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
            cat == "AUDIO" || catCode == "AUD" -> "Auracast / LE Audio"
            label.isNotBlank() -> label
            else -> "Badge Privacy Signal"
        }
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

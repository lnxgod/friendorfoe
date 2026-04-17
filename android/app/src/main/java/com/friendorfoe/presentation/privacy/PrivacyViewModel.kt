package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
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
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    val sensorFusionEngine: SensorFusionEngine,
    private val wifiAnomalyDetector: WifiAnomalyDetector
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
    }

    private val _wifiAnomalies = MutableStateFlow<List<WifiAnomalyDetector.WifiAnomaly>>(emptyList())
    val wifiAnomalies: StateFlow<List<WifiAnomalyDetector.WifiAnomaly>> = _wifiAnomalies.asStateFlow()

    /** All privacy detections, grouped by category */
    val categorizedDetections: StateFlow<Map<PrivacyCategory, List<GlassesDetection>>> =
        skyObjectRepository.glassesDetections.map { detections ->
            detections.groupBy { it.category }
                .toSortedMap(compareByDescending<PrivacyCategory> { it.threatLevel }.thenBy { it.name })
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5000),
            initialValue = emptyMap()
        )

    /** Total device count */
    val totalCount: StateFlow<Int> = skyObjectRepository.glassesDetections.map { it.size }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    /** High threat count (threat level >= 2) */
    val threatCount: StateFlow<Int> = skyObjectRepository.glassesDetections.map { detections ->
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
        return skyObjectRepository.glassesDetections.value
            .find { it.mac == mac }?.rssi
    }

    fun ignoreDevice(mac: String) {
        skyObjectRepository.ignorePrivacyDevice(mac)
    }

    /** Clear all detections and rescan fresh */
    fun refreshDetections() {
        skyObjectRepository.refreshPrivacyDetections()
    }

    fun startDirectionScan(mac: String) {
        bleTracker.startDirectionScan(mac)
    }

    fun finishDirectionScan(): BleTracker.DirectionResult? {
        return bleTracker.finishDirectionScan()
    }
}

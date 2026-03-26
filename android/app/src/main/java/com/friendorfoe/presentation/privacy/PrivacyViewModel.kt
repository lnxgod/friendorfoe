package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository
) : ViewModel() {

    init {
        // Ensure privacy BLE scanning starts even if user navigates directly to Privacy tab
        // (ensureStarted is idempotent — safe to call if already running)
        skyObjectRepository.ensureStarted(0.0, 0.0)
    }

    /** All privacy detections, grouped by category */
    val categorizedDetections: StateFlow<Map<PrivacyCategory, List<GlassesDetection>>> =
        skyObjectRepository.glassesDetections.map { detections ->
            detections.groupBy { it.category }
                // thenBy name breaks ties — TreeMap treats comparator-equal keys as identical
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

    /** BLE tracker for direction finding */
    val bleTracker: BleTracker = skyObjectRepository.bleTracker

    fun ignoreDevice(mac: String) {
        skyObjectRepository.ignorePrivacyDevice(mac)
    }

    fun startDirectionScan(mac: String) {
        bleTracker.startDirectionScan(mac)
    }
}

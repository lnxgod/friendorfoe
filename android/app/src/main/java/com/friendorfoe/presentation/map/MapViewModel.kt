package com.friendorfoe.presentation.map

import android.annotation.SuppressLint
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.DroneMapDto
import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.data.remote.SensorDto
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.usecase.FilterEngine
import com.friendorfoe.presentation.collectBackendWhileEnabled
import com.friendorfoe.sensor.SensorFusionEngine
import com.friendorfoe.sensor.VisualFocusRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject

internal data class MapBackendSnapshot(
    val map: DroneMapDto,
    val activeDroneAlertCount: Int?,
)

internal suspend fun collectMapBackend(
    settings: Flow<DetectionSettings>,
    intervalMs: Long,
    sensorDrones: MutableStateFlow<List<LocatedDroneDto>>,
    remoteSensors: MutableStateFlow<List<SensorDto>>,
    sensorMapOnline: MutableStateFlow<Boolean>,
    droneAlertCount: MutableStateFlow<Int>,
    fetchSnapshot: suspend () -> MapBackendSnapshot,
) {
    collectBackendWhileEnabled(
        settings = settings,
        intervalMs = intervalMs,
        clear = {
            sensorDrones.value = emptyList()
            remoteSensors.value = emptyList()
            sensorMapOnline.value = false
            droneAlertCount.value = 0
        },
        fetch = fetchSnapshot,
        publish = { response ->
            sensorDrones.value = response.map.drones.filter { drone ->
                drone.classification in MAP_DRONE_CLASSIFICATIONS ||
                    drone.droneId.startsWith("rid_") ||
                    drone.droneId.startsWith("probe_") ||
                    drone.droneId.startsWith("FOF-Drone-") ||
                    drone.droneId.startsWith("FoF-Drone-") ||
                    drone.positionSource == "gps"
            }
            remoteSensors.value = response.map.sensors
            sensorMapOnline.value = true
            response.activeDroneAlertCount?.let { droneAlertCount.value = it }
        },
        onFailure = { sensorMapOnline.value = false },
    )
}

private val MAP_DRONE_CLASSIFICATIONS = setOf(
    "confirmed_drone", "likely_drone", "test_drone", "wifi_device",
)

@HiltViewModel
class MapViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    private val aircraftRepository: AircraftRepository,
    private val locationManager: LocationManager,
    private val sensorFusionEngine: SensorFusionEngine,
    private val visualFocusRepository: VisualFocusRepository,
    private val detectionPrefs: DetectionPrefs,
    private val sensorMapApiService: SensorMapApiService
) : ViewModel() {

    companion object {
        private const val TAG = "MapViewModel"
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
        private const val REMOTE_SEARCH_RADIUS_NM = 250
    }

    /** Remote search results — aircraft found at a tapped location */
    private val _remoteSearchResults = MutableStateFlow<List<SkyObject>>(emptyList())
    val remoteSearchResults: StateFlow<List<SkyObject>> = _remoteSearchResults.asStateFlow()

    private val _remoteSearchCenter = MutableStateFlow<Position?>(null)
    val remoteSearchCenter: StateFlow<Position?> = _remoteSearchCenter.asStateFlow()

    private val _remoteSearching = MutableStateFlow(false)
    val remoteSearching: StateFlow<Boolean> = _remoteSearching.asStateFlow()

    /**
     * Search for aircraft at a remote location (long-press on map).
     * Uses 250 NM radius to cover ocean routes, shuttle chase planes, etc.
     */
    fun searchRemoteArea(lat: Double, lon: Double) {
        _remoteSearchCenter.value = Position(lat, lon, 0.0)
        _remoteSearching.value = true
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val result = aircraftRepository.getNearbyAircraft(lat, lon, REMOTE_SEARCH_RADIUS_NM)
                result.onSuccess { nearby ->
                    _remoteSearchResults.value = nearby.aircraft
                    Log.i(TAG, "Remote search at ($lat, $lon): ${nearby.aircraft.size} aircraft in ${REMOTE_SEARCH_RADIUS_NM}NM")
                }.onFailure {
                    Log.w(TAG, "Remote search failed: ${it.message}")
                    _remoteSearchResults.value = emptyList()
                }
            } catch (e: Exception) {
                Log.w(TAG, "Remote search error: ${e.message}")
                _remoteSearchResults.value = emptyList()
            }
            _remoteSearching.value = false
        }
    }

    fun clearRemoteSearch() {
        _remoteSearchResults.value = emptyList()
        _remoteSearchCenter.value = null
    }

    private val _filterState = MutableStateFlow(FilterState())
    val filterState: StateFlow<FilterState> = _filterState.asStateFlow()

    fun updateFilter(filterState: FilterState) {
        _filterState.value = filterState
    }

    val skyObjects: StateFlow<List<SkyObject>> = combine(
        skyObjectRepository.skyObjects,
        _filterState
    ) { objects, filter ->
        FilterEngine.applyFilters(objects, filter)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    private val visualFocusClock = kotlinx.coroutines.flow.flow {
        while (true) {
            emit(System.currentTimeMillis())
            delay(1000L)
        }
    }

    val activeVisualFocusIds: StateFlow<Set<String>> = combine(
        visualFocusRepository.entries,
        visualFocusClock
    ) { entries, nowMs ->
        entries.filterValues { nowMs - it.lastSeenMs <= VisualFocusRepository.DEFAULT_TTL_MS }.keys
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptySet()
    )

    private val _userPosition = MutableStateFlow(
        Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    )
    val userPosition: StateFlow<Position> = _userPosition.asStateFlow()

    private val _selectedObjectId = MutableStateFlow<String?>(null)
    val selectedObjectId: StateFlow<String?> = _selectedObjectId.asStateFlow()

    private val _followCompass = MutableStateFlow(false)
    val followCompass: StateFlow<Boolean> = _followCompass.asStateFlow()

    /** Current compass heading from sensor fusion engine. */
    val compassHeading: StateFlow<Float> = sensorFusionEngine.orientation
        .map { it.azimuthDegrees }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0f)

    fun selectObject(objectId: String?) {
        _selectedObjectId.value = objectId
    }

    fun toggleFollowCompass() {
        val newValue = !_followCompass.value
        _followCompass.value = newValue
        if (newValue) {
            sensorFusionEngine.start()
        }
    }

    // --- Sensor map (ESP32 backend triangulation) ---

    private val _sensorDrones = MutableStateFlow<List<LocatedDroneDto>>(emptyList())
    /** Drones detected by remote ESP32 sensors — only confirmed/likely drones, not trackers or unknown BLE. */
    val sensorDrones: StateFlow<List<LocatedDroneDto>> = _sensorDrones.asStateFlow()

    private val _remoteSensors = MutableStateFlow<List<SensorDto>>(emptyList())
    /** Active ESP32 sensor nodes with their GPS positions. */
    val remoteSensors: StateFlow<List<SensorDto>> = _remoteSensors.asStateFlow()

    private val _sensorMapOnline = MutableStateFlow(false)
    /** True when the backend sensor map API is reachable. */
    val sensorMapOnline: StateFlow<Boolean> = _sensorMapOnline.asStateFlow()

    private val _droneAlertCount = MutableStateFlow(0)
    /** Number of active drone alerts from the backend. */
    val droneAlertCount: StateFlow<Int> = _droneAlertCount.asStateFlow()

    private val sensorPollingRequested = MutableStateFlow(false)
    private val sensorPollingSettings = combine(
        detectionPrefs.settings,
        sensorPollingRequested,
    ) { settings, requested ->
        settings.copy(sensorBackendEnabled = settings.sensorBackendEnabled && requested)
    }

    private val sensorPollJob = viewModelScope.launch(Dispatchers.IO) {
        collectMapBackend(
            settings = sensorPollingSettings,
            intervalMs = 5_000L,
            sensorDrones = _sensorDrones,
            remoteSensors = _remoteSensors,
            sensorMapOnline = _sensorMapOnline,
            droneAlertCount = _droneAlertCount,
            fetchSnapshot = {
                MapBackendSnapshot(
                    map = sensorMapApiService.getDroneMap(),
                    activeDroneAlertCount = try {
                        sensorMapApiService.getDroneAlerts().activeDroneCount
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) {
                        null
                    },
                )
            },
        )
    }

    /** Start polling the backend sensor map endpoint every 5 seconds. */
    fun startSensorMapPolling() {
        sensorPollingRequested.value = true
    }

    fun stopSensorMapPolling() {
        sensorPollingRequested.value = false
    }

    private var locationStarted = java.util.concurrent.atomic.AtomicBoolean(false)
    private var scanningStarted = false

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            _userPosition.value = Position(
                latitude = location.latitude,
                longitude = location.longitude,
                altitudeMeters = location.altitude
            )

            // Ensure scanning is running even if AR was never visited
            if (!scanningStarted) {
                skyObjectRepository.ensureStarted(location.latitude, location.longitude)
                scanningStarted = true
            } else {
                skyObjectRepository.updatePosition(location.latitude, location.longitude)
            }
        }

        @Deprecated("Deprecated in API level 29")
        override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) {}
        override fun onProviderEnabled(provider: String) {}
        override fun onProviderDisabled(provider: String) {}
    }

    @SuppressLint("MissingPermission")
    fun startLocationUpdates() {
        if (!locationStarted.compareAndSet(false, true)) return

        try {
            if (locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                locationManager.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER,
                    LOCATION_UPDATE_INTERVAL_MS,
                    LOCATION_UPDATE_DISTANCE_M,
                    locationListener
                )
            }
            if (locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                locationManager.requestLocationUpdates(
                    LocationManager.NETWORK_PROVIDER,
                    LOCATION_UPDATE_INTERVAL_MS,
                    LOCATION_UPDATE_DISTANCE_M,
                    locationListener
                )
            }

            val lastKnown = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: locationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            if (lastKnown != null) {
                _userPosition.value = Position(
                    latitude = lastKnown.latitude,
                    longitude = lastKnown.longitude,
                    altitudeMeters = lastKnown.altitude
                )
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "Location permission not granted", e)
        }
    }

    fun stopLocationUpdates() {
        if (!locationStarted.compareAndSet(true, false)) return
        try {
            locationManager.removeUpdates(locationListener)
        } catch (e: SecurityException) {
            Log.w(TAG, "Could not remove location updates", e)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopLocationUpdates()
        stopSensorMapPolling()
        if (_followCompass.value) {
            sensorFusionEngine.stop()
        }
    }
}

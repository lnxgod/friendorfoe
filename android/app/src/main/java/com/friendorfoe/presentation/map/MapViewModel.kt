package com.friendorfoe.presentation.map

import android.annotation.SuppressLint
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.SystemClock
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
import com.friendorfoe.data.repository.validatedLocationAccuracyMeters
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.usecase.FilterEngine
import com.friendorfoe.presentation.collectBackendWhileEnabled
import com.friendorfoe.sensor.SensorFusionEngine
import com.friendorfoe.sensor.SensorFusionLease
import com.friendorfoe.sensor.VisualFocusRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject

private const val MAP_FRAME_INTERVAL_MS = 250L
private const val MAX_LAST_KNOWN_LOCATION_AGE_NANOS = 30_000_000_000L

internal data class MapLocationFix(
    val position: Position,
    val elapsedRealtimeNanos: Long,
)

internal fun shouldSeedMapFromLastKnownLocation(
    locationElapsedRealtimeNanos: Long,
    nowElapsedRealtimeNanos: Long,
): Boolean {
    val ageNanos = nowElapsedRealtimeNanos - locationElapsedRealtimeNanos
    return ageNanos in 0..MAX_LAST_KNOWN_LOCATION_AGE_NANOS
}

internal fun usableMapPosition(
    fix: MapLocationFix?,
    nowElapsedRealtimeNanos: Long,
): Position? = fix?.position?.takeIf { position ->
    position.hasValidMapCoordinates() &&
        shouldSeedMapFromLastKnownLocation(
            locationElapsedRealtimeNanos = fix.elapsedRealtimeNanos,
            nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
        )
}

internal fun updateMapPositionForInstance(
    acceptedPosition: Position?,
    candidate: MapLocationFix?,
    nowElapsedRealtimeNanos: Long,
): Position? = usableMapPosition(candidate, nowElapsedRealtimeNanos)
    ?: acceptedPosition?.takeIf(Position::hasValidMapCoordinates)

internal fun selectFreshestMapLastKnownLocationFix(
    gps: MapLocationFix?,
    network: MapLocationFix?,
    nowElapsedRealtimeNanos: Long,
): MapLocationFix? = listOfNotNull(gps, network)
    .filter { candidate ->
        usableMapPosition(candidate, nowElapsedRealtimeNanos) != null
    }
    .maxByOrNull(MapLocationFix::elapsedRealtimeNanos)

internal fun selectFreshestMapLastKnownLocation(
    gps: MapLocationFix?,
    network: MapLocationFix?,
    nowElapsedRealtimeNanos: Long,
): Position? = selectFreshestMapLastKnownLocationFix(
    gps = gps,
    network = network,
    nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
)?.position

internal fun mapFrameClock(
    epochNowMs: () -> Long = System::currentTimeMillis,
    monotonicNowMs: () -> Long = SystemClock::elapsedRealtime,
    waitForNextFrame: suspend (Long) -> Unit = { delay(it) },
): Flow<Long> = flow {
    var nextFrameAtMs = monotonicNowMs()
    while (currentCoroutineContext().isActive) {
        emit(epochNowMs())
        nextFrameAtMs += MAP_FRAME_INTERVAL_MS
        waitForNextFrame((nextFrameAtMs - monotonicNowMs()).coerceAtLeast(0L))
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
internal fun stabilizedMapHeadingFlow(
    followCompass: Flow<Boolean>,
    compassHeading: Flow<Float>,
    monotonicNowMs: () -> Long = SystemClock::elapsedRealtime,
): Flow<Float> {
    val stabilizer = MapHeadingStabilizer()
    return followCompass.flatMapLatest { enabled ->
        if (!enabled) {
            flow {
                stabilizer.reset()
                emit(0f)
            }
        } else {
            compassHeading.mapNotNull { heading ->
                stabilizer.update(heading, monotonicNowMs())
            }
        }
    }
}

internal data class MapBackendSnapshot(
    val map: DroneMapDto,
    val activeDroneAlertCount: Int?,
)

internal class MapBackendIntegrationState(
    val localObjects: StateFlow<List<SkyObject>>,
) {
    val selectedObjectId = MutableStateFlow<String?>(null)
    val sensorDrones = MutableStateFlow<List<LocatedDroneDto>>(emptyList())
    val remoteSensors = MutableStateFlow<List<SensorDto>>(emptyList())
    val sensorMapOnline = MutableStateFlow(false)
    val droneAlertCount = MutableStateFlow(0)
    val pollingRequested = MutableStateFlow(false)

    fun startPolling() {
        pollingRequested.value = true
    }

    fun stopPolling() {
        pollingRequested.value = false
    }
}

internal suspend fun collectMapBackend(
    settings: Flow<DetectionSettings>,
    intervalMs: Long,
    state: MapBackendIntegrationState,
    fetchSnapshot: suspend () -> MapBackendSnapshot,
) {
    collectBackendWhileEnabled(
        settings = settings,
        intervalMs = intervalMs,
        clear = {
            state.sensorDrones.value = emptyList()
            state.remoteSensors.value = emptyList()
            state.sensorMapOnline.value = false
            state.droneAlertCount.value = 0
        },
        fetch = fetchSnapshot,
        publish = { response ->
            state.sensorDrones.value = response.map.drones.filter { drone ->
                drone.classification in MAP_DRONE_CLASSIFICATIONS ||
                    drone.droneId.startsWith("rid_") ||
                    drone.droneId.startsWith("probe_") ||
                    drone.droneId.startsWith("FOF-Drone-") ||
                    drone.droneId.startsWith("FoF-Drone-") ||
                    drone.positionSource == "gps"
            }
            state.remoteSensors.value = response.map.sensors
            state.sensorMapOnline.value = true
            response.activeDroneAlertCount?.let { state.droneAlertCount.value = it }
        },
        onFailure = { state.sensorMapOnline.value = false },
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

    private val backendIntegrationState =
        MapBackendIntegrationState(skyObjectRepository.skyObjects)
    private var compassSensorLease: SensorFusionLease? = null

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
        backendIntegrationState.localObjects,
        _filterState
    ) { objects, filter ->
        FilterEngine.applyFilters(objects, filter)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    private val mapTrackProjector = MapTrackProjector()

    val mapTracks: StateFlow<List<MapTrack>> = combine(
        mapFrameClock(),
        skyObjects,
    ) { nowMs, objects ->
        mapTrackProjector.project(objects, nowMs)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = emptyList(),
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

    private val _userLocationFix = MutableStateFlow<MapLocationFix?>(null)
    internal val userLocationFix: StateFlow<MapLocationFix?> = _userLocationFix.asStateFlow()

    private val _selectedObjectId = backendIntegrationState.selectedObjectId
    val selectedObjectId: StateFlow<String?> = _selectedObjectId.asStateFlow()

    private val _followCompass = MutableStateFlow(false)
    val followCompass: StateFlow<Boolean> = _followCompass.asStateFlow()

    /** Current compass heading from sensor fusion engine. */
    val compassHeading: StateFlow<Float> = sensorFusionEngine.orientation
        .map { it.azimuthDegrees }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0f)

    val stabilizedMapHeading: StateFlow<Float> = stabilizedMapHeadingFlow(
        followCompass = followCompass,
        compassHeading = compassHeading,
    ).stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = 0f,
    )

    fun selectObject(objectId: String?) {
        _selectedObjectId.value = objectId
    }

    fun toggleFollowCompass() {
        if (_followCompass.value) {
            stopFollowingCompass()
        } else {
            if (compassSensorLease == null) {
                compassSensorLease = sensorFusionEngine.acquire()
            }
            _followCompass.value = true
        }
    }

    fun stopFollowingCompass() {
        compassSensorLease?.close()
        compassSensorLease = null
        _followCompass.value = false
    }

    // --- Sensor map (ESP32 backend triangulation) ---

    private val _sensorDrones = backendIntegrationState.sensorDrones
    /** Drones detected by remote ESP32 sensors — only confirmed/likely drones, not trackers or unknown BLE. */
    val sensorDrones: StateFlow<List<LocatedDroneDto>> = _sensorDrones.asStateFlow()

    private val _remoteSensors = backendIntegrationState.remoteSensors
    /** Active ESP32 sensor nodes with their GPS positions. */
    val remoteSensors: StateFlow<List<SensorDto>> = _remoteSensors.asStateFlow()

    private val _sensorMapOnline = backendIntegrationState.sensorMapOnline
    /** True when the backend sensor map API is reachable. */
    val sensorMapOnline: StateFlow<Boolean> = _sensorMapOnline.asStateFlow()

    private val _droneAlertCount = backendIntegrationState.droneAlertCount
    /** Number of active drone alerts from the backend. */
    val droneAlertCount: StateFlow<Int> = _droneAlertCount.asStateFlow()

    private val sensorPollingRequested = backendIntegrationState.pollingRequested
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
            state = backendIntegrationState,
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
        backendIntegrationState.startPolling()
    }

    fun stopSensorMapPolling() {
        backendIntegrationState.stopPolling()
    }

    private var locationStarted = java.util.concurrent.atomic.AtomicBoolean(false)
    private var scanningStarted = false

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            val nowElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos()
            val candidate = location.toMapLocationFix()
            _userLocationFix.value = selectFreshestMapLastKnownLocationFix(
                gps = _userLocationFix.value,
                network = candidate,
                nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
            )

            // Ensure scanning is running even if AR was never visited
            val accuracyMeters = location.validatedLocationAccuracyMeters()
            if (!scanningStarted) {
                skyObjectRepository.ensureStarted(
                    location.latitude,
                    location.longitude,
                    accuracyMeters,
                )
                scanningStarted = true
            } else {
                skyObjectRepository.updatePosition(
                    location.latitude,
                    location.longitude,
                    accuracyMeters,
                )
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

            val gpsLastKnown = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER)
            val networkLastKnown =
                locationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            val nowElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos()
            val lastKnown = selectFreshestMapLastKnownLocationFix(
                gps = gpsLastKnown?.let { location ->
                    location.toMapLocationFix()
                },
                network = networkLastKnown?.let { location ->
                    location.toMapLocationFix()
                },
                nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
            )
            _userLocationFix.value = selectFreshestMapLastKnownLocationFix(
                gps = _userLocationFix.value,
                network = lastKnown,
                nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
            )
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
        compassSensorLease?.close()
        compassSensorLease = null
    }

    private fun Location.toMapLocationFix(): MapLocationFix = MapLocationFix(
        position = Position(
            latitude = latitude,
            longitude = longitude,
            altitudeMeters = altitude,
        ),
        elapsedRealtimeNanos = elapsedRealtimeNanos,
    )
}

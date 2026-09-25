package com.friendorfoe.presentation.list

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
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.data.repository.validatedLocationAccuracyMeters
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.AircraftRange
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.usecase.FilterEngine
import com.friendorfoe.sensor.VisualFocusRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.stateIn
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject

/**
 * ViewModel for the List View screen.
 *
 * Exposes sky objects from [SkyObjectRepository] with nearby visual focus first,
 * then nearby category priority and nearest distance. Within each focus group,
 * objects without a usable distance are placed at the end of the list.
 *
 * Also manages location updates to ensure scanning is started even
 * if the user navigates directly to the List tab.
 */
@HiltViewModel
class ListViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    private val visualFocusRepository: VisualFocusRepository,
    private val locationManager: LocationManager,
    private val detectionPrefs: DetectionPrefs,
) : ViewModel() {

    companion object {
        private const val TAG = "ListViewModel"
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
    }

    private val visualFocusClock = flow {
        while (true) {
            emit(System.currentTimeMillis())
            delay(1000L)
        }
    }

    val settings = detectionPrefs.settings

    fun setAircraftRangeMiles(miles: Int) {
        detectionPrefs.aircraftRangeMiles = AircraftRange.normalizeMiles(miles)
    }

    private val _filterState = MutableStateFlow(FilterState())
    val filterState: StateFlow<FilterState> = _filterState.asStateFlow()

    fun updateFilter(filterState: FilterState) {
        _filterState.value = filterState
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

    /** Filtered detections sorted by nearby visual focus, priority, then distance. */
    val skyObjects: StateFlow<List<SkyObject>> = observeSortedSkyObjectsForList(
        skyObjectRepository.skyObjects,
        _filterState,
        activeVisualFocusIds,
        detectionPrefs.settings,
    ).stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    private val _userPosition = MutableStateFlow(
        Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    )
    val userPosition: StateFlow<Position> = _userPosition.asStateFlow()

    private val locationStarted = AtomicBoolean(false)
    private var scanningStarted = false
    private var acceptedLocationFix: ListLocationFix? = null

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            val candidate = location.toListLocationFix()
            val selected = selectListLocationFix(
                listOfNotNull(acceptedLocationFix, candidate),
                SystemClock.elapsedRealtimeNanos(),
            )
            if (selected != candidate) return
            acceptedLocationFix = candidate
            _userPosition.value = Position(
                latitude = location.latitude,
                longitude = location.longitude,
                altitudeMeters = location.altitude
            )

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
        if (locationStarted.getAndSet(true)) return

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

            val lastKnown = listOfNotNull(
                locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER),
                locationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER),
            )
            val seed = selectListLocationFix(
                lastKnown.map { it.toListLocationFix() },
                SystemClock.elapsedRealtimeNanos(),
            )
            lastKnown.firstOrNull { it.toListLocationFix() == seed }?.let {
                locationListener.onLocationChanged(it)
            }
        } catch (e: SecurityException) {
            locationStarted.set(false)
            Log.e(TAG, "Location permission not granted", e)
        }
    }

    fun stopLocationUpdates() {
        if (!locationStarted.getAndSet(false)) return
        try {
            locationManager.removeUpdates(locationListener)
        } catch (e: SecurityException) {
            Log.w(TAG, "Could not remove location updates", e)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopLocationUpdates()
    }
}

internal fun observeSortedSkyObjectsForList(
    objects: Flow<List<SkyObject>>,
    filter: Flow<FilterState>,
    activeVisualFocusIds: Flow<Set<String>>,
    settings: Flow<DetectionSettings>,
): Flow<List<SkyObject>> = combine(objects, filter, activeVisualFocusIds, settings) { rows, filters, focusIds, preferences ->
    sortSkyObjectsForList(FilterEngine.applyFilters(rows, filters), focusIds, preferences.aircraftRangeMiles)
}

internal fun sortSkyObjectsForList(
    objects: List<SkyObject>,
    activeVisualFocusIds: Set<String>,
    aircraftRangeMiles: Int = AircraftRange.DEFAULT_MILES,
): List<SkyObject> {
    return objects.sortedWith(
        compareByDescending<SkyObject> {
            it.id in activeVisualFocusIds &&
                (it !is Aircraft || AircraftRange.contains(it.distanceMeters, aircraftRangeMiles))
        }
            .thenBy { !it.listSortDistance().isFinite() }
            .thenByDescending { listSurfacePriority(it, aircraftRangeMiles) }
            .thenBy { it.listSortDistance() }
            .thenByDescending { it.confidence }
            .thenBy { it.id }
    )
}

private fun SkyObject.listSortDistance(): Double =
    distanceMeters?.takeIf { it.isFinite() && it >= 0.0 } ?: Double.POSITIVE_INFINITY

private fun Location.toListLocationFix() = ListLocationFix(
    position = Position(latitude, longitude, altitude),
    elapsedRealtimeNanos = elapsedRealtimeNanos,
    accuracyMeters = validatedLocationAccuracyMeters(),
)

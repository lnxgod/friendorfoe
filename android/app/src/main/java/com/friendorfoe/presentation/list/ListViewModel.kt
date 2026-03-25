package com.friendorfoe.presentation.list

import android.annotation.SuppressLint
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.usecase.FilterEngine
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject

/**
 * ViewModel for the List View screen.
 *
 * Exposes sky objects from [SkyObjectRepository] sorted by distance
 * from the user (nearest first). Objects without a known distance
 * are placed at the end of the list.
 *
 * Also manages location updates to ensure scanning is started even
 * if the user navigates directly to the List tab.
 */
@HiltViewModel
class ListViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    private val locationManager: LocationManager
) : ViewModel() {

    companion object {
        private const val TAG = "ListViewModel"
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
    }

    private val _filterState = MutableStateFlow(FilterState())
    val filterState: StateFlow<FilterState> = _filterState.asStateFlow()

    fun updateFilter(filterState: FilterState) {
        _filterState.value = filterState
    }

    /** All detected sky objects filtered and sorted by confidence (highest first), then distance. */
    val skyObjects: StateFlow<List<SkyObject>> = combine(
        skyObjectRepository.skyObjects,
        _filterState
    ) { objects, filter ->
        FilterEngine.applyFilters(objects, filter).sortedWith(
            compareByDescending<SkyObject> { it.confidence }
                .thenBy { it.distanceMeters ?: Double.MAX_VALUE }
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    /** Smart glasses / privacy devices detected nearby. */
    val glassesDetections = skyObjectRepository.glassesDetections

    /** BLE stalker/follower alerts. */
    val stalkerAlerts = skyObjectRepository.stalkerAlerts

    /** BLE direction finder. */
    val bleTracker = skyObjectRepository.bleTracker

    /** Ignore a privacy device (persists across restarts). */
    fun ignoreDevice(mac: String) {
        skyObjectRepository.ignorePrivacyDevice(mac)
    }

    /** Start BLE direction scan to find a device. */
    fun startDirectionScan(mac: String) {
        bleTracker.startDirectionScan(mac)
    }

    private val _userPosition = MutableStateFlow(
        Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    )
    val userPosition: StateFlow<Position> = _userPosition.asStateFlow()

    private val locationStarted = AtomicBoolean(false)
    private var scanningStarted = false

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            _userPosition.value = Position(
                latitude = location.latitude,
                longitude = location.longitude,
                altitudeMeters = location.altitude
            )

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

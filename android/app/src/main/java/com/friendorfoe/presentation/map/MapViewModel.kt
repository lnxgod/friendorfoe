package com.friendorfoe.presentation.map

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
import com.friendorfoe.sensor.SensorFusionEngine
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject

@HiltViewModel
class MapViewModel @Inject constructor(
    private val skyObjectRepository: SkyObjectRepository,
    private val locationManager: LocationManager,
    private val sensorFusionEngine: SensorFusionEngine
) : ViewModel() {

    companion object {
        private const val TAG = "MapViewModel"
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
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

    private var locationStarted = false
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
        if (locationStarted) return
        locationStarted = true

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
        if (!locationStarted) return
        locationStarted = false
        try {
            locationManager.removeUpdates(locationListener)
        } catch (e: SecurityException) {
            Log.w(TAG, "Could not remove location updates", e)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopLocationUpdates()
        if (_followCompass.value) {
            sensorFusionEngine.stop()
        }
    }
}

package com.friendorfoe.presentation.ar

import android.annotation.SuppressLint
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.sensor.CameraFovCalculator
import com.friendorfoe.sensor.DeviceOrientation
import com.friendorfoe.sensor.ScreenPosition
import com.friendorfoe.sensor.SensorFusionEngine
import com.friendorfoe.sensor.SkyPositionMapper
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject

/**
 * ViewModel for the AR viewfinder screen.
 *
 * Connects sensor fusion, sky object detection, and screen position mapping
 * to produce a reactive stream of screen positions for the AR overlay.
 *
 * Manages:
 * - Device orientation from [SensorFusionEngine]
 * - Sky objects from [SkyObjectRepository]
 * - Screen position mapping via [SkyPositionMapper]
 * - GPS location updates for user position
 * - Lifecycle of sensors and detection sources
 */
@HiltViewModel
class ArViewModel @Inject constructor(
    private val sensorFusionEngine: SensorFusionEngine,
    private val skyObjectRepository: SkyObjectRepository,
    private val locationManager: LocationManager
) : ViewModel() {

    companion object {
        private const val TAG = "ArViewModel"

        /** Minimum time between location updates in milliseconds */
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L

        /** Minimum distance between location updates in meters */
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
    }

    private val skyPositionMapper = SkyPositionMapper()
    val cameraFovCalculator = CameraFovCalculator()

    // --- User position ---

    private val _userPosition = MutableStateFlow(
        Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    )
    val userPosition: StateFlow<Position> = _userPosition.asStateFlow()

    // --- GPS status ---

    private val _gpsStatus = MutableStateFlow(GpsStatus.SEARCHING)
    val gpsStatus: StateFlow<GpsStatus> = _gpsStatus.asStateFlow()

    // --- ARCore status (simplified for v1 -- compass-math primary) ---

    private val _arCoreStatus = MutableStateFlow(ArCoreStatus.UNAVAILABLE)
    val arCoreStatus: StateFlow<ArCoreStatus> = _arCoreStatus.asStateFlow()

    // --- Device orientation (exposed for UI) ---

    val orientation: StateFlow<DeviceOrientation> = sensorFusionEngine.orientation

    // --- Screen positions (combined from orientation + sky objects) ---

    val screenPositions: StateFlow<List<ScreenPosition>> = combine(
        sensorFusionEngine.orientation,
        skyObjectRepository.skyObjects,
        _userPosition
    ) { orientation, skyObjects, userPos ->
        if (userPos.latitude == 0.0 && userPos.longitude == 0.0) {
            // No GPS fix yet, return empty
            emptyList()
        } else {
            skyPositionMapper.mapToScreen(
                userPosition = userPos,
                skyObjects = skyObjects,
                orientation = orientation,
                fovCalculator = cameraFovCalculator
            )
        }
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    // --- Counts for status bar ---

    val aircraftCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it is Aircraft } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    val droneCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it is Drone } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    // --- Location listener ---

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            val newPosition = Position(
                latitude = location.latitude,
                longitude = location.longitude,
                altitudeMeters = location.altitude
            )
            _userPosition.value = newPosition
            _gpsStatus.value = GpsStatus.LOCKED

            // Update the sky object repository with new position
            skyObjectRepository.updatePosition(location.latitude, location.longitude)

            Log.d(TAG, "Location updated: (${location.latitude}, ${location.longitude}), alt=${location.altitude}m")
        }

        @Deprecated("Deprecated in API level 29")
        override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) {
            // Required for older API levels
        }

        override fun onProviderEnabled(provider: String) {
            Log.d(TAG, "Location provider enabled: $provider")
        }

        override fun onProviderDisabled(provider: String) {
            Log.d(TAG, "Location provider disabled: $provider")
            _gpsStatus.value = GpsStatus.DISABLED
        }
    }

    /**
     * Start all sensors and detection sources.
     * Call when the AR view becomes active (onResume).
     */
    @SuppressLint("MissingPermission")
    fun startSensors() {
        Log.i(TAG, "Starting sensors and detection")

        // Start sensor fusion for orientation
        sensorFusionEngine.start()

        // Request GPS updates
        try {
            // Try GPS provider first
            if (locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                locationManager.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER,
                    LOCATION_UPDATE_INTERVAL_MS,
                    LOCATION_UPDATE_DISTANCE_M,
                    locationListener
                )
                _gpsStatus.value = GpsStatus.SEARCHING
            }

            // Also request from network provider for faster initial fix
            if (locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                locationManager.requestLocationUpdates(
                    LocationManager.NETWORK_PROVIDER,
                    LOCATION_UPDATE_INTERVAL_MS,
                    LOCATION_UPDATE_DISTANCE_M,
                    locationListener
                )
            }

            // Use last known location as initial position while waiting for GPS fix
            val lastKnown = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: locationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            lastKnown?.let { loc ->
                val pos = Position(
                    latitude = loc.latitude,
                    longitude = loc.longitude,
                    altitudeMeters = loc.altitude
                )
                _userPosition.value = pos
                _gpsStatus.value = GpsStatus.LOCKED

                // Start sky object detection with initial position
                skyObjectRepository.start(loc.latitude, loc.longitude)

                Log.d(TAG, "Using last known location: (${loc.latitude}, ${loc.longitude})")
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "Location permission not granted", e)
            _gpsStatus.value = GpsStatus.NO_PERMISSION
        }
    }

    /**
     * Stop all sensors and detection sources.
     * Call when the AR view is no longer visible (onPause).
     */
    fun stopSensors() {
        Log.i(TAG, "Stopping sensors and detection")
        sensorFusionEngine.stop()
        skyObjectRepository.stop()

        try {
            locationManager.removeUpdates(locationListener)
        } catch (e: SecurityException) {
            Log.w(TAG, "Could not remove location updates", e)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopSensors()
    }
}

/** GPS lock status for the status bar. */
enum class GpsStatus {
    /** Actively searching for GPS fix */
    SEARCHING,

    /** GPS fix acquired */
    LOCKED,

    /** GPS provider is disabled in system settings */
    DISABLED,

    /** Location permission not granted */
    NO_PERMISSION
}

/** ARCore session status. */
enum class ArCoreStatus {
    /** ARCore is tracking successfully */
    TRACKING,

    /** ARCore lost tracking, using compass-math fallback */
    LOST_TRACKING,

    /** ARCore is not available on this device */
    UNAVAILABLE,

    /** ARCore is initializing */
    INITIALIZING
}

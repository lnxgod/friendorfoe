package com.friendorfoe.presentation.ar

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
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
import com.google.ar.core.ArCoreApk
import com.google.ar.core.Session
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.UnavailableException
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
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
    private val locationManager: LocationManager,
    @ApplicationContext private val appContext: Context
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

    // --- ARCore session ---
    private var arSession: Session? = null

    // --- User position ---

    private val _userPosition = MutableStateFlow(
        Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    )
    val userPosition: StateFlow<Position> = _userPosition.asStateFlow()

    // --- GPS status ---

    private val _gpsStatus = MutableStateFlow(GpsStatus.SEARCHING)
    val gpsStatus: StateFlow<GpsStatus> = _gpsStatus.asStateFlow()

    // --- ARCore status ---

    private val _arCoreStatus = MutableStateFlow(ArCoreStatus.INITIALIZING)
    val arCoreStatus: StateFlow<ArCoreStatus> = _arCoreStatus.asStateFlow()

    // --- Selected object for bottom sheet ---

    private val _selectedObjectId = MutableStateFlow<String?>(null)
    val selectedObjectId: StateFlow<String?> = _selectedObjectId.asStateFlow()

    /** Set the selected object ID to show in the bottom sheet. Pass null to dismiss. */
    fun selectObject(objectId: String?) {
        _selectedObjectId.value = objectId
    }

    // --- Sensor accuracy (magnetometer calibration status) ---

    val sensorAccuracy: StateFlow<Int> = sensorFusionEngine.sensorAccuracy

    // --- Network connectivity status ---

    private val _isOnline = MutableStateFlow(true)
    val isOnline: StateFlow<Boolean> = _isOnline.asStateFlow()

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
     *
     * @param activity Optional Activity reference needed for ARCore session creation.
     *                 If null, ARCore will be marked UNAVAILABLE and compass-math is used.
     */
    @SuppressLint("MissingPermission")
    fun startSensors(activity: Activity? = null) {
        Log.i(TAG, "Starting sensors and detection")

        // Start sensor fusion for orientation
        sensorFusionEngine.start()

        // Check network connectivity
        checkConnectivity()

        // Initialize ARCore
        initArCore(activity)

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
     * Initialize ARCore session and check device availability.
     *
     * If ARCore is supported and installed, creates a Session and begins
     * monitoring tracking state. If not available, gracefully falls back to
     * compass-math and marks status as UNAVAILABLE.
     */
    private fun initArCore(activity: Activity?) {
        if (activity == null) {
            Log.w(TAG, "No Activity provided, cannot initialize ARCore session")
            _arCoreStatus.value = ArCoreStatus.UNAVAILABLE
            return
        }

        try {
            val availability = ArCoreApk.getInstance().checkAvailability(appContext)
            when {
                availability.isSupported -> {
                    // ARCore is supported; attempt to create a session
                    _arCoreStatus.value = ArCoreStatus.INITIALIZING
                    try {
                        // Ensure ARCore is installed / up to date
                        val installStatus = ArCoreApk.getInstance()
                            .requestInstall(activity, true)
                        if (installStatus == ArCoreApk.InstallStatus.INSTALLED) {
                            val session = Session(activity)
                            arSession = session
                            session.resume()
                            updateArCoreTrackingState()
                            Log.i(TAG, "ARCore session created and resumed")
                        } else {
                            // Install was requested; will complete on next resume
                            _arCoreStatus.value = ArCoreStatus.INITIALIZING
                            Log.i(TAG, "ARCore install requested, waiting for completion")
                        }
                    } catch (e: UnavailableException) {
                        Log.w(TAG, "ARCore unavailable: ${e.message}")
                        _arCoreStatus.value = ArCoreStatus.UNAVAILABLE
                        arSession = null
                    }
                }
                else -> {
                    Log.i(TAG, "ARCore not supported on this device (availability=$availability)")
                    _arCoreStatus.value = ArCoreStatus.UNAVAILABLE
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error checking ARCore availability", e)
            _arCoreStatus.value = ArCoreStatus.UNAVAILABLE
        }
    }

    /**
     * Poll the current ARCore tracking state and update [_arCoreStatus].
     *
     * Called after session creation and can be called periodically to refresh status.
     * Even when ARCore is tracking, compass-math is still used for label placement in v1.
     */
    fun updateArCoreTrackingState() {
        val session = arSession ?: return
        try {
            val frame = session.update()
            val camera = frame.camera
            _arCoreStatus.value = when (camera.trackingState) {
                TrackingState.TRACKING -> ArCoreStatus.TRACKING
                TrackingState.PAUSED -> ArCoreStatus.LOST_TRACKING
                TrackingState.STOPPED -> ArCoreStatus.LOST_TRACKING
                else -> ArCoreStatus.LOST_TRACKING
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error updating ARCore tracking state", e)
            _arCoreStatus.value = ArCoreStatus.LOST_TRACKING
        }
    }

    /**
     * Reduce sensor polling rate for battery optimization.
     *
     * Call when the app goes to the background (e.g., onStop) to reduce
     * battery drain while still maintaining a minimal level of tracking.
     * Switches sensors from SENSOR_DELAY_GAME (~20ms) to SENSOR_DELAY_NORMAL (~200ms).
     */
    fun reducePolling() {
        Log.i(TAG, "Reducing sensor polling rate for background")
        sensorFusionEngine.setSensorDelay(SensorManager.SENSOR_DELAY_NORMAL)
    }

    /**
     * Restore full sensor polling rate.
     *
     * Call when the app returns to the foreground (e.g., onStart) to resume
     * high-frequency sensor updates for smooth AR overlay rendering.
     */
    fun resumePolling() {
        Log.i(TAG, "Resuming full sensor polling rate")
        sensorFusionEngine.setSensorDelay(SensorManager.SENSOR_DELAY_GAME)
    }

    /**
     * Check current network connectivity and update [_isOnline].
     *
     * Should be called during startSensors() and can be polled periodically.
     */
    fun checkConnectivity() {
        try {
            val cm = appContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            val network = cm.activeNetwork
            val capabilities = network?.let { cm.getNetworkCapabilities(it) }
            _isOnline.value = capabilities?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true
        } catch (e: Exception) {
            Log.w(TAG, "Failed to check connectivity", e)
            _isOnline.value = false
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

        // Pause ARCore session (don't destroy it -- will resume on next startSensors)
        try {
            arSession?.pause()
        } catch (e: Exception) {
            Log.w(TAG, "Error pausing ARCore session", e)
        }

        try {
            locationManager.removeUpdates(locationListener)
        } catch (e: SecurityException) {
            Log.w(TAG, "Could not remove location updates", e)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopSensors()

        // Destroy ARCore session
        try {
            arSession?.close()
            arSession = null
        } catch (e: Exception) {
            Log.w(TAG, "Error closing ARCore session", e)
        }
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

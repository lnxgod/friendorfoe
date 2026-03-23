package com.friendorfoe.presentation.ar

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.util.Log
import androidx.camera.core.Camera
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.data.repository.WeatherRepository
import com.friendorfoe.detection.AcousticDetector
import com.friendorfoe.detection.AcousticResult
import com.friendorfoe.detection.AlertLevel
import com.friendorfoe.detection.AutoCaptureEngine
import com.friendorfoe.detection.CaptureCandidate
import com.friendorfoe.detection.ClassifiedVisualDetection
import com.friendorfoe.detection.DarkTargetScore
import com.friendorfoe.detection.DarkTargetScorer
import com.friendorfoe.detection.DataSourceStatus
import com.friendorfoe.detection.SkyObjectFilter
import com.friendorfoe.detection.ThreatLevel
import com.friendorfoe.detection.VisualDetection
import com.friendorfoe.detection.VisualDetectionAnalyzer
import com.friendorfoe.detection.VisualDetectionRange
import com.friendorfoe.detection.WeatherAdjustment
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.sensor.ArCoreOrientationProvider
import com.friendorfoe.sensor.CameraFovCalculator
import com.friendorfoe.sensor.DeviceOrientation
import com.friendorfoe.sensor.ScreenPosition
import com.friendorfoe.sensor.SensorFusionEngine
import com.friendorfoe.sensor.CompassBiasEstimator
import com.friendorfoe.sensor.SkyPositionMapper
import com.friendorfoe.sensor.TrajectoryPredictor
import com.friendorfoe.sensor.VisualCorrelationEngine
import com.google.ar.core.ArCoreApk
import com.google.ar.core.Session
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.UnavailableException
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.time.Instant
import javax.inject.Inject
import kotlin.math.roundToInt

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
    private val arCoreOrientationProvider: ArCoreOrientationProvider,
    private val visualCorrelationEngine: VisualCorrelationEngine,
    private val skyObjectFilter: SkyObjectFilter,
    private val weatherRepository: WeatherRepository,
    private val darkTargetScorer: DarkTargetScorer,
    private val acousticDetector: AcousticDetector,
    @ApplicationContext private val appContext: Context
) : ViewModel() {

    companion object {
        private const val TAG = "ArViewModel"

        /** Minimum time between location updates in milliseconds */
        private const val LOCATION_UPDATE_INTERVAL_MS = 5000L

        /** Minimum distance between location updates in meters */
        private const val LOCATION_UPDATE_DISTANCE_M = 10f
    }

    init {
        // Auto-unlock if target disappears from sky objects for >30 seconds
        viewModelScope.launch {
            var missingStartMs: Long? = null
            while (isActive) {
                delay(2000L)
                val lockedId = _lockedObjectId.value
                if (lockedId == null) {
                    missingStartMs = null
                } else {
                    val stillPresent = skyObjectRepository.skyObjects.value.any { it.id == lockedId }
                    if (stillPresent) {
                        missingStartMs = null
                    } else {
                        val start = missingStartMs ?: System.currentTimeMillis().also { missingStartMs = it }
                        if (System.currentTimeMillis() - start > 30_000) {
                            Log.d(TAG, "Auto-unlocking $lockedId — target missing for 30s")
                            _lockedObjectId.value = null
                            resetZoom()
                            missingStartMs = null
                        }
                    }
                }
            }
        }
    }

    private val skyPositionMapper = SkyPositionMapper()
    private val trajectoryPredictor = TrajectoryPredictor()
    private val compassBiasEstimator = CompassBiasEstimator()
    val cameraFovCalculator = CameraFovCalculator()
    val visualDetectionAnalyzer = VisualDetectionAnalyzer(skyObjectFilter)

    private var locationListenerRegistered = false

    // --- Dark mode (nighttime strobe detection active) ---

    private val _isDarkMode = MutableStateFlow(false)
    /** True when ambient light is low enough for nighttime strobe detection. */
    val isDarkMode: StateFlow<Boolean> = _isDarkMode.asStateFlow()

    // --- Strobe detection count ---

    val strobeCount: StateFlow<Int> = visualDetectionAnalyzer.strobeDetections
        .map { it.size }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

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
        _showUnidentifiedSheet.value = false  // dismiss unidentified sheet if open
        _selectedObjectId.value = objectId
    }

    // --- Unidentified tap (empty space) bottom sheet ---

    private val _showUnidentifiedSheet = MutableStateFlow(false)
    val showUnidentifiedSheet: StateFlow<Boolean> = _showUnidentifiedSheet.asStateFlow()

    // --- Lock-on tracking ---

    private val _lockedObjectId = MutableStateFlow<String?>(null)
    val lockedObjectId: StateFlow<String?> = _lockedObjectId.asStateFlow()

    // --- Tap-to-auto-capture state ---

    enum class AutoCaptureState { IDLE, TRACKING, CAPTURING, DONE }

    private val _autoCapturePhase = MutableStateFlow(AutoCaptureState.IDLE)
    val autoCapturePhase: StateFlow<AutoCaptureState> = _autoCapturePhase.asStateFlow()

    private var autoCaptureJob: kotlinx.coroutines.Job? = null

    /**
     * One-tap smart spotter: lock on, briefly track (2s), then auto-capture triple photos.
     * Tap on AR label triggers this flow.
     */
    fun snapAndAutoCapture(objectId: String, context: Context) {
        // Cancel any in-progress auto-capture
        autoCaptureJob?.cancel()

        // Dismiss other sheets
        _selectedObjectId.value = null
        _showUnidentifiedSheet.value = false
        _zoomTarget.value = null
        _snapTarget.value = null

        // Lock on
        _lockedObjectId.value = objectId
        _autoCapturePhase.value = AutoCaptureState.TRACKING

        // Auto-zoom based on ground distance
        val sp = screenPositions.value.firstOrNull { it.skyObject.id == objectId }
        val previousZoom = _currentZoomRatio.value
        if (sp != null) {
            zoomToObject(sp.groundDistanceMeters)
        }

        // Get label for file naming
        val skyObj = sp?.skyObject
            ?: skyObjectRepository.skyObjects.value.firstOrNull { it.id == objectId }
        val label = when (skyObj) {
            is Aircraft -> skyObj.callsign ?: skyObj.icaoHex
            is Drone -> skyObj.droneId.take(16)
            else -> objectId
        }

        autoCaptureJob = viewModelScope.launch {
            // Phase 1: Track for 2 seconds (let zoom settle + ML Kit refine bounding box)
            delay(2000L)

            // Check target is still in view
            val currentPos = screenPositions.value.firstOrNull { it.skyObject.id == objectId }
            if (currentPos == null || !currentPos.isInView) {
                Log.d(TAG, "Auto-capture: target lost during tracking")
                _autoCapturePhase.value = AutoCaptureState.IDLE
                _lockedObjectId.value = null
                setZoomRatio(previousZoom)
                android.widget.Toast.makeText(context, "Target lost", android.widget.Toast.LENGTH_SHORT).show()
                return@launch
            }

            // Phase 2: Capture
            _autoCapturePhase.value = AutoCaptureState.CAPTURING

            val capture = imageCaptureRef
            if (capture == null) {
                _autoCapturePhase.value = AutoCaptureState.IDLE
                _lockedObjectId.value = null
                setZoomRatio(previousZoom)
                return@launch
            }

            // Triple capture (clean + annotated + AI-zoomed)
            val panelInfo = buildPanelInfo(skyObj, currentPos)
            val screenBitmap = captureScreenBitmap(context)
            val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
                .format(java.util.Date())

            captureDualPhoto(context, capture, label, screenBitmap, panelInfo) { cleanUri, _ ->
                // After clean + annotated, trigger AI-aimed zoom
                captureSmartZoomedPhoto(context, capture, label, timestamp) { zoomedUri ->
                    val photoCount = listOfNotNull(cleanUri, zoomedUri).size + 1 // +1 for annotated
                    android.widget.Toast.makeText(
                        context,
                        "Captured $label — $photoCount photos saved",
                        android.widget.Toast.LENGTH_SHORT
                    ).show()

                    _autoCapturePhase.value = AutoCaptureState.DONE

                    // Auto-dismiss after brief confirmation
                    viewModelScope.launch {
                        delay(1500L)
                        _autoCapturePhase.value = AutoCaptureState.IDLE
                        _lockedObjectId.value = null
                        setZoomRatio(previousZoom)
                    }
                }
            }
        }
    }

    fun lockOnObject(objectId: String) {
        _selectedObjectId.value = null
        _showUnidentifiedSheet.value = false
        _zoomTarget.value = null
        _lockedObjectId.value = objectId
        // Auto-zoom based on ground distance
        screenPositions.value.firstOrNull { it.skyObject.id == objectId }?.let {
            zoomToObject(it.groundDistanceMeters)
        }
    }

    fun unlockObject() {
        _lockedObjectId.value = null
        resetZoom()
    }

    val detectedDrones: StateFlow<List<Drone>> = skyObjectRepository.skyObjects
        .map { objects -> objects.filterIsInstance<Drone>() }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    // --- Auto-capture ---

    private val _autoCaptureEnabled = MutableStateFlow(true)
    val autoCaptureEnabled: StateFlow<Boolean> = _autoCaptureEnabled.asStateFlow()

    private val capturedThisSession = mutableSetOf<String>()
    private var lastAutoCaptureTimeMs = 0L

    private val _lastAutoCapture = MutableStateFlow<String?>(null)
    /** Brief label of the last auto-captured object (for toast display). Cleared after consumption. */
    val lastAutoCapture: StateFlow<String?> = _lastAutoCapture.asStateFlow()

    fun toggleAutoCapture() {
        _autoCaptureEnabled.value = !_autoCaptureEnabled.value
    }

    fun clearLastAutoCapture() {
        _lastAutoCapture.value = null
    }

    /**
     * Attempt auto-capture by correlating current visual detections with nearby sky objects.
     * Called from a LaunchedEffect when visual detections update.
     */
    fun attemptAutoCapture(context: Context) {
        if (!_autoCaptureEnabled.value) return

        val candidate = AutoCaptureEngine.findCaptureCandidate(
            screenPositions = screenPositions.value,
            visualDetections = visualDetectionAnalyzer.detections.value,
            capturedIds = capturedThisSession,
            lastCaptureTimeMs = lastAutoCaptureTimeMs,
            isLockedOn = _lockedObjectId.value != null
        ) ?: return

        // Mark as captured immediately to prevent re-trigger
        capturedThisSession.add(candidate.skyObject.id)
        lastAutoCaptureTimeMs = System.currentTimeMillis()

        // Build label for the toast and filename
        val label = when (val obj = candidate.skyObject) {
            is Aircraft -> {
                val cs = obj.callsign ?: obj.icaoHex
                val type = obj.aircraftType?.let { " ($it)" } ?: ""
                "$cs$type"
            }
            is Drone -> obj.droneId.take(16)
        }

        // Zoom → stabilize → capture → restore
        viewModelScope.launch {
            val previousZoom = _currentZoomRatio.value
            setZoomRatio(candidate.suggestedZoom)
            delay(300L) // wait for zoom to settle
            val capture = imageCaptureRef
            if (capture != null) {
                capturePhotoToGallery(context, capture, "auto_$label") { uri ->
                    if (uri != null) {
                        Log.i(TAG, "Auto-captured $label at ${candidate.distanceMeters.roundToInt()}m")
                        _lastAutoCapture.value = "Auto-captured $label"
                    }
                }
            }
            delay(200L)
            setZoomRatio(previousZoom)
        }
    }

    // --- Drone proximity alert ---

    /** WiFi-only drones with strong RSSI signal (no GPS position but clearly nearby) */
    val proximityDrones: StateFlow<List<Drone>> = skyObjectRepository.skyObjects
        .map { objects ->
            objects.filterIsInstance<Drone>().filter { drone ->
                drone.source == DetectionSource.WIFI &&
                (drone.position.latitude == 0.0 && drone.position.longitude == 0.0) &&
                (drone.signalStrengthDbm != null && drone.signalStrengthDbm > -60)
            }
        }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    fun showUnidentifiedSheet() {
        _selectedObjectId.value = null  // dismiss object detail if open
        _showUnidentifiedSheet.value = true
    }

    fun dismissUnidentifiedSheet() {
        _showUnidentifiedSheet.value = false
    }

    // --- Sensor accuracy (magnetometer calibration status) ---

    val sensorAccuracy: StateFlow<Int> = sensorFusionEngine.sensorAccuracy

    // --- Network connectivity status ---

    private val _isOnline = MutableStateFlow(true)
    val isOnline: StateFlow<Boolean> = _isOnline.asStateFlow()

    // --- Device orientation (blended ARCore + compass) ---

    val orientation: StateFlow<DeviceOrientation> = combine(
        sensorFusionEngine.orientation,
        arCoreOrientationProvider.arOrientation
    ) { compass, arCore -> arCore ?: compass }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), DeviceOrientation())

    // --- Unmatched visual detections (visual-only, no radio match) ---

    private val _unmatchedVisuals = MutableStateFlow<List<VisualDetection>>(emptyList())
    val unmatchedVisuals: StateFlow<List<VisualDetection>> = _unmatchedVisuals.asStateFlow()

    // --- Zoom target for tap-to-zoom ---

    private val _zoomTarget = MutableStateFlow<VisualDetection?>(null)
    val zoomTarget: StateFlow<VisualDetection?> = _zoomTarget.asStateFlow()

    fun showZoom(detection: VisualDetection) {
        _selectedObjectId.value = null
        _showUnidentifiedSheet.value = false
        _zoomTarget.value = detection
    }

    fun dismissZoom() {
        _zoomTarget.value = null
        resetZoom()
    }

    fun captureZoomFrame(): android.graphics.Bitmap? = visualDetectionAnalyzer.getLastFrame()

    // --- Hardware camera zoom ---

    private var cameraRef: Camera? = null
    private var zoomObserver: androidx.lifecycle.Observer<androidx.camera.core.ZoomState>? = null

    private val _currentZoomRatio = MutableStateFlow(1.0f)
    val currentZoomRatio: StateFlow<Float> = _currentZoomRatio.asStateFlow()

    private val _maxZoomRatio = MutableStateFlow(1.0f)
    val maxZoomRatio: StateFlow<Float> = _maxZoomRatio.asStateFlow()

    private val _minZoomRatio = MutableStateFlow(1.0f)
    val minZoomRatio: StateFlow<Float> = _minZoomRatio.asStateFlow()

    /** Store the CameraX Camera reference and read min/max zoom from camera info. */
    fun setCameraRef(camera: Camera) {
        // Remove previous observer to prevent leak on camera rebind
        cameraRef?.cameraInfo?.zoomState?.let { liveData ->
            zoomObserver?.let { liveData.removeObserver(it) }
        }
        cameraRef = camera
        val observer = androidx.lifecycle.Observer<androidx.camera.core.ZoomState> { zoomState ->
            if (zoomState != null) {
                _maxZoomRatio.value = zoomState.maxZoomRatio
                _minZoomRatio.value = zoomState.minZoomRatio
                _currentZoomRatio.value = zoomState.zoomRatio
            }
        }
        zoomObserver = observer
        camera.cameraInfo.zoomState.observeForever(observer)
        Log.d(TAG, "Camera ref set, zoom range=${camera.cameraInfo.zoomState.value?.minZoomRatio}..${camera.cameraInfo.zoomState.value?.maxZoomRatio}")
    }

    /** Set hardware zoom ratio, clamped to [minZoom, maxZoom]. Spans ultrawide to telephoto. */
    fun setZoomRatio(ratio: Float) {
        val clamped = ratio.coerceIn(_minZoomRatio.value, _maxZoomRatio.value)
        val camera = cameraRef
        if (camera != null) {
            camera.cameraControl.setZoomRatio(clamped)
            _currentZoomRatio.value = clamped
        } else {
            Log.w(TAG, "Camera not available for zoom to $clamped")
        }
    }

    // --- Snap-to photo capture ---

    private var imageCaptureRef: ImageCapture? = null

    private val _snapTarget = MutableStateFlow<SnapTarget?>(null)
    val snapTarget: StateFlow<SnapTarget?> = _snapTarget.asStateFlow()

    /** Store the ImageCapture reference from CameraX setup. */
    fun setImageCaptureRef(imageCapture: ImageCapture) {
        imageCaptureRef = imageCapture
    }

    /**
     * Snap to an object: lock on, auto-zoom, and open the snap photo sheet.
     * Called when a user taps an AR label.
     */
    fun snapToObject(objectId: String) {
        // Dismiss any other sheets
        _selectedObjectId.value = null
        _showUnidentifiedSheet.value = false
        _zoomTarget.value = null

        // Lock on to the object
        _lockedObjectId.value = objectId

        // Auto-zoom based on ground distance
        val sp = screenPositions.value.firstOrNull { it.skyObject.id == objectId }
        if (sp != null) {
            zoomToObject(sp.groundDistanceMeters)
        }

        // Build snap target info from sky object (fall back to repository if not yet in screenPositions)
        val skyObj = sp?.skyObject
            ?: skyObjectRepository.skyObjects.value.firstOrNull { it.id == objectId }
        val label = when (skyObj) {
            is com.friendorfoe.domain.model.Aircraft -> skyObj.callsign ?: skyObj.icaoHex
            is com.friendorfoe.domain.model.Drone -> skyObj.droneId.take(16)
            else -> objectId
        }
        val typeDesc = when (skyObj) {
            is com.friendorfoe.domain.model.Aircraft -> skyObj.aircraftModel ?: skyObj.aircraftType ?: "Aircraft"
            is com.friendorfoe.domain.model.Drone -> skyObj.model ?: skyObj.manufacturer ?: "Drone"
            else -> null
        }

        _snapTarget.value = SnapTarget(
            objectId = objectId,
            label = label,
            typeDescription = typeDesc,
            distanceMeters = sp?.distanceMeters
        )
    }

    /** Dismiss the snap photo sheet, unlock, and reset zoom. */
    fun dismissSnap() {
        _snapTarget.value = null
        _lockedObjectId.value = null
        resetZoom()
    }

    /**
     * Capture a full-resolution photo using the ImageCapture use case.
     * Saves to Pictures/FriendOrFoe/ via MediaStore.
     */
    fun capturePhoto(context: Context, onResult: (Boolean) -> Unit = {}) {
        val capture = imageCaptureRef
        if (capture == null) {
            Log.w(TAG, "ImageCapture not available")
            onResult(false)
            return
        }
        val label = _snapTarget.value?.label ?: "unknown"
        val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
            .format(java.util.Date())
        val snapObj = _snapTarget.value?.let { snap ->
            skyObjectRepository.skyObjects.value.firstOrNull { it.id == snap.objectId }
        }
        val panelInfo = buildPanelInfo(snapObj, screenPositions.value.firstOrNull {
            it.skyObject.id == snapObj?.id
        })
        val screenBitmap = captureScreenBitmap(context)

        captureDualPhoto(context, capture, label, screenBitmap, panelInfo) { cleanUri, _ ->
            // After clean + annotated saved, trigger AI-aimed zoom capture
            captureSmartZoomedPhoto(context, capture, label, timestamp) { _ ->
                onResult(cleanUri != null)
            }
        }
    }

    /** Capture a photo from the main AR screen (no snap target required). Returns the saved URI. */
    fun capturePhotoFromMainScreen(context: Context, onResult: (android.net.Uri?) -> Unit) {
        val capture = imageCaptureRef
        if (capture == null) {
            Log.w(TAG, "ImageCapture not available")
            onResult(null)
            return
        }
        // Use locked object label if available, otherwise "sky"
        val lockedObj = _lockedObjectId.value?.let { lockedId ->
            skyObjectRepository.skyObjects.value.firstOrNull { it.id == lockedId }
        }
        val label = when (lockedObj) {
            is Aircraft -> lockedObj.callsign ?: lockedObj.icaoHex
            is Drone -> lockedObj.droneId.take(16)
            else -> "sky"
        }

        // Build info panel data from locked/nearest object
        val panelInfo = buildPanelInfo(lockedObj, screenPositions.value.firstOrNull {
            it.skyObject.id == lockedObj?.id
        })

        // Capture screen bitmap for annotated version
        val screenBitmap = captureScreenBitmap(context)
        val timestamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
            .format(java.util.Date())

        captureDualPhoto(context, capture, label, screenBitmap, panelInfo) { cleanUri, _ ->
            // After clean + annotated saved, trigger AI-aimed zoom capture
            captureSmartZoomedPhoto(context, capture, label, timestamp) { _ ->
                onResult(cleanUri)
            }
        }
    }

    /**
     * Calculate optimal hardware zoom to frame an ML Kit detection at ~45% of frame width.
     * Formula: newZoom = targetFill * currentZoom / bboxWidth
     */
    private fun calculateSmartZoom(detection: VisualDetection): Float {
        if (detection.width <= 0f) return _currentZoomRatio.value
        val targetFill = 0.45f
        val newZoom = targetFill * _currentZoomRatio.value / detection.width
        return newZoom.coerceIn(_minZoomRatio.value, _maxZoomRatio.value)
    }

    /**
     * Find the best visual detection target for smart zoom capture.
     * Priority: locked object's matched detection > nearest visually confirmed in-view object.
     */
    private fun findSmartZoomTarget(): VisualDetection? {
        val positions = screenPositions.value
        // Try locked object first
        val lockedId = _lockedObjectId.value
        if (lockedId != null) {
            val lockedPos = positions.firstOrNull { it.skyObject.id == lockedId }
            if (lockedPos?.matchedDetection != null) return lockedPos.matchedDetection
        }
        // Fall back to nearest visually confirmed in-view object
        return positions
            .filter { it.isInView && it.visuallyConfirmed && it.matchedDetection != null }
            .minByOrNull { it.distanceMeters }
            ?.matchedDetection
    }

    /**
     * Capture a smart-zoomed photo aimed at the ML Kit bounding box.
     * Zooms hardware to frame the aircraft at ~45% of frame width, captures, then restores zoom.
     */
    fun captureSmartZoomedPhoto(
        context: Context,
        imageCapture: ImageCapture,
        label: String,
        timestamp: String,
        onComplete: (android.net.Uri?) -> Unit
    ) {
        val detection = findSmartZoomTarget()
        if (detection == null) {
            Log.d(TAG, "No visual target for smart zoom capture")
            onComplete(null)
            return
        }

        val smartZoom = calculateSmartZoom(detection)
        val currentZoom = _currentZoomRatio.value

        // Skip if zoom wouldn't change meaningfully (< 0.3x increase)
        if (smartZoom <= currentZoom + 0.3f) {
            Log.d(TAG, "Smart zoom not needed (current=$currentZoom, calculated=$smartZoom)")
            onComplete(null)
            return
        }

        val safeLabel = label.replace(Regex("[^a-zA-Z0-9]"), "_").lowercase()

        viewModelScope.launch {
            // Zoom in
            setZoomRatio(smartZoom)
            delay(300L) // Wait for hardware zoom to settle

            // Capture zoomed photo
            val zoomValues = android.content.ContentValues().apply {
                put(android.provider.MediaStore.Images.Media.DISPLAY_NAME,
                    "friendorfoe_${safeLabel}_${timestamp}_zoomed.jpg")
                put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                put(android.provider.MediaStore.Images.Media.RELATIVE_PATH,
                    "${android.os.Environment.DIRECTORY_PICTURES}/FriendOrFoe")
                put(android.provider.MediaStore.Images.Media.DESCRIPTION,
                    "FriendOrFoe AI-aimed capture: $label")
            }

            val options = ImageCapture.OutputFileOptions.Builder(
                context.contentResolver,
                android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                zoomValues
            ).build()

            imageCapture.takePicture(
                options,
                androidx.core.content.ContextCompat.getMainExecutor(context),
                object : ImageCapture.OnImageSavedCallback {
                    override fun onImageSaved(output: ImageCapture.OutputFileResults) {
                        Log.d(TAG, "Smart zoomed photo saved: ${output.savedUri}")
                        viewModelScope.launch {
                            delay(200L)
                            setZoomRatio(currentZoom) // Restore original zoom
                        }
                        onComplete(output.savedUri)
                    }

                    override fun onError(exception: ImageCaptureException) {
                        Log.e(TAG, "Smart zoom capture failed", exception)
                        viewModelScope.launch {
                            setZoomRatio(currentZoom) // Restore zoom on failure too
                        }
                        onComplete(null)
                    }
                }
            )
        }
    }

    /**
     * Capture the current screen as a Bitmap (camera preview + AR overlay).
     * Uses the DecorView draw method for simplicity.
     */
    private fun captureScreenBitmap(context: Context): Bitmap? {
        return try {
            val activity = context as? Activity ?: return null
            val view = activity.window.decorView.rootView
            val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            val canvas = android.graphics.Canvas(bitmap)
            view.draw(canvas)
            bitmap
        } catch (e: Exception) {
            Log.w(TAG, "Screen capture failed", e)
            null
        }
    }

    /**
     * Build info panel key-value pairs for the annotated screenshot.
     */
    private fun buildPanelInfo(
        skyObject: com.friendorfoe.domain.model.SkyObject?,
        screenPos: ScreenPosition?
    ): List<Pair<String, String>> {
        val info = mutableListOf<Pair<String, String>>()
        val now = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss z", java.util.Locale.US)
            .format(java.util.Date())
        info.add("Timestamp" to now)

        val userPos = _userPosition.value
        if (userPos.latitude != 0.0) {
            info.add("GPS" to "%.5f, %.5f".format(userPos.latitude, userPos.longitude))
        }

        when (skyObject) {
            is Aircraft -> {
                info.add("Callsign" to (skyObject.callsign ?: skyObject.icaoHex))
                skyObject.aircraftType?.let { info.add("Type" to it) }
                info.add("Altitude" to "${(skyObject.position.altitudeMeters * 3.281).toInt()} ft")
                skyObject.position.speedMps?.let {
                    info.add("Speed" to "${(it * 1.944).toInt()} kts")
                }
                skyObject.position.heading?.let {
                    info.add("Heading" to "${it.toInt()}°")
                }
                info.add("Source" to skyObject.source.name)
                screenPos?.let {
                    info.add("Distance" to "%.1f km".format(it.distanceMeters / 1000.0))
                }
                info.add("Category" to skyObject.category.name)
            }
            is Drone -> {
                info.add("Drone ID" to skyObject.droneId)
                info.add("Source" to skyObject.source.name)
                info.add("Confidence" to "%.0f%%".format(skyObject.confidence * 100))
                screenPos?.let {
                    info.add("Distance" to "%.0f m".format(it.distanceMeters))
                }
            }
            else -> {
                info.add("Target" to "No lock")
                val count = screenPositions.value.count { it.isInView }
                info.add("In View" to "$count objects")
            }
        }

        return info
    }

    /** Auto-zoom toward an object based on ground distance (horizontal, not slant). */
    fun zoomToObject(groundDistanceMeters: Double) {
        val maxZoom = _maxZoomRatio.value
        val ratio = when {
            groundDistanceMeters < 500 -> 1.5f      // Overhead/very close
            groundDistanceMeters < 2000 -> 2.0f     // Nearby
            groundDistanceMeters < 5000 -> 3.0f     // Medium
            groundDistanceMeters < 10000 -> 4.0f    // Far
            groundDistanceMeters < 20000 -> 6.0f    // Very far
            else -> maxZoom * 0.75f                  // Distant — leave headroom
        }.coerceAtMost(maxZoom)
        setZoomRatio(ratio)
    }

    /** Reset hardware zoom to 1x. */
    fun resetZoom() {
        setZoomRatio(1.0f)
    }

    /** Zoom in by 15% of the device's zoom range. */
    fun zoomIn() {
        val step = (_maxZoomRatio.value - _minZoomRatio.value) * 0.15f
        setZoomRatio(_currentZoomRatio.value + step)
    }

    /** Zoom out by 15% of the device's zoom range. */
    fun zoomOut() {
        val step = (_maxZoomRatio.value - _minZoomRatio.value) * 0.15f
        setZoomRatio(_currentZoomRatio.value - step)
    }

    // --- Classified unknowns ---

    private val _classifiedUnknowns = MutableStateFlow<List<ClassifiedVisualDetection>>(emptyList())
    val classifiedUnknowns: StateFlow<List<ClassifiedVisualDetection>> = _classifiedUnknowns.asStateFlow()

    val alertCount: StateFlow<Int> = _classifiedUnknowns
        .map { list -> list.count { it.alertLevel == AlertLevel.ALERT } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    // --- Dark target threat assessment ---

    private val _darkTargetScores = MutableStateFlow<List<DarkTargetScore>>(emptyList())
    /** Scored dark targets (visual detections with no radio match). */
    val darkTargetScores: StateFlow<List<DarkTargetScore>> = _darkTargetScores.asStateFlow()

    /** Count of dark targets at or above UNIDENTIFIED threshold. */
    val darkTargetCount: StateFlow<Int> = _darkTargetScores
        .map { list -> list.count { it.level != ThreatLevel.NONE } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    /** Highest active threat level among all dark targets. */
    val maxThreatLevel: StateFlow<ThreatLevel> = _darkTargetScores
        .map { list -> list.maxByOrNull { it.score }?.level ?: ThreatLevel.NONE }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), ThreatLevel.NONE)

    // --- Acoustic detection ---

    private val _acousticResult = MutableStateFlow(AcousticResult.NONE)
    val acousticResult: StateFlow<AcousticResult> = _acousticResult.asStateFlow()

    private val _acousticEnabled = MutableStateFlow(false)
    val acousticEnabled: StateFlow<Boolean> = _acousticEnabled.asStateFlow()

    fun toggleAcousticDetection() {
        _acousticEnabled.value = !_acousticEnabled.value
        if (_acousticEnabled.value) {
            startAcousticMonitoring()
        } else {
            acousticJob?.cancel()
            _acousticResult.value = AcousticResult.NONE
        }
    }

    private var acousticJob: kotlinx.coroutines.Job? = null

    private fun startAcousticMonitoring() {
        acousticJob?.cancel()
        acousticJob = viewModelScope.launch(Dispatchers.IO) {
            acousticDetector.startMonitoring().collect { result ->
                _acousticResult.value = result
            }
        }
    }

    // --- Weather-based detection range ---

    private val _weatherRange = MutableStateFlow<VisualDetectionRange?>(null)
    val weatherRange: StateFlow<VisualDetectionRange?> = _weatherRange.asStateFlow()

    // --- Manual range override ---

    /** User-controlled confidence multiplier override. null = use weather auto value. */
    private val _rangeOverride = MutableStateFlow<Float?>(null)
    val rangeOverride: StateFlow<Float?> = _rangeOverride.asStateFlow()

    /**
     * Set a manual confidence multiplier override (0.2–1.0).
     * Pass null to revert to automatic weather-based value.
     */
    fun setRangeOverride(multiplier: Float?) {
        _rangeOverride.value = multiplier?.coerceIn(0.2f, 1.0f)
        applyConfidenceMultiplier()
    }

    private fun applyConfidenceMultiplier() {
        val effective = _rangeOverride.value
            ?: _weatherRange.value?.confidenceMultiplier
            ?: 1.0f
        visualDetectionAnalyzer.confidenceMultiplier = effective
        Log.d(TAG, "Confidence multiplier → $effective (override=${_rangeOverride.value})")
    }

    // --- Visual detection count ---

    val visualCount: StateFlow<Int> = visualDetectionAnalyzer.detections
        .map { it.size }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    // --- Frame clock for trajectory prediction (ensures extrapolation updates even when sensors are quiet) ---

    private val frameClockFlow = flow {
        while (true) {
            emit(System.currentTimeMillis())
            delay(33L) // ~30 fps
        }
    }

    // --- Screen positions (combined from orientation + sky objects + visual detections) ---

    val screenPositions: StateFlow<List<ScreenPosition>> = combine(
        frameClockFlow,
        orientation,
        skyObjectRepository.skyObjects,
        _userPosition,
        visualDetectionAnalyzer.detections
    ) { nowMs, orient, skyObjects, userPos, visualDetections ->
        // Keep analyzer updated with current pitch for pitch-aware filtering
        visualDetectionAnalyzer.currentPitch = orient.pitchDegrees

        if (userPos.latitude == 0.0 && userPos.longitude == 0.0) {
            // No GPS fix yet, return empty
            _unmatchedVisuals.value = emptyList()
            _classifiedUnknowns.value = emptyList()
            emptyList()
        } else {
            // Apply compass bias correction from visual-radio calibration
            val correctedOrient = orient.copy(
                azimuthDegrees = orient.azimuthDegrees + compassBiasEstimator.biasDegrees
            )

            // Dead-reckon positions forward from last ADS-B report
            val predicted = trajectoryPredictor.predictAll(skyObjects, nowMs)

            val radioPositions = skyPositionMapper.mapPredictedToScreen(
                userPosition = userPos,
                predictedObjects = predicted,
                orientation = correctedOrient,
                fovCalculator = cameraFovCalculator
            )
            // Suppress visual detections when phone points below horizon (ground clutter)
            val effectiveVisualDetections = if (orient.pitchDegrees < -10f) {
                emptyList()
            } else {
                visualDetections
            }
            val scored = visualDetectionAnalyzer.scoredDetections.value
            val result = visualCorrelationEngine.correlate(radioPositions, effectiveVisualDetections, scored)
            _unmatchedVisuals.value = result.unmatchedVisuals
            _classifiedUnknowns.value = result.classifiedUnknowns

            // Feed visual-radio matches back to compass bias estimator
            compassBiasEstimator.observe(
                result.positions,
                Math.toDegrees(cameraFovCalculator.horizontalFovRadians)
            )

            // Score dark targets (unmatched visuals with no radio correlation)
            val currentAcoustic = _acousticResult.value
            val currentDarkMode = _isDarkMode.value
            _darkTargetScores.value = result.classifiedUnknowns.map { classified ->
                darkTargetScorer.score(classified, currentDarkMode, currentAcoustic)
            }

            result.positions
        }
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

    // --- Locked object screen position (derived from screenPositions + lockedObjectId) ---

    val lockedScreenPosition: StateFlow<ScreenPosition?> = combine(
        screenPositions, _lockedObjectId
    ) { positions, lockedId ->
        if (lockedId == null) null else positions.firstOrNull { it.skyObject.id == lockedId }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)

    // --- Counts for status bar ---

    val aircraftCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it is Aircraft } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    val droneCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it is Drone } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    val militaryCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it.category == ObjectCategory.MILITARY || it.category == ObjectCategory.GOVERNMENT } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    val emergencyCount: StateFlow<Int> = skyObjectRepository.skyObjects
        .map { objects -> objects.count { it.category == ObjectCategory.EMERGENCY } }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)

    // --- Data source status ---

    val dataSourceStatus: StateFlow<DataSourceStatus> = skyObjectRepository.dataSourceStatus

    // --- Detection log (all detected objects with on-screen status) ---

    private val _detectionLogExpanded = MutableStateFlow(false)
    val detectionLogExpanded: StateFlow<Boolean> = _detectionLogExpanded.asStateFlow()

    fun toggleDetectionLog() {
        _detectionLogExpanded.value = !_detectionLogExpanded.value
    }

    val detectionLog: StateFlow<List<DetectionLogEntry>> = combine(
        skyObjectRepository.skyObjects,
        screenPositions,
        skyObjectRepository.dataSourceStatus,
        skyObjectRepository.lastError
    ) { allObjects, screenPos, status, lastErr ->
        val inViewIds = screenPos.filter { it.isInView }.map { it.skyObject.id }.toSet()

        val entries = allObjects.map { obj ->
            val isOnScreen = obj.id in inViewIds
            when (obj) {
                is Aircraft -> {
                    val altFeet = (obj.position.altitudeMeters * 3.281).roundToInt()
                    val altStr = if (altFeet >= 18000) "FL${altFeet / 100}" else "${altFeet}ft"
                    val distMeters = screenPos.firstOrNull { it.skyObject.id == obj.id }?.distanceMeters ?: 0.0
                    val distStr = if (distMeters > 800.0) {
                        "%.1f mi".format(distMeters / 1609.344)
                    } else if (distMeters > 0.0) {
                        "${distMeters.roundToInt()}m"
                    } else ""
                    val detailParts = listOf(altStr, obj.aircraftType ?: "unknown type", distStr)
                        .filter { it.isNotBlank() }
                    DetectionLogEntry(
                        timestamp = obj.lastUpdated,
                        source = obj.source,
                        label = obj.callsign ?: obj.icaoHex,
                        detail = detailParts.joinToString(", "),
                        isOnScreen = isOnScreen
                    )
                }
                is Drone -> {
                    val distStr = obj.estimatedDistanceMeters?.let { "~${it.roundToInt()}m" } ?: ""
                    val rssiStr = obj.signalStrengthDbm?.let { "${it}dBm" } ?: ""
                    DetectionLogEntry(
                        timestamp = obj.lastUpdated,
                        source = obj.source,
                        label = obj.droneId,
                        detail = listOf(obj.manufacturer ?: "", rssiStr, distStr)
                            .filter { it.isNotBlank() }.joinToString(", "),
                        isOnScreen = isOnScreen
                    )
                }
            }
        }.toMutableList()

        // Add status entry for non-primary data source states
        when (status) {
            DataSourceStatus.ADSBFI_FALLBACK -> { /* primary source, no status entry needed */ }
            DataSourceStatus.AIRPLANES_LIVE_FALLBACK -> {
                entries.add(0, DetectionLogEntry(
                    timestamp = Instant.now(),
                    source = DetectionSource.ADS_B,
                    label = "ADS-B: airplanes.live",
                    detail = "adsb.fi unavailable, using airplanes.live",
                    isOnScreen = false
                ))
            }
            DataSourceStatus.OPENSKY_FALLBACK -> {
                entries.add(0, DetectionLogEntry(
                    timestamp = Instant.now(),
                    source = DetectionSource.ADS_B,
                    label = "ADS-B: OpenSky",
                    detail = "Using OpenSky (rate-limited)",
                    isOnScreen = false
                ))
            }
            DataSourceStatus.RATE_LIMITED -> {
                entries.add(0, DetectionLogEntry(
                    timestamp = Instant.now(),
                    source = DetectionSource.ADS_B,
                    label = "ADS-B: Rate Limited",
                    detail = lastErr ?: "Waiting to retry",
                    isOnScreen = false
                ))
            }
            DataSourceStatus.OFFLINE -> {
                entries.add(0, DetectionLogEntry(
                    timestamp = Instant.now(),
                    source = DetectionSource.ADS_B,
                    label = "ADS-B: Offline",
                    detail = lastErr ?: "No connection",
                    isOnScreen = false
                ))
            }
        }

        entries.toList()
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        initialValue = emptyList()
    )

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

            // Ensure scanners are running (idempotent — safe if already started by Map)
            skyObjectRepository.ensureStarted(location.latitude, location.longitude)

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

        // Calibrate camera FOV from hardware
        try {
            val cameraManager = appContext.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val backCameraId = cameraManager.cameraIdList.firstOrNull { id ->
                cameraManager.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
            }
            if (backCameraId != null) {
                val characteristics = cameraManager.getCameraCharacteristics(backCameraId)
                cameraFovCalculator.calculateFromCharacteristics(characteristics)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Could not query camera FOV, using defaults", e)
        }
        // Swap H/V for portrait mode (app is locked to portrait)
        cameraFovCalculator.swapForPortrait()

        // Check network connectivity
        checkConnectivity()

        // Monitor dark mode status
        startDetectionStatusPolling()

        // Start weather polling
        startWeatherPolling()

        // Initialize ARCore
        initArCore(activity)

        // Request GPS updates (guard against double-registration)
        if (locationListenerRegistered) {
            Log.d(TAG, "Location listener already registered, skipping")
        } else {
            try {
                val gpsEnabled = locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)
                val networkEnabled = locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)

                // Try GPS provider first
                if (gpsEnabled) {
                    locationManager.requestLocationUpdates(
                        LocationManager.GPS_PROVIDER,
                        LOCATION_UPDATE_INTERVAL_MS,
                        LOCATION_UPDATE_DISTANCE_M,
                        locationListener
                    )
                    _gpsStatus.value = GpsStatus.SEARCHING
                }

                // Also request from network provider for faster initial fix
                if (networkEnabled) {
                    locationManager.requestLocationUpdates(
                        LocationManager.NETWORK_PROVIDER,
                        LOCATION_UPDATE_INTERVAL_MS,
                        LOCATION_UPDATE_DISTANCE_M,
                        locationListener
                    )
                    if (!gpsEnabled) {
                        _gpsStatus.value = GpsStatus.SEARCHING
                    }
                }

                // Neither provider enabled — mark GPS as disabled
                if (!gpsEnabled && !networkEnabled) {
                    _gpsStatus.value = GpsStatus.DISABLED
                    Log.w(TAG, "Both GPS and NETWORK providers are disabled")
                }

                locationListenerRegistered = true

                // Use last known location as initial position while waiting for GPS fix
                val lastKnown = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                    ?: locationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)

                val initialLat = lastKnown?.latitude ?: 0.0
                val initialLon = lastKnown?.longitude ?: 0.0

                if (lastKnown != null) {
                    _userPosition.value = Position(
                        latitude = initialLat,
                        longitude = initialLon,
                        altitudeMeters = lastKnown.altitude
                    )
                    _gpsStatus.value = GpsStatus.LOCKED
                    Log.d(TAG, "Using last known location: ($initialLat, $initialLon)")
                }

                // Don't start ADS-B polling if we have no valid position
                if (initialLat != 0.0 || initialLon != 0.0) {
                    skyObjectRepository.ensureStarted(initialLat, initialLon)
                } else {
                    // Start BLE/WiFi scanners only (they don't need GPS)
                    skyObjectRepository.ensureStarted(0.0, 0.0)
                }
            } catch (e: SecurityException) {
                Log.e(TAG, "Location permission not granted", e)
                _gpsStatus.value = GpsStatus.NO_PERMISSION
            }
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
                        // Close previous session to avoid GL/native memory leak
                        arSession?.let { old ->
                            try { old.close() } catch (_: Exception) {}
                            arSession = null
                        }

                        // Ensure ARCore is installed / up to date
                        val installStatus = ArCoreApk.getInstance()
                            .requestInstall(activity, true)
                        if (installStatus == ArCoreApk.InstallStatus.INSTALLED) {
                            val session = Session(activity)
                            arSession = session
                            session.resume()
                            arCoreOrientationProvider.setSession(session)
                            startArCoreUpdateLoop()
                            updateArCoreTrackingState()
                            Log.i(TAG, "ARCore session created and resumed")
                        } else {
                            // Install was requested; will complete on next resume
                            _arCoreStatus.value = ArCoreStatus.INITIALIZING
                            Log.i(TAG, "ARCore install requested, waiting for completion")
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "ARCore session creation failed: ${e.message}")
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

    private var detectionStatusJob: kotlinx.coroutines.Job? = null

    /**
     * Poll the visual detection analyzer for dark mode status.
     * Updates the UI state flows at ~2 Hz (low overhead).
     */
    private fun startDetectionStatusPolling() {
        detectionStatusJob?.cancel()
        detectionStatusJob = viewModelScope.launch {
            while (isActive) {
                _isDarkMode.value = visualDetectionAnalyzer.isDarkMode
                delay(500L)
            }
        }
    }

    private var weatherPollJob: kotlinx.coroutines.Job? = null

    /**
     * Poll weather conditions every 10 minutes and update confidence multiplier.
     */
    private fun startWeatherPolling() {
        weatherPollJob?.cancel()
        weatherPollJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                val pos = _userPosition.value
                if (pos.latitude != 0.0 || pos.longitude != 0.0) {
                    try {
                        val weather = weatherRepository.getWeather(pos.latitude, pos.longitude)
                        if (weather != null) {
                            val range = WeatherAdjustment.fromWeatherConditions(
                                visibilityMeters = weather.visibilityMeters,
                                cloudCoverPercent = weather.cloudCoverPercent,
                                precipitationType = weather.precipitationType
                            )
                            _weatherRange.value = range
                            applyConfidenceMultiplier()
                            Log.d(TAG, "Weather updated: ${range.description}, auto multiplier=${range.confidenceMultiplier}")
                        } else {
                            Log.d(TAG, "Weather unavailable, using manual override or default")
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Weather fetch failed, using defaults", e)
                    }
                }
                delay(10 * 60 * 1000L) // 10 minutes
            }
        }
    }

    private var arCoreUpdateJob: kotlinx.coroutines.Job? = null

    /**
     * Start a coroutine that updates ARCore orientation at ~30fps.
     */
    private fun startArCoreUpdateLoop() {
        arCoreUpdateJob?.cancel()
        arCoreUpdateJob = viewModelScope.launch(Dispatchers.Default) {
            while (isActive) {
                val orient = sensorFusionEngine.orientation.value
                arCoreOrientationProvider.update(orient.azimuthDegrees, orient.pitchDegrees)
                delay(33L) // ~30fps
            }
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
            val camera = frame.camera ?: return
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
        Log.i(TAG, "Stopping AR sensors (scanning continues for other screens)")
        sensorFusionEngine.stop()
        arCoreOrientationProvider.stop()

        // Cancel ARCore update job FIRST, then pause session (avoids race on session.update())
        arCoreUpdateJob?.cancel()
        arCoreUpdateJob = null

        weatherPollJob?.cancel()
        detectionStatusJob?.cancel()
        acousticJob?.cancel()
        visualCorrelationEngine.reset()
        trajectoryPredictor.reset()
        compassBiasEstimator.reset()

        // Now safe to pause ARCore session (no concurrent update() calls)
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
        locationListenerRegistered = false
    }

    override fun onCleared() {
        super.onCleared()
        stopSensors()
        // Remove zoom observer to prevent leak
        cameraRef?.cameraInfo?.zoomState?.let { liveData ->
            zoomObserver?.let { liveData.removeObserver(it) }
        }
        visualDetectionAnalyzer.close()

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

/** Entry in the detection log panel. */
data class DetectionLogEntry(
    val timestamp: Instant,
    val source: DetectionSource,
    val label: String,
    val detail: String,
    val isOnScreen: Boolean
)

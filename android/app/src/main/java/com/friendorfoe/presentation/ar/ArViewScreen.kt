package com.friendorfoe.presentation.ar

import android.util.Log
import android.view.ViewGroup
import androidx.camera.core.Camera
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import com.friendorfoe.detection.VisualDetectionAnalyzer
import java.util.concurrent.Executors
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import android.app.Activity
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.detection.AlertLevel
import com.friendorfoe.detection.ClassifiedVisualDetection
import com.friendorfoe.detection.DataSourceStatus
import com.friendorfoe.detection.VisualClassification
import com.friendorfoe.detection.VisualDetection
import com.friendorfoe.detection.VisualDetectionRange
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.detail.AircraftDetailContent
import com.friendorfoe.presentation.detail.DetailState
import com.friendorfoe.presentation.detail.DetailViewModel
import com.friendorfoe.presentation.detail.DroneDetailContent
import com.friendorfoe.presentation.util.categoryBadge
import com.friendorfoe.presentation.util.categoryColor
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.sensor.ScreenPosition
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * AR Viewfinder screen -- the main screen of the app.
 *
 * Layers (bottom to top):
 * 1. CameraX full-screen preview (background)
 * 2. Transparent Canvas with floating labels at screen positions
 * 3. Status bar at the bottom
 *
 * Each label is color-coded by category, shows callsign/ID and altitude,
 * and is tappable to navigate to the detail screen.
 *
 * @param onObjectTapped Callback when a sky object label is tapped, receives object ID
 * @param viewModel The AR ViewModel, injected via Hilt
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ArViewScreen(
    onObjectTapped: (String) -> Unit,
    viewModel: ArViewModel = hiltViewModel(),
    detailViewModel: DetailViewModel = hiltViewModel()
) {
    val lifecycleOwner = LocalLifecycleOwner.current
    val activity = LocalContext.current as? Activity

    // Collect state from ViewModel
    val screenPositions by viewModel.screenPositions.collectAsStateWithLifecycle()
    val aircraftCount by viewModel.aircraftCount.collectAsStateWithLifecycle()
    val droneCount by viewModel.droneCount.collectAsStateWithLifecycle()
    val militaryCount by viewModel.militaryCount.collectAsStateWithLifecycle()
    val emergencyCount by viewModel.emergencyCount.collectAsStateWithLifecycle()
    val gpsStatus by viewModel.gpsStatus.collectAsStateWithLifecycle()
    val arCoreStatus by viewModel.arCoreStatus.collectAsStateWithLifecycle()
    val selectedObjectId by viewModel.selectedObjectId.collectAsStateWithLifecycle()
    val detailState by detailViewModel.detailState.collectAsStateWithLifecycle()
    val nearbyCandidates by detailViewModel.nearbyCandidates.collectAsStateWithLifecycle()
    val positionTrail by detailViewModel.positionTrail.collectAsStateWithLifecycle()
    val dataSourceStatus by viewModel.dataSourceStatus.collectAsStateWithLifecycle()
    val detectionLog by viewModel.detectionLog.collectAsStateWithLifecycle()
    val detectionLogExpanded by viewModel.detectionLogExpanded.collectAsStateWithLifecycle()
    val orientation by viewModel.orientation.collectAsStateWithLifecycle()
    val sensorAccuracy by viewModel.sensorAccuracy.collectAsStateWithLifecycle()
    val showUnidentifiedSheet by viewModel.showUnidentifiedSheet.collectAsStateWithLifecycle()
    val detectedDrones by viewModel.detectedDrones.collectAsStateWithLifecycle()
    val unmatchedVisuals by viewModel.unmatchedVisuals.collectAsStateWithLifecycle()
    val classifiedUnknowns by viewModel.classifiedUnknowns.collectAsStateWithLifecycle()
    val visualCount by viewModel.visualCount.collectAsStateWithLifecycle()
    val alertCount by viewModel.alertCount.collectAsStateWithLifecycle()
    val zoomTarget by viewModel.zoomTarget.collectAsStateWithLifecycle()
    val weatherRange by viewModel.weatherRange.collectAsStateWithLifecycle()
    val rangeOverride by viewModel.rangeOverride.collectAsStateWithLifecycle()
    val isDarkMode by viewModel.isDarkMode.collectAsStateWithLifecycle()
    val strobeCount by viewModel.strobeCount.collectAsStateWithLifecycle()
    val currentZoomRatio by viewModel.currentZoomRatio.collectAsStateWithLifecycle()
    val maxZoomRatio by viewModel.maxZoomRatio.collectAsStateWithLifecycle()
    val lockedObjectId by viewModel.lockedObjectId.collectAsStateWithLifecycle()
    val lockedScreenPosition by viewModel.lockedScreenPosition.collectAsStateWithLifecycle()

    // Manage sensor lifecycle: start on resume, stop on pause
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> viewModel.startSensors(activity)
                Lifecycle.Event.ON_PAUSE -> viewModel.stopSensors()
                else -> {}
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            // Stop sensors when leaving AR view (e.g., tab switch)
            viewModel.stopSensors()
        }
    }

    // Load detail when an object is selected
    LaunchedEffect(selectedObjectId) {
        selectedObjectId?.let { detailViewModel.loadDetail(it) }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        // Layer 1: CameraX Preview + ImageAnalysis
        CameraPreview(
            visualDetectionAnalyzer = viewModel.visualDetectionAnalyzer,
            onCameraReady = { camera -> viewModel.setCameraRef(camera) },
            modifier = Modifier.fillMaxSize()
        )

        // Layer 2: AR Overlay with floating labels + visual bounding boxes
        ArOverlay(
            screenPositions = screenPositions,
            unmatchedVisuals = unmatchedVisuals,
            classifiedUnknowns = classifiedUnknowns,
            lockedObjectId = lockedObjectId,
            lockedScreenPosition = lockedScreenPosition,
            orientation = orientation,
            onLabelTapped = { objectId -> viewModel.selectObject(objectId) },
            onLabelLongPressed = { objectId -> viewModel.lockOnObject(objectId) },
            onVisualTapped = { detection -> viewModel.showZoom(detection) },
            onEmptySpaceTapped = { viewModel.showUnidentifiedSheet() },
            onReticleTapped = { viewModel.unlockObject() },
            modifier = Modifier.fillMaxSize()
        )

        // Layer 3: Compass strip at top
        CompassOverlay(
            azimuthDegrees = orientation.azimuthDegrees,
            sensorAccuracy = sensorAccuracy,
            pitchDegrees = orientation.pitchDegrees,
            modifier = Modifier
                .fillMaxWidth()
                .align(Alignment.TopCenter)
        )

        // Lock-on HUD badge: shown when an object is locked
        if (lockedObjectId != null) {
            val lockedLabel = screenPositions
                .firstOrNull { it.skyObject.id == lockedObjectId }
                ?.let { sp ->
                    when (val obj = sp.skyObject) {
                        is Aircraft -> obj.callsign ?: obj.icaoHex
                        is Drone -> obj.droneId.take(12)
                    }
                } ?: "..."
            Row(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 56.dp)
                    .background(
                        color = Color(0xDD00BCD4),
                        shape = RoundedCornerShape(20.dp)
                    )
                    .padding(horizontal = 14.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = "LOCKED: $lockedLabel",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "X",
                    color = Color.White.copy(alpha = 0.8f),
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .clickable { viewModel.unlockObject() }
                        .padding(horizontal = 4.dp)
                )
            }
        }

        // Ground-pointing banner: shown when camera aims below horizon
        if (orientation.pitchDegrees < -10f) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .align(Alignment.Center)
                    .padding(horizontal = 32.dp)
                    .background(
                        color = Color(0xBBFF8F00),
                        shape = RoundedCornerShape(12.dp)
                    )
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "Camera pointing below horizon \u2014 aim higher to detect aircraft",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center
                )
            }
        }

        // Layer 4: Status bar + detection log at bottom
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .align(Alignment.BottomCenter)
        ) {
            // Expandable detection log panel
            AnimatedVisibility(
                visible = detectionLogExpanded,
                enter = slideInVertically(initialOffsetY = { it }),
                exit = slideOutVertically(targetOffsetY = { it })
            ) {
                DetectionLogPanel(
                    entries = detectionLog,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            // Status bar (tappable to toggle detection log)
            StatusBar(
                aircraftCount = aircraftCount,
                droneCount = droneCount,
                militaryCount = militaryCount,
                emergencyCount = emergencyCount,
                visualCount = visualCount,
                alertCount = alertCount,
                gpsStatus = gpsStatus,
                arCoreStatus = arCoreStatus,
                dataSourceStatus = dataSourceStatus,
                weatherRange = weatherRange,
                rangeOverride = rangeOverride,
                isDarkMode = isDarkMode,
                strobeCount = strobeCount,
                onRangeOverrideChange = { viewModel.setRangeOverride(it) },
                onTap = { viewModel.toggleDetectionLog() },
                modifier = Modifier.fillMaxWidth()
            )
        }
    }

    // Auto-zoom when detail bottom sheet opens
    LaunchedEffect(selectedObjectId) {
        if (selectedObjectId != null) {
            val sp = screenPositions.firstOrNull { it.skyObject.id == selectedObjectId }
            if (sp != null) {
                viewModel.zoomToObject(sp.distanceMeters)
            }
        } else {
            viewModel.resetZoom()
        }
    }

    // Bottom sheet for detail card
    if (selectedObjectId != null) {
        val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)

        ModalBottomSheet(
            onDismissRequest = {
                viewModel.selectObject(null)
                viewModel.resetZoom()
            },
            sheetState = sheetState
        ) {
            when (val state = detailState) {
                is DetailState.Loading -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(200.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                }
                is DetailState.AircraftLoaded -> {
                    AircraftDetailContent(
                        aircraft = state.aircraft,
                        detail = state.detail,
                        onZoom = {
                            val sp = screenPositions.firstOrNull { it.skyObject.id == state.aircraft.id }
                            if (sp != null) {
                                val synth = VisualDetection(
                                    trackingId = sp.skyObject.id.hashCode(),
                                    centerX = sp.screenX,
                                    centerY = sp.screenY,
                                    width = 0.15f,
                                    height = 0.15f,
                                    labels = listOf(state.aircraft.callsign ?: state.aircraft.icaoHex),
                                    timestampMs = System.currentTimeMillis()
                                )
                                viewModel.selectObject(null)
                                viewModel.showZoom(synth)
                            }
                        },
                        onLockOn = {
                            viewModel.selectObject(null)
                            viewModel.lockOnObject(state.aircraft.id)
                        }
                    )
                }
                is DetailState.DroneLoaded -> {
                    DroneDetailContent(
                        drone = state.drone,
                        nearbyCandidates = nearbyCandidates,
                        positionTrail = positionTrail,
                        onZoom = {
                            val sp = screenPositions.firstOrNull { it.skyObject.id == state.drone.id }
                            if (sp != null) {
                                val synth = VisualDetection(
                                    trackingId = sp.skyObject.id.hashCode(),
                                    centerX = sp.screenX,
                                    centerY = sp.screenY,
                                    width = 0.15f,
                                    height = 0.15f,
                                    labels = listOf(state.drone.manufacturer ?: state.drone.droneId),
                                    timestampMs = System.currentTimeMillis()
                                )
                                viewModel.selectObject(null)
                                viewModel.showZoom(synth)
                            }
                        },
                        onLockOn = {
                            viewModel.selectObject(null)
                            viewModel.lockOnObject(state.drone.id)
                        }
                    )
                }
                is DetailState.Error -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(200.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(text = "Error: ${state.message}")
                    }
                }
                is DetailState.Idle -> {
                    // Dismiss the sheet — Idle means nothing to show
                    LaunchedEffect(Unit) {
                        viewModel.selectObject(null)
                    }
                }
            }
        }
    }

    // Bottom sheet for zoom view (tapped visual detection)
    zoomTarget?.let { detection ->
        val classified = classifiedUnknowns.firstOrNull { it.detection.trackingId == detection.trackingId }
        ZoomViewSheet(
            detection = detection,
            classified = classified,
            getFrame = { viewModel.captureZoomFrame() },
            currentZoomRatio = currentZoomRatio,
            maxZoomRatio = maxZoomRatio,
            onZoomChange = { viewModel.setZoomRatio(it) },
            onDismiss = { viewModel.dismissZoom() }
        )
    }

    // Bottom sheet for unidentified tap (empty space)
    if (showUnidentifiedSheet) {
        val unidentifiedSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)

        ModalBottomSheet(
            onDismissRequest = { viewModel.dismissUnidentifiedSheet() },
            sheetState = unidentifiedSheetState
        ) {
            UnidentifiedTapContent(
                drones = detectedDrones,
                onDroneSelected = { droneId ->
                    viewModel.dismissUnidentifiedSheet()
                    viewModel.selectObject(droneId)
                }
            )
        }
    }
}

// ---- CameraX Preview ----

/**
 * Full-screen CameraX preview using AndroidView wrapping PreviewView.
 *
 * Binds the camera preview use case to the lifecycle owner so it
 * automatically starts/stops with the composable lifecycle.
 */
@Composable
private fun CameraPreview(
    visualDetectionAnalyzer: VisualDetectionAnalyzer,
    onCameraReady: (Camera) -> Unit = {},
    modifier: Modifier = Modifier
) {
    val lifecycleOwner = LocalLifecycleOwner.current
    val analysisExecutor = remember { Executors.newSingleThreadExecutor() }

    DisposableEffect(Unit) {
        onDispose {
            analysisExecutor.shutdown()
        }
    }

    AndroidView(
        factory = { ctx ->
            val previewView = PreviewView(ctx).apply {
                layoutParams = ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT
                )
                scaleType = PreviewView.ScaleType.FILL_CENTER
                implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            }

            // Bind camera in factory so it only runs once
            val cameraProviderFuture = ProcessCameraProvider.getInstance(ctx)
            cameraProviderFuture.addListener({
                try {
                    val cameraProvider = cameraProviderFuture.get()

                    val preview = Preview.Builder()
                        .build()
                        .also { it.setSurfaceProvider(previewView.surfaceProvider) }

                    // ImageAnalysis for ML Kit visual detection
                    val imageAnalysis = ImageAnalysis.Builder()
                        .setTargetResolution(android.util.Size(1920, 1080))
                        .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                        .build()
                        .also {
                            it.setAnalyzer(
                                analysisExecutor,
                                visualDetectionAnalyzer
                            )
                        }

                    // Use back camera (pointing at sky)
                    val cameraSelector = CameraSelector.DEFAULT_BACK_CAMERA

                    // Unbind all and rebind with both Preview and ImageAnalysis
                    cameraProvider.unbindAll()
                    val camera = cameraProvider.bindToLifecycle(
                        lifecycleOwner,
                        cameraSelector,
                        preview,
                        imageAnalysis
                    )
                    onCameraReady(camera)
                } catch (e: Exception) {
                    Log.e("CameraPreview", "Camera initialization failed", e)
                }
            }, ContextCompat.getMainExecutor(ctx))

            previewView
        },
        modifier = modifier
    )
}

// ---- AR Overlay ----

/**
 * Transparent Canvas overlay that draws floating labels at screen positions.
 *
 * Features:
 * - Color-coded rounded rect labels based on ObjectCategory
 * - Animated positions for smooth movement during phone panning
 * - Overlap avoidance: labels that would overlap are pushed down
 * - Tap detection: tapping a label triggers [onLabelTapped]
 *
 * @param screenPositions List of screen positions to render
 * @param onLabelTapped Callback with sky object ID when a label is tapped
 */
@Composable
private fun ArOverlay(
    screenPositions: List<ScreenPosition>,
    unmatchedVisuals: List<VisualDetection>,
    classifiedUnknowns: List<ClassifiedVisualDetection>,
    lockedObjectId: String?,
    lockedScreenPosition: ScreenPosition?,
    orientation: com.friendorfoe.sensor.DeviceOrientation,
    onLabelTapped: (String) -> Unit,
    onLabelLongPressed: (String) -> Unit,
    onVisualTapped: (VisualDetection) -> Unit,
    onEmptySpaceTapped: () -> Unit,
    onReticleTapped: () -> Unit,
    modifier: Modifier = Modifier
) {
    // Filter to only in-view objects
    val visiblePositions = remember(screenPositions) {
        screenPositions.filter { it.isInView }
    }

    // Build lookup for classified unknowns by tracking ID
    val classifiedMap = remember(classifiedUnknowns) {
        classifiedUnknowns.associateBy { it.detection.trackingId }
    }

    // Pulsing animation for ALERT-level detections
    val infiniteTransition = rememberInfiniteTransition(label = "alertPulse")
    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.4f,
        targetValue = 1.0f,
        animationSpec = infiniteRepeatable(
            animation = tween(600),
            repeatMode = RepeatMode.Reverse
        ),
        label = "pulseAlpha"
    )

    // Animate each label's position for smooth movement
    val animatedPositions = visiblePositions.map { pos ->
        val animatedX by animateFloatAsState(
            targetValue = pos.screenX,
            animationSpec = tween(durationMillis = 150),
            label = "labelX_${pos.skyObject.id}"
        )
        val animatedY by animateFloatAsState(
            targetValue = pos.screenY,
            animationSpec = tween(durationMillis = 150),
            label = "labelY_${pos.skyObject.id}"
        )
        AnimatedLabelData(
            screenPosition = pos,
            animatedX = animatedX,
            animatedY = animatedY
        )
    }

    // Track label bounding rects for tap detection
    var labelRects by remember { mutableStateOf<List<LabelHitTarget>>(emptyList()) }
    // Keep detection references for visual tap targets
    var visualHitDetections by remember { mutableStateOf<Map<String, VisualDetection>>(emptyMap()) }

    Canvas(
        modifier = modifier
            .pointerInput(Unit) {
                detectTapGestures(
                    onTap = { offset ->
                        // Check if tap hit the reticle (locked object in view) → unlock
                        if (lockedObjectId != null && lockedScreenPosition?.isInView == true) {
                            val reticleX = lockedScreenPosition.screenX * size.width
                            val reticleY = lockedScreenPosition.screenY * size.height
                            val reticleRadius = 60f
                            val dx = offset.x - reticleX
                            val dy = offset.y - reticleY
                            if (dx * dx + dy * dy <= reticleRadius * reticleRadius) {
                                onReticleTapped()
                                return@detectTapGestures
                            }
                        }
                        // Check if tap hit any label
                        labelRects.forEach { target ->
                            if (target.rect.contains(offset)) {
                                if (target.objectId.startsWith("visual_")) {
                                    val detection = visualHitDetections[target.objectId]
                                    if (detection != null) {
                                        onVisualTapped(detection)
                                    } else {
                                        onEmptySpaceTapped()
                                    }
                                } else {
                                    onLabelTapped(target.objectId)
                                }
                                return@detectTapGestures
                            }
                        }
                        // No direct hit — user tapped empty space
                        onEmptySpaceTapped()
                    },
                    onLongPress = { offset ->
                        // Long-press on a label → lock-on
                        labelRects.forEach { target ->
                            if (target.rect.contains(offset) && !target.objectId.startsWith("visual_")) {
                                onLabelLongPressed(target.objectId)
                                return@detectTapGestures
                            }
                        }
                    }
                )
            }
    ) {
        val canvasWidth = size.width
        val canvasHeight = size.height
        val hitTargets = mutableListOf<LabelHitTarget>()
        val detectionRefs = mutableMapOf<String, VisualDetection>()

        // Layer 1: Bounding boxes for matched radio objects
        animatedPositions.forEach { labelInfo ->
            val detection = labelInfo.screenPosition.matchedDetection
            if (detection != null) {
                val color = categoryColor(labelInfo.screenPosition.skyObject.category)
                drawBoundingBox(detection, color, canvasWidth, canvasHeight)
            }
        }

        // Layer 2: Classified visual detections with classification-based colors
        unmatchedVisuals.forEach { visual ->
            val classified = classifiedMap[visual.trackingId]
            val classification = classified?.classification
                ?: visual.visualClassification
                ?: VisualClassification.UNKNOWN_FLYING

            // Skip static detections
            if (classification == VisualClassification.LIKELY_STATIC) return@forEach

            val alertLevel = classified?.alertLevel ?: AlertLevel.NORMAL
            val boxColor = classificationBoxColor(classification)

            // Use pulsing alpha for ALERT-level
            val effectiveAlpha = if (alertLevel == AlertLevel.ALERT) pulseAlpha else 1.0f
            drawBoundingBox(visual, boxColor.copy(alpha = boxColor.alpha * effectiveAlpha), canvasWidth, canvasHeight)

            // Draw classification tag
            val tagText = classificationTagText(classification)
            val tagRect = drawClassifiedTag(visual, tagText, boxColor, canvasWidth, canvasHeight)
            val visualId = "visual_${visual.trackingId ?: visual.timestampMs}"
            hitTargets.add(LabelHitTarget(objectId = visualId, rect = tagRect))
            detectionRefs[visualId] = visual
        }

        // Layer 3: Radio labels on top (with overlap avoidance)
        val resolvedPositions = resolveOverlaps(animatedPositions, canvasWidth, canvasHeight)

        resolvedPositions.forEach { labelInfo ->
            val isLocked = lockedObjectId != null &&
                labelInfo.screenPosition.skyObject.id == lockedObjectId
            val isDimmed = lockedObjectId != null && !isLocked
            val rect = drawLabel(
                labelInfo = labelInfo,
                canvasWidth = canvasWidth,
                canvasHeight = canvasHeight,
                isLocked = isLocked,
                isDimmed = isDimmed,
                pulseAlpha = pulseAlpha
            )
            hitTargets.add(
                LabelHitTarget(
                    objectId = labelInfo.screenPosition.skyObject.id,
                    rect = rect
                )
            )
        }

        // Layer 4: Off-screen directional arrows for nearby objects outside FOV
        val offScreenObjects = screenPositions.filter {
            !it.isInView && it.distanceMeters > 0 && it.distanceMeters <= 13_000.0
        }.sortedBy { it.distanceMeters }.take(8)

        offScreenObjects.forEach { sp ->
            val arrowRect = drawEdgeArrow(
                screenPosition = sp,
                canvasWidth = canvasWidth,
                canvasHeight = canvasHeight
            )
            hitTargets.add(
                LabelHitTarget(
                    objectId = sp.skyObject.id,
                    rect = arrowRect
                )
            )
        }

        // Layer 5: Lock-on reticle or guidance arrow
        if (lockedObjectId != null) {
            val lockedSp = lockedScreenPosition
            if (lockedSp != null && lockedSp.isInView) {
                // Object is in view — draw animated reticle
                drawReticle(
                    centerX = lockedSp.screenX * canvasWidth,
                    centerY = lockedSp.screenY * canvasHeight,
                    pulseAlpha = pulseAlpha
                )
            } else if (lockedSp != null) {
                // Object is off-screen — draw guidance arrow at center
                drawGuidanceArrow(
                    targetBearing = lockedSp.bearingDegrees,
                    targetElevation = lockedSp.elevationDegrees,
                    currentAzimuth = orientation.azimuthDegrees,
                    currentPitch = orientation.pitchDegrees,
                    canvasWidth = canvasWidth,
                    canvasHeight = canvasHeight,
                    pulseAlpha = pulseAlpha
                )
            }
        }

        labelRects = hitTargets
        visualHitDetections = detectionRefs
    }
}

/**
 * Draw a single floating label on the Canvas.
 *
 * Returns the bounding Rect for tap detection.
 */
private fun DrawScope.drawLabel(
    labelInfo: AnimatedLabelData,
    canvasWidth: Float,
    canvasHeight: Float,
    isLocked: Boolean = false,
    isDimmed: Boolean = false,
    pulseAlpha: Float = 1f
): Rect {
    val skyObject = labelInfo.screenPosition.skyObject
    val color = categoryColor(skyObject.category)
    val dimFactor = if (isDimmed) 0.4f else 1f

    // Label dimensions
    val labelWidth = 240f
    val labelHeight = if (isLocked) 74f else 60f
    val cornerRadius = 8f

    // Position: center the label at the animated screen coordinates
    val centerX = labelInfo.resolvedX * canvasWidth
    val centerY = labelInfo.resolvedY * canvasHeight
    val left = (centerX - labelWidth / 2).coerceIn(4f, canvasWidth - labelWidth - 4f)
    val top = (centerY - labelHeight / 2).coerceIn(4f, canvasHeight - labelHeight - 4f)

    // Draw background rounded rect with semi-transparency
    drawRoundRect(
        color = color.copy(alpha = 0.75f * dimFactor),
        topLeft = Offset(left, top),
        size = Size(labelWidth, labelHeight),
        cornerRadius = CornerRadius(cornerRadius, cornerRadius)
    )

    val isEmergency = skyObject.category == ObjectCategory.EMERGENCY

    // Draw border — cyan pulsing if locked, magenta pulsing if emergency,
    // bright green if visually confirmed, white otherwise
    val borderColor = if (isLocked) {
        Color(0xFF00BCD4).copy(alpha = pulseAlpha)  // cyan pulsing
    } else if (isEmergency) {
        Color(0xFFE91E63).copy(alpha = pulseAlpha)  // magenta pulsing — always visible
    } else if (labelInfo.screenPosition.visuallyConfirmed) {
        Color(0xFF76FF03).copy(alpha = dimFactor)  // bright green — visual confirmation
    } else {
        Color.White.copy(alpha = 0.6f * dimFactor)
    }
    val borderWidth = if (isLocked) 3f else if (isEmergency) 3f
        else if (labelInfo.screenPosition.visuallyConfirmed) 2.5f else 1.5f
    drawRoundRect(
        color = borderColor,
        topLeft = Offset(left, top),
        size = Size(labelWidth, labelHeight),
        cornerRadius = CornerRadius(cornerRadius, cornerRadius),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = borderWidth)
    )

    // Draw "LOCKED" badge for locked object
    if (isLocked) {
        val badgeW = 60f
        val badgeH = 16f
        val badgeLeft = left + labelWidth / 2f - badgeW / 2f
        val badgeTop = top + labelHeight - badgeH - 4f
        drawRoundRect(
            color = Color(0xFF00BCD4).copy(alpha = 0.9f),
            topLeft = Offset(badgeLeft, badgeTop),
            size = Size(badgeW, badgeH),
            cornerRadius = CornerRadius(4f, 4f)
        )
        drawContext.canvas.nativeCanvas.apply {
            val bp = android.graphics.Paint().apply {
                this.color = android.graphics.Color.WHITE
                textSize = 12f
                isAntiAlias = true
                typeface = android.graphics.Typeface.DEFAULT_BOLD
                textAlign = android.graphics.Paint.Align.CENTER
            }
            drawText("LOCKED", badgeLeft + badgeW / 2f, badgeTop + badgeH - 3f, bp)
        }
    }

    // Draw text content
    val primaryLine = getLabelText(skyObject)
    val secondaryLine = getSecondaryLabelText(skyObject, labelInfo.screenPosition.distanceMeters)

    // Compute category badge width before text layout to reserve space
    val catBadge = categoryBadge(skyObject.category)
    val catBadgeW = if (catBadge != null) catBadge.first.length * 10f + 8f else 0f
    val textBudget = labelWidth - 16f - if (catBadge != null) catBadgeW + 8f else 0f

    drawContext.canvas.nativeCanvas.apply {
        val textAlpha = (255 * dimFactor).toInt()
        val textPaint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.argb(textAlpha, 255, 255, 255)
            textSize = 28f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            setShadowLayer(2f, 1f, 1f, android.graphics.Color.BLACK)
        }

        val subtextPaint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.argb(textAlpha, 255, 255, 255)
            textSize = 22f
            isAntiAlias = true
            setShadowLayer(2f, 1f, 1f, android.graphics.Color.BLACK)
        }

        // Primary text (callsign + type for aircraft, drone ID for drones)
        val primaryText = ellipsize(primaryLine, textPaint, textBudget)
        drawText(primaryText, left + 8f, top + 24f, textPaint)

        // Secondary text (altitude for aircraft, manufacturer for drones)
        val secondaryEllipsized = ellipsize(secondaryLine, subtextPaint, labelWidth - 16f)
        drawText(secondaryEllipsized, left + 8f, top + 50f, subtextPaint)

        // Category badge (MIL/GOV/HELI/EMG/CGO/GND) in top-right corner
        if (catBadge != null) {
            val (catLabel, catColor) = catBadge
            val catBadgeH = 18f
            val catBadgeLeft = left + labelWidth - catBadgeW - 4f
            val catBadgeTop = top + 3f
            // Badge background
            val catArgb = catColor.toArgb()
            val catBadgePaint = android.graphics.Paint().apply {
                this.color = android.graphics.Color.argb(
                    (230 * dimFactor).toInt(),
                    android.graphics.Color.red(catArgb),
                    android.graphics.Color.green(catArgb),
                    android.graphics.Color.blue(catArgb)
                )
                isAntiAlias = true
            }
            val catRect = android.graphics.RectF(
                catBadgeLeft, catBadgeTop,
                catBadgeLeft + catBadgeW, catBadgeTop + catBadgeH
            )
            drawRoundRect(catRect, 4f, 4f, catBadgePaint)
            // Badge text
            val catTextPaint = android.graphics.Paint().apply {
                this.color = android.graphics.Color.WHITE
                textSize = 13f
                isAntiAlias = true
                typeface = android.graphics.Typeface.DEFAULT_BOLD
                textAlign = android.graphics.Paint.Align.CENTER
            }
            drawText(catLabel, catBadgeLeft + catBadgeW / 2f, catBadgeTop + catBadgeH - 4f, catTextPaint)
        }

        // Source badge: small colored circle with source initial at bottom-right
        val (sourceBadgeColor, sourceBadgeChar) = when (skyObject.source) {
            DetectionSource.ADS_B -> android.graphics.Color.rgb(76, 175, 80) to "A"      // green
            DetectionSource.REMOTE_ID -> android.graphics.Color.rgb(156, 39, 176) to "B"  // purple
            DetectionSource.WIFI -> android.graphics.Color.rgb(33, 150, 243) to "W"       // blue
        }
        val badgeRadius = 9f
        val badgeCenterX = left + labelWidth - badgeRadius - 6f
        val badgeCenterY = top + labelHeight - badgeRadius - 4f
        val badgePaint = android.graphics.Paint().apply {
            this.color = sourceBadgeColor
            isAntiAlias = true
        }
        drawCircle(badgeCenterX, badgeCenterY, badgeRadius, badgePaint)
        val badgeTextPaint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.WHITE
            textSize = 14f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            textAlign = android.graphics.Paint.Align.CENTER
        }
        drawText(sourceBadgeChar, badgeCenterX, badgeCenterY + 5f, badgeTextPaint)
    }

    return Rect(left, top, left + labelWidth, top + labelHeight)
}

/**
 * Draw a bounding box around a visual detection with corner bracket L-shapes.
 */
private fun DrawScope.drawBoundingBox(
    detection: VisualDetection,
    color: Color,
    canvasWidth: Float,
    canvasHeight: Float
) {
    val left = (detection.centerX - detection.width / 2f) * canvasWidth
    val top = (detection.centerY - detection.height / 2f) * canvasHeight
    val boxWidth = detection.width * canvasWidth
    val boxHeight = detection.height * canvasHeight
    val stroke = androidx.compose.ui.graphics.drawscope.Stroke(width = 2f)
    val bracketLen = minOf(boxWidth, boxHeight, 40f) * 0.3f

    // Full rect (subtle)
    drawRoundRect(
        color = color.copy(alpha = 0.35f),
        topLeft = Offset(left, top),
        size = Size(boxWidth, boxHeight),
        cornerRadius = CornerRadius(4f, 4f),
        style = stroke
    )

    // Corner brackets (bright)
    val bracketColor = color.copy(alpha = 0.9f)
    val bracketStroke = androidx.compose.ui.graphics.drawscope.Stroke(width = 3f)
    val right = left + boxWidth
    val bottom = top + boxHeight

    // Top-left
    drawLine(bracketColor, Offset(left, top), Offset(left + bracketLen, top), strokeWidth = 3f)
    drawLine(bracketColor, Offset(left, top), Offset(left, top + bracketLen), strokeWidth = 3f)
    // Top-right
    drawLine(bracketColor, Offset(right, top), Offset(right - bracketLen, top), strokeWidth = 3f)
    drawLine(bracketColor, Offset(right, top), Offset(right, top + bracketLen), strokeWidth = 3f)
    // Bottom-left
    drawLine(bracketColor, Offset(left, bottom), Offset(left + bracketLen, bottom), strokeWidth = 3f)
    drawLine(bracketColor, Offset(left, bottom), Offset(left, bottom - bracketLen), strokeWidth = 3f)
    // Bottom-right
    drawLine(bracketColor, Offset(right, bottom), Offset(right - bracketLen, bottom), strokeWidth = 3f)
    drawLine(bracketColor, Offset(right, bottom), Offset(right, bottom - bracketLen), strokeWidth = 3f)
}

/**
 * Draw a classification-colored tag above a visual detection's bounding box.
 *
 * Returns the bounding Rect for tap detection.
 */
private fun DrawScope.drawClassifiedTag(
    detection: VisualDetection,
    tagText: String,
    color: Color,
    canvasWidth: Float,
    canvasHeight: Float
): Rect {
    val boxLeft = (detection.centerX - detection.width / 2f) * canvasWidth
    val boxTop = (detection.centerY - detection.height / 2f) * canvasHeight
    val boxWidth = detection.width * canvasWidth

    val tagWidth = 90f
    val tagHeight = 28f
    val tagLeft = (boxLeft + boxWidth / 2f - tagWidth / 2f).coerceIn(4f, canvasWidth - tagWidth - 4f)
    val tagTop = (boxTop - tagHeight - 4f).coerceAtLeast(4f)

    // Tag background
    drawRoundRect(
        color = color.copy(alpha = 0.8f),
        topLeft = Offset(tagLeft, tagTop),
        size = Size(tagWidth, tagHeight),
        cornerRadius = CornerRadius(6f, 6f)
    )

    // Tag text
    drawContext.canvas.nativeCanvas.apply {
        val paint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.WHITE
            textSize = 20f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            textAlign = android.graphics.Paint.Align.CENTER
        }
        drawText(
            tagText.take(10),
            tagLeft + tagWidth / 2f,
            tagTop + tagHeight - 6f,
            paint
        )
    }

    return Rect(tagLeft, tagTop, tagLeft + tagWidth, tagTop + tagHeight)
}

/**
 * Draw a small colored arrow at the nearest screen edge for an off-screen object.
 * Shows callsign/ID + distance to help user know which way to point the phone.
 * Returns the bounding Rect for tap detection.
 */
private fun DrawScope.drawEdgeArrow(
    screenPosition: ScreenPosition,
    canvasWidth: Float,
    canvasHeight: Float
): Rect {
    val skyObject = screenPosition.skyObject
    val color = categoryColor(skyObject.category)
    val margin = 40f
    val arrowSize = 14f

    // Determine edge position from raw (unclamped) screen coordinates
    // screenX/screenY are clamped to 0..1, so use them to find the edge
    val rawX = screenPosition.screenX * canvasWidth
    val rawY = screenPosition.screenY * canvasHeight

    // Clamp to screen edges with margin
    val edgeX = rawX.coerceIn(margin, canvasWidth - margin)
    val edgeY = rawY.coerceIn(margin + 60f, canvasHeight - margin - 80f) // avoid compass/status bar

    // Determine arrow direction (pointing toward the object, outward from screen center)
    val centerX = canvasWidth / 2f
    val centerY = canvasHeight / 2f
    val dx = rawX - centerX
    val dy = rawY - centerY
    val angle = atan2(dy, dx)

    // Draw arrow triangle pointing outward
    val arrowPath = Path().apply {
        moveTo(edgeX + cos(angle) * arrowSize, edgeY + sin(angle) * arrowSize)
        lineTo(
            edgeX + cos(angle + 2.4f) * arrowSize,
            edgeY + sin(angle + 2.4f) * arrowSize
        )
        lineTo(
            edgeX + cos(angle - 2.4f) * arrowSize,
            edgeY + sin(angle - 2.4f) * arrowSize
        )
        close()
    }
    drawPath(arrowPath, color.copy(alpha = 0.9f))

    // Draw mini label: callsign + distance
    val label = when (skyObject) {
        is Aircraft -> skyObject.callsign ?: skyObject.icaoHex
        is Drone -> skyObject.droneId.take(8)
    }
    val distStr = formatDistance(screenPosition.distanceMeters)
    val labelText = if (distStr.isNotEmpty()) "$label $distStr" else label

    val tagWidth = 120f
    val tagHeight = 24f
    // Position tag so it doesn't overlap the arrow
    val tagOffsetX = if (edgeX < canvasWidth / 2) arrowSize + 4f else -(tagWidth + arrowSize + 4f)
    val tagLeft = (edgeX + tagOffsetX).coerceIn(4f, canvasWidth - tagWidth - 4f)
    val tagTop = (edgeY - tagHeight / 2f).coerceIn(4f, canvasHeight - tagHeight - 4f)

    drawRoundRect(
        color = color.copy(alpha = 0.7f),
        topLeft = Offset(tagLeft, tagTop),
        size = Size(tagWidth, tagHeight),
        cornerRadius = CornerRadius(4f, 4f)
    )

    drawContext.canvas.nativeCanvas.apply {
        val paint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.WHITE
            textSize = 18f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            setShadowLayer(2f, 1f, 1f, android.graphics.Color.BLACK)
        }
        val displayText = ellipsize(labelText, paint, tagWidth - 8f)
        drawText(displayText, tagLeft + 4f, tagTop + tagHeight - 5f, paint)
    }

    return Rect(tagLeft - arrowSize, tagTop, tagLeft + tagWidth + arrowSize, tagTop + tagHeight)
}

/**
 * Draw an animated lock-on reticle (crosshair) when the locked object is in view.
 */
private fun DrawScope.drawReticle(
    centerX: Float,
    centerY: Float,
    pulseAlpha: Float
) {
    val cyan = Color(0xFF00BCD4)
    val outerRadius = 50f
    val innerRadius = 20f
    val crossLen = 35f

    // Outer circle (pulsing)
    drawCircle(
        color = cyan.copy(alpha = 0.5f * pulseAlpha),
        radius = outerRadius,
        center = Offset(centerX, centerY),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f)
    )

    // Inner circle
    drawCircle(
        color = cyan.copy(alpha = 0.8f * pulseAlpha),
        radius = innerRadius,
        center = Offset(centerX, centerY),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2f)
    )

    // Crosshair lines (4 segments with gap at center)
    val gap = innerRadius + 4f
    val lineAlpha = 0.9f * pulseAlpha
    // Top
    drawLine(cyan.copy(alpha = lineAlpha), Offset(centerX, centerY - gap), Offset(centerX, centerY - gap - crossLen), strokeWidth = 2f)
    // Bottom
    drawLine(cyan.copy(alpha = lineAlpha), Offset(centerX, centerY + gap), Offset(centerX, centerY + gap + crossLen), strokeWidth = 2f)
    // Left
    drawLine(cyan.copy(alpha = lineAlpha), Offset(centerX - gap, centerY), Offset(centerX - gap - crossLen, centerY), strokeWidth = 2f)
    // Right
    drawLine(cyan.copy(alpha = lineAlpha), Offset(centerX + gap, centerY), Offset(centerX + gap + crossLen, centerY), strokeWidth = 2f)

    // Center dot
    drawCircle(
        color = cyan.copy(alpha = pulseAlpha),
        radius = 4f,
        center = Offset(centerX, centerY)
    )
}

/**
 * Draw a guidance arrow at screen center pointing toward the locked off-screen object,
 * with a text readout showing how far to pan.
 */
private fun DrawScope.drawGuidanceArrow(
    targetBearing: Float,
    targetElevation: Float,
    currentAzimuth: Float,
    currentPitch: Float,
    canvasWidth: Float,
    canvasHeight: Float,
    pulseAlpha: Float
) {
    val cyan = Color(0xFF00BCD4)
    val cx = canvasWidth / 2f
    val cy = canvasHeight / 2f

    // Calculate angular deltas
    var bearingDelta = targetBearing - currentAzimuth
    if (bearingDelta > 180f) bearingDelta -= 360f
    if (bearingDelta < -180f) bearingDelta += 360f
    val elevationDelta = targetElevation - currentPitch

    // Angle from center toward the target
    val angle = atan2(-elevationDelta, bearingDelta)  // negative elevation because screen Y is inverted

    val arrowLen = 60f
    val arrowTipX = cx + cos(angle) * 80f
    val arrowTipY = cy + sin(angle) * 80f

    // Draw arrow triangle
    val arrowPath = Path().apply {
        moveTo(arrowTipX + cos(angle) * arrowLen, arrowTipY + sin(angle) * arrowLen)
        lineTo(
            arrowTipX + cos(angle + 2.5f) * arrowLen * 0.5f,
            arrowTipY + sin(angle + 2.5f) * arrowLen * 0.5f
        )
        lineTo(
            arrowTipX + cos(angle - 2.5f) * arrowLen * 0.5f,
            arrowTipY + sin(angle - 2.5f) * arrowLen * 0.5f
        )
        close()
    }
    drawPath(arrowPath, cyan.copy(alpha = 0.8f * pulseAlpha))

    // Draw arrow shaft
    drawLine(
        color = cyan.copy(alpha = 0.6f * pulseAlpha),
        start = Offset(cx, cy),
        end = Offset(arrowTipX, arrowTipY),
        strokeWidth = 3f
    )

    // Guidance text
    val dirParts = mutableListOf<String>()
    if (kotlin.math.abs(bearingDelta) > 2f) {
        val dir = if (bearingDelta > 0) "right" else "left"
        dirParts.add("Pan $dir ${kotlin.math.abs(bearingDelta).toInt()}\u00B0")
    }
    if (kotlin.math.abs(elevationDelta) > 2f) {
        val dir = if (elevationDelta > 0) "up" else "down"
        dirParts.add("$dir ${kotlin.math.abs(elevationDelta).toInt()}\u00B0")
    }
    val guidanceText = dirParts.joinToString(", ").ifEmpty { "Nearby" }

    drawContext.canvas.nativeCanvas.apply {
        val paint = android.graphics.Paint().apply {
            color = android.graphics.Color.WHITE
            textSize = 24f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            textAlign = android.graphics.Paint.Align.CENTER
            setShadowLayer(3f, 1f, 1f, android.graphics.Color.BLACK)
        }
        drawText(guidanceText, cx, cy + 100f, paint)
    }

    // Outer pulsing ring at center
    drawCircle(
        color = cyan.copy(alpha = 0.3f * pulseAlpha),
        radius = 90f,
        center = Offset(cx, cy),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.5f)
    )
}

/** Color for visual detection bounding box based on classification. */
private fun classificationBoxColor(classification: VisualClassification): Color {
    return when (classification) {
        VisualClassification.LIKELY_DRONE -> Color(0xFFF44336)   // red
        VisualClassification.UNKNOWN_FLYING -> Color(0xFFFF9800) // amber
        VisualClassification.LIKELY_BIRD -> Color(0xFF757575)    // dim gray
        VisualClassification.LIKELY_AIRCRAFT -> Color(0xFF4CAF50) // green
        VisualClassification.LIKELY_STATIC -> Color.Transparent
    }
}

/** Tag text for visual detection based on classification. */
private fun classificationTagText(classification: VisualClassification): String {
    return when (classification) {
        VisualClassification.LIKELY_DRONE -> "DRONE?"
        VisualClassification.UNKNOWN_FLYING -> "?"
        VisualClassification.LIKELY_BIRD -> "BIRD?"
        VisualClassification.LIKELY_AIRCRAFT -> "AIRCRAFT?"
        VisualClassification.LIKELY_STATIC -> ""
    }
}

// ---- Label text helpers ----

/**
 * Primary label line: callsign + aircraft type abbreviation for aircraft,
 * drone ID for drones.
 *
 * Examples:
 * - Aircraft: "UAL123 B738"
 * - Drone: "FPV-DRONE-01"
 */
private fun getLabelText(skyObject: com.friendorfoe.domain.model.SkyObject): String {
    return when (skyObject) {
        is Aircraft -> {
            val callsign = skyObject.callsign ?: skyObject.icaoHex
            val type = skyObject.aircraftType
            if (type != null) "$callsign $type" else callsign
        }
        is Drone -> skyObject.droneId.take(14)
    }
}

/**
 * Secondary label line: altitude + distance for aircraft, manufacturer + distance for drones.
 *
 * Examples:
 * - Aircraft: "FL350 · 8.2 mi" or "5200ft · 1.1 mi"
 * - Drone: "DJI · 0.3 mi" or "DJI · 250m"
 */
private fun getSecondaryLabelText(
    skyObject: com.friendorfoe.domain.model.SkyObject,
    distanceMeters: Double
): String {
    val distStr = formatDistance(distanceMeters)
    return when (skyObject) {
        is Aircraft -> {
            val altFeet = (skyObject.position.altitudeMeters * 3.281).roundToInt()
            val altStr = if (altFeet >= 18000) {
                "FL${altFeet / 100}"
            } else {
                "${altFeet}ft"
            }
            val spdStr = skyObject.position.speedMps?.let {
                "${(it / 0.5144f).roundToInt()}kt"
            } ?: ""
            val parts = listOfNotNull(
                altStr,
                spdStr.ifEmpty { null },
                distStr.ifEmpty { null }
            )
            parts.joinToString(" · ")
        }
        is Drone -> {
            val mfr = skyObject.manufacturer ?: "Unknown"
            if (distStr.isNotEmpty()) "$mfr · $distStr" else mfr
        }
    }
}

/**
 * Format distance for display: miles if > 800m, meters if closer.
 */
private fun formatDistance(distanceMeters: Double): String {
    if (distanceMeters <= 0.0) return ""
    return if (distanceMeters > 800.0) {
        val miles = distanceMeters / 1609.344
        "%.1f mi".format(miles)
    } else {
        "${distanceMeters.roundToInt()}m"
    }
}

/**
 * Ellipsize text to fit within maxWidth pixels.
 */
private fun ellipsize(text: String, paint: android.graphics.Paint, maxWidth: Float): String {
    if (paint.measureText(text) <= maxWidth) return text
    var truncated = text
    while (truncated.isNotEmpty() && paint.measureText("$truncated...") > maxWidth) {
        truncated = truncated.dropLast(1)
    }
    return if (truncated.isEmpty()) text.take(3) else "$truncated..."
}

// ---- Category color mapping (uses shared utility) ----

// ---- Overlap avoidance ----

/**
 * Resolve overlapping labels by pushing them down vertically.
 *
 * Sorts labels by Y position (top to bottom), then for each label,
 * checks if it overlaps any previously placed label. If so, shifts it
 * down below the conflicting label.
 *
 * This is a simple greedy algorithm that works well for typical
 * sky object densities (5-30 objects in view at once).
 */
private fun resolveOverlaps(
    labels: List<AnimatedLabelData>,
    canvasWidth: Float,
    canvasHeight: Float
): List<AnimatedLabelData> {
    if (labels.isEmpty()) return labels

    val labelWidth = 240f
    val labelHeight = 60f
    val padding = 8f

    // Sort by Y position (top-most first)
    val sorted = labels.sortedBy { it.animatedY }
    val resolved = mutableListOf<AnimatedLabelData>()

    for (label in sorted) {
        var resolvedX = label.animatedX
        var resolvedY = label.animatedY

        // Check against all previously placed labels
        for (placed in resolved) {
            val placedLeft = placed.resolvedX * canvasWidth - labelWidth / 2
            val placedRight = placedLeft + labelWidth
            val placedTop = placed.resolvedY * canvasHeight - labelHeight / 2
            val placedBottom = placedTop + labelHeight + padding

            val currentLeft = resolvedX * canvasWidth - labelWidth / 2
            val currentRight = currentLeft + labelWidth
            val currentTop = resolvedY * canvasHeight - labelHeight / 2
            val currentBottom = currentTop + labelHeight

            // Check horizontal overlap
            val horizontalOverlap = currentLeft < placedRight && currentRight > placedLeft
            // Check vertical overlap
            val verticalOverlap = currentTop < placedBottom && currentBottom > placedTop

            if (horizontalOverlap && verticalOverlap) {
                // Push current label below the placed label
                resolvedY = (placedBottom + padding + labelHeight / 2) / canvasHeight
            }
        }

        resolved.add(label.copy(resolvedX = resolvedX, resolvedY = resolvedY))
    }

    return resolved
}

// ---- Status Bar ----

/**
 * Bottom status bar showing detection counts, GPS status, and ARCore status.
 *
 * Always visible during the AR view, with a semi-transparent dark background.
 */
@Composable
private fun StatusBar(
    aircraftCount: Int,
    droneCount: Int,
    militaryCount: Int,
    emergencyCount: Int,
    visualCount: Int,
    alertCount: Int,
    gpsStatus: GpsStatus,
    arCoreStatus: ArCoreStatus,
    dataSourceStatus: DataSourceStatus,
    weatherRange: VisualDetectionRange?,
    rangeOverride: Float?,
    isDarkMode: Boolean,
    strobeCount: Int,
    onRangeOverrideChange: (Float?) -> Unit,
    onTap: () -> Unit,
    modifier: Modifier = Modifier
) {
    var showRangeSlider by remember { mutableStateOf(false) }

    Column(modifier = modifier) {
        // Weather visibility banner (when poor)
        val effectiveMultiplier = rangeOverride ?: weatherRange?.confidenceMultiplier ?: 1.0f
        if (effectiveMultiplier < 0.8f) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFFFF9800).copy(alpha = 0.3f))
                    .padding(horizontal = 12.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                val reason = if (rangeOverride != null) "Manual override" else "Limited visibility"
                Text(
                    text = "$reason — visual detection range reduced",
                    color = Color(0xFFFFEB3B),
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium
                )
            }
        }

        // Range override slider (shown when user taps VIS indicator)
        AnimatedVisibility(
            visible = showRangeSlider,
            enter = slideInVertically(initialOffsetY = { it }),
            exit = slideOutVertically(targetOffsetY = { it })
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color.Black.copy(alpha = 0.85f))
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = "Detection Range",
                        color = Color.White,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold
                    )
                    val overrideLabel = if (rangeOverride != null) {
                        "%.0f%% (manual)".format(effectiveMultiplier * 100)
                    } else {
                        "%.0f%% (auto)".format(effectiveMultiplier * 100)
                    }
                    Text(
                        text = overrideLabel,
                        color = Color.White.copy(alpha = 0.7f),
                        fontSize = 12.sp
                    )
                }
                Slider(
                    value = effectiveMultiplier,
                    onValueChange = { onRangeOverrideChange(it) },
                    valueRange = 0.2f..1.0f,
                    steps = 7,
                    colors = SliderDefaults.colors(
                        thumbColor = Color(0xFF00BCD4),
                        activeTrackColor = Color(0xFF00BCD4),
                        inactiveTrackColor = Color.White.copy(alpha = 0.3f)
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(text = "Low", color = Color(0xFFF44336), fontSize = 10.sp)
                    if (rangeOverride != null) {
                        Text(
                            text = "Reset to Auto",
                            color = Color(0xFF00BCD4),
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Medium,
                            modifier = Modifier.clickable { onRangeOverrideChange(null) }
                        )
                    }
                    Text(text = "Max", color = Color(0xFF4CAF50), fontSize = 10.sp)
                }
            }
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    color = Color.Black.copy(alpha = 0.7f),
                    shape = RoundedCornerShape(topStart = 12.dp, topEnd = 12.dp)
                )
                .clickable(onClick = onTap)
                .padding(horizontal = 16.dp, vertical = 10.dp)
                .height(32.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Aircraft count
            StatusItem(
                label = "Aircraft: $aircraftCount",
                color = Color(0xFF4CAF50)
            )

            // Drone count
            StatusItem(
                label = "Drones: $droneCount",
                color = Color(0xFF2196F3)
            )

            // Military count (MIL + GOV)
            if (militaryCount > 0) {
                StatusItem(
                    label = "MIL: $militaryCount",
                    color = Color(0xFFF44336)
                )
            }

            // Emergency count
            if (emergencyCount > 0) {
                StatusItem(
                    label = "EMG: $emergencyCount",
                    color = Color(0xFFE91E63)
                )
            }

            // Visual detection count
            if (visualCount > 0) {
                StatusItem(
                    label = "VIS: $visualCount",
                    color = Color(0xFF00BCD4)  // cyan
                )
            }

            // Nighttime strobe mode indicator
            if (isDarkMode) {
                val strobeLabel = if (strobeCount > 0) "NIGHT: $strobeCount" else "NIGHT"
                StatusItem(
                    label = strobeLabel,
                    color = if (strobeCount > 0) Color(0xFFCE93D8) else Color(0xFF7E57C2)  // purple
                )
            }

            // Alert count (ALERT-level unidentified objects)
            if (alertCount > 0) {
                StatusItem(
                    label = "ALERT: $alertCount",
                    color = Color(0xFFF44336)
                )
            }

            // Visibility indicator (tappable to toggle range slider)
            run {
                val visColor = when {
                    effectiveMultiplier >= 0.9f -> Color(0xFF4CAF50)  // green
                    effectiveMultiplier >= 0.6f -> Color(0xFFFFEB3B)  // yellow
                    else -> Color(0xFFF44336)                         // red
                }
                val visLabel = if (rangeOverride != null) {
                    "RNG: %.0f%%".format(effectiveMultiplier * 100)
                } else if (weatherRange != null) {
                    @Suppress("UNNECESSARY_NOT_NULL_ASSERTION")
                    val range = weatherRange!!
                    val visKm = range.effectiveRangeMeters / 1000f
                    if (visKm >= 1f) "VIS: %.0fkm".format(visKm) else "VIS: ${range.effectiveRangeMeters}m"
                } else {
                    "RNG"
                }
                Row(
                    modifier = Modifier.clickable { showRangeSlider = !showRangeSlider },
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    Canvas(modifier = Modifier.size(10.dp).padding(end = 2.dp)) {
                        drawCircle(color = visColor, radius = size.minDimension / 2f, center = center)
                    }
                    Text(text = visLabel, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                }
            }

            // Data source status badge
            when (dataSourceStatus) {
                DataSourceStatus.ADSBFI_FALLBACK -> {
                    StatusItem(label = "adsb.fi", color = Color(0xFF4CAF50))
                }
                DataSourceStatus.AIRPLANES_LIVE_FALLBACK -> {
                    StatusItem(label = "AL", color = Color(0xFFFFEB3B))
                }
                DataSourceStatus.OPENSKY_FALLBACK -> {
                    StatusItem(label = "OpenSky", color = Color(0xFFFF9800))
                }
                DataSourceStatus.RATE_LIMITED -> {
                    StatusItem(label = "Rate Ltd", color = Color(0xFFF44336))
                }
                DataSourceStatus.OFFLINE -> {
                    StatusItem(label = "Offline", color = Color(0xFFF44336))
                }
            }

            // GPS indicator
            val gpsColor = when (gpsStatus) {
                GpsStatus.LOCKED -> Color(0xFF4CAF50)
                GpsStatus.SEARCHING -> Color(0xFFFFEB3B)
                GpsStatus.DISABLED -> Color(0xFFF44336)
                GpsStatus.NO_PERMISSION -> Color(0xFFF44336)
            }
            val gpsLabel = when (gpsStatus) {
                GpsStatus.LOCKED -> "GPS"
                GpsStatus.SEARCHING -> "GPS..."
                GpsStatus.DISABLED -> "No GPS"
                GpsStatus.NO_PERMISSION -> "No Perm"
            }
            StatusItem(label = gpsLabel, color = gpsColor)

            // ARCore status
            val arLabel = when (arCoreStatus) {
                ArCoreStatus.TRACKING -> "AR"
                ArCoreStatus.LOST_TRACKING -> "Compass"
                ArCoreStatus.UNAVAILABLE -> "Compass"
                ArCoreStatus.INITIALIZING -> "AR..."
            }
            val arColor = when (arCoreStatus) {
                ArCoreStatus.TRACKING -> Color(0xFF4CAF50)
                ArCoreStatus.LOST_TRACKING -> Color(0xFFFFEB3B)
                ArCoreStatus.UNAVAILABLE -> Color(0xFF9E9E9E)
                ArCoreStatus.INITIALIZING -> Color(0xFFFFEB3B)
            }
            StatusItem(label = arLabel, color = arColor)
        }
    }
}

/**
 * Single status bar item: colored dot + label text.
 */
@Composable
private fun StatusItem(
    label: String,
    color: Color,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Canvas(
            modifier = Modifier
                .size(10.dp)
                .padding(end = 2.dp)
        ) {
            drawCircle(
                color = color,
                radius = size.minDimension / 2f,
                center = center
            )
        }
        Text(
            text = label,
            color = Color.White,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium
        )
    }
}

// ---- Detection Log Panel ----

/**
 * Expandable panel showing all detected objects from all sources.
 * Each entry shows source icon, label, detail, and on-screen indicator.
 */
@Composable
private fun DetectionLogPanel(
    entries: List<DetectionLogEntry>,
    modifier: Modifier = Modifier
) {
    LazyColumn(
        modifier = modifier
            .heightIn(max = 300.dp)
            .background(Color.Black.copy(alpha = 0.8f))
            .padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        if (entries.isEmpty()) {
            item {
                Text(
                    text = "No detections yet",
                    color = Color.White.copy(alpha = 0.6f),
                    fontSize = 13.sp,
                    modifier = Modifier.padding(vertical = 8.dp)
                )
            }
        }
        items(entries, key = { "${it.source}_${it.label}" }) { entry ->
            DetectionLogRow(entry = entry)
            HorizontalDivider(color = Color.White.copy(alpha = 0.15f))
        }
    }
}

/**
 * Single row in the detection log: source label, name, detail, on-screen dot.
 */
@Composable
private fun DetectionLogRow(
    entry: DetectionLogEntry,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        // Source badge
        val sourceLabel = when (entry.source) {
            DetectionSource.ADS_B -> "ADS-B"
            DetectionSource.REMOTE_ID -> "BLE"
            DetectionSource.WIFI -> "WiFi"
        }
        val sourceColor = when (entry.source) {
            DetectionSource.ADS_B -> Color(0xFF4CAF50)
            DetectionSource.REMOTE_ID -> Color(0xFF9C27B0)
            DetectionSource.WIFI -> Color(0xFF2196F3)
        }
        Text(
            text = sourceLabel,
            color = sourceColor,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .background(sourceColor.copy(alpha = 0.2f), RoundedCornerShape(4.dp))
                .padding(horizontal = 4.dp, vertical = 2.dp)
        )

        // Label and detail
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = entry.label,
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1
            )
            if (entry.detail.isNotBlank()) {
                Text(
                    text = entry.detail,
                    color = Color.White.copy(alpha = 0.7f),
                    fontSize = 11.sp,
                    maxLines = 1
                )
            }
        }

        // On-screen indicator dot
        Canvas(modifier = Modifier.size(10.dp)) {
            drawCircle(
                color = if (entry.isOnScreen) Color(0xFF4CAF50) else Color(0xFF9E9E9E),
                radius = size.minDimension / 2f,
                center = center
            )
        }
    }
}

// ---- Unidentified Tap Content ----

/**
 * Content for the bottom sheet shown when the user taps empty space in the AR view.
 * Shows detected drones as candidates, or a message if none are detected.
 */
@Composable
private fun UnidentifiedTapContent(
    drones: List<Drone>,
    onDroneSelected: (String) -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp)
    ) {
        // Amber header
        Text(
            text = "Unidentified Object",
            color = Color(0xFFFF9800),
            fontSize = 20.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        Text(
            text = "You tapped an area with no labeled object.",
            color = Color.White.copy(alpha = 0.7f),
            fontSize = 13.sp,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        if (drones.isEmpty()) {
            Text(
                text = "No drones currently detected via BLE Remote ID or WiFi. " +
                    "If you see something in the sky, it may not be broadcasting an ID signal.",
                color = Color.White.copy(alpha = 0.6f),
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        } else {
            Text(
                text = "Detected drones that may match what you see:",
                color = Color.White.copy(alpha = 0.8f),
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 8.dp)
            )
            drones.forEach { drone ->
                DroneCandidateRow(
                    drone = drone,
                    onTap = { onDroneSelected(drone.id) }
                )
                HorizontalDivider(color = Color.White.copy(alpha = 0.15f))
            }
        }
    }
}

/**
 * A single drone candidate row with source badge, manufacturer, drone ID, and signal info.
 */
@Composable
private fun DroneCandidateRow(
    drone: Drone,
    onTap: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // Source badge
        val (sourceLabel, sourceColor) = when (drone.source) {
            DetectionSource.REMOTE_ID -> "BLE" to Color(0xFF9C27B0)
            DetectionSource.WIFI -> "WiFi" to Color(0xFF2196F3)
            else -> "Other" to Color(0xFF9E9E9E)
        }
        Text(
            text = sourceLabel,
            color = sourceColor,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier
                .background(sourceColor.copy(alpha = 0.2f), RoundedCornerShape(4.dp))
                .padding(horizontal = 6.dp, vertical = 2.dp)
        )

        // Manufacturer + drone ID + signal
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = drone.manufacturer ?: "Unknown manufacturer",
                color = Color.White,
                fontSize = 14.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1
            )
            val detailParts = mutableListOf(drone.droneId.take(16))
            drone.signalStrengthDbm?.let { detailParts.add("${it} dBm") }
            drone.estimatedDistanceMeters?.let { detailParts.add("~${it.roundToInt()}m") }
            Text(
                text = detailParts.joinToString(" · "),
                color = Color.White.copy(alpha = 0.6f),
                fontSize = 12.sp,
                maxLines = 1
            )
        }
    }
}

// ---- Data classes ----

/**
 * Animated label data combining the original screen position with
 * animated and overlap-resolved coordinates.
 */
private data class AnimatedLabelData(
    val screenPosition: ScreenPosition,
    val animatedX: Float,
    val animatedY: Float,
    val resolvedX: Float = animatedX,
    val resolvedY: Float = animatedY
)

/**
 * Hit target for tap detection on a label.
 */
private data class LabelHitTarget(
    val objectId: String,
    val rect: Rect
)

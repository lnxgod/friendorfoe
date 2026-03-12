package com.friendorfoe.presentation.ar

import android.util.Log
import android.view.ViewGroup
import androidx.camera.core.CameraSelector
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
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
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
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
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.detail.AircraftDetailContent
import com.friendorfoe.presentation.detail.DetailState
import com.friendorfoe.presentation.detail.DetailViewModel
import com.friendorfoe.presentation.detail.DroneDetailContent
import com.friendorfoe.sensor.ScreenPosition
import kotlin.math.roundToInt

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

    // Collect state from ViewModel
    val screenPositions by viewModel.screenPositions.collectAsStateWithLifecycle()
    val aircraftCount by viewModel.aircraftCount.collectAsStateWithLifecycle()
    val droneCount by viewModel.droneCount.collectAsStateWithLifecycle()
    val gpsStatus by viewModel.gpsStatus.collectAsStateWithLifecycle()
    val arCoreStatus by viewModel.arCoreStatus.collectAsStateWithLifecycle()
    val selectedObjectId by viewModel.selectedObjectId.collectAsStateWithLifecycle()
    val detailState by detailViewModel.detailState.collectAsStateWithLifecycle()

    // Manage sensor lifecycle: start on resume, stop on pause
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> viewModel.startSensors()
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
        // Layer 1: CameraX Preview
        CameraPreview(
            modifier = Modifier.fillMaxSize()
        )

        // Layer 2: AR Overlay with floating labels
        ArOverlay(
            screenPositions = screenPositions,
            onLabelTapped = { objectId -> viewModel.selectObject(objectId) },
            modifier = Modifier.fillMaxSize()
        )

        // Layer 3: Status bar at bottom
        StatusBar(
            aircraftCount = aircraftCount,
            droneCount = droneCount,
            gpsStatus = gpsStatus,
            arCoreStatus = arCoreStatus,
            modifier = Modifier
                .fillMaxWidth()
                .align(Alignment.BottomCenter)
        )
    }

    // Bottom sheet for detail card
    if (selectedObjectId != null) {
        val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)

        ModalBottomSheet(
            onDismissRequest = { viewModel.selectObject(null) },
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
                        detail = state.detail
                    )
                }
                is DetailState.DroneLoaded -> {
                    DroneDetailContent(drone = state.drone)
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
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(200.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                }
            }
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
    modifier: Modifier = Modifier
) {
    val lifecycleOwner = LocalLifecycleOwner.current

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

                    // Use back camera (pointing at sky)
                    val cameraSelector = CameraSelector.DEFAULT_BACK_CAMERA

                    // Unbind all and rebind
                    cameraProvider.unbindAll()
                    cameraProvider.bindToLifecycle(
                        lifecycleOwner,
                        cameraSelector,
                        preview
                    )
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
    onLabelTapped: (String) -> Unit,
    modifier: Modifier = Modifier
) {
    // Filter to only in-view objects
    val visiblePositions = remember(screenPositions) {
        screenPositions.filter { it.isInView }
    }

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

    Canvas(
        modifier = modifier
            .pointerInput(Unit) {
                detectTapGestures { offset ->
                    // Check if tap hit any label
                    labelRects.forEach { target ->
                        if (target.rect.contains(offset)) {
                            onLabelTapped(target.objectId)
                            return@detectTapGestures
                        }
                    }
                }
            }
    ) {
        val canvasWidth = size.width
        val canvasHeight = size.height
        val hitTargets = mutableListOf<LabelHitTarget>()

        // Calculate label positions with overlap avoidance
        val resolvedPositions = resolveOverlaps(animatedPositions, canvasWidth, canvasHeight)

        resolvedPositions.forEach { labelInfo ->
            val rect = drawLabel(
                labelInfo = labelInfo,
                canvasWidth = canvasWidth,
                canvasHeight = canvasHeight
            )
            hitTargets.add(
                LabelHitTarget(
                    objectId = labelInfo.screenPosition.skyObject.id,
                    rect = rect
                )
            )
        }

        labelRects = hitTargets
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
    canvasHeight: Float
): Rect {
    val skyObject = labelInfo.screenPosition.skyObject
    val color = categoryColor(skyObject.category)

    // Label dimensions
    val labelWidth = 200f
    val labelHeight = 60f
    val cornerRadius = 8f

    // Position: center the label at the animated screen coordinates
    val centerX = labelInfo.resolvedX * canvasWidth
    val centerY = labelInfo.resolvedY * canvasHeight
    val left = (centerX - labelWidth / 2).coerceIn(4f, canvasWidth - labelWidth - 4f)
    val top = (centerY - labelHeight / 2).coerceIn(4f, canvasHeight - labelHeight - 4f)

    // Draw background rounded rect with semi-transparency
    drawRoundRect(
        color = color.copy(alpha = 0.75f),
        topLeft = Offset(left, top),
        size = Size(labelWidth, labelHeight),
        cornerRadius = CornerRadius(cornerRadius, cornerRadius)
    )

    // Draw border
    drawRoundRect(
        color = Color.White.copy(alpha = 0.6f),
        topLeft = Offset(left, top),
        size = Size(labelWidth, labelHeight),
        cornerRadius = CornerRadius(cornerRadius, cornerRadius),
        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.5f)
    )

    // Draw text content
    val primaryLine = getLabelText(skyObject)
    val secondaryLine = getSecondaryLabelText(skyObject)

    drawContext.canvas.nativeCanvas.apply {
        val textPaint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.WHITE
            textSize = 28f
            isAntiAlias = true
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            setShadowLayer(2f, 1f, 1f, android.graphics.Color.BLACK)
        }

        val subtextPaint = android.graphics.Paint().apply {
            this.color = android.graphics.Color.WHITE
            textSize = 22f
            isAntiAlias = true
            setShadowLayer(2f, 1f, 1f, android.graphics.Color.BLACK)
        }

        // Primary text (callsign + type for aircraft, drone ID for drones)
        val primaryText = ellipsize(primaryLine, textPaint, labelWidth - 16f)
        drawText(primaryText, left + 8f, top + 24f, textPaint)

        // Secondary text (altitude for aircraft, manufacturer for drones)
        val secondaryEllipsized = ellipsize(secondaryLine, subtextPaint, labelWidth - 16f)
        drawText(secondaryEllipsized, left + 8f, top + 50f, subtextPaint)
    }

    return Rect(left, top, left + labelWidth, top + labelHeight)
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
 * Secondary label line: altitude for aircraft, manufacturer for drones.
 *
 * Examples:
 * - Aircraft: "35000ft"
 * - Drone: "DJI"
 */
private fun getSecondaryLabelText(skyObject: com.friendorfoe.domain.model.SkyObject): String {
    return when (skyObject) {
        is Aircraft -> {
            val altFeet = (skyObject.position.altitudeMeters * 3.281).roundToInt()
            "${altFeet}ft"
        }
        is Drone -> skyObject.manufacturer ?: "Unknown"
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

// ---- Category color mapping ----

/**
 * Map an [ObjectCategory] to its AR overlay color.
 *
 * - COMMERCIAL: green (0xFF4CAF50) -- friendly/airline
 * - GENERAL_AVIATION: yellow (0xFFFFEB3B) -- private/GA
 * - MILITARY: red (0xFFF44336) -- military
 * - DRONE: blue (0xFF2196F3) -- UAS
 * - UNKNOWN: gray (0xFF9E9E9E) -- unidentified
 */
private fun categoryColor(category: ObjectCategory): Color {
    return when (category) {
        ObjectCategory.COMMERCIAL -> Color(0xFF4CAF50)
        ObjectCategory.GENERAL_AVIATION -> Color(0xFFFFEB3B)
        ObjectCategory.MILITARY -> Color(0xFFF44336)
        ObjectCategory.DRONE -> Color(0xFF2196F3)
        ObjectCategory.UNKNOWN -> Color(0xFF9E9E9E)
    }
}

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

    val labelWidth = 200f
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
    gpsStatus: GpsStatus,
    arCoreStatus: ArCoreStatus,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .background(
                color = Color.Black.copy(alpha = 0.7f),
                shape = RoundedCornerShape(topStart = 12.dp, topEnd = 12.dp)
            )
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

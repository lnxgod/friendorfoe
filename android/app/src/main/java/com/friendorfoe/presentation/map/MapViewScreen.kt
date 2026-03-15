package com.friendorfoe.presentation.map

import android.graphics.Paint
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.graphics.Bitmap
import android.graphics.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Explore
import androidx.compose.material.icons.filled.Navigation
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.detail.AircraftDetailContent
import com.friendorfoe.presentation.detail.DetailState
import com.friendorfoe.presentation.detail.DetailViewModel
import com.friendorfoe.presentation.detail.DroneDetailContent
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.util.categoryColorArgb
import org.osmdroid.config.Configuration
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Polygon
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MapViewScreen(
    onObjectTapped: (String) -> Unit,
    viewModel: MapViewModel = hiltViewModel(),
    detailViewModel: DetailViewModel = hiltViewModel()
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    val skyObjects by viewModel.skyObjects.collectAsStateWithLifecycle()
    val userPosition by viewModel.userPosition.collectAsStateWithLifecycle()
    val selectedObjectId by viewModel.selectedObjectId.collectAsStateWithLifecycle()
    val detailState by detailViewModel.detailState.collectAsStateWithLifecycle()
    val followCompass by viewModel.followCompass.collectAsStateWithLifecycle()
    val compassHeading by viewModel.compassHeading.collectAsStateWithLifecycle()

    // Configure osmdroid
    LaunchedEffect(Unit) {
        Configuration.getInstance().userAgentValue = context.packageName
    }

    // Manage location lifecycle
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> viewModel.startLocationUpdates()
                Lifecycle.Event.ON_PAUSE -> viewModel.stopLocationUpdates()
                else -> {}
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            viewModel.stopLocationUpdates()
        }
    }

    // Load detail when selected
    LaunchedEffect(selectedObjectId) {
        selectedObjectId?.let { detailViewModel.loadDetail(it) }
    }

    val mapView = remember {
        MapView(context).apply {
            setTileSource(TileSourceFactory.MAPNIK)
            setMultiTouchControls(true)
            controller.setZoom(10.0)
            zoomController.setVisibility(org.osmdroid.views.CustomZoomButtonsController.Visibility.NEVER)
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        AndroidView(
            factory = { mapView },
            modifier = Modifier.fillMaxSize(),
            update = { map ->
                // Apply compass rotation when follow mode is on
                if (followCompass) {
                    map.mapOrientation = -compassHeading
                } else {
                    map.mapOrientation = 0f
                }

                // Update map center to user position
                if (userPosition.latitude != 0.0 || userPosition.longitude != 0.0) {
                    val userGeoPoint = GeoPoint(userPosition.latitude, userPosition.longitude)

                    // Clear existing overlays and re-add
                    map.overlays.clear()

                    // Distance rings at 10 NM and 25 NM
                    addDistanceRing(map, userGeoPoint, 10.0)
                    addDistanceRing(map, userGeoPoint, 25.0)

                    // FOV cone showing camera direction (only when following compass)
                    if (followCompass) {
                        addFovCone(map, userGeoPoint, compassHeading, map.zoomLevelDouble)
                    }

                    // User position marker
                    val userMarker = Marker(map).apply {
                        position = userGeoPoint
                        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                        title = "You"
                        icon = createUserDrawable(context, followCompass, compassHeading)
                    }
                    map.overlays.add(userMarker)

                    // Aircraft markers
                    for (obj in skyObjects) {
                        if (obj.position.latitude == 0.0 && obj.position.longitude == 0.0) continue
                        val geoPoint = GeoPoint(obj.position.latitude, obj.position.longitude)
                        val color = categoryColorArgb(obj.category)

                        val marker = Marker(map).apply {
                            position = geoPoint
                            setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                            title = getMarkerTitle(obj)
                            snippet = getMarkerSnippet(obj)
                            icon = createCategoryMarkerDrawable(context, obj.category, color, getHeading(obj))
                            setOnMarkerClickListener { _, _ ->
                                viewModel.selectObject(obj.id)
                                true
                            }
                        }
                        map.overlays.add(marker)
                    }

                    // Center map on user when following compass, or if moved significantly
                    if (followCompass) {
                        map.controller.animateTo(userGeoPoint)
                    } else {
                        val currentCenter = map.mapCenter
                        val dist = userGeoPoint.distanceToAsDouble(
                            GeoPoint(currentCenter.latitude, currentCenter.longitude)
                        )
                        if (dist > 500) {
                            map.controller.animateTo(userGeoPoint)
                        }
                    }

                    map.invalidate()
                }
            }
        )

        // Compass follow toggle FAB
        FloatingActionButton(
            onClick = { viewModel.toggleFollowCompass() },
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(16.dp)
                .size(48.dp),
            shape = CircleShape,
            containerColor = if (followCompass) Color(0xFF2196F3) else Color(0xFF424242)
        ) {
            Icon(
                imageVector = if (followCompass) Icons.Filled.Navigation else Icons.Filled.Explore,
                contentDescription = if (followCompass) "Disable compass follow" else "Follow compass",
                tint = Color.White,
                modifier = if (followCompass) Modifier.rotate(-compassHeading) else Modifier
            )
        }
    }

    // Bottom sheet for detail
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

private fun getMarkerTitle(obj: SkyObject): String {
    return when (obj) {
        is Aircraft -> {
            val callsign = obj.callsign ?: obj.icaoHex
            if (obj.aircraftType != null) "$callsign (${obj.aircraftType})" else callsign
        }
        is Drone -> obj.droneId
    }
}

private fun getMarkerSnippet(obj: SkyObject): String {
    return when (obj) {
        is Aircraft -> {
            val altFeet = (obj.position.altitudeMeters * 3.281).roundToInt()
            "${obj.aircraftType ?: "Unknown"} - ${altFeet}ft"
        }
        is Drone -> obj.manufacturer ?: "Unknown drone"
    }
}

private fun getHeading(obj: SkyObject): Float {
    return obj.position.heading ?: 0f
}

/**
 * Add a FOV cone overlay showing the camera's approximate field of view direction.
 * Draws a filled wedge from the user's position in the compass heading direction.
 */
private fun addFovCone(map: MapView, center: GeoPoint, headingDeg: Float, zoomLevel: Double) {
    // Cone radius scales with zoom: larger at low zoom, smaller at high zoom
    val radiusMeters = when {
        zoomLevel >= 14 -> 500.0
        zoomLevel >= 12 -> 1500.0
        zoomLevel >= 10 -> 5000.0
        else -> 15000.0
    }
    val halfAngle = 30.0 // 60° camera FOV approximation
    val steps = 20

    val points = mutableListOf<GeoPoint>()
    points.add(center) // apex at user position

    // Arc from (heading - halfAngle) to (heading + halfAngle)
    for (i in 0..steps) {
        val angle = headingDeg - halfAngle + (2.0 * halfAngle * i / steps)
        val angleRad = Math.toRadians(angle)
        val point = offsetPoint(center, radiusMeters, angleRad)
        points.add(point)
    }
    points.add(center) // close the polygon

    val cone = Polygon(map).apply {
        this.points = points
        fillPaint.apply {
            color = 0x302196F3 // semi-transparent blue
            style = Paint.Style.FILL
        }
        outlinePaint.apply {
            color = 0x802196F3.toInt()
            strokeWidth = 2f
            style = Paint.Style.STROKE
        }
    }
    map.overlays.add(cone)
}

/**
 * Calculate a GeoPoint offset from a center point by a distance and bearing.
 */
private fun offsetPoint(center: GeoPoint, distanceMeters: Double, bearingRad: Double): GeoPoint {
    val earthRadius = 6_371_000.0
    val lat1 = Math.toRadians(center.latitude)
    val lon1 = Math.toRadians(center.longitude)
    val angDist = distanceMeters / earthRadius

    val lat2 = Math.asin(
        sin(lat1) * cos(angDist) + cos(lat1) * sin(angDist) * cos(bearingRad)
    )
    val lon2 = lon1 + Math.atan2(
        sin(bearingRad) * sin(angDist) * cos(lat1),
        cos(angDist) - sin(lat1) * sin(lat2)
    )
    return GeoPoint(Math.toDegrees(lat2), Math.toDegrees(lon2))
}

/**
 * Add a distance ring circle overlay at the given radius in nautical miles.
 */
private fun addDistanceRing(map: MapView, center: GeoPoint, radiusNm: Double) {
    val radiusMeters = radiusNm * 1852.0
    val circle = Polygon(map).apply {
        points = Polygon.pointsAsCircle(center, radiusMeters)
        fillPaint.apply {
            color = 0x10FFFFFF
            style = Paint.Style.FILL
        }
        outlinePaint.apply {
            color = 0x80FFFFFF.toInt()
            strokeWidth = 2f
            style = Paint.Style.STROKE
        }
        title = "${radiusNm.toInt()} NM"
    }
    map.overlays.add(circle)
}

/**
 * Create user position marker. When following compass, shows a directional arrow.
 * Otherwise shows a simple circle.
 */
private fun createUserDrawable(context: android.content.Context, followCompass: Boolean, heading: Float): Drawable {
    val density = context.resources.displayMetrics.density
    val sizeDp = if (followCompass) 32 else 24
    val sizePx = (sizeDp * density).toInt()
    val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)
    val cx = sizePx / 2f
    val cy = sizePx / 2f

    if (followCompass) {
        // Draw directional arrow pointing up (map is rotated to match heading)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF2196F3.toInt()
            style = Paint.Style.FILL
        }
        val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFFFFFFFF.toInt()
            style = Paint.Style.STROKE
            strokeWidth = 2f * density
        }
        val unit = sizePx / 10f
        val path = android.graphics.Path().apply {
            moveTo(cx, cy - 4 * unit) // tip
            lineTo(cx + 3 * unit, cy + 3 * unit) // bottom right
            lineTo(cx, cy + 1.5f * unit) // notch
            lineTo(cx - 3 * unit, cy + 3 * unit) // bottom left
            close()
        }
        canvas.drawPath(path, paint)
        canvas.drawPath(path, borderPaint)
    } else {
        // Simple circle
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF2196F3.toInt()
            style = Paint.Style.FILL
        }
        val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFFFFFFFF.toInt()
            style = Paint.Style.STROKE
            strokeWidth = 3f * density
        }
        val radius = sizePx / 2f
        canvas.drawCircle(radius, radius, radius - 2 * density, paint)
        canvas.drawCircle(radius, radius, radius - 2 * density, borderPaint)
    }

    return BitmapDrawable(context.resources, bitmap)
}


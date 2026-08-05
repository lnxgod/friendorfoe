package com.friendorfoe.presentation.map

import android.content.Context
import android.os.SystemClock
import android.view.View
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Explore
import androidx.compose.material.icons.filled.Navigation
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.presentation.detail.AircraftDetailContent
import com.friendorfoe.presentation.detail.DetailState
import com.friendorfoe.presentation.detail.DetailViewModel
import com.friendorfoe.presentation.detail.DroneDetailContent
import com.friendorfoe.presentation.filter.FilterBar
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsable
import com.friendorfoe.domain.model.Position
import org.osmdroid.config.Configuration
import org.osmdroid.events.MapEventsReceiver
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.MapEventsOverlay

internal class MapScreenLifecycleActions(
    private val canStartLocation: () -> Boolean = { true },
    private val startLocation: () -> Unit,
    private val stopLocation: () -> Unit,
    private val startPolling: () -> Unit,
    private val stopPolling: () -> Unit,
) {
    fun onEvent(event: Lifecycle.Event) {
        when (event) {
            Lifecycle.Event.ON_RESUME -> {
                if (canStartLocation()) startLocation()
                startPolling()
            }
            Lifecycle.Event.ON_PAUSE -> {
                stopLocation()
                stopPolling()
            }
            else -> Unit
        }
    }

    fun dispose() {
        stopLocation()
        stopPolling()
    }
}

internal data class MapOverlayPlan(
    val renderTargets: Boolean,
    val renderUserMarker: Boolean,
    val renderPreciseUserOverlays: Boolean,
    val locationPermissionUsable: Boolean,
)

internal fun mapOverlayPlan(
    locationPermissionState: PermissionUiState,
    hasValidUserPosition: Boolean,
): MapOverlayPlan {
    val hasUsableUserPosition = locationPermissionState.isUsable() && hasValidUserPosition
    return MapOverlayPlan(
        renderTargets = true,
        renderUserMarker = hasUsableUserPosition,
        renderPreciseUserOverlays =
            locationPermissionState == PermissionUiState.Granted && hasValidUserPosition,
        locationPermissionUsable = locationPermissionState.isUsable(),
    )
}

@Composable
internal fun <T : View> StableAndroidViewHost(
    revealed: Boolean,
    factory: (Context) -> T,
    modifier: Modifier = Modifier,
    update: (T) -> Unit = {},
) {
    AndroidView(
        factory = factory,
        modifier = modifier.alpha(if (revealed) 1f else 0f),
        update = update,
    )
}

internal fun View.installMapCameraTouchListener(
    isMapRevealed: () -> Boolean,
    onUserTouch: () -> Unit,
) {
    setOnTouchListener { _, event ->
        if (!isMapRevealed()) {
            true
        } else {
            if (event.action == android.view.MotionEvent.ACTION_DOWN) onUserTouch()
            false
        }
    }
}

@Composable
internal fun MapLocatingOverlay(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        awaitPointerEvent(PointerEventPass.Initial).changes.forEach { it.consume() }
                    }
                }
            }
            .background(MaterialTheme.colorScheme.surface),
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.foundation.layout.Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(12.dp),
        ) {
            CircularProgressIndicator()
            Text(
                text = "Finding your location…",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
internal fun rememberMapCameraOwnership(mapInstance: Any): MutableState<Boolean> =
    remember(mapInstance) { mutableStateOf(false) }

@Composable
internal fun rememberAcceptedMapPosition(
    mapInstance: Any,
    locationPermissionUsable: Boolean,
    candidate: MapLocationFix?,
    nowElapsedRealtimeNanos: () -> Long = SystemClock::elapsedRealtimeNanos,
): Position? {
    var acceptedPosition by remember(mapInstance, locationPermissionUsable) {
        mutableStateOf<Position?>(null)
    }
    LaunchedEffect(mapInstance, candidate, locationPermissionUsable) {
        acceptedPosition = updateMapPositionForInstance(
            acceptedPosition = acceptedPosition,
            candidate = candidate,
            nowElapsedRealtimeNanos = nowElapsedRealtimeNanos(),
        )
    }
    return acceptedPosition
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MapViewScreen(
    onObjectTapped: (String) -> Unit,
    viewModel: MapViewModel = hiltViewModel(),
    detailViewModel: DetailViewModel = hiltViewModel(),
    locationPermissionState: PermissionUiState = PermissionUiState.Granted,
    onRequestLocation: () -> Unit = {},
    onOpenLocationSettings: () -> Unit = {},
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    val mapTracks by viewModel.mapTracks.collectAsStateWithLifecycle()
    val formationPoints by viewModel.formationPoints.collectAsStateWithLifecycle()
    val filterState by viewModel.filterState.collectAsStateWithLifecycle()
    val userLocationFix by viewModel.userLocationFix.collectAsStateWithLifecycle()
    val selectedObjectId by viewModel.selectedObjectId.collectAsStateWithLifecycle()
    val detailState by detailViewModel.detailState.collectAsStateWithLifecycle()
    val followCompass by viewModel.followCompass.collectAsStateWithLifecycle()
    val stabilizedMapHeading by viewModel.stabilizedMapHeading.collectAsStateWithLifecycle()
    val activeVisualFocusIds by viewModel.activeVisualFocusIds.collectAsStateWithLifecycle()
    val sensorDrones by viewModel.sensorDrones.collectAsStateWithLifecycle()
    val remoteSensors by viewModel.remoteSensors.collectAsStateWithLifecycle()
    val remoteSearchResults by viewModel.remoteSearchResults.collectAsStateWithLifecycle()
    val remoteSearchCenter by viewModel.remoteSearchCenter.collectAsStateWithLifecycle()
    val remoteSearching by viewModel.remoteSearching.collectAsStateWithLifecycle()

    // Configure osmdroid
    LaunchedEffect(Unit) {
        Configuration.getInstance().userAgentValue = context.packageName
    }

    // Manage location lifecycle
    DisposableEffect(lifecycleOwner, locationPermissionState) {
        val lifecycleActions = MapScreenLifecycleActions(
            canStartLocation = locationPermissionState::isUsable,
            startLocation = viewModel::startLocationUpdates,
            stopLocation = viewModel::stopLocationUpdates,
            startPolling = viewModel::startSensorMapPolling,
            stopPolling = viewModel::stopSensorMapPolling,
        )
        val observer = LifecycleEventObserver { _, event -> lifecycleActions.onEvent(event) }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            lifecycleActions.dispose()
        }
    }

    LaunchedEffect(locationPermissionState) {
        if (locationPermissionState.isUsable()) viewModel.startLocationUpdates()
        else viewModel.stopLocationUpdates()
    }

    // Load detail when selected
    LaunchedEffect(selectedObjectId) {
        selectedObjectId?.let { detailViewModel.loadDetail(it) }
    }

    val isDarkTheme = androidx.compose.foundation.isSystemInDarkTheme()

    val mapView = remember {
        MapView(context).apply {
            setTileSource(TileSourceFactory.MAPNIK)
            setMultiTouchControls(true)
            controller.setZoom(10.0)
            zoomController.setVisibility(org.osmdroid.views.CustomZoomButtonsController.Visibility.NEVER)
            // Long-press: search 250 NM radius at tapped point (ocean search)
            overlays.add(0, MapEventsOverlay(object : MapEventsReceiver {
                override fun singleTapConfirmedHelper(p: org.osmdroid.util.GeoPoint): Boolean = false
                override fun longPressHelper(p: org.osmdroid.util.GeoPoint): Boolean {
                    viewModel.searchRemoteArea(p.latitude, p.longitude)
                    android.widget.Toast.makeText(context,
                        "Searching 250 NM around ${String.format("%.2f", p.latitude)}, ${String.format("%.2f", p.longitude)}…",
                        android.widget.Toast.LENGTH_SHORT).show()
                    return true
                }
            }))
        }
    }
    val cameraOwnership = rememberMapCameraOwnership(mapView)
    var userControlsCamera by cameraOwnership
    val overlayController = remember(mapView) {
        MapOverlayController(
            context = context,
            map = mapView,
            onLocalObjectSelected = viewModel::selectObject,
            onRemoteObjectSelected = { objectId ->
                viewModel.selectObject(objectId)
                onObjectTapped(objectId)
            },
        )
    }
    val userPosition = rememberAcceptedMapPosition(
        mapInstance = mapView,
        locationPermissionUsable = locationPermissionState.isUsable(),
        candidate = userLocationFix,
    )
        ?: Position(latitude = 0.0, longitude = 0.0, altitudeMeters = 0.0)
    val overlayPlan = mapOverlayPlan(
        locationPermissionState = locationPermissionState,
        hasValidUserPosition = userPosition.hasValidMapCoordinates(),
    )
    val revealMap = shouldRevealMap(
        locationPermissionState = locationPermissionState,
        userPosition = userPosition,
    )
    DisposableEffect(mapView, viewModel, cameraOwnership, revealMap) {
        mapView.installMapCameraTouchListener(
            isMapRevealed = { revealMap },
            onUserTouch = {
                cameraOwnership.value = true
                viewModel.stopFollowingCompass()
            },
        )
        onDispose { mapView.setOnTouchListener(null) }
    }

    // Apply dark mode color filter to map tiles
    LaunchedEffect(isDarkTheme) {
        if (isDarkTheme) {
            mapView.overlayManager.tilesOverlay.setColorFilter(
                android.graphics.ColorMatrixColorFilter(
                    android.graphics.ColorMatrix(floatArrayOf(
                        -1f, 0f, 0f, 0f, 255f,
                         0f,-1f, 0f, 0f, 255f,
                         0f, 0f,-1f, 0f, 255f,
                         0f, 0f, 0f, 1f,   0f
                    ))
                )
            )
        } else {
            mapView.overlayManager.tilesOverlay.setColorFilter(null)
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        StableAndroidViewHost(
            revealed = revealMap,
            factory = { mapView },
            modifier = Modifier.fillMaxSize(),
            update = {
                overlayController.render(
                    mapTracks = mapTracks,
                    formationPoints = formationPoints,
                    userPosition = userPosition,
                    followCompass = followCompass,
                    stabilizedMapHeading = stabilizedMapHeading,
                    activeVisualFocusIds = activeVisualFocusIds,
                    remoteSensors = remoteSensors,
                    sensorDrones = sensorDrones,
                    remoteSearchResults = remoteSearchResults,
                    remoteSearchCenter = remoteSearchCenter,
                    userControlsCamera = userControlsCamera,
                    overlayPlan = overlayPlan,
                )
            },
        )
        if (!revealMap) {
            MapLocatingOverlay()
        }

        if (!locationPermissionState.isUsable()) {
            Surface(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(start = 16.dp, end = 16.dp, bottom = 88.dp),
                shape = RoundedCornerShape(16.dp),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f),
                tonalElevation = 4.dp,
            ) {
                androidx.compose.foundation.layout.Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(8.dp),
                ) {
                    Text("Map works without your location", style = MaterialTheme.typography.titleSmall)
                    Text(
                        "Browse and search the map now. Allow location only when you want nearby centering.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (locationPermissionState != PermissionUiState.Loading) {
                        Button(
                            onClick = if (locationPermissionState == PermissionUiState.Denied) {
                                onRequestLocation
                            } else {
                                onOpenLocationSettings
                            },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(
                                if (locationPermissionState == PermissionUiState.Denied) {
                                    "Use my location"
                                } else {
                                    "Open app settings"
                                }
                            )
                        }
                    }
                }
            }
        } else if (locationPermissionState == PermissionUiState.Approximate) {
            Surface(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(start = 16.dp, end = 16.dp, bottom = 88.dp),
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.94f),
            ) {
                Text(
                    "Approximate location · precise distance and bearing are hidden",
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        // Remote search indicator
        if (remoteSearching) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = androidx.compose.ui.Alignment.Center
            ) {
                CircularProgressIndicator(
                    modifier = Modifier.size(48.dp),
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        // Filter bar overlay
        FilterBar(
            filterState = filterState,
            onFilterStateChange = { viewModel.updateFilter(it) },
            resultCount = mapTracks.size + formationPoints.size,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.85f))
        )

        // Compass follow toggle FAB
        FloatingActionButton(
            onClick = {
                if (!followCompass) userControlsCamera = false
                viewModel.toggleFollowCompass()
            },
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
                modifier = if (followCompass) Modifier.rotate(-stabilizedMapHeading) else Modifier
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
                is DetailState.HistoricalLoaded -> {
                    LaunchedEffect(Unit) { viewModel.selectObject(null) }
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

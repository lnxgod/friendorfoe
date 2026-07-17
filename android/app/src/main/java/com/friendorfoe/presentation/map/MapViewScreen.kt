package com.friendorfoe.presentation.map

import androidx.compose.foundation.background
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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
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
import com.friendorfoe.presentation.detail.AircraftDetailContent
import com.friendorfoe.presentation.detail.DetailState
import com.friendorfoe.presentation.detail.DetailViewModel
import com.friendorfoe.presentation.detail.DroneDetailContent
import com.friendorfoe.presentation.filter.FilterBar
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.flow.transformLatest
import org.osmdroid.config.Configuration
import org.osmdroid.events.MapEventsReceiver
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.MapEventsOverlay

private const val MAP_PAN_TIMEOUT_MS = 10_000L

@OptIn(ExperimentalCoroutinesApi::class)
internal fun mapPanActivity(
    gestures: Flow<Unit>,
    timeoutMs: Long = MAP_PAN_TIMEOUT_MS,
): Flow<Boolean> = gestures
    .transformLatest {
        emit(true)
        delay(timeoutMs)
        emit(false)
    }
    .onStart { emit(false) }

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
    val mapTracks by viewModel.mapTracks.collectAsStateWithLifecycle()
    val filterState by viewModel.filterState.collectAsStateWithLifecycle()
    val userPosition by viewModel.userPosition.collectAsStateWithLifecycle()
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
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    viewModel.startLocationUpdates()
                    viewModel.startSensorMapPolling()
                }
                Lifecycle.Event.ON_PAUSE -> {
                    viewModel.stopLocationUpdates()
                    viewModel.stopSensorMapPolling()
                }
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

    val panGestures = remember {
        MutableSharedFlow<Unit>(
            extraBufferCapacity = 1,
            onBufferOverflow = BufferOverflow.DROP_OLDEST,
        )
    }
    val isUserPanning by remember(panGestures) {
        mapPanActivity(panGestures)
    }.collectAsState(initial = false)

    val isDarkTheme = androidx.compose.foundation.isSystemInDarkTheme()

    val mapView = remember {
        MapView(context).apply {
            setTileSource(TileSourceFactory.MAPNIK)
            setMultiTouchControls(true)
            controller.setZoom(10.0)
            zoomController.setVisibility(org.osmdroid.views.CustomZoomButtonsController.Visibility.NEVER)
            // Detect user pan/zoom gestures to temporarily disable auto-center
            setOnTouchListener { _, event ->
                if (event.action == android.view.MotionEvent.ACTION_MOVE ||
                    event.action == android.view.MotionEvent.ACTION_DOWN) {
                    panGestures.tryEmit(Unit)
                }
                false  // Don't consume — let the map handle it
            }
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
        AndroidView(
            factory = { mapView },
            modifier = Modifier.fillMaxSize(),
            update = {
                overlayController.render(
                    mapTracks = mapTracks,
                    userPosition = userPosition,
                    followCompass = followCompass,
                    stabilizedMapHeading = stabilizedMapHeading,
                    activeVisualFocusIds = activeVisualFocusIds,
                    remoteSensors = remoteSensors,
                    sensorDrones = sensorDrones,
                    remoteSearchResults = remoteSearchResults,
                    remoteSearchCenter = remoteSearchCenter,
                    isUserPanning = isUserPanning,
                )
            }
        )

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
            resultCount = skyObjects.size,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.85f))
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

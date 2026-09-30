package com.friendorfoe.presentation.trails

import androidx.compose.foundation.layout.*
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.splitAircraftTrail
import com.friendorfoe.data.repository.trackDistanceMeters
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import org.osmdroid.config.Configuration
import org.osmdroid.tileprovider.tilesource.TileSourceFactory
import org.osmdroid.util.BoundingBox
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun FlightPathScreen(onBack: () -> Unit, viewModel: FlightPathViewModel = hiltViewModel()) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var exportText by remember { mutableStateOf<String?>(null) }
    var exportMessage by remember { mutableStateOf<String?>(null) }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/gpx+xml")) { uri ->
        val text = exportText
        exportText = null
        if (uri != null && text != null) scope.launch {
            exportMessage = try {
                withContext(Dispatchers.IO) {
                    checkNotNull(context.contentResolver.openOutputStream(uri)).bufferedWriter().use { it.write(text) }
                }
                "GPX exported"
            } catch (e: kotlinx.coroutines.CancellationException) { throw e
            } catch (_: Exception) { "Could not export this flight. Try again." }
        }
    }
    FlightPathContent(state, onBack, viewModel::retry, viewModel::save, onExport = { points ->
        exportText = flightGpx(state.label, points)
        export.launch("friendorfoe-flight.gpx")
    }, exportMessage = exportMessage)
}

@Composable
@OptIn(ExperimentalLayoutApi::class)
fun FlightPathContent(state: FlightPathState, onBack: () -> Unit, onRetry: () -> Unit = {},
    onSave: (List<TrackingEntity>) -> Unit = {}, onExport: (List<TrackingEntity>) -> Unit = {}, exportMessage: String? = null) {
    var windowHours by rememberSaveable { mutableIntStateOf(24) }
    val points = remember(state.points, state.nowMs, windowHours) {
        val duration = if (windowHours == 0) 15 * 60_000L else windowHours * 3_600_000L
        if (state.isSaved) state.points else state.points.filter { it.timestamp >= state.nowMs - duration }
    }
    var selectedTimestamp by rememberSaveable { mutableStateOf<Long?>(null) }
    val selectedIndex = points.indexOfLast { it.timestamp == selectedTimestamp }
        .takeIf { it >= 0 } ?: points.lastIndex
    val selected = points.getOrNull(selectedIndex)
    var fitRequest by remember { mutableIntStateOf(0) }
    var playing by remember { mutableStateOf(false) }
    var playbackSpeed by rememberSaveable { mutableIntStateOf(4) }
    var playbackTimeMs by remember { mutableLongStateOf(0L) }
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_PAUSE) playing = false }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(playing, playbackSpeed, points) {
        if (playing && points.size > 1) {
            while (playing) {
                delay(250)
                playbackTimeMs = (playbackTimeMs + playbackSpeed * 250L).coerceAtMost(points.last().timestamp)
                selectedTimestamp = recordedPositionAt(points, playbackTimeMs)?.timestamp
                if (playbackTimeMs >= points.last().timestamp) playing = false
            }
        }
    }
    Column(Modifier.fillMaxSize()) {
        FofSecondaryScreenHeader("Flight path", onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(state.label, style = MaterialTheme.typography.headlineSmall)
            Text(if (state.isSaved) "Saved flight · kept until deleted" else "Recorded on this phone · up to 24 hours", style = MaterialTheme.typography.bodyMedium)
            Text(
                "Paths contain received positions only. Gaps in coverage are left disconnected.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!state.isSaved) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(0 to "15 min", 1 to "1 hour", 24 to "24 hours").forEach { (hours, label) ->
                    FilterChip(
                        selected = windowHours == hours,
                        onClick = { playing = false; windowHours = hours; selectedTimestamp = null },
                        label = { Text(label) },
                    )
                }
            }
            when {
                state.loading -> CircularProgressIndicator()
                state.error != null -> {
                    Text(state.error)
                    TextButton(onClick = onRetry) { Text("Retry") }
                }
                points.isEmpty() -> {
                    Text("No recorded positions in this period", style = MaterialTheme.typography.titleMedium)
                    Text("Keep aircraft detection running to collect a path. Older flights cannot be reconstructed from a single sighting.")
                }
                else -> {
                    FlightPathMap(points, selected, windowHours to fitRequest, Modifier.fillMaxWidth().height(300.dp))
                    val uri = LocalUriHandler.current
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        TextButton(onClick = { fitRequest++ }) { Text("Fit path") }
                        TextButton(onClick = { uri.openUri("https://www.openstreetmap.org/copyright") }) {
                            Text("© OpenStreetMap")
                        }
                    }
                    val segments = remember(points) { splitAircraftTrail(points) }
                    val meters = segments.sumOf { segment -> segment.zipWithNext().sumOf { (a, b) -> trackDistanceMeters(a, b) } }
                    Text("${points.size} positions · ${segments.size} segment${if (segments.size == 1) "" else "s"} · ${"%.1f".format(meters / 1609.344)} mi observed")
                    Text("First: ${formatTrackTime(points.first().timestamp)}")
                    Text("Last: ${formatTrackTime(points.last().timestamp)}")
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (!state.isSaved) TextButton(enabled = !state.saving, onClick = { onSave(points) }, modifier = Modifier.testTag("save_flight")) {
                            Text(if (state.saving) "Saving…" else "Save this flight")
                        }
                        TextButton(onClick = { onExport(points) }, modifier = Modifier.testTag("export_flight")) { Text("Export GPX") }
                    }
                    state.saveMessage?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                    exportMessage?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                    if (points.size > 1) {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = {
                                if (!playing) playbackTimeMs = selectedTimestamp?.takeIf { it < points.last().timestamp } ?: points.first().timestamp
                                playing = !playing
                            }, modifier = Modifier.testTag("flight_play_pause")) { Text(if (playing) "Pause" else "Play flight") }
                            listOf(1, 4, 16).forEach { speed ->
                                FilterChip(selected = playbackSpeed == speed, onClick = { playbackSpeed = speed }, label = { Text("${speed}×") })
                            }
                        }
                        Text("Scrub recorded positions", style = MaterialTheme.typography.titleSmall)
                        Slider(
                            value = selectedIndex.toFloat(),
                            onValueChange = { playing = false; selectedTimestamp = points[it.roundToInt().coerceIn(points.indices)].timestamp },
                            valueRange = 0f..points.lastIndex.toFloat(),
                            modifier = Modifier.testTag("flight_path_scrubber").semantics {
                                stateDescription = selected?.let { formatTrackTime(it.timestamp) }.orEmpty()
                            },
                        )
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        TextButton(
                            enabled = selectedIndex > 0,
                            onClick = { playing = false; selectedTimestamp = points[selectedIndex - 1].timestamp },
                            modifier = Modifier.testTag("flight_path_previous"),
                        ) { Text("Previous") }
                        TextButton(
                            enabled = selectedTimestamp != null,
                            onClick = { playing = false; selectedTimestamp = null; fitRequest++ },
                            modifier = Modifier.testTag("flight_path_latest"),
                        ) { Text("Latest position") }
                        TextButton(
                            enabled = selectedIndex < points.lastIndex,
                            onClick = { playing = false; selectedTimestamp = points[selectedIndex + 1].timestamp },
                            modifier = Modifier.testTag("flight_path_next"),
                        ) { Text("Next") }
                    }
                    Text(
                        if (selectedTimestamp == null) "Following latest received position"
                        else "Reviewing position ${selectedIndex + 1} of ${points.size}",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    FlightMetricChart("Altitude", "ft", points, selected) { it.altitudeMeters * 3.28084 }
                    FlightMetricChart("Speed", "kt", points, selected) { it.speedMps?.takeIf { v -> v.isFinite() && v >= 0 }?.times(1.94384) }
                    selected?.let { point ->
                        Text(formatTrackTime(point.timestamp), style = MaterialTheme.typography.titleMedium)
                        Text("${(point.altitudeMeters * 3.28084).roundToInt()} ft" +
                            (point.speedMps?.takeIf { it.isFinite() && it >= 0 }?.let { " · ${(it * 1.94384).roundToInt()} kt" } ?: ""))
                        Text("%.5f, %.5f".format(Locale.US, point.latitude, point.longitude))
                    }
                }
            }
        }
    }
}

@Composable
private fun FlightPathMap(
    points: List<TrackingEntity>,
    selected: TrackingEntity?,
    fitKey: Any,
    modifier: Modifier,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val map = remember {
        Configuration.getInstance().userAgentValue = context.packageName
        MapView(context).apply {
            setDestroyMode(false)
            setTileSource(TileSourceFactory.MAPNIK)
            setMultiTouchControls(true)
            controller.setZoom(11.0)
        }
    }
    val paths = remember(map) { AircraftTrailOverlay(map) }
    fun pointMarker(label: String, color: Int) = Marker(map).apply {
        title = label
        setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
        icon = android.graphics.drawable.GradientDrawable().apply {
            shape = android.graphics.drawable.GradientDrawable.OVAL
            setColor(color)
            setStroke((2 * context.resources.displayMetrics.density).toInt(), android.graphics.Color.WHITE)
            val diameter = (16 * context.resources.displayMetrics.density).toInt()
            setSize(diameter, diameter)
        }
        map.overlays.add(this)
    }
    val startMarker = remember(map) { pointMarker("First recorded position", android.graphics.Color.DKGRAY) }
    val marker = remember(map) { pointMarker("Selected recorded position", android.graphics.Color.rgb(0, 137, 194)) }
    DisposableEffect(map, lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> map.onResume()
                Lifecycle.Event.ON_PAUSE -> map.onPause()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) map.onResume()
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            map.onPause()
        }
    }
    DisposableEffect(map) {
        onDispose { map.onDetach() }
    }
    DisposableEffect(map, fitKey) {
        val fit = Runnable {
            val locations = points.map { GeoPoint(it.latitude, it.longitude) }
            if (locations.size < 2 || locations.distinct().size < 2) {
                locations.firstOrNull()?.let { map.controller.setCenter(it) }
            } else {
                map.zoomToBoundingBox(BoundingBox.fromGeoPointsSafe(locations), false, 64)
            }
        }
        map.post(fit)
        onDispose { map.removeCallbacks(fit) }
    }
    AndroidView(
        factory = { map }, modifier = modifier.clipToBounds().testTag("flight_path_map"),
        update = {
            paths.render(points)
            points.firstOrNull()?.let { startMarker.position = GeoPoint(it.latitude, it.longitude) }
            selected?.let { point ->
                marker.position = GeoPoint(point.latitude, point.longitude)
                marker.snippet = formatTrackTime(point.timestamp)
            }
            map.invalidate()
        },
    )
}

internal fun formatTrackTime(timestamp: Long): String = Instant.ofEpochMilli(timestamp)
    .atZone(ZoneId.systemDefault()).format(DateTimeFormatter.ofPattern("MMM d, HH:mm:ss", Locale.getDefault()))

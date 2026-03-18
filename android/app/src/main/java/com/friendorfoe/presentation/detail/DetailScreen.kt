package com.friendorfoe.presentation.detail

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Divider
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.detection.WifiChannelUtil
import com.friendorfoe.detection.WifiOuiDatabase
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.util.IcaoCountryLookup
import com.friendorfoe.presentation.util.categoryColor
import com.friendorfoe.presentation.util.silhouetteForTypeCode
import com.friendorfoe.presentation.util.silhouetteForCategory
import com.friendorfoe.presentation.util.silhouetteDrawableRes
import java.time.Duration
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.roundToInt
import androidx.compose.foundation.Image
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.ZoomIn
import androidx.compose.material3.Icon
import coil.compose.AsyncImage
import coil.compose.AsyncImagePainter
import coil.compose.SubcomposeAsyncImage
import coil.compose.SubcomposeAsyncImageContent
import com.friendorfoe.presentation.util.getAircraftPhotoUrl
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.clickable
import androidx.compose.material.icons.filled.OpenInNew

/**
 * Detail screen showing full information about a specific sky object.
 *
 * Fetches enrichment data from the backend on appear and displays:
 * - Aircraft: identity, route, stats, detection source
 * - Drone: ID, manufacturer, operator location, signal info
 *
 * @param objectId The unique ID of the sky object to display
 * @param onBack Callback to navigate back
 * @param viewModel Detail ViewModel injected via Hilt
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DetailScreen(
    objectId: String,
    onBack: () -> Unit,
    onNavigateToDroneGuide: ((String?) -> Unit)? = null,
    onNavigateToAircraftGuide: ((String?) -> Unit)? = null,
    viewModel: DetailViewModel = hiltViewModel()
) {
    val detailState by viewModel.detailState.collectAsStateWithLifecycle()
    val positionTrail by viewModel.positionTrail.collectAsStateWithLifecycle()

    // Load detail on first composition
    LaunchedEffect(objectId) {
        viewModel.loadDetail(objectId)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Object Detail") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Text(
                            text = "\u2190",
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
        ) {
            when (val state = detailState) {
                is DetailState.Idle -> {
                    CenteredMessage("Select an object to view details")
                }

                is DetailState.Loading -> {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                }

                is DetailState.AircraftLoaded -> {
                    AircraftDetailContent(
                        aircraft = state.aircraft,
                        detail = state.detail,
                        onNavigateToAircraftGuide = onNavigateToAircraftGuide
                    )
                }

                is DetailState.DroneLoaded -> {
                    DroneDetailContent(
                        drone = state.drone,
                        positionTrail = positionTrail,
                        onNavigateToDroneGuide = onNavigateToDroneGuide
                    )
                }

                is DetailState.Error -> {
                    CenteredMessage("Error: ${state.message}")
                }
            }
        }
    }
}

// ---- Aircraft Detail ----

/**
 * Full detail card for an aircraft, showing identity, route, stats, and detection info.
 */
@Composable
internal fun AircraftDetailContent(
    aircraft: Aircraft,
    detail: AircraftDetailDto?,
    onZoom: (() -> Unit)? = null,
    onLockOn: (() -> Unit)? = null,
    onNavigateToAircraftGuide: ((String?) -> Unit)? = null
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // Header with category color and callsign
        AircraftHeader(aircraft, detail)

        // Aircraft photo (or silhouette fallback)
        PhotoPlaceholder(
            aircraftType = detail?.aircraftType ?: aircraft.aircraftType,
            aircraftDescription = detail?.aircraftDescription ?: aircraft.aircraftModel,
            category = aircraft.category,
            photoUrl = aircraft.photoUrl
        )

        // Identity section
        SectionCard(title = "Identity") {
            DetailRow("Callsign", aircraft.callsign ?: "Unknown")
            DetailRow("ICAO Hex", aircraft.icaoHex)
            DetailRow("Registration", detail?.registration ?: aircraft.registration ?: "Unknown")
            DetailRow("Type", detail?.aircraftType ?: aircraft.aircraftType ?: "Unknown")
            DetailRow("Model", detail?.aircraftDescription ?: aircraft.aircraftModel ?: "Unknown")
            DetailRow("Operator", detail?.operator ?: aircraft.airline ?: "Unknown")
            IcaoCountryLookup.countryFromIcaoHex(aircraft.icaoHex)?.let { country ->
                DetailRow("Country", country)
            }
            DetailRow("Category", formatCategory(aircraft.category))
        }

        // Route section
        val origin = detail?.route?.origin ?: aircraft.origin
        val destination = detail?.route?.destination ?: aircraft.destination
        if (origin != null || destination != null) {
            SectionCard(title = "Route") {
                RouteDisplay(
                    origin = origin ?: "???",
                    destination = destination ?: "???"
                )
            }
        }

        // Current stats
        SectionCard(title = "Current Position") {
            val altFeet = (aircraft.position.altitudeMeters * 3.281).roundToInt()
            DetailRow("Altitude", "$altFeet ft (${aircraft.position.altitudeMeters.roundToInt()} m)")

            aircraft.position.speedMps?.let { speedMps ->
                val speedKnots = (speedMps * 1.944).roundToInt()
                DetailRow("Speed", "$speedKnots kts (${speedMps.roundToInt()} m/s)")
            }

            aircraft.position.verticalRateMps?.let { vr ->
                val fpm = (vr / 0.3048f * 60f).roundToInt()
                val label = when {
                    fpm > 100 -> "Climbing"
                    fpm < -100 -> "Descending"
                    else -> "Level"
                }
                DetailRow("Vertical Rate", "$fpm fpm ($label)")
            }

            aircraft.position.heading?.let { heading ->
                DetailRow("Heading", "${heading.roundToInt()}\u00B0 ${headingToCardinal(heading)}")
            }

            aircraft.distanceMeters?.let { dist ->
                DetailRow("Distance", formatDistance(dist))
            }

            DetailRow("Latitude", String.format("%.5f", aircraft.position.latitude))
            DetailRow("Longitude", String.format("%.5f", aircraft.position.longitude))

            aircraft.squawk?.let { squawk ->
                DetailRow("Squawk", formatSquawk(squawk))
            }

            DetailRow("On Ground", if (aircraft.isOnGround) "Yes" else "No")
        }

        // Classification signals (for MILITARY/GOVERNMENT/EMERGENCY)
        if (!aircraft.classificationSignals.isNullOrEmpty()) {
            SectionCard(title = "Classification Signals") {
                aircraft.classificationSignals.forEach { signal ->
                    DetailRow("Signal", signal)
                }
            }
        }

        // Detection source
        SectionCard(title = "Detection") {
            DetectionSourceBadge(
                source = aircraft.source,
                confidence = aircraft.confidence
            )
        }

        // External lookup links
        SectionCard(title = "Look Up") {
            ExternalLinkRow("ADS-B Exchange", "https://globe.adsbexchange.com/?icao=${aircraft.icaoHex}")
            aircraft.callsign?.let { cs ->
                ExternalLinkRow("FlightAware", "https://flightaware.com/live/flight/$cs")
            }
        }

        // Zoom button (only in AR bottom sheet context)
        if (onZoom != null) {
            Button(
                onClick = onZoom,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF2196F3)
                )
            ) {
                Icon(
                    imageVector = Icons.Default.ZoomIn,
                    contentDescription = "Zoom",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text("Zoom & Capture")
            }
        }

        // Lock-on button (only in AR bottom sheet context)
        if (onLockOn != null) {
            Button(
                onClick = onLockOn,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF009688)
                )
            ) {
                Icon(
                    imageVector = Icons.Default.MyLocation,
                    contentDescription = "Lock On",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text("Lock On & Track")
            }
        }

        // Aircraft Reference Guide link
        if (onNavigateToAircraftGuide != null) {
            val typeCode = detail?.aircraftType ?: aircraft.aircraftType
            Button(
                onClick = { onNavigateToAircraftGuide(typeCode) },
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF1565C0)
                )
            ) {
                Icon(
                    imageVector = Icons.Default.Info,
                    contentDescription = "Aircraft Guide",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    if (typeCode != null) "View $typeCode in Aircraft Guide"
                    else "Browse Aircraft Reference Guide"
                )
            }
        }

        Spacer(modifier = Modifier.height(16.dp))
    }
}

// ---- Drone Detail ----

/**
 * Full detail card for a drone, showing ID, manufacturer, operator info, and signal data.
 */
@Composable
internal fun DroneDetailContent(
    drone: Drone,
    nearbyCandidates: List<Drone> = emptyList(),
    positionTrail: List<SkyObjectRepository.TrailPoint> = emptyList(),
    onZoom: (() -> Unit)? = null,
    onLockOn: (() -> Unit)? = null,
    onNavigateToDroneGuide: ((String?) -> Unit)? = null
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // Header
        DroneHeader(drone)

        // Identity section
        SectionCard(title = "Drone Identity") {
            DetailRow("Drone ID", drone.droneId)
            DetailRow("Manufacturer", drone.manufacturer ?: "Unknown")
            DetailRow("Model", drone.model ?: "Unknown")
            drone.operatorId?.let { opId ->
                DetailRow("Operator ID", opId)
            }
            drone.uaTypeLabel()?.let { label ->
                DetailRow("UA Type", label)
            }
            drone.selfIdText?.let { DetailRow("Self-ID", it) }
            drone.idTypeLabel()?.let { DetailRow("ID Type", it) }
        }

        // Position section
        SectionCard(title = "Position") {
            val altFeet = (drone.position.altitudeMeters * 3.281).roundToInt()
            if (drone.position.latitude != 0.0 || drone.position.longitude != 0.0) {
                DetailRow("Altitude", "$altFeet ft (${drone.position.altitudeMeters.roundToInt()} m)")
                DetailRow("Latitude", String.format("%.5f", drone.position.latitude))
                DetailRow("Longitude", String.format("%.5f", drone.position.longitude))

                drone.position.heading?.let { heading ->
                    DetailRow("Heading", "${heading.roundToInt()}\u00B0 ${headingToCardinal(heading)}")
                }

                drone.position.speedMps?.let { speed ->
                    DetailRow("Speed", "${speed.roundToInt()} m/s")
                }

                drone.geodeticAltitudeMeters?.let { geoAlt ->
                    val geoAltFeet = (geoAlt * 3.281).roundToInt()
                    if (geoAltFeet != (drone.position.altitudeMeters * 3.281).roundToInt()) {
                        DetailRow("Geodetic Alt", "$geoAltFeet ft (${geoAlt.roundToInt()} m)")
                    }
                }

                drone.heightAglMeters?.let { agl ->
                    val aglFeet = (agl * 3.281).roundToInt()
                    DetailRow("Height AGL", "$aglFeet ft (${agl.roundToInt()} m)")
                }

                drone.verticalSpeedMps?.let { vs ->
                    val arrow = when {
                        vs > 0.25f -> "\u2191"   // up arrow
                        vs < -0.25f -> "\u2193"  // down arrow
                        else -> "\u2194"          // level
                    }
                    DetailRow("Vertical Speed", "$arrow ${"%.1f".format(vs)} m/s")
                }

                drone.horizontalAccuracyMeters?.let { acc ->
                    DetailRow("H. Accuracy", "\u00B1 ${"%.0f".format(acc)} m")
                }

                drone.verticalAccuracyMeters?.let { acc ->
                    DetailRow("V. Accuracy", "\u00B1 ${"%.0f".format(acc)} m")
                }
            } else {
                DetailRow("GPS Position", "Not available")
                val reason = when {
                    drone.source == DetectionSource.WIFI && drone.confidence < 0.5f ->
                        "Detected by WiFi SSID only. Sub-250g drones are exempt from " +
                        "FAA Remote ID and do not broadcast GPS coordinates."
                    drone.source == DetectionSource.WIFI ->
                        "WiFi detection \u2014 no GPS coordinates in broadcast"
                    else -> "No position data available"
                }
                Text(
                    text = reason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                    modifier = Modifier.padding(top = 4.dp)
                )
            }

            drone.distanceMeters?.let { dist ->
                DetailRow("Distance", formatDistance(dist))
            }

        }

        // Operator location (Remote ID only)
        if (drone.operatorLatitude != null && drone.operatorLongitude != null) {
            SectionCard(title = "Operator Location") {
                DetailRow("Latitude", String.format("%.5f", drone.operatorLatitude))
                DetailRow("Longitude", String.format("%.5f", drone.operatorLongitude))
            }
        }

        // Transmitter section (WiFi radio details)
        val hasTransmitterInfo = drone.bssid != null || drone.signalStrengthDbm != null || drone.ssid != null
        if (hasTransmitterInfo) {
            SectionCard(title = "Transmitter") {
                drone.ssid?.let {
                    DetailRow("SSID", it)
                }
                drone.bssid?.let { bssid ->
                    DetailRow("BSSID", bssid)
                    WifiOuiDatabase.lookup(bssid)?.let { ouiEntry ->
                        DetailRow("Hardware", ouiEntry.fullName)
                    }
                }
                drone.frequencyMhz?.let {
                    DetailRow("Channel", WifiChannelUtil.frequencyToChannelLabel(it))
                }
                drone.channelWidthMhz?.let {
                    DetailRow("Bandwidth", "$it MHz")
                }
                drone.signalStrengthDbm?.let {
                    DetailRow("Signal", "$it dBm")
                }
                drone.estimatedDistanceMeters?.let {
                    DetailRow("Est. Distance", formatDistance(it) + " (signal)")
                }
            }
        }

        // Flight Path
        SectionCard(title = "Flight Path") {
            if (positionTrail.isEmpty()) {
                DetailRow("Status", "No position history available")
            } else {
                DetailRow("Track Points", "${positionTrail.size}")
                val firstTime = positionTrail.first().timestamp
                val lastTime = positionTrail.last().timestamp
                val duration = Duration.between(firstTime, lastTime)
                val minutes = duration.toMinutes()
                val seconds = duration.seconds % 60
                DetailRow("Duration", "${minutes}m ${seconds}s")

                // Total distance traveled
                var totalDist = 0.0
                for (i in 1 until positionTrail.size) {
                    totalDist += haversineMeters(
                        positionTrail[i - 1].lat, positionTrail[i - 1].lon,
                        positionTrail[i].lat, positionTrail[i].lon
                    )
                }
                DetailRow("Distance Traveled", formatDistance(totalDist))

                Spacer(modifier = Modifier.height(4.dp))
                Divider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))
                Spacer(modifier = Modifier.height(4.dp))

                val timeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")
                    .withZone(ZoneId.systemDefault())
                val recentPoints = positionTrail.takeLast(5)
                recentPoints.forEach { point ->
                    val altFeet = (point.altM * 3.281).roundToInt()
                    DetailRow(
                        timeFormatter.format(point.timestamp),
                        "${String.format("%.5f", point.lat)}, ${String.format("%.5f", point.lon)} · ${altFeet}ft"
                    )
                }
                if (positionTrail.size > 5) {
                    DetailRow("", "... ${positionTrail.size - 5} earlier points")
                }
            }
        }

        // Detection source
        SectionCard(title = "Detection") {
            DetectionSourceBadge(
                source = drone.source,
                confidence = drone.confidence
            )
        }

        // Drone Reference Guide link
        if (onNavigateToDroneGuide != null) {
            val isUnknown = drone.manufacturer == null || drone.manufacturer == "Unknown" || drone.manufacturer == "Generic"
            val isKnownManufacturerUnknownModel = drone.manufacturer != null &&
                drone.manufacturer != "Unknown" && drone.manufacturer != "Generic" &&
                drone.model == null

            if (isUnknown) {
                Button(
                    onClick = { onNavigateToDroneGuide(null) },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0xFF9C27B0)
                    )
                ) {
                    Text("Browse Drone Reference Guide")
                }
            } else if (isKnownManufacturerUnknownModel) {
                Button(
                    onClick = { onNavigateToDroneGuide(drone.manufacturer) },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0xFF9C27B0)
                    )
                ) {
                    Text("Could this be a ${drone.manufacturer} drone?")
                }
            }
        }

        // Nearby Drone IDs
        SectionCard(title = "Nearby Drone IDs") {
            if (nearbyCandidates.isEmpty()) {
                DetailRow("Status", "No other drone IDs detected")
            } else {
                nearbyCandidates.forEachIndexed { index, candidate ->
                    if (index > 0) {
                        Spacer(modifier = Modifier.height(4.dp))
                        Divider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))
                        Spacer(modifier = Modifier.height(4.dp))
                    }
                    DetailRow("Drone ID", candidate.droneId.take(16))
                    val sourceLabel = when (candidate.source) {
                        DetectionSource.REMOTE_ID -> "BLE"
                        DetectionSource.WIFI_NAN -> "NaN"
                        DetectionSource.WIFI_BEACON -> "Beacon"
                        DetectionSource.WIFI -> "WiFi"
                        else -> candidate.source.name
                    }
                    DetailRow("Source", sourceLabel)
                    candidate.manufacturer?.let { mfr ->
                        DetailRow("Manufacturer", mfr)
                    }
                    val candidateHasPos = candidate.position.latitude != 0.0 || candidate.position.longitude != 0.0
                    val tappedHasPos = drone.position.latitude != 0.0 || drone.position.longitude != 0.0
                    if (tappedHasPos && candidateHasPos) {
                        val dist = haversineMeters(
                            drone.position.latitude, drone.position.longitude,
                            candidate.position.latitude, candidate.position.longitude
                        )
                        DetailRow("Distance", formatDistance(dist))
                    } else {
                        DetailRow("Distance", "WiFi only")
                    }
                }
            }
        }

        // Zoom button (only in AR bottom sheet context)
        if (onZoom != null) {
            Button(
                onClick = onZoom,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF2196F3)
                )
            ) {
                Icon(
                    imageVector = Icons.Default.ZoomIn,
                    contentDescription = "Zoom",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text("Zoom & Capture")
            }
        }

        // Lock-on button (only in AR bottom sheet context)
        if (onLockOn != null) {
            Button(
                onClick = onLockOn,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF009688)
                )
            ) {
                Icon(
                    imageVector = Icons.Default.MyLocation,
                    contentDescription = "Lock On",
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text("Lock On & Track")
            }
        }

        Spacer(modifier = Modifier.height(16.dp))
    }
}

// ---- Shared UI Components ----

/**
 * Header banner for an aircraft with category color and callsign.
 */
@Composable
private fun AircraftHeader(aircraft: Aircraft, detail: AircraftDetailDto?) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = categoryColor(aircraft.category).copy(alpha = 0.15f)
        ),
        shape = RoundedCornerShape(12.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Category color dot
            Box(
                modifier = Modifier
                    .size(16.dp)
                    .clip(CircleShape)
                    .background(categoryColor(aircraft.category))
            )

            Spacer(modifier = Modifier.width(12.dp))

            Column {
                Text(
                    text = aircraft.callsign ?: aircraft.icaoHex,
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = detail?.aircraftDescription ?: aircraft.aircraftModel
                        ?: aircraft.aircraftType ?: "Unknown Aircraft",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                )
                val airline = detail?.operator ?: aircraft.airline
                if (airline != null) {
                    Text(
                        text = airline,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                    )
                }
            }
        }
    }
}

/**
 * Header banner for a drone with blue category indicator (amber for visual-only).
 */
@Composable
private fun DroneHeader(drone: Drone) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = Color(0xFF2196F3).copy(alpha = 0.15f)
        ),
        shape = RoundedCornerShape(12.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(16.dp)
                    .clip(CircleShape)
                    .background(Color(0xFF2196F3))
            )

            Spacer(modifier = Modifier.width(12.dp))

            Column {
                Text(
                    text = drone.manufacturer ?: "Unknown Drone",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = drone.model ?: "ID: ${drone.droneId.take(16)}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                )
                Text(
                    text = when (drone.source) {
                        DetectionSource.REMOTE_ID -> "Remote ID (BLE)"
                        DetectionSource.WIFI_NAN -> "Remote ID (NaN)"
                        DetectionSource.WIFI_BEACON -> "Remote ID (Beacon)"
                        DetectionSource.WIFI -> "WiFi Detection"
                        else -> drone.source.name
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                )
            }
        }
    }
}

/**
 * Aircraft photo card. Tries to load a real photo from GitHub-hosted images,
 * falls back to tinted vector silhouette when no photo is available or loading fails.
 */
@Composable
private fun PhotoPlaceholder(
    aircraftType: String?,
    aircraftDescription: String?,
    category: ObjectCategory,
    photoUrl: String? = null
) {
    val silhouette = silhouetteForTypeCode(aircraftType) ?: silhouetteForCategory(category)
    val drawableRes = silhouetteDrawableRes(silhouette)
    val tintColor = categoryColor(category)
    // Prefer backend-enriched photo URL, then GitHub-hosted type photo
    val imageUrl = photoUrl ?: getAircraftPhotoUrl(aircraftType)

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .height(180.dp),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            if (imageUrl != null) {
                SubcomposeAsyncImage(
                    model = imageUrl,
                    contentDescription = aircraftDescription ?: aircraftType ?: "Aircraft",
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop
                ) {
                    when (painter.state) {
                        is AsyncImagePainter.State.Success -> {
                            SubcomposeAsyncImageContent(
                                modifier = Modifier
                                    .fillMaxSize()
                                    .clip(RoundedCornerShape(12.dp))
                            )
                        }
                        is AsyncImagePainter.State.Loading -> {
                            // Show silhouette while loading
                            SilhouetteFallback(drawableRes, tintColor, aircraftType)
                        }
                        else -> {
                            // Error or empty — show silhouette
                            SilhouetteFallback(drawableRes, tintColor, aircraftType)
                        }
                    }
                }
            } else {
                SilhouetteFallback(drawableRes, tintColor, aircraftType)
            }
        }
    }
}

@Composable
private fun SilhouetteFallback(drawableRes: Int, tintColor: Color, aircraftType: String?) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Image(
            painter = painterResource(id = drawableRes),
            contentDescription = aircraftType ?: "Aircraft",
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .height(120.dp),
            contentScale = ContentScale.Fit,
            colorFilter = ColorFilter.tint(tintColor)
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = aircraftType ?: "Unknown",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
        )
    }
}

/**
 * Route display showing origin -> destination with arrow.
 */
@Composable
private fun RouteDisplay(origin: String, destination: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = origin,
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )

        Text(
            text = "  \u2192  ",
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
        )

        Text(
            text = destination,
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )
    }
}

/**
 * Reusable section card with a title header.
 */
@Composable
private fun SectionCard(
    title: String,
    content: @Composable () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(8.dp))
            Divider(color = MaterialTheme.colorScheme.outlineVariant)
            Spacer(modifier = Modifier.height(8.dp))
            content()
        }
    }
}

/**
 * Single key-value detail row.
 */
@Composable
private fun DetailRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
            modifier = Modifier.weight(0.4f)
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            textAlign = TextAlign.End,
            modifier = Modifier.weight(0.6f)
        )
    }
}

/**
 * Clickable row that opens a URL in the system browser.
 */
@Composable
private fun ExternalLinkRow(label: String, url: String) {
    val context = androidx.compose.ui.platform.LocalContext.current
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable {
                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
            }
            .padding(vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Medium
        )
        Icon(
            imageVector = Icons.Default.OpenInNew,
            contentDescription = "Open",
            modifier = Modifier.size(16.dp),
            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
        )
    }
}

/**
 * Detection source badge showing source type and confidence level.
 */
@Composable
private fun DetectionSourceBadge(
    source: DetectionSource,
    confidence: Float
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(
                text = "Source",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
            )
            Text(
                text = when (source) {
                    DetectionSource.ADS_B -> "ADS-B Transponder"
                    DetectionSource.REMOTE_ID -> "FAA Remote ID (BLE)"
                    DetectionSource.WIFI_NAN -> "FAA Remote ID (WiFi NaN)"
                    DetectionSource.WIFI_BEACON -> "FAA Remote ID (WiFi Beacon)"
                    DetectionSource.WIFI -> "WiFi SSID Pattern"
                },
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium
            )
        }

        // Confidence badge
        val confidencePercent = (confidence * 100).roundToInt()
        val confidenceColor = when {
            confidence >= 0.8f -> Color(0xFF4CAF50)
            confidence >= 0.5f -> Color(0xFFFFEB3B)
            else -> Color(0xFFF44336)
        }

        Box(
            modifier = Modifier
                .background(
                    color = confidenceColor.copy(alpha = 0.2f),
                    shape = RoundedCornerShape(16.dp)
                )
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            Text(
                text = "$confidencePercent% confidence",
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.Bold,
                color = confidenceColor
            )
        }
    }
}

/**
 * Centered message text for empty/error states.
 */
@Composable
private fun CenteredMessage(message: String) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = message,
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
        )
    }
}

// ---- Helper functions ----

/**
 * Format category enum to human-readable string.
 */
private fun formatCategory(category: ObjectCategory): String {
    return when (category) {
        ObjectCategory.COMMERCIAL -> "Commercial"
        ObjectCategory.GENERAL_AVIATION -> "General Aviation"
        ObjectCategory.MILITARY -> "Military"
        ObjectCategory.HELICOPTER -> "Helicopter"
        ObjectCategory.GOVERNMENT -> "Government"
        ObjectCategory.EMERGENCY -> "Emergency"
        ObjectCategory.CARGO -> "Cargo"
        ObjectCategory.DRONE -> "Drone / UAS"
        ObjectCategory.GROUND_VEHICLE -> "Ground Vehicle"
        ObjectCategory.UNKNOWN -> "Unknown"
    }
}

private fun formatSquawk(squawk: String): String {
    return when (squawk) {
        "7700" -> "$squawk - EMERGENCY"
        "7600" -> "$squawk - COMM FAILURE"
        "7500" -> "$squawk - HIJACK"
        "1200" -> "$squawk - VFR"
        "1000" -> "$squawk - IFR (no assigned code)"
        "7777" -> "$squawk - MILITARY INTERCEPT"
        else -> squawk
    }
}

/**
 * Format distance in meters to human-readable string.
 */
private fun formatDistance(meters: Double): String {
    return when {
        meters < 1000 -> "${meters.roundToInt()} m"
        meters < 10_000 -> String.format("%.1f km", meters / 1000)
        else -> "${(meters / 1000).roundToInt()} km"
    }
}

/**
 * Convert a heading in degrees to a cardinal direction abbreviation.
 */
private fun haversineMeters(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val r = 6_371_000.0
    val dLat = Math.toRadians(lat2 - lat1)
    val dLon = Math.toRadians(lon2 - lon1)
    val a = kotlin.math.sin(dLat / 2) * kotlin.math.sin(dLat / 2) +
            kotlin.math.cos(Math.toRadians(lat1)) * kotlin.math.cos(Math.toRadians(lat2)) *
            kotlin.math.sin(dLon / 2) * kotlin.math.sin(dLon / 2)
    return r * 2 * kotlin.math.atan2(kotlin.math.sqrt(a), kotlin.math.sqrt(1 - a))
}

private fun headingToCardinal(heading: Float): String {
    val normalized = ((heading % 360f) + 360f) % 360f
    return when {
        normalized < 22.5f -> "N"
        normalized < 67.5f -> "NE"
        normalized < 112.5f -> "E"
        normalized < 157.5f -> "SE"
        normalized < 202.5f -> "S"
        normalized < 247.5f -> "SW"
        normalized < 292.5f -> "W"
        normalized < 337.5f -> "NW"
        else -> "N"
    }
}

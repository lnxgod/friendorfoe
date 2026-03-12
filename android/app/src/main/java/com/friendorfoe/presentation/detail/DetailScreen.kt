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
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import kotlin.math.roundToInt

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
    viewModel: DetailViewModel = hiltViewModel()
) {
    val detailState by viewModel.detailState.collectAsStateWithLifecycle()

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
                        detail = state.detail
                    )
                }

                is DetailState.DroneLoaded -> {
                    DroneDetailContent(drone = state.drone)
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
    detail: AircraftDetailDto?
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

        // Photo placeholder
        PhotoPlaceholder(
            photoUrl = detail?.photoUrl ?: aircraft.photoUrl,
            aircraftType = detail?.aircraftModel ?: aircraft.aircraftType
        )

        // Identity section
        SectionCard(title = "Identity") {
            DetailRow("Callsign", aircraft.callsign ?: "Unknown")
            DetailRow("ICAO Hex", aircraft.icaoHex)
            DetailRow("Registration", detail?.registration ?: aircraft.registration ?: "Unknown")
            DetailRow("Type", detail?.aircraftType ?: aircraft.aircraftType ?: "Unknown")
            DetailRow("Model", detail?.aircraftModel ?: aircraft.aircraftModel ?: "Unknown")
            DetailRow("Airline", detail?.airline ?: aircraft.airline ?: "Unknown")
            DetailRow("Category", formatCategory(aircraft.category))
        }

        // Route section
        val origin = detail?.origin ?: aircraft.origin
        val destination = detail?.destination ?: aircraft.destination
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

            aircraft.position.heading?.let { heading ->
                DetailRow("Heading", "${heading.roundToInt()}\u00B0 ${headingToCardinal(heading)}")
            }

            aircraft.distanceMeters?.let { dist ->
                DetailRow("Distance", formatDistance(dist))
            }

            DetailRow("Latitude", String.format("%.5f", aircraft.position.latitude))
            DetailRow("Longitude", String.format("%.5f", aircraft.position.longitude))

            aircraft.squawk?.let { squawk ->
                DetailRow("Squawk", squawk)
            }

            DetailRow("On Ground", if (aircraft.isOnGround) "Yes" else "No")
        }

        // Detection source
        SectionCard(title = "Detection") {
            DetectionSourceBadge(
                source = aircraft.source,
                confidence = aircraft.confidence
            )
        }

        Spacer(modifier = Modifier.height(16.dp))
    }
}

// ---- Drone Detail ----

/**
 * Full detail card for a drone, showing ID, manufacturer, operator info, and signal data.
 */
@Composable
internal fun DroneDetailContent(drone: Drone) {
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
            drone.ssid?.let { ssid ->
                DetailRow("WiFi SSID", ssid)
            }
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
            } else {
                DetailRow("Position", "Not available (WiFi detection)")
            }

            drone.distanceMeters?.let { dist ->
                DetailRow("Distance", formatDistance(dist))
            }

            drone.estimatedDistanceMeters?.let { dist ->
                DetailRow("Est. Distance", formatDistance(dist) + " (signal-based)")
            }
        }

        // Operator location (Remote ID only)
        if (drone.operatorLatitude != null && drone.operatorLongitude != null) {
            SectionCard(title = "Operator Location") {
                DetailRow("Latitude", String.format("%.5f", drone.operatorLatitude))
                DetailRow("Longitude", String.format("%.5f", drone.operatorLongitude))
            }
        }

        // Signal info
        if (drone.signalStrengthDbm != null) {
            SectionCard(title = "Signal") {
                DetailRow("Signal Strength", "${drone.signalStrengthDbm} dBm")
            }
        }

        // Detection source
        SectionCard(title = "Detection") {
            DetectionSourceBadge(
                source = drone.source,
                confidence = drone.confidence
            )
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
                    text = detail?.aircraftModel ?: aircraft.aircraftModel
                        ?: aircraft.aircraftType ?: "Unknown Aircraft",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                )
                val airline = detail?.airline ?: aircraft.airline
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
 * Header banner for a drone with blue category indicator.
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
 * Photo placeholder card. Displays a placeholder for aircraft photos.
 *
 * Note: Uses a text placeholder since Coil is not in the dependency list.
 * When Coil is added, replace with AsyncImage for actual photo loading.
 */
@Composable
private fun PhotoPlaceholder(photoUrl: String?, aircraftType: String?) {
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
            if (photoUrl != null) {
                // TODO: Replace with AsyncImage when Coil dependency is added
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = aircraftType ?: "Aircraft",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = "Photo available",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                    )
                }
            } else {
                Text(
                    text = aircraftType ?: "No photo available",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f)
                )
            }
        }
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
 * Map [ObjectCategory] to its display color.
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

/**
 * Format category enum to human-readable string.
 */
private fun formatCategory(category: ObjectCategory): String {
    return when (category) {
        ObjectCategory.COMMERCIAL -> "Commercial"
        ObjectCategory.GENERAL_AVIATION -> "General Aviation"
        ObjectCategory.MILITARY -> "Military"
        ObjectCategory.DRONE -> "Drone / UAS"
        ObjectCategory.UNKNOWN -> "Unknown"
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

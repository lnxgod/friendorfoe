package com.friendorfoe.presentation.list

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject

/**
 * List View screen showing all detected sky objects sorted by distance.
 *
 * Displays each object with a color-coded category indicator, callsign/ID,
 * type info, altitude, distance, and detection source icon.
 *
 * @param onObjectTapped Callback when a list item is tapped, receives object ID
 */
@Composable
fun ListViewScreen(
    onObjectTapped: (String) -> Unit,
    viewModel: ListViewModel = hiltViewModel()
) {
    val skyObjects by viewModel.skyObjects.collectAsStateWithLifecycle()

    if (skyObjects.isEmpty()) {
        EmptyListState()
    } else {
        LazyColumn(
            modifier = Modifier.fillMaxSize()
        ) {
            items(
                items = skyObjects,
                key = { it.id }
            ) { skyObject ->
                SkyObjectItem(
                    skyObject = skyObject,
                    onClick = { onObjectTapped(skyObject.id) }
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outlineVariant,
                    thickness = 0.5.dp
                )
            }
        }
    }
}

@Composable
private fun EmptyListState() {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = "No objects detected",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = "Aircraft and drones will appear here\nwhen detected nearby",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                textAlign = androidx.compose.ui.text.style.TextAlign.Center
            )
        }
    }
}

@Composable
private fun SkyObjectItem(
    skyObject: SkyObject,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Category color dot
        Box(
            modifier = Modifier
                .size(12.dp)
                .clip(CircleShape)
                .background(categoryColor(skyObject.category))
        )

        Spacer(modifier = Modifier.width(12.dp))

        // Primary and secondary text
        Column(
            modifier = Modifier.weight(1f)
        ) {
            Text(
                text = primaryText(skyObject),
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = secondaryText(skyObject),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }

        Spacer(modifier = Modifier.width(8.dp))

        // Altitude and distance
        Column(
            horizontalAlignment = Alignment.End
        ) {
            Text(
                text = formatAltitude(skyObject.position.altitudeMeters),
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = formatDistance(skyObject.distanceMeters),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        Spacer(modifier = Modifier.width(8.dp))

        // Detection source icon
        Icon(
            imageVector = detectionSourceIcon(skyObject.source),
            contentDescription = skyObject.source.name,
            modifier = Modifier.size(18.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/** Returns the primary display text for a sky object (callsign or drone ID). */
private fun primaryText(skyObject: SkyObject): String = when (skyObject) {
    is Aircraft -> skyObject.callsign ?: skyObject.icaoHex
    is Drone -> skyObject.droneId
}

/** Returns secondary descriptive text (aircraft type or manufacturer). */
private fun secondaryText(skyObject: SkyObject): String = when (skyObject) {
    is Aircraft -> skyObject.aircraftModel ?: skyObject.aircraftType ?: "Unknown aircraft"
    is Drone -> {
        val parts = listOfNotNull(skyObject.manufacturer, skyObject.model)
        if (parts.isNotEmpty()) parts.joinToString(" ") else "Unknown drone"
    }
}

/** Maps an [ObjectCategory] to its display color. */
private fun categoryColor(category: ObjectCategory): Color = when (category) {
    ObjectCategory.COMMERCIAL -> Color(0xFF4CAF50)      // Green
    ObjectCategory.GENERAL_AVIATION -> Color(0xFFFFC107) // Yellow/Amber
    ObjectCategory.MILITARY -> Color(0xFFF44336)         // Red
    ObjectCategory.DRONE -> Color(0xFF2196F3)            // Blue
    ObjectCategory.UNKNOWN -> Color(0xFF9E9E9E)          // Gray
}

/**
 * Formats altitude for display.
 * Uses flight levels (FL) for altitudes at or above 18,000ft (transition altitude),
 * otherwise uses feet with comma formatting.
 */
private fun formatAltitude(altitudeMeters: Double): String {
    val feet = (altitudeMeters * 3.281).toInt()
    return if (feet >= 18000) {
        "FL${feet / 100}"
    } else {
        "${"%,d".format(feet)}ft"
    }
}

/**
 * Formats distance for display.
 * Uses nautical miles for distances >= 1nm, otherwise km with one decimal.
 */
private fun formatDistance(distanceMeters: Double?): String {
    if (distanceMeters == null) return "--"
    val nm = distanceMeters / 1852.0
    return if (nm >= 1.0) {
        "${"%.0f".format(nm)}nm"
    } else {
        val km = distanceMeters / 1000.0
        "${"%.1f".format(km)}km"
    }
}

/** Maps a [DetectionSource] to its Material icon. */
private fun detectionSourceIcon(source: DetectionSource): ImageVector = when (source) {
    DetectionSource.ADS_B -> Icons.Default.CellTower
    DetectionSource.REMOTE_ID -> Icons.Default.Bluetooth
    DetectionSource.WIFI -> Icons.Default.Wifi
}

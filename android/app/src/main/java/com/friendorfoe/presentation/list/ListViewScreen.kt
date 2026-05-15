package com.friendorfoe.presentation.list

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.platform.LocalContext
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.badge.BadgeUsbDetection
import com.friendorfoe.data.badge.BadgeDisplayClassPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicyClasses
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicy
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.presentation.filter.FilterBar
import com.friendorfoe.presentation.util.categoryBadge
import com.friendorfoe.presentation.util.categoryColor
import androidx.compose.foundation.shape.RoundedCornerShape

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
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
    viewModel: ListViewModel = hiltViewModel()
) {
    val skyObjects by viewModel.skyObjects.collectAsStateWithLifecycle()
    val filterState by viewModel.filterState.collectAsStateWithLifecycle()
    val badgeUsbState by viewModel.badgeUsbState.collectAsStateWithLifecycle()

    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    viewModel.startLocationUpdates()
                    viewModel.startBadgeUsb()
                }
                Lifecycle.Event.ON_PAUSE -> {
                    viewModel.stopLocationUpdates()
                    viewModel.stopBadgeUsb()
                }
                else -> {}
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        FilterBar(
            filterState = filterState,
            onFilterStateChange = { viewModel.updateFilter(it) },
            resultCount = skyObjects.size,
            onNavigateToReferenceGuide = onNavigateToReferenceGuide,
            onNavigateToAbout = onNavigateToAbout
        )

        BadgeUsbPanel(
            state = badgeUsbState,
            onConnect = viewModel::connectBadgeUsb,
            onPing = viewModel::pingBadgeUsb,
            onRefreshStatus = viewModel::refreshBadgeStatus,
            onSetMode = viewModel::setBadgeMode,
            onReboot = viewModel::rebootBadge,
            onBootloader = viewModel::badgeBootloader,
            onFlashScannerFirmware = viewModel::flashBadgeScannerFirmware,
            onApplyDisplayPolicy = viewModel::applyBadgeDisplayPolicy,
            onResetDisplayPolicy = viewModel::resetBadgeDisplayPolicy,
            onRefreshDisplayPolicy = viewModel::refreshBadgeStatus
        )

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
}

@Composable
private fun BadgeUsbPanel(
    state: BadgeUsbState,
    onConnect: () -> Unit,
    onPing: () -> Unit,
    onRefreshStatus: () -> Unit,
    onSetMode: (String) -> Unit,
    onReboot: () -> Unit,
    onBootloader: () -> Unit,
    onFlashScannerFirmware: (String, String, ByteArray) -> Unit,
    onApplyDisplayPolicy: (BadgeDisplayPolicy) -> Unit,
    onResetDisplayPolicy: () -> Unit,
    onRefreshDisplayPolicy: () -> Unit
) {
    val context = LocalContext.current
    var pendingFirmwareUart by remember { mutableStateOf("ble") }
    val firmwarePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            val bytes = runCatching {
                context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
            }.getOrNull()
            if (bytes != null && bytes.isNotEmpty()) {
                val name = uri.lastPathSegment
                    ?.substringAfterLast('/')
                    ?.substringAfterLast(':')
                    ?.ifBlank { null }
                    ?: "scanner-s3-combo-fof_badge.bin"
                onFlashScannerFirmware(pendingFirmwareUart, name, bytes)
            }
        }
    }
    val accent = when (state.status) {
        BadgeUsbStatus.CONNECTED -> Color(0xFF2E7D32)
        BadgeUsbStatus.AP_CONNECTED -> Color(0xFF2E7D32)
        BadgeUsbStatus.CONNECTING -> MaterialTheme.colorScheme.primary
        BadgeUsbStatus.PERMISSION_NEEDED -> Color(0xFF1565C0)
        BadgeUsbStatus.ERROR -> MaterialTheme.colorScheme.error
        BadgeUsbStatus.DISCONNECTED -> MaterialTheme.colorScheme.outline
    }
    val latest = state.detections.firstOrNull()
    val badgeStatus = state.controlStatus
    val controlsAvailable = state.status == BadgeUsbStatus.CONNECTED ||
        state.status == BadgeUsbStatus.AP_CONNECTED
    var filtersExpanded by remember { mutableStateOf(false) }
    var draftPolicy by remember { mutableStateOf(badgeStatus?.displayPolicy ?: defaultBadgeDisplayPolicy()) }

    LaunchedEffect(badgeStatus?.displayPolicyHash) {
        badgeStatus?.displayPolicy?.let { draftPolicy = it }
    }

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp),
        shape = RoundedCornerShape(8.dp),
        tonalElevation = 2.dp,
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.65f)
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .clip(CircleShape)
                        .background(accent)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "Badge Control",
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        text = state.deviceName ?: state.message,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
                if (controlsAvailable) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = onRefreshStatus) {
                            Text("Status")
                        }
                        if (state.status == BadgeUsbStatus.CONNECTED) {
                            OutlinedButton(onClick = onPing) {
                                Text("Ping")
                            }
                        }
                    }
                } else {
                    Button(onClick = onConnect) {
                        Text("Connect")
                    }
                }
            }

            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = "${state.message} | ${state.eventCount} badge events",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )

            if (badgeStatus != null) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = "${badgeStatus.reporting.networkMode.uppercase()} | Upload ${badgeStatus.reporting.uploadsOk}/${badgeStatus.reporting.uploadsFail} | Threat ${badgeStatus.threatScore.toInt()} | DRN ${badgeStatus.counts.drone} META ${badgeStatus.counts.meta} TAG ${badgeStatus.counts.tracker}",
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                val scannerText = badgeStatus.scanners.joinToString(" | ") {
                    "${it.uart.ifBlank { "?" }} ${it.health.ifBlank { if (it.connected) "ok" else "missing" }} ${it.scanProfile.ifBlank { it.slotRole }}"
                }
                if (scannerText.isNotBlank()) {
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = scannerText,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
                val entityText = badgeStatus.entities.joinToString(" | ") { "${it.label} ${it.score}" }
                if (entityText.isNotBlank()) {
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = entityText,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
                if (controlsAvailable) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = { onSetMode("local_ap") }) {
                            Text("Local AP")
                        }
                        OutlinedButton(onClick = { onSetMode("backend") }) {
                            Text("Backend")
                        }
                        OutlinedButton(onClick = { onSetMode("usb_only") }) {
                            Text("USB")
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = onReboot) {
                            Text("Reboot")
                        }
                        OutlinedButton(onClick = onBootloader) {
                            Text("Bootloader")
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = {
                            pendingFirmwareUart = "ble"
                            firmwarePicker.launch(arrayOf("application/octet-stream", "*/*"))
                        }) {
                            Text("Flash BLE")
                        }
                        OutlinedButton(onClick = {
                            pendingFirmwareUart = "wifi"
                            firmwarePicker.launch(arrayOf("application/octet-stream", "*/*"))
                        }) {
                            Text("Flash WiFi")
                        }
                    }
                    Spacer(modifier = Modifier.height(8.dp))
                    BadgeDisplayFiltersSection(
                        expanded = filtersExpanded,
                        onExpandedChange = { filtersExpanded = it },
                        policy = draftPolicy,
                        displayPolicyHash = badgeStatus.displayPolicyHash,
                        filteredCounts = badgeStatus.filteredCounts,
                        onPolicyChange = { draftPolicy = it },
                        onApply = { onApplyDisplayPolicy(draftPolicy) },
                        onReset = {
                            draftPolicy = defaultBadgeDisplayPolicy()
                            onResetDisplayPolicy()
                        },
                        onRefresh = onRefreshDisplayPolicy
                    )
                }
            }

            state.firmwareProgress?.let { progress ->
                Spacer(modifier = Modifier.height(6.dp))
                val target = progress.uart.ifBlank { "scanner" }
                val status = progress.error.ifBlank {
                    "${progress.kind} $target ${progress.stage} ${progress.percent}%"
                }
                Text(
                    text = status,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (progress.error.isBlank()) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        MaterialTheme.colorScheme.error
                    },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            if (latest != null) {
                Spacer(modifier = Modifier.height(6.dp))
                Text(
                    text = badgeDetectionText(latest),
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            } else if (state.lastLine != null) {
                Spacer(modifier = Modifier.height(6.dp))
                Text(
                    text = state.lastLine,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

private fun badgeDetectionText(detection: BadgeUsbDetection): String {
    val label = friendlyBadgeLabel(detection)
    val confidence = (detection.confidence * 100f).toInt().coerceIn(0, 100)
    val rssi = if (detection.rssi < 0) " ${detection.rssi}dBm" else ""
    return "$label  $confidence%$rssi"
}

@Composable
private fun BadgeDisplayFiltersSection(
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    policy: BadgeDisplayPolicy,
    displayPolicyHash: Long,
    filteredCounts: Map<String, Int>,
    onPolicyChange: (BadgeDisplayPolicy) -> Unit,
    onApply: () -> Unit,
    onReset: () -> Unit,
    onRefresh: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                RoundedCornerShape(8.dp)
            )
            .padding(8.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Display Filters",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "Badge-only LCD and scanner emission policy  #$displayPolicyHash",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            OutlinedButton(onClick = { onExpandedChange(!expanded) }) {
                Text(if (expanded) "Hide" else "Edit")
            }
        }

        if (!expanded) return@Column

        Spacer(modifier = Modifier.height(8.dp))
        BadgeDisplayPolicyClasses.forEach { info ->
            val config = policy.classes[info.key] ?: BadgeDisplayClassPolicy()
            val filtered = filteredCounts[info.key].orZero()
            BadgeDisplayClassRow(
                label = info.label,
                filtered = filtered,
                config = config,
                onChange = { next ->
                    onPolicyChange(
                        policy.copy(classes = policy.classes + (info.key to next))
                    )
                }
            )
            HorizontalDivider(
                modifier = Modifier.padding(vertical = 6.dp),
                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.45f)
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Button(onClick = onApply) {
                Text("Apply")
            }
            OutlinedButton(onClick = onReset) {
                Text("Reset Defaults")
            }
            OutlinedButton(onClick = onRefresh) {
                Text("Refresh")
            }
        }
    }
}

@Composable
private fun BadgeDisplayClassRow(
    label: String,
    filtered: Int,
    config: BadgeDisplayClassPolicy,
    onChange: (BadgeDisplayClassPolicy) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = "Suppressed $filtered  |  Priority ${config.priority}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onChange(config.copy(enabled = it)) }
            )
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = "Lane",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("off", "lower", "top", "both").forEach { lane ->
                OutlinedButton(
                    onClick = { onChange(config.copy(lane = lane)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = lane.uppercase(),
                        style = MaterialTheme.typography.labelSmall,
                        color = if (config.lane == lane) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        maxLines = 1
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = "Minimum proximity",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("present", "near", "close").forEach { prox ->
                OutlinedButton(
                    onClick = { onChange(config.copy(minProximity = prox)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = prox.uppercase(),
                        style = MaterialTheme.typography.labelSmall,
                        color = if (config.minProximity == prox) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        maxLines = 1
                    )
                }
            }
        }
        Slider(
            value = config.priority.toFloat(),
            onValueChange = {
                onChange(config.copy(priority = it.toInt().coerceIn(0, 100)))
            },
            valueRange = 0f..100f
        )
    }
}

private fun Int?.orZero(): Int = this ?: 0

private fun friendlyBadgeLabel(detection: BadgeUsbDetection): String {
    val text = "${detection.manufacturer} ${detection.id}".lowercase()
    return when {
        "meta" in text || "ray-ban" in text || "rayban" in text || "oakley" in text -> "Meta Glasses"
        "dji" in text -> "DJI Drone"
        "remote" in text || detection.source == 0 || detection.source == 3 -> "Remote ID"
        "airtag" in text || "tracker" in text || "tile" in text || "findmy" in text -> "Tracker"
        detection.source == 5 || detection.source == 7 -> "Wi-Fi Anomaly"
        else -> detection.manufacturer.ifBlank { "Badge event" }
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
    val rowBackground = when (skyObject.category) {
        ObjectCategory.MILITARY -> Modifier.background(Color(0xFFF44336).copy(alpha = 0.08f))
        ObjectCategory.GOVERNMENT -> Modifier.background(Color(0xFFE65100).copy(alpha = 0.08f))
        ObjectCategory.EMERGENCY -> Modifier.background(Color(0xFFE91E63).copy(alpha = 0.10f))
        else -> Modifier
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .then(rowBackground)
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
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = primaryText(skyObject),
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false)
                )
                val badge = categoryBadge(skyObject.category)
                if (badge != null) {
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = badge.first,
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Bold,
                        color = Color.White,
                        modifier = Modifier
                            .background(badge.second, RoundedCornerShape(4.dp))
                            .padding(horizontal = 4.dp, vertical = 1.dp)
                    )
                }
            }
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
    is Aircraft -> {
        val callsign = skyObject.callsign ?: skyObject.icaoHex
        if (skyObject.aircraftType != null) "$callsign  ${skyObject.aircraftType}" else callsign
    }
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
    return if (distanceMeters > 800.0) {
        val miles = distanceMeters / 1609.344
        if (miles >= 10.0) "${"%.0f".format(miles)} mi"
        else "${"%.1f".format(miles)} mi"
    } else {
        "${distanceMeters.toInt()} m"
    }
}

/** Maps a [DetectionSource] to its Material icon. */
private fun detectionSourceIcon(source: DetectionSource): ImageVector = when (source) {
    DetectionSource.ADS_B -> Icons.Default.CellTower
    DetectionSource.REMOTE_ID -> Icons.Default.Bluetooth
    DetectionSource.WIFI_NAN -> Icons.Default.Wifi
    DetectionSource.WIFI_BEACON -> Icons.Default.Wifi
    DetectionSource.WIFI -> Icons.Default.Wifi
}

@Composable
private fun PrivacyScannerSection(
    detections: List<GlassesDetection>,
    onIgnore: (String) -> Unit,
    onTrack: (String) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    var selectedDetail by remember { mutableStateOf<GlassesDetection?>(null) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.error.copy(alpha = 0.08f))
    ) {
        // Header — always visible, tap to expand/collapse
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { expanded = !expanded }
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Default.Visibility,
                contentDescription = "Privacy Scanner",
                tint = MaterialTheme.colorScheme.error,
                modifier = Modifier.size(18.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = "Privacy Scanner",
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.error
            )
            Spacer(modifier = Modifier.width(6.dp))
            Text(
                text = "${detections.size} device${if (detections.size != 1) "s" else ""}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error.copy(alpha = 0.7f)
            )
            Spacer(modifier = Modifier.weight(1f))
            Text(
                text = if (expanded) "\u25B2" else "\u25BC",
                color = MaterialTheme.colorScheme.error
            )
        }

        // Expanded — show all devices with actions
        if (expanded) {
            for (det in detections) {
                HorizontalDivider(color = MaterialTheme.colorScheme.error.copy(alpha = 0.15f))
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { selectedDetail = det }
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = if (det.hasCamera) "\uD83D\uDCF7" else "\uD83D\uDD0A",
                            modifier = Modifier.width(22.dp)
                        )
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = "${det.manufacturer} ${det.deviceType}",
                                style = MaterialTheme.typography.bodyMedium,
                                fontWeight = FontWeight.Medium
                            )
                            if (det.deviceName != null) {
                                Text(
                                    text = det.deviceName,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        }
                        Text(
                            text = "${det.rssi}dB",
                            style = MaterialTheme.typography.bodySmall,
                            fontWeight = FontWeight.Medium,
                            color = MaterialTheme.colorScheme.error
                        )
                    }
                    // Parsed details row
                    if (det.details.isNotEmpty()) {
                        Text(
                            text = det.details.entries.take(4).joinToString(" | ") { "${it.key}: ${it.value}" },
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                            modifier = Modifier.padding(start = 22.dp, top = 2.dp)
                        )
                    }
                    // Match reason + confidence
                    Text(
                        text = "Match: ${det.matchReason} (${(det.confidence * 100).toInt()}%)",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
                        modifier = Modifier.padding(start = 22.dp, top = 1.dp)
                    )
                    // Action buttons
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(start = 22.dp, top = 4.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = "Ignore",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.clickable { onIgnore(det.mac) }
                        )
                        Text(
                            text = "Track",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary,
                            fontWeight = FontWeight.Medium,
                            modifier = Modifier.clickable { onTrack(det.mac) }
                        )
                        Text(
                            text = "Details",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.clickable { selectedDetail = det }
                        )
                    }
                }
            }
        }
    }

    // Detail bottom sheet
    if (selectedDetail != null) {
        val det = selectedDetail!!
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { selectedDetail = null },
            title = {
                Text("${det.manufacturer} ${det.deviceType}")
            },
            text = {
                Column {
                    if (det.deviceName != null) {
                        Text("Name: ${det.deviceName}")
                    }
                    Text("MAC: ${det.mac}")
                    Text("RSSI: ${det.rssi} dBm")
                    Text("Confidence: ${(det.confidence * 100).toInt()}%")
                    Text("Match: ${det.matchReason}")
                    Text("Camera: ${if (det.hasCamera) "Yes" else "No"}")
                    if (det.details.isNotEmpty()) {
                        Spacer(modifier = Modifier.height(8.dp))
                        Text("Parsed Details:", fontWeight = FontWeight.Medium)
                        for ((key, value) in det.details) {
                            Text("  $key: $value", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            },
            confirmButton = {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    androidx.compose.material3.TextButton(onClick = {
                        onIgnore(det.mac)
                        selectedDetail = null
                    }) { Text("Ignore") }
                    androidx.compose.material3.TextButton(onClick = {
                        onTrack(det.mac)
                        selectedDetail = null
                    }) { Text("Track") }
                    androidx.compose.material3.TextButton(onClick = { selectedDetail = null }) {
                        Text("Close")
                    }
                }
            }
        )
    }
}

@Composable
private fun StalkerAlertBanner(alerts: List<BleTracker.StalkerAlert>) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFFD32F2F).copy(alpha = 0.15f))
            .padding(horizontal = 16.dp, vertical = 10.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "\u26A0\uFE0F",
                modifier = Modifier.width(24.dp)
            )
            Text(
                text = "STALKER ALERT",
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Bold,
                color = Color(0xFFD32F2F)
            )
        }
        for (alert in alerts.take(3)) {
            val dev = alert.device
            val duration = dev.durationMs / 1000
            val label = dev.deviceType ?: dev.deviceName ?: dev.mac.takeLast(8)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                val threatIcon = when (alert.threatLevel) {
                    3 -> "\uD83D\uDED1" // stop sign
                    2 -> "\u26A0\uFE0F" // warning
                    else -> "\u2139\uFE0F" // info
                }
                Text(text = threatIcon, modifier = Modifier.width(20.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "$label (${dev.manufacturer ?: "Unknown"})",
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Medium
                    )
                    Text(
                        text = "${alert.reason} for ${duration}s | ${dev.sightingCount} sightings",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Text(
                    text = "${dev.peakRssi}dB",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFFD32F2F)
                )
            }
        }
    }
}

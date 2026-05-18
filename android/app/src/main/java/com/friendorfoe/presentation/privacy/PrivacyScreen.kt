package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory

/** Section group definition for threat-level grouping */
private data class SectionGroup(
    val title: String,
    val threatLevel: Int,
    val color: @Composable () -> Color,
    val icon: String
)

private val sectionGroups = listOf(
    SectionGroup("THREATS", 3, { Color(0xFFD32F2F) }, "\uD83D\uDD34"),
    SectionGroup("AWARENESS", 2, { Color(0xFFFF9800) }, "\uD83D\uDFE0"),
    SectionGroup("NEARBY", 1, { Color(0xFFFFC107) }, "\uD83D\uDFE1"),
    SectionGroup("INFO", 0, { Color(0xFF9E9E9E) }, "\u26AA"),
)

@Composable
fun PrivacyScreen(
    viewModel: PrivacyViewModel = hiltViewModel()
) {
    val categorized by viewModel.categorizedDetections.collectAsStateWithLifecycle()
    val totalCount by viewModel.totalCount.collectAsStateWithLifecycle()
    val threatCount by viewModel.threatCount.collectAsStateWithLifecycle()
    val badgeUsbState by viewModel.badgeUsbState.collectAsStateWithLifecycle()

    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> viewModel.startBadgeUsb()
                Lifecycle.Event.ON_PAUSE -> viewModel.stopBadgeUsb()
                else -> {}
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
        }
    }

    // Track expanded categories (high-threat auto-expanded)
    val expandedCategories = remember {
        mutableStateOf(setOf(PrivacyCategory.SMART_GLASSES,
            PrivacyCategory.HIDDEN_CAMERA, PrivacyCategory.ATTACK_TOOL,
            PrivacyCategory.ULTRASONIC_BEACON, PrivacyCategory.RETAIL_TRACKER,
            PrivacyCategory.SURVEILLANCE_CAMERA, PrivacyCategory.ALPR_CAMERA,
            PrivacyCategory.MOBILE_KEY_LOCK,
            PrivacyCategory.BABY_MONITOR, PrivacyCategory.THERMAL_CAMERA,
            PrivacyCategory.CONFERENCE_CAMERA, PrivacyCategory.VIDEO_INTERCOM,
            PrivacyCategory.SMART_SPEAKER, PrivacyCategory.SMART_HOME_HUB,
            PrivacyCategory.GPS_TRACKER, PrivacyCategory.OBD_TRACKER,
            PrivacyCategory.VENUE_BEACON, PrivacyCategory.EVENT_BADGE,
            PrivacyCategory.BLE_HID, PrivacyCategory.AURACAST,
            PrivacyCategory.APPLE_CONTINUITY))
    }

    // Track collapsed sections (all expanded by default)
    val collapsedSections = remember { mutableStateOf(setOf<Int>()) }

    var selectedDetail by remember { mutableStateOf<GlassesDetection?>(null) }
    var selectedBadgeEntity by remember { mutableStateOf<BadgeThreatEntity?>(null) }
    var trackingTarget by remember { mutableStateOf<GlassesDetection?>(null) }
    val ultrasonicAlerts by viewModel.ultrasonicAlerts.collectAsStateWithLifecycle()
    val wifiAnomalies by viewModel.wifiAnomalies.collectAsStateWithLifecycle()

    Column(modifier = Modifier.fillMaxSize()) {
        // WiFi anomaly banner (Pwnagotchi, evil twin, karma attack)
        if (wifiAnomalies.isNotEmpty()) {
            val worst = wifiAnomalies.maxByOrNull { it.threatLevel } ?: wifiAnomalies.first()
            val (emoji, title) = when (worst.type) {
                "pwnagotchi"   -> "\u26A0\uFE0F" to "Pwnagotchi Detected!"
                "evil_twin"    -> "\uD83D\uDEA8" to "Evil Twin AP Detected!"
                "karma_attack" -> "\uD83D\uDEA8" to "Karma Attack Detected!"
                "rogue_ap"     -> "\u26A0\uFE0F" to "Rogue AP Detected!"
                else           -> "\u26A0\uFE0F" to "WiFi Anomaly"
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFFD32F2F).copy(alpha = 0.15f))
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(text = emoji, modifier = Modifier.width(28.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Bold,
                        color = Color(0xFFD32F2F)
                    )
                    Text(
                        text = worst.details,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        // Ultrasonic beacon alert banner (high priority, above everything)
        if (ultrasonicAlerts.isNotEmpty()) {
            val alert = ultrasonicAlerts.last()
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFFD32F2F).copy(alpha = 0.15f))
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(text = "\uD83D\uDD0A", modifier = Modifier.width(28.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "Ultrasonic Beacon Detected!",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Bold,
                        color = Color(0xFFD32F2F)
                    )
                    Text(
                        text = "${"%.0f".format(alert.frequencyHz)} Hz \u2022 SNR ${"%.1f".format(alert.snrDb)} dB \u2022 ${alert.persistenceFrames} frames",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        BadgeUsbStatusRow(
            state = badgeUsbState,
            onAction = {
                if (badgeUsbState.status == BadgeUsbStatus.CONNECTED ||
                    badgeUsbState.status == BadgeUsbStatus.AP_CONNECTED) {
                    viewModel.refreshBadgeStatus()
                } else {
                    viewModel.connectBadgeUsb()
                }
            }
        )

        BadgeDetailPanel(
            state = badgeUsbState,
            onNext = viewModel::badgeNextFocus,
            onDetail = viewModel::badgeToggleDetail,
            onBack = viewModel::badgeBackFromDetail,
            onRefresh = viewModel::refreshBadgeStatus,
            onEntityDetails = { selectedBadgeEntity = it }
        )

        // Status bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    if (threatCount > 0) MaterialTheme.colorScheme.error.copy(alpha = 0.12f)
                    else MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.3f)
                )
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = if (threatCount > 0) "\uD83D\uDEA8" else "\uD83D\uDD12",
                modifier = Modifier.width(28.dp)
            )
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = if (threatCount > 0) "Privacy Threats Detected"
                           else "Privacy Scanner Active",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = if (threatCount > 0) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurface
                )
                Text(
                    text = "$totalCount device${if (totalCount != 1) "s" else ""} detected" +
                           if (threatCount > 0) " \u2022 $threatCount threat${if (threatCount != 1) "s" else ""}" else "",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        if (categorized.isEmpty()) {
            // Empty state
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(32.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text(text = "\uD83D\uDD12", style = MaterialTheme.typography.displaySmall)
                Spacer(modifier = Modifier.height(16.dp))
                Text(
                    text = "No privacy devices detected",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Medium
                )
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = "Scanning for smart glasses, cameras, trackers,\nspeakers, locks, and other devices nearby",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center
                )
            }
        } else {
            // Sectioned category tree
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                for (section in sectionGroups) {
                    // Get categories in this section that have detections
                    val sectionCategories = categorized.filter { it.key.threatLevel == section.threatLevel }
                    if (sectionCategories.isEmpty()) continue

                    val sectionDeviceCount = sectionCategories.values.sumOf { it.size }
                    val isSectionCollapsed = section.threatLevel in collapsedSections.value

                    // Section group header
                    item(key = "section_${section.threatLevel}") {
                        SectionHeader(
                            section = section,
                            deviceCount = sectionDeviceCount,
                            isCollapsed = isSectionCollapsed,
                            onClick = {
                                collapsedSections.value = if (isSectionCollapsed) {
                                    collapsedSections.value - section.threatLevel
                                } else {
                                    collapsedSections.value + section.threatLevel
                                }
                            }
                        )
                    }

                    if (!isSectionCollapsed) {
                        sectionCategories.forEach { (category, devices) ->
                            // Category header (indented under section)
                            item(key = "header_${category.name}") {
                                val isExpanded = category in expandedCategories.value
                                CategoryHeader(
                                    category = category,
                                    count = devices.size,
                                    isExpanded = isExpanded,
                                    onClick = {
                                        expandedCategories.value = if (isExpanded) {
                                            expandedCategories.value - category
                                        } else {
                                            expandedCategories.value + category
                                        }
                                    }
                                )
                            }

                            // Device cards (if expanded)
                            if (category in expandedCategories.value) {
                                items(
                                    items = devices.sortedByDescending { it.rssi },
                                    key = { "device_${it.mac}_${it.matchReason}" }
                                ) { detection ->
                                    DeviceCard(
                                        detection = detection,
                                        onIgnore = { viewModel.ignoreDevice(detection.mac) },
                                        onTrack = { trackingTarget = detection },
                                        onDetails = { selectedDetail = detection }
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Detail dialog
    if (selectedDetail != null) {
        DeviceDetailDialog(
            detection = selectedDetail!!,
            onIgnore = {
                viewModel.ignoreDevice(selectedDetail!!.mac)
                selectedDetail = null
            },
            onTrack = {
                trackingTarget = selectedDetail
                selectedDetail = null
            },
            onDismiss = { selectedDetail = null }
        )
    }

    if (selectedBadgeEntity != null) {
        BadgeEntityDetailDialog(
            entity = selectedBadgeEntity!!,
            onDismiss = { selectedBadgeEntity = null }
        )
    }

    // Direction scan overlay (full-screen)
    if (trackingTarget != null) {
        DirectionScanOverlay(
            detection = trackingTarget!!,
            viewModel = viewModel,
            onDismiss = { trackingTarget = null }
        )
    }
}

@Composable
private fun BadgeUsbStatusRow(
    state: BadgeUsbState,
    onAction: () -> Unit
) {
    val connected = state.status == BadgeUsbStatus.CONNECTED ||
        state.status == BadgeUsbStatus.AP_CONNECTED
    val accent = when (state.status) {
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.AP_CONNECTED -> Color(0xFF2E7D32)
        BadgeUsbStatus.CONNECTING -> MaterialTheme.colorScheme.primary
        BadgeUsbStatus.PERMISSION_NEEDED -> Color(0xFF1565C0)
        BadgeUsbStatus.ERROR -> MaterialTheme.colorScheme.error
        BadgeUsbStatus.DISCONNECTED -> MaterialTheme.colorScheme.outline
    }
    val counts = state.controlStatus?.counts
    val summary = if (counts != null) {
        "DRN ${counts.drone}  META ${counts.meta}  TAG ${counts.tracker}  WIFI ${counts.wifiAnomaly}"
    } else {
        state.message
    }
    val scannerSummary = state.controlStatus?.scanners
        ?.joinToString("  ") {
            "${it.uart.ifBlank { "?" }.uppercase()} ${it.health.ifBlank { if (it.connected) "ok" else "missing" }}"
        }
        .orEmpty()
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onAction)
            .background(accent.copy(alpha = if (connected) 0.10f else 0.06f))
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = "USB-C",
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            color = accent,
            modifier = Modifier.width(58.dp)
        )
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = if (connected) "Badge live privacy feed" else state.message,
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = if (scannerSummary.isNotBlank()) "$summary  |  $scannerSummary" else summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
private fun BadgeDetailPanel(
    state: BadgeUsbState,
    onNext: () -> Unit,
    onDetail: () -> Unit,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    onEntityDetails: (BadgeThreatEntity) -> Unit
) {
    val status = state.controlStatus ?: return
    val connected = state.status == BadgeUsbStatus.CONNECTED ||
        state.status == BadgeUsbStatus.AP_CONNECTED
    val display = status.displayState
    val accent = badgeHealthColor(status)
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(accent.copy(alpha = 0.08f))
            .padding(horizontal = 16.dp, vertical = 10.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Badge Detail Panel",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = accent
                )
                Text(
                    text = buildString {
                        append(if (connected) "live " else "cached ")
                        append(status.modeLabel.ifBlank { status.mode })
                        append("  |  DRN ${status.counts.drone}")
                        append(" META ${status.counts.meta}")
                        append(" TAG ${status.counts.tracker}")
                        append(" WIFI ${status.counts.wifiAnomaly}")
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            TextButton(onClick = onRefresh) { Text("Refresh") }
        }

        BadgeFocusedDisplayRow(display = display)

        Row(
            modifier = Modifier.padding(top = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TextButton(onClick = onNext) { Text("Next") }
            TextButton(onClick = onDetail) {
                Text(if (display?.detailMode == true) "Page" else "Detail")
            }
            TextButton(onClick = onBack) { Text("Back") }
        }

        val warning = status.badgeWarningText()
        if (warning.isNotBlank()) {
            Text(
                text = warning,
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(top = 4.dp)
            )
        }

        Text(
            text = "Stack main/display/USB ${status.stackMainFree}/${status.stackDisplayFree}/${status.stackUsbFree}  UART ${status.stackUartBleFree}/${status.stackUartWifiFree}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 4.dp)
        )
        Text(
            text = "Heap ${formatBytes(status.heapInternalFree)} free  PSRAM ${formatBytes(status.psramFree)} / ${formatBytes(status.psramTotal)}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )

        val filtered = status.filteredCounts.filterValues { it > 0 }
        if (filtered.isNotEmpty()) {
            Text(
                text = "Filtered " + filtered.entries.take(5)
                    .joinToString("  ") { "${it.key}:${it.value}" },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(top = 2.dp)
            )
        }

        if (status.scanners.isNotEmpty()) {
            Text(
                text = status.scanners.joinToString("  ") {
                    "${it.uart.ifBlank { "slot${it.slot}" }.uppercase()} ${it.health.ifBlank { if (it.connected) "ok" else "missing" }} ${it.scanProfile.ifBlank { it.slotRole }}"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(top = 2.dp)
            )
        }

        status.entities.take(6).forEach { entity ->
            BadgeEntityRow(entity = entity, onClick = { onEntityDetails(entity) })
        }
    }
}

@Composable
private fun BadgeFocusedDisplayRow(display: BadgeDisplayState?) {
    val title = display?.title?.ifBlank { "No badge focus" } ?: "No badge focus"
    val detail = display?.detail.orEmpty()
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = listOfNotNull(
                    display?.lane?.takeIf { it.isNotBlank() },
                    detail.takeIf { it.isNotBlank() },
                    display?.evidence?.takeIf { it.isNotBlank() && it != detail },
                    display?.displayId?.takeIf { it.isNotBlank() },
                    display?.rssi?.takeIf { it != 0 }?.let { "${it}dB" }
                ).joinToString("  |  ").ifBlank { "Badge LCD focus is idle" },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
        Text(
            text = "${(display?.focusIndex ?: 0) + 1}/${(display?.focusTotal ?: 0).coerceAtLeast(1)}",
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )
    }
}

@Composable
private fun BadgeEntityRow(entity: BadgeThreatEntity, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(top = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = entity.label.ifBlank { entity.threatClass.ifBlank { "Badge Signal" } },
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = listOfNotNull(
                    entity.detail.takeIf { it.isNotBlank() },
                    entity.evidence.takeIf { it.isNotBlank() && it != entity.detail },
                    entity.displayId.takeIf { it.isNotBlank() },
                    entity.source.takeIf { it.isNotBlank() },
                    entity.category.takeIf { it.isNotBlank() },
                    "${entity.ageSeconds}s"
                ).joinToString("  |  "),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
        Text(
            text = "${entity.rssi}dB",
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.Bold,
            color = when {
                entity.rssi >= -60 -> MaterialTheme.colorScheme.error
                entity.rssi >= -75 -> Color(0xFFFF9800)
                else -> MaterialTheme.colorScheme.primary
            }
        )
    }
}

@Composable
private fun BadgeEntityDetailDialog(
    entity: BadgeThreatEntity,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(entity.label.ifBlank { "Badge Signal" }) },
        text = {
            Column {
                if (entity.detail.isNotBlank()) DetailRow("Detail", entity.detail)
                if (entity.evidence.isNotBlank()) DetailRow("Evidence", entity.evidence)
                DetailRow("Class", entity.threatClass.ifBlank { "unknown" })
                if (entity.category.isNotBlank()) DetailRow("Category", entity.category)
                if (entity.code.isNotBlank()) DetailRow("Code", entity.code)
                if (entity.source.isNotBlank()) DetailRow("Source", entity.source)
                if (entity.displayId.isNotBlank()) DetailRow("Display ID", entity.displayId)
                DetailRow("Score", "${entity.score}")
                if (entity.confidencePct > 0) DetailRow("Confidence", "${entity.confidencePct}%")
                DetailRow("RSSI", "${entity.rssi} dBm")
                DetailRow("Best RSSI", "${entity.bestRssi} dBm")
                DetailRow("Age", "${entity.ageSeconds}s")
                DetailRow("Seen", "${entity.seenCount} packets / ${entity.events} events")
                if (entity.lat != null && entity.lon != null) {
                    DetailRow("GPS", "%.6f, %.6f".format(entity.lat, entity.lon))
                }
                if (entity.operatorLat != null && entity.operatorLon != null) {
                    DetailRow("Operator", "%.6f, %.6f".format(entity.operatorLat, entity.operatorLon))
                }
                if (!entity.operatorId.isNullOrBlank()) DetailRow("Operator ID", entity.operatorId)
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        }
    )
}

@Composable
private fun badgeHealthColor(status: BadgeControlStatus): Color = when {
    status.safeMode || status.crashCount > 0 -> MaterialTheme.colorScheme.error
    status.stackMainFree in 1..1023 || status.stackDisplayFree in 1..1023 ||
        status.stackUsbFree in 1..1023 || status.stackUartBleFree in 1..1023 ||
        status.stackUartWifiFree in 1..1023 -> Color(0xFFFF9800)
    status.psramTotal == 0L -> Color(0xFFFF9800)
    else -> Color(0xFF2E7D32)
}

private fun BadgeControlStatus.badgeWarningText(): String = buildList {
    if (safeMode) add("safe mode${safeReason.takeIf { it.isNotBlank() }?.let { ": $it" } ?: ""}")
    if (crashCount > 0) add("crashes $crashCount")
    if (resetReason.isNotBlank() && !resetExpected &&
        !resetReason.equals("POWERON", ignoreCase = true)) {
        add("reset $resetReason")
    }
    if (psramTotal == 0L) add("PSRAM missing")
    val lowStacks = listOf(
        "main" to stackMainFree,
        "display" to stackDisplayFree,
        "usb" to stackUsbFree,
        "ble-uart" to stackUartBleFree,
        "wifi-uart" to stackUartWifiFree
    ).filter { (_, value) -> value in 1..1023 }
    if (lowStacks.isNotEmpty()) {
        add("low stack " + lowStacks.joinToString(",") { "${it.first}:${it.second}" })
    }
}.joinToString("  |  ")

private fun formatBytes(bytes: Long): String = when {
    bytes <= 0L -> "0B"
    bytes >= 1024L * 1024L -> "%.1fMB".format(bytes / (1024f * 1024f))
    bytes >= 1024L -> "${bytes / 1024L}KB"
    else -> "${bytes}B"
}

@Composable
private fun SectionHeader(
    section: SectionGroup,
    deviceCount: Int,
    isCollapsed: Boolean,
    onClick: () -> Unit
) {
    val sectionColor = section.color()

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .background(sectionColor.copy(alpha = 0.08f))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = if (isCollapsed) "\u25B6" else "\u25BC",
            color = sectionColor,
            fontWeight = FontWeight.Bold
        )
        Spacer(modifier = Modifier.width(6.dp))
        Text(text = section.icon)
        Spacer(modifier = Modifier.width(6.dp))
        Text(
            text = section.title,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            color = sectionColor,
            modifier = Modifier.weight(1f)
        )
        Text(
            text = "$deviceCount",
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            color = sectionColor,
            modifier = Modifier
                .background(sectionColor.copy(alpha = 0.15f), MaterialTheme.shapes.small)
                .padding(horizontal = 10.dp, vertical = 3.dp)
        )
    }
}

@Composable
private fun CategoryHeader(
    category: PrivacyCategory,
    count: Int,
    isExpanded: Boolean,
    onClick: () -> Unit
) {
    val threatColor = when (category.threatLevel) {
        3 -> MaterialTheme.colorScheme.error
        2 -> Color(0xFFFF9800)
        1 -> Color(0xFFFFC107)
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f))
            .padding(start = 28.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = if (isExpanded) "\u25BC" else "\u25B6",
            color = threatColor,
            style = MaterialTheme.typography.bodySmall
        )
        Spacer(modifier = Modifier.width(6.dp))
        Text(text = category.icon, modifier = Modifier.width(22.dp))
        Text(
            text = category.label,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.SemiBold,
            color = threatColor,
            modifier = Modifier.weight(1f)
        )
        Text(
            text = "$count",
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
            color = threatColor,
            modifier = Modifier
                .background(threatColor.copy(alpha = 0.12f), MaterialTheme.shapes.extraSmall)
                .padding(horizontal = 6.dp, vertical = 1.dp)
        )
    }
}

@Composable
private fun DeviceCard(
    detection: GlassesDetection,
    onIgnore: () -> Unit,
    onTrack: () -> Unit,
    onDetails: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onDetails)
            .padding(start = 40.dp, end = 16.dp, top = 6.dp, bottom = 6.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = if (detection.hasCamera) "\uD83D\uDCF7" else "\uD83D\uDD0A",
                modifier = Modifier.width(22.dp)
            )
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = "${detection.manufacturer} ${detection.deviceType}",
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier.weight(1f, fill = false)
                    )
                    if (detection.seenMacs.size > 1) {
                        Text(
                            text = " \u00B7 ${detection.seenMacs.size} MACs",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                        )
                    }
                }
                if (detection.deviceName != null) {
                    Text(
                        text = detection.deviceName,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    text = "${detection.rssi}dB",
                    style = MaterialTheme.typography.bodySmall,
                    fontWeight = FontWeight.Medium,
                    color = when {
                        detection.rssi > -50 -> MaterialTheme.colorScheme.error
                        detection.rssi > -70 -> Color(0xFFFF9800)
                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                    }
                )
                Text(
                    text = "${(detection.confidence * 100).toInt()}%",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                )
            }
        }

        // Parsed details
        if (detection.details.isNotEmpty()) {
            Text(
                text = detection.details.entries.take(3).joinToString(" \u2022 ") { "${it.key}: ${it.value}" },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                modifier = Modifier.padding(start = 22.dp, top = 2.dp)
            )
        }

        // Match reason
        Text(
            text = detection.matchReason,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
            modifier = Modifier.padding(start = 22.dp, top = 1.dp)
        )

        // Action buttons
        Row(
            modifier = Modifier.padding(start = 22.dp, top = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Text(
                text = "Ignore",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.clickable(onClick = onIgnore)
            )
            Text(
                text = "Track",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.clickable(onClick = onTrack)
            )
        }

        HorizontalDivider(
            color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f),
            modifier = Modifier.padding(top = 6.dp)
        )
    }
}

@Composable
private fun DeviceDetailDialog(
    detection: GlassesDetection,
    onIgnore: () -> Unit,
    onTrack: () -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = if (detection.hasCamera) "\uD83D\uDCF7" else "\uD83D\uDD0A",
                    modifier = Modifier.width(28.dp)
                )
                Text("${detection.manufacturer} ${detection.deviceType}")
            }
        },
        text = {
            Column {
                if (detection.deviceName != null) DetailRow("Name", detection.deviceName)
                DetailRow("MAC", detection.mac)
                DetailRow("RSSI", "${detection.rssi} dBm")
                DetailRow("Confidence", "${(detection.confidence * 100).toInt()}%")
                DetailRow("Match", detection.matchReason)
                DetailRow("Category", detection.category.label)
                DetailRow("Camera", if (detection.hasCamera) "Yes" else "No")

                if (detection.details.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text("Parsed Details", fontWeight = FontWeight.Medium,
                         style = MaterialTheme.typography.titleSmall)
                    Spacer(modifier = Modifier.height(4.dp))
                    for ((key, value) in detection.details) {
                        DetailRow(key, value)
                    }
                }
            }
        },
        confirmButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onIgnore) { Text("Ignore") }
                TextButton(onClick = onTrack) { Text("Track") }
                TextButton(onClick = onDismiss) { Text("Close") }
            }
        }
    )
}

@Composable
private fun DetailRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 1.dp)
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(90.dp)
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            fontWeight = FontWeight.Medium
        )
    }
}

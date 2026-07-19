package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.alerts.SkyAlertCandidate
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.components.FofStatusStrip
import com.friendorfoe.presentation.components.FofTone

private data class InvestigationDialogSource(
    val title: String,
    val target: BleInvestigationTarget,
    val detection: GlassesDetection,
)

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
    onNavigateToEmfSweep: (() -> Unit)? = null,
    onNavigateToIrCameraScan: (() -> Unit)? = null,
    viewModel: PrivacyViewModel = hiltViewModel()
) {
    val categorized by viewModel.categorizedDetections.collectAsStateWithLifecycle()
    val totalCount by viewModel.totalCount.collectAsStateWithLifecycle()
    val threatCount by viewModel.threatCount.collectAsStateWithLifecycle()
    val backendOnlyMode by viewModel.backendOnlyMode.collectAsStateWithLifecycle()
    val investigationResult by viewModel.investigationResult.collectAsStateWithLifecycle()

    // Track expanded categories (high-threat auto-expanded)
    val expandedCategories = remember {
        mutableStateOf(setOf(PrivacyCategory.SMART_GLASSES,
            PrivacyCategory.HIDDEN_CAMERA, PrivacyCategory.ATTACK_TOOL,
            PrivacyCategory.ULTRASONIC_BEACON, PrivacyCategory.RETAIL_TRACKER,
            PrivacyCategory.SURVEILLANCE_CAMERA, PrivacyCategory.ALPR_CAMERA,
            PrivacyCategory.MOBILE_KEY_LOCK,
            PrivacyCategory.BABY_MONITOR, PrivacyCategory.THERMAL_CAMERA,
            PrivacyCategory.CONFERENCE_CAMERA, PrivacyCategory.VIDEO_INTERCOM,
            PrivacyCategory.VOICE_RECORDER, PrivacyCategory.REMOTE_LISTENING,
            PrivacyCategory.SMART_PEN,
            PrivacyCategory.PAYMENT_READER,
            PrivacyCategory.SMART_SPEAKER, PrivacyCategory.SMART_HOME_HUB,
            PrivacyCategory.GPS_TRACKER, PrivacyCategory.OBD_TRACKER,
            PrivacyCategory.VENUE_BEACON, PrivacyCategory.EVENT_BADGE,
            PrivacyCategory.BLE_HID, PrivacyCategory.AURACAST,
            PrivacyCategory.APPLE_CONTINUITY))
    }

    // Track collapsed sections (all expanded by default)
    val collapsedSections = remember { mutableStateOf(setOf<Int>()) }

    var selectedDetail by remember { mutableStateOf<GlassesDetection?>(null) }
    var investigationDialogSource by remember { mutableStateOf<InvestigationDialogSource?>(null) }
    var trackingTarget by remember { mutableStateOf<GlassesDetection?>(null) }
    val ultrasonicAlerts by viewModel.ultrasonicAlerts.collectAsStateWithLifecycle()
    val wifiAnomalies by viewModel.wifiAnomalies.collectAsStateWithLifecycle()
    val stalkerAlerts by viewModel.stalkerAlerts.collectAsStateWithLifecycle()
    val skyAlertCandidates by viewModel.skyAlertCandidates.collectAsStateWithLifecycle()

    Column(modifier = Modifier.fillMaxSize()) {
        // WiFi anomaly banner (Pwnagotchi, evil twin, karma attack)
        if (wifiAnomalies.isNotEmpty()) {
            val worst = wifiAnomalies.maxByOrNull { it.threatLevel } ?: wifiAnomalies.first()
            val title = when (worst.type) {
                "pwnagotchi" -> "Pwnagotchi detected"
                "evil_twin" -> "Evil twin AP detected"
                "karma_attack" -> "Karma attack detected"
                "rogue_ap" -> "Rogue AP detected"
                else -> "WiFi anomaly"
            }
            FofStatusStrip(
                label = "WIFI",
                title = title,
                detail = worst.details,
                tone = FofTone.Danger
            )
        }

        // Ultrasonic beacon alert banner (high priority, above everything)
        if (ultrasonicAlerts.isNotEmpty()) {
            val alert = ultrasonicAlerts.last()
            FofStatusStrip(
                label = "ULTRA",
                title = "Ultrasonic beacon detected",
                detail = "${"%.0f".format(alert.frequencyHz)} Hz | SNR ${"%.1f".format(alert.snrDb)} dB | ${alert.persistenceFrames} frames",
                tone = FofTone.Danger
            )
        }

        if (stalkerAlerts.isNotEmpty()) {
            val alert = stalkerAlerts.maxByOrNull { it.threatLevel } ?: stalkerAlerts.first()
            val presentation = PrivacyAlertPolicy.stalkerPresentation(
                reason = alert.reason,
                threatLevel = alert.threatLevel,
            )
            FofStatusStrip(
                label = "BLE",
                title = presentation.title,
                detail = "${alert.device.deviceName ?: alert.device.deviceType ?: alert.device.mac} appears ${alert.reason}",
                tone = presentation.tone
            )
        }

        if (backendOnlyMode) {
            BackendOnlyPausedRow(
                onUsePhoneScanner = viewModel::enablePhonePrivacyScanning
            )
        }

        SweepToolsRow(
            onNavigateToEmfSweep = onNavigateToEmfSweep,
            onNavigateToIrCameraScan = onNavigateToIrCameraScan
        )

        FofStatusStrip(
            label = "SCAN",
            title = if (threatCount > 0) "Privacy threats detected" else "Privacy scanner active",
            detail = "$totalCount device${if (totalCount != 1) "s" else ""} detected" +
                if (threatCount > 0) " | $threatCount threat${if (threatCount != 1) "s" else ""}" else "",
            tone = if (threatCount > 0) FofTone.Danger else FofTone.Primary
        )

        LazyColumn(modifier = Modifier.fillMaxSize()) {
            if (categorized.isEmpty()) {
                item(key = "empty_privacy_devices") {
                    FofEmptyState(
                        title = "No privacy devices detected",
                        detail = "Scanning for smart glasses, cameras, trackers, speakers, locks, and other nearby devices.",
                        label = "PRIVACY",
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(32.dp)
                    )
                }
            } else {
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
            item(key = "sky_alerts_footer") {
                SkyAlertsFooter(candidates = skyAlertCandidates)
            }
        }
    }

    // Detail dialog
    if (selectedDetail != null) {
        val detection = selectedDetail!!
        DeviceDetailDialog(
            detection = detection,
            onIgnore = {
                viewModel.ignoreDevice(selectedDetail!!.mac)
                selectedDetail = null
            },
            onTrack = {
                trackingTarget = selectedDetail
                selectedDetail = null
            },
            onInvestigate = detection.investigationTarget?.let { target ->
                {
                    viewModel.clearInvestigation()
                    investigationDialogSource = InvestigationDialogSource(
                        title = detection.deviceName ?: detection.deviceType,
                        target = target,
                        detection = detection,
                    )
                    selectedDetail = null
                }
            },
            onDismiss = { selectedDetail = null }
        )
    }

    investigationDialogSource?.let { source ->
        BleInvestigationDialog(
            source = source,
            result = investigationResult,
            onStart = { viewModel.investigate(source.detection) },
            onCancel = viewModel::cancelInvestigation,
            onDismiss = {
                viewModel.clearInvestigation()
                investigationDialogSource = null
            },
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
private fun BackendOnlyPausedRow(
    onUsePhoneScanner: () -> Unit
) {
    FofStatusStrip(
        label = "PHONE",
        title = "Local privacy scan paused",
        detail = "Backend-only mode is on; backend privacy feeds remain available.",
        tone = FofTone.Primary,
        actionLabel = "Use Phone",
        onAction = onUsePhoneScanner
    )
}

@Composable
private fun SweepToolsRow(
    onNavigateToEmfSweep: (() -> Unit)?,
    onNavigateToIrCameraScan: (() -> Unit)?
) {
    FofSection(
        title = "Sweep Tools",
        subtitle = "Run close-range checks when a signal needs a second look.",
        modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        FofActionRow(
            title = "EMF Sweep",
            description = "Magnetometer check for nearby electronics",
            enabled = onNavigateToEmfSweep != null,
            onClick = onNavigateToEmfSweep
        )
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        FofActionRow(
            title = "IR Camera Scan",
            description = "Front-camera scan for active IR light sources",
            enabled = onNavigateToIrCameraScan != null,
            onClick = onNavigateToIrCameraScan
        )
    }
}

@Composable
private fun SkyAlertsFooter(
    candidates: List<SkyAlertCandidate>
) {
    FofSection(
        title = "Sky Alerts",
        subtitle = "Current alert matches",
        modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        if (candidates.isEmpty()) {
            Text(
                text = "No sky alerts in current settings",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 12.dp)
            )
        } else {
            val visibleCandidates = candidates.take(8)
            visibleCandidates.forEachIndexed { index, candidate ->
                FofActionRow(
                    title = candidate.title,
                    description = candidate.body.ifBlank { "Nearby object" },
                    trailingLabel = ""
                )
                if (index != visibleCandidates.lastIndex) {
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                }
            }
            if (candidates.size > visibleCandidates.size) {
                Text(
                    text = "${candidates.size - visibleCandidates.size} more",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 8.dp)
                )
            }
        }
    }
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
    onInvestigate: (() -> Unit)?,
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
            Column(
                modifier = Modifier.fillMaxWidth(),
                horizontalAlignment = Alignment.End,
            ) {
                if (onInvestigate != null) {
                    TextButton(
                        onClick = onInvestigate,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Icon(
                            imageVector = Icons.Default.Search,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                        )
                        Spacer(Modifier.width(6.dp))
                        Text("Investigate")
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = onIgnore) { Text("Ignore") }
                    TextButton(onClick = onTrack) { Text("Track") }
                    TextButton(onClick = onDismiss) { Text("Close") }
                }
            }
        }
    )
}

@Composable
private fun BleInvestigationDialog(
    source: InvestigationDialogSource,
    result: BleInvestigationResult?,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onDismiss: () -> Unit,
) {
    val running = result?.state in setOf(
        BleInvestigationState.QUEUED,
        BleInvestigationState.SCANNING,
        BleInvestigationState.CONNECTING,
        BleInvestigationState.DISCOVERING,
        BleInvestigationState.READING,
    )
    val terminal = result?.state in setOf(
        BleInvestigationState.COMPLETE,
        BleInvestigationState.FAILED,
        BleInvestigationState.CANCELLED,
    )
    AlertDialog(
        onDismissRequest = { if (!running) onDismiss() },
        title = {
            Column {
                Text(
                    text = "BLE Investigation",
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = source.title,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        },
        text = {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(max = 520.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (result == null) {
                    InvestigationDetailRow("Route", "Phone BLE")
                    InvestigationDetailRow(
                        "Mode",
                        if (source.target.mode == BleInvestigationMode.GATT) "GATT inspection"
                        else "Passive capture",
                    )
                    source.target.mac?.let { InvestigationDetailRow("Target", it) }
                }

                if (result != null) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = investigationStateLabel(result.state),
                                style = MaterialTheme.typography.titleSmall,
                                fontWeight = FontWeight.SemiBold,
                            )
                            Text(
                                text = result.transport.replace('-', ' '),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        if (running) {
                            IconButton(onClick = onCancel) {
                                Icon(
                                    imageVector = Icons.Default.Close,
                                    contentDescription = "Cancel investigation retrieval",
                                )
                            }
                        }
                    }

                    if (result.mode == BleInvestigationMode.PASSIVE_CAPTURE) {
                        InvestigationSectionHeading("Passive Evidence")
                    } else {
                        InvestigationSectionHeading("Summary")
                    }
                    Text(
                        text = result.summary.ifBlank { investigationStateLabel(result.state) },
                        style = MaterialTheme.typography.bodySmall,
                        softWrap = true,
                    )

                    result.error?.let {
                        InvestigationDetailRow("Error", it, MaterialTheme.colorScheme.error)
                    }
                    result.connectable?.let {
                        InvestigationDetailRow("Connectable", if (it) "Yes" else "No")
                    }
                    if (result.bonded || result.encrypted || result.authenticationRequired) {
                        InvestigationSectionHeading("Security")
                        InvestigationDetailRow("Bonded", if (result.bonded) "Yes" else "No")
                        InvestigationDetailRow("Encrypted", if (result.encrypted) "Yes" else "No")
                        if (result.authenticationRequired) {
                            InvestigationDetailRow(
                                "Access",
                                "Authentication required",
                                MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                    if (result.services.isNotEmpty()) {
                        InvestigationSectionHeading("Services")
                        result.services.forEach { uuid ->
                            InvestigationDetailRow(bleUuidLabel(uuid), uuid)
                        }
                    }
                    if (result.characteristics.isNotEmpty()) {
                        InvestigationSectionHeading("Characteristics")
                        result.characteristics.forEach { characteristic ->
                            val properties = characteristic.properties.sorted().joinToString(", ")
                            InvestigationDetailRow(
                                bleUuidLabel(characteristic.uuid),
                                "${characteristic.uuid}${if (properties.isBlank()) "" else " | $properties"}",
                            )
                        }
                    }
                    if (result.reads.isNotEmpty()) {
                        InvestigationSectionHeading("Read Values")
                        result.reads.forEach { (uuid, value) ->
                            InvestigationDetailRow(bleUuidLabel(uuid), formatBleReadValue(value))
                        }
                    }
                    if (result.truncated) {
                        InvestigationDetailRow(
                            "Result",
                            "Additional evidence was truncated",
                            MaterialTheme.colorScheme.error,
                        )
                    }
                }
            }
        },
        confirmButton = {
            when {
                result == null -> Button(
                    onClick = onStart,
                    enabled = source.target.mode == BleInvestigationMode.GATT,
                ) { Text("Start") }
                terminal -> TextButton(onClick = onDismiss) { Text("Close") }
            }
        },
        dismissButton = {
            if (!running && result == null) {
                TextButton(onClick = onDismiss) { Text("Close") }
            }
        },
    )
}

@Composable
private fun InvestigationSectionHeading(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
    )
}

@Composable
private fun InvestigationDetailRow(
    label: String,
    value: String,
    valueColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(82.dp),
            maxLines = 2,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            color = valueColor,
            modifier = Modifier.weight(1f),
            softWrap = true,
        )
    }
}

private fun investigationStateLabel(state: BleInvestigationState): String = when (state) {
    BleInvestigationState.IDLE -> "Ready"
    BleInvestigationState.QUEUED -> "Queued"
    BleInvestigationState.SCANNING -> "Scanning"
    BleInvestigationState.CONNECTING -> "Connecting"
    BleInvestigationState.DISCOVERING -> "Discovering services"
    BleInvestigationState.READING -> "Reading characteristics"
    BleInvestigationState.COMPLETE -> "Complete"
    BleInvestigationState.FAILED -> "Failed"
    BleInvestigationState.CANCELLED -> "Retrieval cancelled"
}

private fun bleUuidLabel(uuid: String): String = when (uuid.uppercase()) {
    "1800", "00001800-0000-1000-8000-00805F9B34FB" -> "Generic Access"
    "180A", "0000180A-0000-1000-8000-00805F9B34FB" -> "Device Info"
    "2A00", "00002A00", "00002A00-0000-1000-8000-00805F9B34FB" -> "Device Name"
    "2A24", "00002A24", "00002A24-0000-1000-8000-00805F9B34FB" -> "Model"
    "2A25", "00002A25", "00002A25-0000-1000-8000-00805F9B34FB" -> "Serial"
    "2A26", "00002A26", "00002A26-0000-1000-8000-00805F9B34FB" -> "Firmware"
    "2A29", "00002A29", "00002A29-0000-1000-8000-00805F9B34FB" -> "Manufacturer"
    else -> "UUID"
}

private fun formatBleReadValue(valueHex: String): String {
    if (valueHex.length % 2 != 0 || valueHex.any { it !in "0123456789ABCDEFabcdef" }) return valueHex
    val bytes = valueHex.chunked(2).mapNotNull { it.toIntOrNull(16)?.toByte() }
    val text = bytes.toByteArray().toString(Charsets.UTF_8).trimEnd('\u0000')
    val readable = text.isNotEmpty() && text.all { it == '\n' || it == '\r' || it == '\t' || !it.isISOControl() }
    return if (readable) "$text | $valueHex" else valueHex
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
            fontWeight = FontWeight.Medium,
            modifier = Modifier.weight(1f),
            softWrap = true,
        )
    }
}

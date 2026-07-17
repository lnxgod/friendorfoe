package com.friendorfoe.presentation.privacy

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
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
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicy
import com.friendorfoe.data.badge.defaultBadgeTheme
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyDetectionOrigin
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.alerts.SkyAlertCandidate
import com.friendorfoe.presentation.badge.BadgeAppearanceSection
import com.friendorfoe.presentation.badge.BadgeDisplayFiltersSection
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.components.FofStatusStrip
import com.friendorfoe.presentation.components.FofTone
import java.time.Instant

private data class InvestigationDialogSource(
    val title: String,
    val origin: PrivacyDetectionOrigin,
    val target: BleInvestigationTarget,
    val detection: GlassesDetection? = null,
    val badgeEntity: BadgeThreatEntity? = null,
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
    val badgeUsbState by viewModel.badgeUsbState.collectAsStateWithLifecycle()
    val backendOnlyMode by viewModel.backendOnlyMode.collectAsStateWithLifecycle()
    val investigationResult by viewModel.investigationResult.collectAsStateWithLifecycle()

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
    var selectedBadgeEntity by remember { mutableStateOf<BadgeThreatEntity?>(null) }
    var investigationDialogSource by remember { mutableStateOf<InvestigationDialogSource?>(null) }
    var selectedInvestigationRoute by remember { mutableStateOf(BleInvestigationRoute.AUTO) }
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

        BadgeUsbStatusRow(
            state = badgeUsbState,
            onAction = {
                if (badgeUsbState.status == BadgeUsbStatus.CONNECTED ||
                    badgeUsbState.status == BadgeUsbStatus.AP_CONNECTED ||
                    badgeUsbState.status == BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED ||
                    badgeUsbState.status == BadgeUsbStatus.BLE_CONNECTED) {
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
            onSetMode = viewModel::setBadgeMode,
            onReboot = viewModel::rebootBadge,
            onBootloader = viewModel::badgeBootloader,
            onRelayScannerFirmware = viewModel::relayBadgeScannerFirmware,
            onFlashScannerFirmware = viewModel::flashBadgeScannerFirmware,
            onApplyDisplayPolicy = viewModel::applyBadgeDisplayPolicy,
            onResetDisplayPolicy = viewModel::resetBadgeDisplayPolicy,
            onApplyTheme = viewModel::applyBadgeTheme,
            onResetTheme = viewModel::resetBadgeTheme,
            onEntityDetails = { selectedBadgeEntity = it }
        )

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
                    selectedInvestigationRoute = BleInvestigationRoute.AUTO
                    investigationDialogSource = InvestigationDialogSource(
                        title = detection.deviceName ?: detection.deviceType,
                        origin = detection.origin,
                        target = target,
                        detection = detection,
                    )
                    selectedDetail = null
                }
            },
            onDismiss = { selectedDetail = null }
        )
    }

    if (selectedBadgeEntity != null) {
        val entity = selectedBadgeEntity!!
        val badgeDetection = entity.toPrivacyDetection(Instant.now())
        BadgeEntityDetailDialog(
            entity = entity,
            onInvestigate = badgeDetection?.investigationTarget?.let { target ->
                {
                    viewModel.clearInvestigation()
                    selectedInvestigationRoute = BleInvestigationRoute.AUTO
                    investigationDialogSource = InvestigationDialogSource(
                        title = entity.label.ifBlank { "Badge Signal" },
                        origin = badgeDetection.origin,
                        target = target,
                        badgeEntity = entity,
                    )
                    selectedBadgeEntity = null
                }
            },
            onDismiss = { selectedBadgeEntity = null }
        )
    }

    investigationDialogSource?.let { source ->
        val routeDecisions = BleInvestigationRoute.entries.associateWith { route ->
            viewModel.investigationRouteDecision(source.origin, source.target, route)
        }
        BleInvestigationDialog(
            source = source,
            selectedRoute = selectedInvestigationRoute,
            routeDecisions = routeDecisions,
            result = investigationResult,
            onRouteSelected = { selectedInvestigationRoute = it },
            onStart = {
                source.detection?.let { viewModel.investigate(it, selectedInvestigationRoute) }
                source.badgeEntity?.let {
                    viewModel.investigateBadgeEntity(it, selectedInvestigationRoute)
                }
            },
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
        detail = "Backend-only mode is on; badge/API feeds still work.",
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
private fun BadgeUsbStatusRow(
    state: BadgeUsbState,
    onAction: () -> Unit
) {
    val connected = state.status == BadgeUsbStatus.CONNECTED ||
        state.status == BadgeUsbStatus.AP_CONNECTED ||
        state.status == BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED ||
        state.status == BadgeUsbStatus.BLE_CONNECTED
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
    val transportLabel = when (state.status) {
        BadgeUsbStatus.CONNECTED -> state.transportLabel.ifBlank { "USB-C" }
        BadgeUsbStatus.AP_CONNECTED -> state.transportLabel.ifBlank { "Badge AP" }
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED -> state.transportLabel.ifBlank { "Debug Bridge" }
        BadgeUsbStatus.BLE_CONNECTED -> state.transportLabel.ifBlank { "BLE" }
        BadgeUsbStatus.CONNECTING,
        BadgeUsbStatus.PERMISSION_NEEDED -> "USB-C"
        BadgeUsbStatus.ERROR,
        BadgeUsbStatus.DISCONNECTED -> "Badge"
    }
    val headline = when (state.status) {
        BadgeUsbStatus.CONNECTED -> "USB-C badge live privacy feed"
        BadgeUsbStatus.AP_CONNECTED -> "Badge AP live privacy feed"
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED -> "Debug Bridge badge live privacy feed"
        BadgeUsbStatus.BLE_CONNECTED -> "BLE badge live privacy feed"
        else -> state.message
    }
    val tone = when (state.status) {
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.AP_CONNECTED,
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
        BadgeUsbStatus.BLE_CONNECTED -> FofTone.Success
        BadgeUsbStatus.CONNECTING,
        BadgeUsbStatus.PERMISSION_NEEDED -> FofTone.Primary
        BadgeUsbStatus.ERROR -> FofTone.Danger
        BadgeUsbStatus.DISCONNECTED -> FofTone.Neutral
    }
    FofStatusStrip(
        label = transportLabel.uppercase().take(8),
        title = headline,
        detail = if (scannerSummary.isNotBlank()) "$summary  |  $scannerSummary" else summary,
        tone = tone,
        actionLabel = if (connected) "Refresh" else "Connect",
        onAction = onAction
    )
}

@Composable
private fun BadgeDetailPanel(
    state: BadgeUsbState,
    onNext: () -> Unit,
    onDetail: () -> Unit,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    onSetMode: (String) -> Unit,
    onReboot: () -> Unit,
    onBootloader: () -> Unit,
    onRelayScannerFirmware: (String) -> Unit,
    onFlashScannerFirmware: (String, String, ByteArray) -> Unit,
    onApplyDisplayPolicy: (BadgeDisplayPolicy) -> Unit,
    onResetDisplayPolicy: () -> Unit,
    onApplyTheme: (BadgeTheme) -> Unit,
    onResetTheme: () -> Unit,
    onEntityDetails: (BadgeThreatEntity) -> Unit
) {
    val status = state.controlStatus ?: return
    val connected = state.status == BadgeUsbStatus.CONNECTED ||
        state.status == BadgeUsbStatus.AP_CONNECTED ||
        state.status == BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED ||
        state.status == BadgeUsbStatus.BLE_CONNECTED
    val display = status.displayState
    val accent = badgeHealthColor(status)
    var filtersExpanded by remember { mutableStateOf(false) }
    var appearanceExpanded by remember { mutableStateOf(false) }
    var operationsExpanded by remember { mutableStateOf(false) }
    var draftPolicy by remember { mutableStateOf(status.displayPolicy) }
    var draftTheme by remember { mutableStateOf(status.theme) }
    LaunchedEffect(status.displayPolicyHash) {
        draftPolicy = status.displayPolicy
    }
    LaunchedEffect(status.themeHash) {
        draftTheme = status.theme
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(accent.copy(alpha = 0.08f))
            .padding(horizontal = 16.dp, vertical = 10.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Badge Control Center",
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
        if (status.bleControl.enabled || status.bleControl.lastError.isNotBlank()) {
            Text(
                text = buildString {
                    append("BLE tether ")
                    append(if (status.bleControl.enabled) "ready" else "off")
                    if (status.bleControl.connected) append(" connected")
                    if (status.bleControl.bonded) append(" bonded")
                    if (status.bleControl.encrypted) append(" encrypted")
                    status.bleControl.pairingAgeSeconds
                        ?.takeIf { it >= 0 }
                        ?.let { append(" pair ${it}/${status.bleControl.pairingWindowSeconds}s") }
                    if (status.bleControl.lastError.isNotBlank()) {
                        append("  |  ${status.bleControl.lastError}")
                    }
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }

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

        if (connected) {
            Spacer(modifier = Modifier.height(8.dp))
            BadgeOperationsSection(
                expanded = operationsExpanded,
                onExpandedChange = { operationsExpanded = it },
                state = state,
                onSetMode = onSetMode,
                onReboot = onReboot,
                onBootloader = onBootloader,
                onRelayScannerFirmware = onRelayScannerFirmware,
                onFlashScannerFirmware = onFlashScannerFirmware
            )
            Spacer(modifier = Modifier.height(8.dp))
            BadgeAppearanceSection(
                expanded = appearanceExpanded,
                onExpandedChange = { appearanceExpanded = it },
                theme = draftTheme,
                themeHash = status.themeHash,
                onThemeChange = { draftTheme = it },
                onApply = { onApplyTheme(draftTheme) },
                onReset = {
                    draftTheme = defaultBadgeTheme()
                    onResetTheme()
                },
                onRefresh = onRefresh
            )
            Spacer(modifier = Modifier.height(8.dp))
            BadgeDisplayFiltersSection(
                expanded = filtersExpanded,
                onExpandedChange = { filtersExpanded = it },
                policy = draftPolicy,
                displayPolicyHash = status.displayPolicyHash,
                filteredCounts = status.filteredCounts,
                onPolicyChange = { draftPolicy = it },
                onApply = { onApplyDisplayPolicy(draftPolicy) },
                onReset = {
                    draftPolicy = defaultBadgeDisplayPolicy()
                    onResetDisplayPolicy()
                },
                onRefresh = onRefresh
            )
        }

        status.entities.take(6).forEach { entity ->
            BadgeEntityRow(entity = entity, onClick = { onEntityDetails(entity) })
        }
    }
}

@Composable
private fun BadgeOperationsSection(
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    state: BadgeUsbState,
    onSetMode: (String) -> Unit,
    onReboot: () -> Unit,
    onBootloader: () -> Unit,
    onRelayScannerFirmware: (String) -> Unit,
    onFlashScannerFirmware: (String, String, ByteArray) -> Unit
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

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                MaterialTheme.shapes.small
            )
            .padding(8.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Badge Operations",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = listOfNotNull(
                        state.transportLabel.ifBlank { null },
                        state.controlStatus?.modeLabel?.ifBlank { null },
                        state.firmwareProgress?.stage?.ifBlank { null }
                    ).joinToString("  |  ").ifBlank { state.message },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            OutlinedButton(onClick = { onExpandedChange(!expanded) }) {
                Text(if (expanded) "Hide" else "Open")
            }
        }

        if (!expanded) return@Column

        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "Mode",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.fillMaxWidth()
        ) {
            OutlinedButton(
                onClick = { onSetMode("local_ap") },
                modifier = Modifier.weight(1f)
            ) { Text("Local AP", maxLines = 1) }
            OutlinedButton(
                onClick = { onSetMode("backend") },
                modifier = Modifier.weight(1f)
            ) { Text("Backend", maxLines = 1) }
            OutlinedButton(
                onClick = { onSetMode("usb_only") },
                modifier = Modifier.weight(1f)
            ) { Text("USB", maxLines = 1) }
        }

        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "Scanner Firmware",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.fillMaxWidth()
        ) {
            Button(
                onClick = {
                    pendingFirmwareUart = "ble"
                    firmwarePicker.launch(arrayOf("application/octet-stream", "*/*"))
                },
                modifier = Modifier.weight(1f)
            ) { Text("BLE Slot", maxLines = 1) }
            Button(
                onClick = {
                    pendingFirmwareUart = "wifi"
                    firmwarePicker.launch(arrayOf("application/octet-stream", "*/*"))
                },
                modifier = Modifier.weight(1f)
            ) { Text("WiFi Slot", maxLines = 1) }
        }
        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 6.dp)
        ) {
            OutlinedButton(
                onClick = { onRelayScannerFirmware("ble") },
                modifier = Modifier.weight(1f)
            ) { Text("Relay BLE", maxLines = 1) }
            OutlinedButton(
                onClick = { onRelayScannerFirmware("wifi") },
                modifier = Modifier.weight(1f)
            ) { Text("Relay WiFi", maxLines = 1) }
        }

        state.firmwareProgress?.let { progress ->
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = progress.error.ifBlank {
                    "${progress.kind} ${progress.uart.ifBlank { "scanner" }} ${progress.stage} ${progress.percent}%"
                },
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

        Spacer(modifier = Modifier.height(8.dp))
        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.fillMaxWidth()
        ) {
            OutlinedButton(
                onClick = onReboot,
                modifier = Modifier.weight(1f)
            ) { Text("Reboot", maxLines = 1) }
            OutlinedButton(
                onClick = onBootloader,
                modifier = Modifier.weight(1f)
            ) { Text("Bootloader", maxLines = 1) }
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
    onInvestigate: (() -> Unit)?,
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
                if (entity.ssid.isNotBlank()) DetailRow("SSID", entity.ssid)
                if (entity.bssid.isNotBlank()) DetailRow("BSSID", entity.bssid)
                if (entity.authMode >= 0) DetailRow("WiFi Auth", badgeWifiAuthLabel(entity.authMode))
                if (entity.freqMhz > 0) DetailRow("Frequency", "${entity.freqMhz} MHz")
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
                TextButton(onClick = onDismiss) { Text("Close") }
            }
        }
    )
}

private fun badgeWifiAuthLabel(authMode: Int): String = when (authMode) {
    0 -> "Open"
    1 -> "WEP"
    2 -> "WPA"
    3 -> "WPA2"
    4 -> "WPA/WPA2"
    5 -> "WPA2 Enterprise"
    6 -> "WPA3"
    7 -> "WPA2/WPA3"
    8 -> "WAPI"
    9 -> "OWE"
    10 -> "WPA3 Enterprise"
    else -> "Unknown ($authMode)"
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun BleInvestigationDialog(
    source: InvestigationDialogSource,
    selectedRoute: BleInvestigationRoute,
    routeDecisions: Map<BleInvestigationRoute, BleInvestigationRouteDecision>,
    result: BleInvestigationResult?,
    onRouteSelected: (BleInvestigationRoute) -> Unit,
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
    val selectedAvailable = routeDecisions[selectedRoute]?.route != null

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
                    SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                        BleInvestigationRoute.entries.forEachIndexed { index, route ->
                            val available = routeDecisions[route]?.route != null
                            val name = when (route) {
                                BleInvestigationRoute.AUTO -> "Auto"
                                BleInvestigationRoute.PHONE -> "Phone"
                                BleInvestigationRoute.BADGE -> "Badge"
                            }
                            SegmentedButton(
                                selected = selectedRoute == route,
                                onClick = { onRouteSelected(route) },
                                enabled = available,
                                shape = SegmentedButtonDefaults.itemShape(
                                    index = index,
                                    count = BleInvestigationRoute.entries.size,
                                ),
                                label = {
                                    Text(
                                        text = if (available) name else "$name\nUnavailable",
                                        style = MaterialTheme.typography.labelSmall,
                                        textAlign = TextAlign.Center,
                                        maxLines = 2,
                                        softWrap = true,
                                    )
                                },
                            )
                        }
                    }
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
                    enabled = selectedAvailable,
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

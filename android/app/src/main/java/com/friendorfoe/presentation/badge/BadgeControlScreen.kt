package com.friendorfoe.presentation.badge

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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.badge.BadgeControlTransportPolicy
import com.friendorfoe.data.badge.BadgeDisplayNavAction
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbActivity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicy
import com.friendorfoe.data.badge.defaultBadgeTheme
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationState

internal const val MAX_BADGE_LIVE_FEED_ITEMS = 32

internal fun boundedBadgeActivityFeed(
    activity: List<BadgeUsbActivity>,
    initialFocusKey: String?,
): List<BadgeUsbActivity> {
    val focused = initialFocusKey?.let { key -> activity.firstOrNull { it.key == key } }
        ?: return activity.take(MAX_BADGE_LIVE_FEED_ITEMS)
    return listOf(focused) + activity.asSequence()
        .filterNot { it.key == focused.key }
        .take(MAX_BADGE_LIVE_FEED_ITEMS - 1)
        .toList()
}

@Composable
fun BadgeControlScreen(
    initialFocusKey: String? = null,
    viewModel: BadgeControlViewModel = hiltViewModel(),
) {
    val state by viewModel.badgeState.collectAsStateWithLifecycle()
    val profiles by viewModel.themeProfiles.collectAsStateWithLifecycle()
    val investigation by viewModel.investigation.collectAsStateWithLifecycle()
    var draftTheme by remember(state.controlStatus?.themeHash) {
        mutableStateOf(state.controlStatus?.theme ?: defaultBadgeTheme())
    }
    var draftPolicy by remember(state.controlStatus?.displayPolicyHash) {
        mutableStateOf(state.controlStatus?.displayPolicy ?: defaultBadgeDisplayPolicy())
    }
    var appearanceExpanded by remember { mutableStateOf(false) }
    var filtersExpanded by remember { mutableStateOf(false) }
    var pendingDanger by remember { mutableStateOf<BadgeDangerAction?>(null) }
    var selectedEntity by remember { mutableStateOf<BadgeThreatEntity?>(null) }
    val commandsEnabled = BadgeControlTransportPolicy.allowsCommandSurface(state.status)
    val refreshEnabled = BadgeControlTransportPolicy.allowsStatusRefresh(state.status)

    fun dispatchDanger(event: BadgeDangerEvent) {
        val transition = reduceBadgeDangerCommand(
            pending = pendingDanger,
            event = event,
            commandsEnabled = commandsEnabled,
        )
        pendingDanger = transition.pending
        transition.confirmed?.let(viewModel::execute)
    }

    LaunchedEffect(commandsEnabled) {
        if (!commandsEnabled) {
            dispatchDanger(BadgeDangerEvent.Cancel)
        }
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            BadgeHeader()
        }
        item {
            BadgeStatusSection(
                state = state,
                refreshEnabled = refreshEnabled,
                onGrantUsbAccess = viewModel::grantUsbAccess,
                onRefresh = viewModel::refresh,
            )
        }
        item {
            BadgeLiveFeedSection(
                state = state,
                initialFocusKey = initialFocusKey,
                investigation = investigation,
                onCancelInvestigation = viewModel::cancelInvestigation,
                onEntityDetails = { selectedEntity = it },
            )
        }
        item {
            BadgeLcdRemoteSection(
                display = state.controlStatus?.displayState,
                commandsEnabled = commandsEnabled,
                onNavigate = viewModel::displayNav,
            )
        }
        item {
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp)
                    .testTag("badge_appearance"),
                shape = MaterialTheme.shapes.medium,
                tonalElevation = 2.dp,
            ) {
                BadgeAppearanceSection(
                    expanded = appearanceExpanded,
                    onExpandedChange = { appearanceExpanded = it },
                    theme = draftTheme,
                    appliedTheme = state.controlStatus?.theme ?: defaultBadgeTheme(),
                    themeHash = state.controlStatus?.themeHash ?: 0,
                    profiles = profiles,
                    onThemeChange = { draftTheme = it },
                    onCreateProfile = viewModel::createProfile,
                    onRenameProfile = viewModel::renameProfile,
                    onReplaceProfile = viewModel::replaceProfile,
                    onDeleteProfile = viewModel::deleteProfile,
                    onApply = viewModel::applyTheme,
                    onRefresh = viewModel::refresh,
                    commandsEnabled = commandsEnabled,
                    refreshEnabled = refreshEnabled,
                )
            }
        }
        item {
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp)
                    .testTag("badge_filters"),
                shape = MaterialTheme.shapes.medium,
                tonalElevation = 2.dp,
            ) {
                BadgeDisplayFiltersSection(
                    expanded = filtersExpanded,
                    onExpandedChange = { filtersExpanded = it },
                    policy = draftPolicy,
                    displayPolicyHash = state.controlStatus?.displayPolicyHash ?: 0,
                    filteredCounts = state.controlStatus?.filteredCounts.orEmpty(),
                    onPolicyChange = { draftPolicy = it },
                    onApply = { viewModel.applyDisplayPolicy(draftPolicy) },
                    onReset = {
                        draftPolicy = defaultBadgeDisplayPolicy()
                        viewModel.resetDisplayPolicy()
                    },
                    onRefresh = viewModel::refresh,
                    remoteActionsEnabled = commandsEnabled,
                    refreshEnabled = refreshEnabled,
                )
            }
        }
        item {
            BadgeOperationsSection(
                state = state,
                commandsEnabled = commandsEnabled,
                onSetMode = viewModel::setMode,
                onResetTheme = viewModel::resetTheme,
                onDanger = { action -> dispatchDanger(BadgeDangerEvent.Request(action)) },
            )
        }
        item { Spacer(modifier = Modifier.height(6.dp)) }
    }

    pendingDanger?.let { action ->
        BadgeDangerConfirmationDialog(
            action = action,
            commandsEnabled = commandsEnabled,
            onConfirm = { dispatchDanger(BadgeDangerEvent.Confirm) },
            onDismiss = { dispatchDanger(BadgeDangerEvent.Cancel) },
        )
    }

    selectedEntity?.let { entity ->
        BadgeEntityDetailDialog(
            entity = entity,
            canInvestigate = state.badgeInvestigationAvailable() && viewModel.canInvestigate(entity),
            onInvestigate = {
                viewModel.investigate(entity)
                selectedEntity = null
            },
            onDismiss = { selectedEntity = null },
        )
    }
}

@Composable
internal fun BadgeHeader() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            imageVector = BadgeMarkIcon,
            contentDescription = null,
            tint = BadgeMarkGold,
            modifier = Modifier
                .size(40.dp)
                .testTag("badge_triforce"),
        )
        Text(
            text = "Badge",
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.Bold,
        )
    }
}

@Composable
private fun BadgeStatusSection(
    state: BadgeUsbState,
    refreshEnabled: Boolean,
    onGrantUsbAccess: () -> Unit,
    onRefresh: () -> Unit,
) {
    val identityLines = badgeStatusIdentityLines(state)
    BadgeSection(
        title = "Badge USB",
        subtitle = state.transportLabel.ifBlank { state.status.name.replace('_', ' ') },
        modifier = Modifier.testTag("badge_status"),
    ) {
        identityLines.forEachIndexed { index, line ->
            Text(
                text = line,
                style = if (index == 0 && state.deviceName != null) {
                    MaterialTheme.typography.bodyMedium
                } else {
                    MaterialTheme.typography.bodySmall
                },
                color = if (line == state.message && state.status == BadgeUsbStatus.ERROR) {
                    MaterialTheme.colorScheme.error
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
            )
        }
        if (badgeStatusIsStale(state)) {
            Text(
                text = "Cached badge status  •  controls disabled",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
        Text(
            text = "${state.eventCount} events  •  ${state.controlStatus?.version ?: "Status unavailable"}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        state.controlStatus?.let { status ->
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 9.dp),
                shape = MaterialTheme.shapes.small,
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
            ) {
                Column(
                    modifier = Modifier.padding(9.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    Text(
                        text = "System health",
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.SemiBold,
                    )
                    badgeStatusHealthRows(status).forEach { (label, value) ->
                        BadgeStatusMetricRow(label, value)
                    }
                }
            }
        }
        Row(
            modifier = Modifier.padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (state.status == BadgeUsbStatus.PERMISSION_NEEDED) {
                Button(onClick = onGrantUsbAccess) { Text("Grant USB access") }
            }
            OutlinedButton(onClick = onRefresh, enabled = refreshEnabled) {
                Text("Refresh status")
            }
        }
    }
}

@Composable
private fun BadgeStatusMetricRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.23f),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.weight(0.77f),
        )
    }
}

@Composable
private fun BadgeLiveFeedSection(
    state: BadgeUsbState,
    initialFocusKey: String?,
    investigation: BleInvestigationResult?,
    onCancelInvestigation: () -> Unit,
    onEntityDetails: (BadgeThreatEntity) -> Unit,
) {
    val entries = boundedBadgeActivityFeed(state.activity, initialFocusKey)
    val focusedKey = initialFocusKey?.takeIf { key -> entries.firstOrNull()?.key == key }

    BadgeSection(
        title = "Live badge feed",
        subtitle = "Latest ${entries.size} USB events${if (focusedKey != null) "  •  focused" else ""}",
        modifier = Modifier.testTag("badge_live_feed"),
    ) {
        if (entries.isEmpty()) {
            Text(
                text = "Badge events will appear here when status or detections arrive.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        } else {
            entries.forEach { entry -> BadgeFeedRow(entry, entry.key == focusedKey) }
        }
        state.controlStatus?.entities.orEmpty().take(8).forEach { entity ->
            BadgeEntityRow(entity = entity, onClick = { onEntityDetails(entity) })
        }
        investigation?.let { result ->
            BadgeInvestigationResultCard(
                result = result,
                onCancel = onCancelInvestigation,
            )
        }
    }
}

@Composable
private fun BadgeFeedRow(entry: BadgeUsbActivity, focused: Boolean) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .background(
                if (focused) BadgeMarkGold.copy(alpha = 0.14f) else Color.Transparent,
                MaterialTheme.shapes.small,
            )
            .padding(horizontal = 6.dp, vertical = 4.dp),
    ) {
        Text(
            text = entry.title,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = if (focused) FontWeight.Bold else FontWeight.Medium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (entry.detail.isNotBlank()) {
            Text(
                text = entry.detail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun BadgeEntityRow(entity: BadgeThreatEntity, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 6.dp, vertical = 7.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = entity.label.ifBlank { entity.threatClass.ifBlank { "Badge signal" } },
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = listOfNotNull(
                    entity.detail.takeIf { it.isNotBlank() },
                    entity.threatClass.takeIf { it.isNotBlank() },
                    entity.source.takeIf { it.isNotBlank() },
                ).joinToString("  •  "),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Text(
            text = "${entity.rssi} dBm",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun BadgeEntityDetailDialog(
    entity: BadgeThreatEntity,
    canInvestigate: Boolean,
    onInvestigate: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(entity.label.ifBlank { "Badge signal" }) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                BadgeInvestigationDetail("Class", entity.threatClass.ifBlank { "unknown" })
                entity.detail.takeIf { it.isNotBlank() }?.let {
                    BadgeInvestigationDetail("Detail", it)
                }
                entity.evidence.takeIf { it.isNotBlank() }?.let {
                    BadgeInvestigationDetail("Evidence", it)
                }
                entity.bssid.takeIf { it.isNotBlank() }?.let {
                    BadgeInvestigationDetail("BSSID", it)
                }
                entity.displayId.takeIf { it.isNotBlank() }?.let {
                    BadgeInvestigationDetail("Display ID", it)
                }
                BadgeInvestigationDetail("RSSI", "${entity.rssi} dBm")
                BadgeInvestigationDetail("Age", "${entity.lastSeenSeconds.coerceAtLeast(0)}s")
            }
        },
        confirmButton = {
            if (canInvestigate) {
                Button(onClick = onInvestigate) { Text("Investigate") }
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Close") } },
    )
}

@Composable
private fun BadgeInvestigationResultCard(
    result: BleInvestigationResult,
    onCancel: () -> Unit,
) {
    val running = result.state in BADGE_INVESTIGATION_ACTIVE_STATES
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 10.dp)
            .background(BadgeMarkGold.copy(alpha = 0.10f), MaterialTheme.shapes.small)
            .padding(9.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "BLE investigation · ${badgeInvestigationStateLabel(result.state)}",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = result.transport.replace('-', ' '),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (running) {
                TextButton(onClick = onCancel) { Text("Cancel") }
            }
        }
        Text(
            text = result.summary.ifBlank { badgeInvestigationStateLabel(result.state) },
            style = MaterialTheme.typography.bodySmall,
        )
        result.error?.let { error ->
            BadgeInvestigationDetail("Error", error, MaterialTheme.colorScheme.error)
        }
        result.targetMac?.let { BadgeInvestigationDetail("Target", it) }
        if (result.mode == BleInvestigationMode.PASSIVE_CAPTURE) {
            BadgeInvestigationDetail("Mode", "Passive capture")
        }
        result.connectable?.let {
            BadgeInvestigationDetail("Connectable", if (it) "Yes" else "No")
        }
        if (result.bonded || result.encrypted || result.authenticationRequired) {
            BadgeInvestigationDetail("Bonded", if (result.bonded) "Yes" else "No")
            BadgeInvestigationDetail("Encrypted", if (result.encrypted) "Yes" else "No")
            if (result.authenticationRequired) {
                BadgeInvestigationDetail(
                    "Access",
                    "Authentication required",
                    MaterialTheme.colorScheme.error,
                )
            }
        }
        if (result.services.isNotEmpty()) {
            BadgeInvestigationDetail("Services", result.services.joinToString())
        }
        if (result.characteristics.isNotEmpty()) {
            BadgeInvestigationDetail(
                "Characteristics",
                result.characteristics.joinToString { it.uuid },
            )
        }
        if (result.reads.isNotEmpty()) {
            BadgeInvestigationDetail(
                "Reads",
                result.reads.entries.joinToString { "${it.key}: ${it.value}" },
            )
        }
        if (result.truncated) {
            BadgeInvestigationDetail(
                "Result",
                "Additional evidence was truncated",
                MaterialTheme.colorScheme.error,
            )
        }
    }
}

@Composable
private fun BadgeInvestigationDetail(
    label: String,
    value: String,
    valueColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.32f),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            color = valueColor,
            modifier = Modifier.weight(0.68f),
        )
    }
}

private fun badgeInvestigationStateLabel(state: BleInvestigationState): String = when (state) {
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

@Composable
private fun BadgeLcdRemoteSection(
    display: BadgeDisplayState?,
    commandsEnabled: Boolean,
    onNavigate: (BadgeDisplayNavAction) -> Unit,
) {
    BadgeSection(
        title = "LCD remote",
        subtitle = display?.title?.ifBlank { "No active LCD item" } ?: "No LCD snapshot",
        modifier = Modifier.testTag("badge_lcd_remote"),
    ) {
        display?.detail?.takeIf { it.isNotBlank() }?.let {
            Text(it, style = MaterialTheme.typography.bodySmall)
        }
        listOf(
            listOf(
                "Next" to BadgeDisplayNavAction.NEXT,
                "Detail" to BadgeDisplayNavAction.DETAIL,
            ),
            listOf(
                "Page" to BadgeDisplayNavAction.PAGE,
                "Back" to BadgeDisplayNavAction.BACK,
            ),
        ).forEach { row ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 7.dp),
                horizontalArrangement = Arrangement.spacedBy(7.dp),
            ) {
                row.forEach { (label, action) ->
                    OutlinedButton(
                        onClick = { onNavigate(action) },
                        enabled = commandsEnabled,
                        modifier = Modifier.weight(1f),
                    ) { Text(label, maxLines = 1) }
                }
            }
        }
    }
}

@Composable
private fun BadgeOperationsSection(
    state: BadgeUsbState,
    commandsEnabled: Boolean,
    onSetMode: (String) -> Unit,
    onResetTheme: () -> Unit,
    onDanger: (BadgeDangerAction) -> Unit,
) {
    BadgeSection(
        title = "Operations",
        subtitle = "Mode ${state.controlStatus?.modeLabel ?: "unavailable"}",
        modifier = Modifier.testTag("badge_operations"),
    ) {
        Text("Network mode", style = MaterialTheme.typography.labelLarge)
        Row(
            modifier = Modifier.padding(top = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            listOf(
                "Local AP" to "local_ap",
                "Backend" to "backend",
                "USB only" to "usb_only",
            ).forEach { (label, mode) ->
                OutlinedButton(
                    onClick = { onSetMode(mode) },
                    enabled = commandsEnabled,
                    modifier = Modifier.weight(1f),
                ) { Text(label, maxLines = 1) }
            }
        }
        OutlinedButton(
            onClick = onResetTheme,
            enabled = commandsEnabled,
            modifier = Modifier.padding(top = 8.dp),
        ) {
            Text("Reset badge theme")
        }

        Text(
            text = "Danger zone",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(top = 14.dp),
        )
        OutlinedButton(
            onClick = { onDanger(BadgeDangerAction.REBOOT) },
            enabled = commandsEnabled,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 7.dp),
        ) { Text("Reboot") }
    }
}

@Composable
private fun BadgeDangerConfirmationDialog(
    action: BadgeDangerAction,
    commandsEnabled: Boolean,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    val prompt = when (action) {
        BadgeDangerAction.REBOOT -> "Reboot the badge now?"
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(prompt) },
        text = { Text("This sends a command over the active USB control transport.") },
        confirmButton = {
            Button(onClick = onConfirm, enabled = commandsEnabled) { Text("Confirm") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun BadgeSection(
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp),
        shape = MaterialTheme.shapes.medium,
        tonalElevation = 2.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(modifier = Modifier.height(9.dp))
            content()
        }
    }
}

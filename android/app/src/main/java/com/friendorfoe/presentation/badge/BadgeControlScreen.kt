package com.friendorfoe.presentation.badge

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.badge.BadgeControlTransportPolicy
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeUsbActivity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicy
import com.friendorfoe.data.badge.defaultBadgeTheme

internal const val MAX_BADGE_LIVE_FEED_ITEMS = 32

@Composable
fun BadgeControlScreen(
    initialFocusKey: String? = null,
    viewModel: BadgeControlViewModel = hiltViewModel(),
) {
    val state by viewModel.badgeState.collectAsStateWithLifecycle()
    val profiles by viewModel.themeProfiles.collectAsStateWithLifecycle()
    var draftTheme by remember(state.controlStatus?.themeHash) {
        mutableStateOf(state.controlStatus?.theme ?: defaultBadgeTheme())
    }
    var draftPolicy by remember(state.controlStatus?.displayPolicyHash) {
        mutableStateOf(state.controlStatus?.displayPolicy ?: defaultBadgeDisplayPolicy())
    }
    var appearanceExpanded by remember { mutableStateOf(false) }
    var filtersExpanded by remember { mutableStateOf(false) }
    var pendingDanger by remember { mutableStateOf<BadgeDangerAction?>(null) }
    val commandsEnabled = BadgeControlTransportPolicy.allowsCommandSurface(state.status)
    val refreshEnabled = BadgeControlTransportPolicy.allowsStatusRefresh(state.status)

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            BadgeStatusSection(
                state = state,
                refreshEnabled = refreshEnabled,
                onGrantUsbAccess = viewModel::grantUsbAccess,
                onRefresh = viewModel::refresh,
            )
        }
        item { BadgeLiveFeedSection(state, initialFocusKey) }
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
                onDanger = { pendingDanger = it },
            )
        }
        item { Spacer(modifier = Modifier.height(6.dp)) }
    }

    pendingDanger?.let { action ->
        BadgeDangerConfirmationDialog(
            action = action,
            onConfirm = {
                pendingDanger = null
                viewModel.execute(action)
            },
            onDismiss = { pendingDanger = null },
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
    BadgeSection(
        title = "Badge USB",
        subtitle = state.transportLabel.ifBlank { state.status.name.replace('_', ' ') },
        modifier = Modifier.testTag("badge_status"),
    ) {
        Text(
            text = state.deviceName ?: state.message,
            style = MaterialTheme.typography.bodyMedium,
        )
        Text(
            text = "${state.eventCount} events  •  ${state.controlStatus?.version ?: "Status unavailable"}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
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
private fun BadgeLiveFeedSection(
    state: BadgeUsbState,
    initialFocusKey: String?,
) {
    val bounded = state.activity.take(MAX_BADGE_LIVE_FEED_ITEMS)
    val focused = initialFocusKey?.let { key -> bounded.firstOrNull { it.key == key } }
    val entries = if (focused == null) bounded else listOf(focused) + bounded.filterNot {
        it.key == focused.key
    }

    BadgeSection(
        title = "Live badge feed",
        subtitle = "Latest ${entries.size} USB events${if (focused != null) "  •  focused" else ""}",
        modifier = Modifier.testTag("badge_live_feed"),
    ) {
        if (entries.isEmpty()) {
            Text(
                text = "Badge events will appear here when status or detections arrive.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        } else {
            entries.forEach { entry -> BadgeFeedRow(entry, entry.key == focused?.key) }
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
private fun BadgeLcdRemoteSection(
    display: BadgeDisplayState?,
    commandsEnabled: Boolean,
    onNavigate: (String) -> Unit,
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
            listOf("Prev" to "prev", "Up" to "up", "Next" to "next"),
            listOf("Back" to "back", "Select" to "select", "Down" to "down"),
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
            text = BadgeControlTransportPolicy.scannerFirmwareRecoveryHeading(),
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(top = 14.dp),
        )
        Text(
            text = BadgeControlTransportPolicy.scannerFirmwareStagingGuidance(),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            modifier = Modifier.padding(top = 7.dp),
            horizontalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            OutlinedButton(
                onClick = { onDanger(BadgeDangerAction.RECOVER_SLOT_0) },
                enabled = commandsEnabled,
                modifier = Modifier.weight(1f),
            ) { Text("Recover Slot 0") }
            OutlinedButton(
                onClick = { onDanger(BadgeDangerAction.RECOVER_SLOT_1) },
                enabled = commandsEnabled,
                modifier = Modifier.weight(1f),
            ) { Text("Recover Slot 1") }
        }

        Text(
            text = "Danger zone",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(top = 14.dp),
        )
        Row(
            modifier = Modifier.padding(top = 7.dp),
            horizontalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            OutlinedButton(
                onClick = { onDanger(BadgeDangerAction.REBOOT) },
                enabled = commandsEnabled,
                modifier = Modifier.weight(1f),
            ) { Text("Reboot") }
            OutlinedButton(
                onClick = { onDanger(BadgeDangerAction.BOOTLOADER) },
                enabled = commandsEnabled,
                modifier = Modifier.weight(1f),
            ) { Text("Bootloader") }
        }
    }
}

@Composable
private fun BadgeDangerConfirmationDialog(
    action: BadgeDangerAction,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    val prompt = when (action) {
        BadgeDangerAction.REBOOT -> "Reboot the badge now?"
        BadgeDangerAction.BOOTLOADER -> "Enter badge bootloader now?"
        BadgeDangerAction.RECOVER_SLOT_0 -> "Recover scanner slot 0 now?"
        BadgeDangerAction.RECOVER_SLOT_1 -> "Recover scanner slot 1 now?"
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(prompt) },
        text = { Text("This sends a command over the active USB control transport.") },
        confirmButton = { Button(onClick = onConfirm) { Text("Confirm") } },
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

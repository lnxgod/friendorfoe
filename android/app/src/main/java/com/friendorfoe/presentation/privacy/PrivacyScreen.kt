package com.friendorfoe.presentation.privacy

import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Block
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofErrorState
import com.friendorfoe.presentation.components.FofLoadingState
import com.friendorfoe.presentation.components.FofNoMatchesState
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsable
import com.friendorfoe.presentation.permissions.permissionExplanation
import com.friendorfoe.presentation.permissions.permissionRecovery
import com.friendorfoe.presentation.permissions.permissionTitle
import com.friendorfoe.presentation.permissions.rememberPermissionBindings
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

data class PrivacyActions(
    val onRecoverSource: (PrivacySourceKind) -> Unit = {},
    val onEnablePhoneScan: () -> Unit = {},
    val onResolveSourcePermission: (PrivacySourceKind) -> Unit = {},
    val onTurnOnBluetooth: () -> Unit = {},
    val onQueryChanged: (String) -> Unit = {},
    val onToggleCategory: (PrivacyCategory) -> Unit = {},
    val onToggleSource: (PrivacySourceKind) -> Unit = {},
    val onClearFilters: () -> Unit = {},
    val onRetryAllSources: () -> Unit = {},
    val onOpenBackendSettings: (() -> Unit)? = null,
    val onOpenIgnoredDevices: (() -> Unit)? = null,
    val onIgnore: (PrivacyFinding) -> Unit = {},
    val onTrack: (PrivacyFinding) -> Unit = {},
    val onOpenDetails: (PrivacyFindingKey) -> Unit = {},
)

@Composable
fun PrivacyScreen(
    onOpenIgnoredDevices: (() -> Unit)? = null,
    onOpenInfo: (() -> Unit)? = null,
    onOpenFinding: ((PrivacyFindingKey) -> Unit)? = null,
    viewModel: PrivacyViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val directionState by viewModel.directionSweepState.collectAsStateWithLifecycle()
    val directionResult by viewModel.directionResultText.collectAsStateWithLifecycle()
    val permissionBindings = rememberPermissionBindings()
    var detailKey by remember { mutableStateOf<PrivacyFindingKey?>(null) }
    var trackingKey by remember { mutableStateOf<PrivacyFindingKey?>(null) }
    var pendingPermissionSource by remember { mutableStateOf<PrivacySourceKind?>(null) }
    var awaitingSettingsPermissionSource by remember {
        mutableStateOf<PrivacySourceKind?>(null)
    }
    val bluetoothSettingsLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        if (shouldRetryAfterBluetoothEnable(result.resultCode)) {
            viewModel.recover(PrivacySourceKind.PHONE_BLE)
        }
    }

    val awaitingSource = awaitingSettingsPermissionSource
    val awaitingFeature = awaitingSource?.let(::permissionFeatureForPrivacySource)
    val awaitingState = awaitingFeature?.let(permissionBindings::stateFor)
    LaunchedEffect(awaitingSource, awaitingState) {
        if (awaitingSource != null && awaitingState?.isUsable() == true) {
            viewModel.onPrivacyPermissionResolved(awaitingSource)
            awaitingSettingsPermissionSource = null
        }
    }

    DisposableEffect(viewModel) {
        onDispose { viewModel.cancelDirectionSweep() }
    }

    PrivacyContent(
        state = state,
        actions = PrivacyActions(
            onRecoverSource = viewModel::recover,
            onEnablePhoneScan = {
                val permissionState = permissionBindings.stateFor(
                    AppFeature.PHONE_PRIVACY_SCAN,
                )
                if (permissionState.isUsable()) {
                    viewModel.enablePhonePrivacyScanning()
                } else {
                    pendingPermissionSource = PrivacySourceKind.PHONE_BLE
                }
            },
            onResolveSourcePermission = { source ->
                permissionFeatureForPrivacySource(source)?.let { feature ->
                    if (permissionBindings.stateFor(feature).isUsable()) {
                        viewModel.onPrivacyPermissionResolved(source)
                    } else {
                        pendingPermissionSource = source
                    }
                } ?: viewModel.recover(source)
            },
            onTurnOnBluetooth = {
                bluetoothSettingsLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            },
            onQueryChanged = viewModel::updateQuery,
            onToggleCategory = viewModel::toggleCategory,
            onToggleSource = viewModel::toggleSource,
            onClearFilters = viewModel::clearFilters,
            onRetryAllSources = viewModel::retryAllFailed,
            onOpenBackendSettings = onOpenInfo,
            onOpenIgnoredDevices = onOpenIgnoredDevices,
            onIgnore = viewModel::ignore,
            onTrack = {
                if (viewModel.startDirectionSweep(it)) {
                    trackingKey = it.observationKey
                }
            },
            onOpenDetails = { key ->
                if (onOpenFinding != null) onOpenFinding(key) else detailKey = key
            },
        ),
    )

    pendingPermissionSource?.let { source ->
        val feature = requireNotNull(permissionFeatureForPrivacySource(source))
        val permissionState = permissionBindings.stateFor(feature)
        val canRequest = permissionState == PermissionUiState.Denied
        AlertDialog(
            onDismissRequest = { pendingPermissionSource = null },
            title = { Text(permissionTitle(feature)) },
            text = {
                Text(
                    if (canRequest) permissionExplanation(feature)
                    else permissionRecovery(feature, permissionState),
                )
            },
            confirmButton = {
                TextButton(
                    enabled = permissionState != PermissionUiState.Loading,
                    onClick = {
                        pendingPermissionSource = null
                        when {
                            permissionState.isUsable() -> {
                                viewModel.onPrivacyPermissionResolved(source)
                            }
                            canRequest -> {
                                permissionBindings.request(feature) { resolved ->
                                    if (resolved.isUsable()) {
                                        viewModel.onPrivacyPermissionResolved(source)
                                    }
                                }
                            }
                            else -> {
                                awaitingSettingsPermissionSource = source
                                permissionBindings.openSettings(feature)
                            }
                        }
                    },
                ) {
                    Text(if (canRequest) "Continue" else "Open settings")
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingPermissionSource = null }) { Text("Cancel") }
            },
        )
    }

    detailKey?.let { key ->
        val finding = state.visibleFindings.singleOrNull { it.routableKey == key }
        PrivacyFindingDetailDialog(
            finding = finding,
            onDismiss = { detailKey = null },
        )
    }
    trackingKey?.let { key ->
        val finding = state.visibleFindings.singleOrNull { it.observationKey == key }
        PrivacySignalSweepDialog(
            finding = finding,
            state = directionState,
            resultText = directionResult,
            onFinish = viewModel::finishDirectionSweep,
            onRetry = {
                finding?.let { current ->
                    if (!viewModel.startDirectionSweep(current)) trackingKey = null
                }
            },
            onDismiss = {
                viewModel.cancelDirectionSweep()
                trackingKey = null
            },
        )
    }
}

@Composable
fun PrivacyContent(
    state: PrivacyUiState,
    actions: PrivacyActions,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("privacy_content"),
        contentPadding = PaddingValues(bottom = 24.dp),
    ) {
        item {
            PrivacyHeader(state, actions.onOpenIgnoredDevices)
        }
        if (state.sourceSummaries.isNotEmpty()) {
            item {
                PrivacySourceHealthSummary(state.sourceSummaries, actions)
            }
        }
        if (state.lastUpdatedWallMs != null) {
            item {
                Text(
                    text = "Updated ${formatPrivacyWallTime(state.lastUpdatedWallMs)}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
        }
        item {
            PrivacySearchAndFilters(state, actions)
        }

        when (val body = state.body) {
            PrivacyBodyState.Loading -> item {
                FofLoadingState("Checking Phone, Backend, Badge, and Wi-Fi sources")
            }
            PrivacyBodyState.Empty -> item {
                FofEmptyState("No current findings")
            }
            is PrivacyBodyState.NoMatches -> item {
                FofNoMatchesState(body.activeFilterCount, actions.onClearFilters)
            }
            is PrivacyBodyState.RetryableFailure -> item {
                FofErrorState(
                    title = "Privacy sources unavailable",
                    detail = body.message,
                    actionLabel = "Retry",
                    onAction = actions.onRetryAllSources,
                )
            }
            is PrivacyBodyState.PermissionBlocked -> item {
                val blockedSource = state.sourceHealth.firstOrNull {
                    it.state == SourceHealthState.PERMISSION_BLOCKED
                }?.source ?: PrivacySourceKind.PHONE_BLE
                val appFeature = permissionFeatureForPrivacySource(blockedSource)
                FofErrorState(
                    title = "Permission needed",
                    detail = body.message,
                    actionLabel = if (appFeature == null) "Connect badge" else "Grant access",
                    onAction = {
                        if (appFeature == null) {
                            actions.onRecoverSource(blockedSource)
                        } else {
                            actions.onResolveSourcePermission(blockedSource)
                        }
                    },
                )
            }
            is PrivacyBodyState.Unsupported -> item {
                FofEmptyState(body.message)
            }
            is PrivacyBodyState.Stale -> item {
                FofEmptyState(
                    title = "Results are stale",
                    detail = body.message,
                    modifier = Modifier.fillMaxWidth().padding(24.dp),
                )
            }
            PrivacyBodyState.Content -> {
                PrivacySection.entries.forEach { section ->
                    val rows = state.visibleFindings.filter { it.section() == section }
                    if (rows.isNotEmpty()) {
                        item(key = "section_${section.name}") {
                            PrivacySectionStrip(section, rows.size)
                        }
                        items(
                            items = rows,
                            key = { it.observationKey.encoded },
                        ) { finding ->
                            PrivacyFindingRow(finding, actions)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PrivacyHeader(
    state: PrivacyUiState,
    onOpenIgnoredDevices: (() -> Unit)?,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 16.dp, bottom = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text("Privacy", style = MaterialTheme.typography.headlineSmall)
            Text(
                text = state.findingCountLabel,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (state.threatCount > 0) {
            Surface(
                shape = RoundedCornerShape(8.dp),
                color = MaterialTheme.colorScheme.error.copy(alpha = 0.10f),
            ) {
                Text(
                    text = "${state.threatCount} need attention",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 7.dp),
                )
            }
        }
        if (onOpenIgnoredDevices != null) {
            IconButton(
                onClick = onOpenIgnoredDevices,
                modifier = Modifier.size(48.dp),
            ) {
                Icon(Icons.Default.Block, contentDescription = "Ignored findings")
            }
        }
    }
}

@Composable
private fun PrivacySourceHealthSummary(
    summaries: List<PrivacySourceSummary>,
    actions: PrivacyActions,
) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
    ) {
        summaries.forEachIndexed { index, summary ->
            PrivacySourceStatusRow(summary, actions)
            if (index != summaries.lastIndex) {
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            }
        }
    }
}

@Composable
private fun PrivacySourceStatusRow(
    summary: PrivacySourceSummary,
    actions: PrivacyActions,
) {
    val color = sourceHealthColor(summary.state)
    Row(
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(8.dp).background(color, RoundedCornerShape(4.dp)))
        Spacer(Modifier.width(10.dp))
        Text(
            text = summary.group.label,
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.width(72.dp),
        )
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = summary.state.userLabel(),
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.SemiBold,
                color = color,
            )
            summary.message?.takeIf { it.isNotBlank() }?.let {
                Text(
                    text = it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        val action = when {
            summary.group == PrivacySourceGroup.PHONE &&
                summary.state == SourceHealthState.PAUSED -> {
                "Turn on" to actions.onEnablePhoneScan
            }
            summary.recoveryLabel == "Grant permission" -> {
                if (permissionFeatureForPrivacySource(summary.recoverySource) == null) {
                    "Connect badge" to {
                        actions.onRecoverSource(summary.recoverySource)
                    }
                } else {
                    "Grant access" to {
                        actions.onResolveSourcePermission(summary.recoverySource)
                    }
                }
            }
            summary.recoveryLabel == "Turn on Bluetooth" -> {
                "Turn on Bluetooth" to actions.onTurnOnBluetooth
            }
            summary.recoveryLabel == "Fix backend URL" -> {
                actions.onOpenBackendSettings?.let { "Open Info" to it }
            }
            summary.recoveryLabel.isNullOrBlank() -> null
            else -> {
                summary.recoveryLabel to {
                    actions.onRecoverSource(summary.recoverySource)
                }
            }
        }
        if (action != null) {
            TextButton(
                onClick = action.second,
                modifier = Modifier.heightIn(min = 48.dp),
            ) {
                Text(action.first)
            }
        }
    }
}

private fun permissionFeatureForPrivacySource(source: PrivacySourceKind): AppFeature? = when (source) {
    PrivacySourceKind.PHONE_BLE,
    PrivacySourceKind.WIFI_ANALYSIS,
    -> AppFeature.PHONE_PRIVACY_SCAN

    PrivacySourceKind.PHONE_ULTRASONIC -> AppFeature.ULTRASONIC
    else -> null
}

internal fun shouldRetryAfterBluetoothEnable(resultCode: Int): Boolean =
    resultCode == Activity.RESULT_OK

@Composable
private fun PrivacySearchAndFilters(
    state: PrivacyUiState,
    actions: PrivacyActions,
) {
    var categoriesOpen by remember { mutableStateOf(false) }
    var sourcesOpen by remember { mutableStateOf(false) }
    Column(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        OutlinedTextField(
            value = state.filters.query,
            onValueChange = actions.onQueryChanged,
            modifier = Modifier.fillMaxWidth().testTag("privacy_search"),
            singleLine = true,
            label = { Text("Search current findings") },
            leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
            trailingIcon = if (state.filters.query.isNotEmpty()) {
                {
                    IconButton(
                        onClick = { actions.onQueryChanged("") },
                        modifier = Modifier.size(48.dp),
                    ) {
                        Icon(Icons.Default.Close, contentDescription = "Clear search")
                    }
                }
            } else {
                null
            },
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box {
                FilterChip(
                    selected = state.filters.categories.isNotEmpty(),
                    onClick = { categoriesOpen = true },
                    label = {
                        Text(filterLabel("Categories", state.filters.categories.size))
                    },
                    leadingIcon = { Icon(Icons.Default.FilterList, contentDescription = null) },
                    modifier = Modifier.heightIn(min = 48.dp),
                )
                DropdownMenu(
                    expanded = categoriesOpen,
                    onDismissRequest = { categoriesOpen = false },
                ) {
                    state.availableCategories.forEach { category ->
                        DropdownMenuItem(
                            text = { Text(category.label) },
                            onClick = { actions.onToggleCategory(category) },
                            leadingIcon = {
                                Checkbox(
                                    checked = category in state.filters.categories,
                                    onCheckedChange = null,
                                )
                            },
                        )
                    }
                }
            }
            Box {
                FilterChip(
                    selected = state.filters.sources.isNotEmpty(),
                    onClick = { sourcesOpen = true },
                    label = { Text(filterLabel("Sources", state.filters.sources.size)) },
                    modifier = Modifier.heightIn(min = 48.dp),
                )
                DropdownMenu(
                    expanded = sourcesOpen,
                    onDismissRequest = { sourcesOpen = false },
                ) {
                    state.availableSources.forEach { source ->
                        DropdownMenuItem(
                            text = { Text(source.userLabel()) },
                            onClick = { actions.onToggleSource(source) },
                            leadingIcon = {
                                Checkbox(
                                    checked = source in state.filters.sources,
                                    onCheckedChange = null,
                                )
                            },
                        )
                    }
                }
            }
            if (state.filters.activeFilterCount > 0) {
                TextButton(
                    onClick = actions.onClearFilters,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) {
                    Text("Clear")
                }
            }
        }
    }
}

@Composable
private fun PrivacySectionStrip(section: PrivacySection, count: Int) {
    val color = sectionColor(section)
    Surface(
        modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
        color = color.copy(alpha = 0.10f),
        tonalElevation = 0.dp,
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = section.label,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.Bold,
                color = color,
            )
            Spacer(Modifier.weight(1f))
            Text(
                text = count.toString(),
                style = MaterialTheme.typography.labelMedium,
                color = color,
            )
        }
    }
}

@Composable
private fun PrivacyFindingRow(
    finding: PrivacyFinding,
    actions: PrivacyActions,
) {
    val accent = sectionColor(finding.section())
    val rowModifier = if (finding.routableKey != null) {
        Modifier.clickable { actions.onOpenDetails(requireNotNull(finding.routableKey)) }
    } else {
        Modifier
    }
    Row(
        modifier = rowModifier
            .fillMaxWidth()
            .testTag("finding_${finding.displayId}")
            .semantics {
                contentDescription = buildString {
                    append(finding.severity.userLabel())
                    append(", ")
                    append(finding.source.userLabel())
                    append(", ")
                    append(finding.title)
                }
            },
    ) {
        Box(Modifier.width(4.dp).heightIn(min = 136.dp).background(accent))
        Column(
            modifier = Modifier.weight(1f).padding(horizontal = 12.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            Row(verticalAlignment = Alignment.Top) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = finding.title,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = buildFindingMetadata(finding),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                finding.signalDbm?.let {
                    Text(
                        text = "$it dBm",
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(start = 8.dp),
                    )
                }
            }
            finding.evidence?.takeIf { it.isNotBlank() }?.let {
                Text(
                    text = it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            finding.limitation?.takeIf { it.isNotBlank() }?.let {
                Text(
                    text = it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                            RoundedCornerShape(8.dp),
                        )
                        .padding(8.dp),
                )
            }
            PrivacyFindingActions(finding, actions)
        }
    }
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
}

@Composable
private fun PrivacyFindingActions(
    finding: PrivacyFinding,
    actions: PrivacyActions,
) {
    if (!finding.capabilities.canIgnore &&
        !finding.capabilities.canTrack &&
        finding.routableKey == null
    ) {
        return
    }
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (finding.capabilities.canIgnore) {
            TextButton(
                onClick = { actions.onIgnore(finding) },
                modifier = Modifier.heightIn(min = 48.dp)
                    .testTag("finding_${finding.displayId}_ignore"),
            ) {
                Text("Ignore")
            }
        }
        if (finding.capabilities.canTrack) {
            TextButton(
                onClick = { actions.onTrack(finding) },
                modifier = Modifier.heightIn(min = 48.dp)
                    .testTag("finding_${finding.displayId}_track")
                    .semantics {
                        contentDescription = "Open RSSI direction sweep for ${finding.title}"
                    },
            ) {
                Icon(Icons.Default.MyLocation, contentDescription = null, Modifier.size(18.dp))
                Spacer(Modifier.width(6.dp))
                Text("Track")
            }
        }
        finding.routableKey?.let { key ->
            TextButton(
                onClick = { actions.onOpenDetails(key) },
                modifier = Modifier.heightIn(min = 48.dp)
                    .testTag("finding_${finding.displayId}_details"),
            ) {
                Text("Details")
            }
        }
    }
}

@Composable
private fun PrivacyFindingDetailDialog(
    finding: PrivacyFinding?,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(finding?.title ?: "Item no longer current") },
        text = {
            if (finding == null) {
                Text("This exact finding is no longer in the current list.")
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(buildFindingMetadata(finding), style = MaterialTheme.typography.labelMedium)
                    finding.evidence?.let { Text(it) }
                    finding.limitation?.let {
                        Text(it, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    finding.signalDbm?.let { Text("Signal: $it dBm") }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                Text("Close")
            }
        },
    )
}

@Composable
private fun PrivacySignalSweepDialog(
    finding: PrivacyFinding?,
    state: DirectionSweepState,
    resultText: String,
    onFinish: () -> Unit,
    onRetry: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("RSSI direction sweep") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(finding?.title ?: "Signal is no longer current")
                finding?.signalDbm?.let {
                    Text(
                        text = "$it dBm",
                        style = MaterialTheme.typography.headlineSmall,
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
                when (state) {
                    DirectionSweepState.Idle -> Text("Sweep stopped.")
                    is DirectionSweepState.Sampling -> {
                        Text(
                            "Turn slowly through a full circle while holding the phone steady. " +
                                "A less negative dBm value is stronger.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Text(
                            buildString {
                                state.currentDbm?.let { append("$it dBm  •  ") }
                                append("${state.sampleCount} of 40 samples")
                            },
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                    is DirectionSweepState.InsufficientSamples,
                    is DirectionSweepState.Complete -> Text(
                        resultText,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        confirmButton = {
            when (state) {
                is DirectionSweepState.Sampling -> {
                    TextButton(onClick = onFinish, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text("Finish")
                    }
                }
                is DirectionSweepState.InsufficientSamples -> {
                    TextButton(onClick = onRetry, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text("Try again")
                    }
                }
                DirectionSweepState.Idle,
                is DirectionSweepState.Complete -> {
                    TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text("Done")
                    }
                }
            }
        },
        dismissButton = {
            if (state is DirectionSweepState.Sampling ||
                state is DirectionSweepState.InsufficientSamples
            ) {
                TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text("Cancel")
                }
            }
        },
    )
}

private fun filterLabel(label: String, selected: Int): String =
    if (selected == 0) label else "$label · $selected"

private fun buildFindingMetadata(finding: PrivacyFinding): String = buildList {
    add(finding.severity.userLabel())
    add(finding.source.userLabel())
    add(finding.category.label)
    add(
        when (finding.freshness) {
            FindingFreshness.LIVE -> "Live"
            FindingFreshness.STALE -> "Stale"
            FindingFreshness.PAUSED_CACHED -> "Paused copy"
            FindingFreshness.EXPIRED -> "Expired"
        },
    )
    if (finding.ownership == Ownership.OWNED) add("Your device")
}.joinToString(" · ")

private fun FindingSeverity.userLabel(): String = when (this) {
    FindingSeverity.CRITICAL -> "Threat"
    FindingSeverity.AWARENESS -> "Awareness"
    FindingSeverity.NEARBY -> "Nearby"
    FindingSeverity.INFO -> "Info"
}

private fun SourceHealthState.userLabel(): String = when (this) {
    SourceHealthState.LOADING -> "Checking"
    SourceHealthState.LIVE -> "Live"
    SourceHealthState.STALE -> "Stale"
    SourceHealthState.PAUSED -> "Off"
    SourceHealthState.PERMISSION_BLOCKED -> "Permission needed"
    SourceHealthState.UNSUPPORTED -> "Unavailable"
    SourceHealthState.FAILED -> "Failed"
}

@Composable
private fun sourceHealthColor(state: SourceHealthState): Color = when (state) {
    SourceHealthState.LIVE -> MaterialTheme.colorScheme.secondary
    SourceHealthState.LOADING -> MaterialTheme.colorScheme.primary
    SourceHealthState.STALE,
    SourceHealthState.PAUSED -> MaterialTheme.colorScheme.tertiary
    SourceHealthState.PERMISSION_BLOCKED,
    SourceHealthState.FAILED -> MaterialTheme.colorScheme.error
    SourceHealthState.UNSUPPORTED -> MaterialTheme.colorScheme.outline
}

@Composable
private fun sectionColor(section: PrivacySection): Color = when (section) {
    PrivacySection.THREATS -> MaterialTheme.colorScheme.error
    PrivacySection.AWARENESS -> MaterialTheme.colorScheme.tertiary
    PrivacySection.NEARBY -> MaterialTheme.colorScheme.primary
    PrivacySection.INFO -> MaterialTheme.colorScheme.outline
}

private fun formatPrivacyWallTime(wallMs: Long): String = runCatching {
    DateTimeFormatter.ofPattern("h:mm a")
        .withZone(ZoneId.systemDefault())
        .format(Instant.ofEpochMilli(wallMs))
}.getOrDefault("recently")

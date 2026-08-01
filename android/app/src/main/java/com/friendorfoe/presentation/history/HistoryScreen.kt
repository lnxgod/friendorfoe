package com.friendorfoe.presentation.history

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.cleared
import com.friendorfoe.presentation.components.CollectionBodyState
import com.friendorfoe.presentation.components.FofConfirmationDialog
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofFailureState
import com.friendorfoe.presentation.components.FofLoadingState
import com.friendorfoe.presentation.components.FofNoMatchesState
import com.friendorfoe.presentation.components.FofScreenHeader
import com.friendorfoe.presentation.components.FofStaleBanner
import com.friendorfoe.presentation.filter.CompactFilterBar
import com.friendorfoe.presentation.filter.FilterModalSheet
import com.friendorfoe.presentation.util.categoryColor
import com.friendorfoe.presentation.util.isMilitary
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

internal const val HISTORY_RETENTION_COPY =
    "History may include observation and phone coordinates. Records stay on this device " +
        "until you delete them, clear History, clear app data, or uninstall."

@Suppress("UNUSED_PARAMETER")
@Composable
fun HistoryScreen(
    onEntryTapped: (Long) -> Unit,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
    viewModel: HistoryViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    var filtersOpen by rememberSaveable { mutableStateOf(false) }

    HistoryContent(
        state = state,
        actions = HistoryActions(
            onQueryChanged = { query ->
                viewModel.updateFilter(state.filter.copy(searchQuery = query))
            },
            onOpenFilters = { filtersOpen = true },
            onClearFilters = { viewModel.updateFilter(state.filter.cleared()) },
            onOpenRow = onEntryTapped,
            onRequestDelete = viewModel::requestDelete,
            onRequestClearAll = viewModel::requestClearAll,
            onRetry = viewModel::retryHistory,
            onDismissDeletion = viewModel::dismissDeletion,
            onConfirmDeletion = { viewModel.confirmDeletion() },
        ),
    )

    if (filtersOpen) {
        FilterModalSheet(
            filterState = state.filter,
            onFilterStateChange = viewModel::updateFilter,
            onDismiss = { filtersOpen = false },
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun HistoryContent(
    state: HistoryUiState,
    actions: HistoryActions,
) {
    val headerCount = when (state.body) {
        CollectionBodyState.Loading, is CollectionBodyState.Failed -> null
        else -> state.totalCount
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                FofScreenHeader(
                    title = "History",
                    count = headerCount,
                    countLabel = if (headerCount == 1) "detection" else "detections",
                )
            }
            TextButton(
                onClick = actions.onRequestClearAll,
                enabled = state.totalCount > 0 && !state.deletionInProgress,
                modifier = Modifier.heightIn(min = 48.dp).testTag("history_clear_all"),
            ) {
                Text("Clear all")
            }
        }

        Text(
            text = HISTORY_RETENTION_COPY,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
        )

        CompactFilterBar(
            filterState = state.filter,
            resultCount = headerCount?.let { visibleHistoryCount(state.body) },
            activeFilterCount = state.activeFilterCount,
            onQueryChanged = actions.onQueryChanged,
            onOpenFilters = actions.onOpenFilters,
            onClearFilters = actions.onClearFilters,
        )

        when (val body = state.body) {
            CollectionBodyState.Loading -> FofLoadingState("Loading History")
            CollectionBodyState.Empty -> EmptyHistoryState()
            is CollectionBodyState.Content -> HistoryRows(
                rows = body.rows,
                actions = actions,
            )
            is CollectionBodyState.Stale -> HistoryRows(
                rows = body.rows,
                actions = actions,
                staleMessage = body.message,
                staleAgeMs = body.ageMs,
            )
            is CollectionBodyState.NoMatches -> FofNoMatchesState(
                activeFilterCount = body.activeFilterCount,
                onClearFilters = actions.onClearFilters,
            )
            is CollectionBodyState.Failed -> FofFailureState(
                message = body.message,
                onRetry = actions.onRetry.takeIf { body.canRetry },
            )
        }
    }

    when (val pending = state.pendingDeletion) {
        is PendingHistoryDeletion.Row -> FofConfirmationDialog(
            title = "Delete ${pending.label}?",
            message = "This permanently removes this detection from on-device history.",
            confirmLabel = when {
                state.deletionInProgress -> "Deleting…"
                state.deletionError != null -> "Retry"
                else -> "Delete"
            },
            onConfirm = actions.onConfirmDeletion,
            onDismiss = actions.onDismissDeletion,
            inProgress = state.deletionInProgress,
            error = state.deletionError,
            confirmTag = "history_confirm_delete_row",
            dismissTag = "history_cancel_delete",
        )
        PendingHistoryDeletion.All -> FofConfirmationDialog(
            title = "Clear all history?",
            message = "This permanently removes every detection stored on this device.",
            confirmLabel = when {
                state.deletionInProgress -> "Clearing…"
                state.deletionError != null -> "Retry"
                else -> "Clear all"
            },
            onConfirm = actions.onConfirmDeletion,
            onDismiss = actions.onDismissDeletion,
            inProgress = state.deletionInProgress,
            error = state.deletionError,
            confirmTag = "history_confirm_clear_all",
            dismissTag = "history_cancel_delete",
        )
        null -> Unit
    }
}

@Suppress("UNUSED_PARAMETER")
@Composable
internal fun HistoryContent(
    state: HistoryUiState,
    onFilterChanged: (FilterState) -> Unit,
    onEntryTapped: (Long) -> Unit,
    onRequestDelete: (HistoryEntity) -> Unit,
    onRequestClearAll: () -> Unit,
    onDismissDeletion: () -> Unit,
    onConfirmDeletion: () -> Unit,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
) {
    HistoryContent(
        state = state,
        actions = HistoryActions(
            onQueryChanged = { onFilterChanged(state.filter.copy(searchQuery = it)) },
            onOpenFilters = {
                onFilterChanged(state.filter.copy(isAdvancedExpanded = true))
            },
            onClearFilters = { onFilterChanged(state.filter.cleared()) },
            onOpenRow = onEntryTapped,
            onRequestDelete = onRequestDelete,
            onRequestClearAll = onRequestClearAll,
            onDismissDeletion = onDismissDeletion,
            onConfirmDeletion = onConfirmDeletion,
        ),
    )
    if (state.filter.isAdvancedExpanded) {
        FilterModalSheet(
            filterState = state.filter,
            onFilterStateChange = onFilterChanged,
            onDismiss = {
                onFilterChanged(state.filter.copy(isAdvancedExpanded = false))
            },
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun HistoryRows(
    rows: List<HistoryEntity>,
    actions: HistoryActions,
    staleMessage: String? = null,
    staleAgeMs: Long? = null,
) {
    val grouped = groupRowsByDate(rows)
    LazyColumn(modifier = Modifier.fillMaxSize().testTag("history_results")) {
        if (staleMessage != null) {
            item(key = "stale") {
                FofStaleBanner(
                    message = staleMessage,
                    ageMs = staleAgeMs,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
        }
        grouped.forEach { (dateLabel, entries) ->
            stickyHeader(key = dateLabel) { DateGroupHeader(dateLabel) }
            items(items = entries, key = HistoryEntity::id) { entry ->
                HistoryItem(
                    entry = entry,
                    onClick = { actions.onOpenRow(entry.id) },
                    onDelete = { actions.onRequestDelete(entry) },
                )
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outlineVariant,
                    thickness = 0.5.dp,
                )
            }
        }
    }
}

@Composable
private fun EmptyHistoryState() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        FofEmptyState(
            title = "No saved detections",
            detail = "Detections you keep will appear here.",
        )
    }
}

private fun visibleHistoryCount(body: CollectionBodyState<HistoryEntity>): Int = when (body) {
    is CollectionBodyState.Content -> body.rows.size
    is CollectionBodyState.Stale -> body.rows.size
    else -> 0
}

private fun groupRowsByDate(entries: List<HistoryEntity>): Map<String, List<HistoryEntity>> {
    val zone = ZoneId.systemDefault()
    val today = LocalDate.now(zone)
    val yesterday = today.minusDays(1)
    val dateFormatter = DateTimeFormatter.ofPattern("MMMM d, yyyy", Locale.getDefault())
    val grouped = linkedMapOf<String, MutableList<HistoryEntity>>()
    entries.forEach { entry ->
        val date = Instant.ofEpochMilli(entry.lastSeen).atZone(zone).toLocalDate()
        val label = when (date) {
            today -> "Today"
            yesterday -> "Yesterday"
            else -> date.format(dateFormatter)
        }
        grouped.getOrPut(label) { mutableListOf() }.add(entry)
    }
    return grouped
}

@Composable
private fun DateGroupHeader(dateLabel: String) {
    Surface(modifier = Modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.surfaceVariant) {
        Text(
            text = dateLabel,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun HistoryItem(entry: HistoryEntity, onClick: () -> Unit, onDelete: () -> Unit) {
    val rowBackground = when {
        isMilitary(entry.category) -> Modifier.background(Color(0xFFF44336).copy(alpha = 0.08f))
        entry.category.equals("emergency", ignoreCase = true) ->
            Modifier.background(Color(0xFFE91E63).copy(alpha = 0.10f))
        else -> Modifier
    }

    Row(
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).then(rowBackground)
            .clickable(onClick = onClick).testTag("history_row_${entry.id}")
            .padding(start = 16.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(12.dp).clip(CircleShape).background(categoryColor(entry.category)))
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = entry.displayName,
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.Medium,
            )
            if (!entry.description.isNullOrBlank()) {
                Text(
                    text = entry.description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                text = historyCategoryLabel(entry.category),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(
                text = formatTime(entry.lastSeen),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            DetectionSourceBadge(entry.detectionSource)
        }
        IconButton(
            onClick = onDelete,
            modifier = Modifier.size(48.dp).testTag("history_delete_${entry.id}"),
        ) {
            Icon(Icons.Default.DeleteOutline, contentDescription = "Delete ${entry.displayName}")
        }
    }
}

@Composable
private fun DetectionSourceBadge(source: String) {
    val (icon, label) = detectionSourceInfo(source)
    Surface(shape = RoundedCornerShape(4.dp), color = MaterialTheme.colorScheme.surfaceVariant) {
        Row(
            Modifier.padding(horizontal = 4.dp, vertical = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(12.dp))
            Spacer(Modifier.width(2.dp))
            Text(label, style = MaterialTheme.typography.labelSmall)
        }
    }
}

private fun formatTime(epochMillis: Long): String = Instant.ofEpochMilli(epochMillis)
    .atZone(ZoneId.systemDefault())
    .format(DateTimeFormatter.ofPattern("h:mm a", Locale.getDefault()))

internal fun detectionSourceLabel(source: String): String = when (source.lowercase()) {
    "ads_b" -> "ADS-B"
    "remote_id" -> "Remote ID"
    "wifi_nan", "wifi_beacon" -> "Remote ID · Wi-Fi"
    "wifi" -> "Phone"
    "configured_backend" -> "Configured backend"
    else -> source
}

internal fun historyCategoryLabel(category: String): String = when (category.lowercase()) {
    "commercial" -> "Commercial"
    "general_aviation" -> "General aviation"
    "military" -> "Military"
    "helicopter" -> "Helicopter"
    "government" -> "Government"
    "emergency" -> "Emergency"
    "cargo" -> "Cargo"
    "drone" -> "Drone"
    "ground_vehicle" -> "Ground vehicle"
    "unknown" -> "Unknown"
    else -> category.replace('_', ' ').replaceFirstChar { it.titlecase(Locale.getDefault()) }
}

private fun detectionSourceInfo(source: String): Pair<ImageVector, String> = when (source.lowercase()) {
    "ads_b", "configured_backend" -> Icons.Default.CellTower to detectionSourceLabel(source)
    "remote_id" -> Icons.Default.Bluetooth to detectionSourceLabel(source)
    "wifi_nan", "wifi_beacon", "wifi" -> Icons.Default.Wifi to detectionSourceLabel(source)
    else -> Icons.Default.CellTower to source
}

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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.presentation.components.CollectionBodyState
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofFailureState
import com.friendorfoe.presentation.components.FofLoadingState
import com.friendorfoe.presentation.components.FofNoMatchesState
import com.friendorfoe.presentation.components.FofScreenHeader
import com.friendorfoe.presentation.filter.FilterBar
import com.friendorfoe.presentation.util.categoryColor
import com.friendorfoe.presentation.util.isMilitary
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

@Composable
fun HistoryScreen(
    onEntryTapped: (Long) -> Unit,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
    viewModel: HistoryViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()

    HistoryContent(
        state = state,
        onFilterChanged = viewModel::updateFilter,
        onEntryTapped = onEntryTapped,
        onRequestDelete = viewModel::requestDelete,
        onRequestClearAll = viewModel::requestClearAll,
        onDismissDeletion = viewModel::dismissDeletion,
        onConfirmDeletion = viewModel::confirmDeletion,
        onNavigateToReferenceGuide = onNavigateToReferenceGuide,
        onNavigateToAbout = onNavigateToAbout,
    )
}

@OptIn(ExperimentalFoundationApi::class)
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
    Column(modifier = Modifier.fillMaxSize()) {
        FilterBar(
            filterState = state.filter,
            onFilterStateChange = onFilterChanged,
            resultCount = (state.body as? CollectionBodyState.Content)?.rows?.size ?: 0,
            onNavigateToReferenceGuide = onNavigateToReferenceGuide,
            onNavigateToAbout = onNavigateToAbout,
        )

        Column(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
        ) {
            FofScreenHeader(
                title = "History",
                count = state.totalCount,
                countLabel = if (state.totalCount == 1) "detection" else "detections",
            )
            TextButton(
                onClick = onRequestClearAll,
                enabled = state.totalCount > 0,
                modifier = Modifier.align(Alignment.End).testTag("history_clear_all"),
            ) { Text("Clear all") }
        }

        Text(
            text = "History stays on this device until you delete it or clear app data.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        )

        when (val body = state.body) {
            CollectionBodyState.Loading -> FofLoadingState("Loading history")
            CollectionBodyState.Empty -> EmptyHistoryState()
            is CollectionBodyState.Content -> {
                val grouped = groupRowsByDate(body.rows)
                LazyColumn(modifier = Modifier.fillMaxSize()) {
                    grouped.forEach { (dateLabel, entries) ->
                        stickyHeader(key = dateLabel) { DateGroupHeader(dateLabel) }
                        items(items = entries, key = HistoryEntity::id) { entry ->
                            HistoryItem(
                                entry = entry,
                                onClick = { onEntryTapped(entry.id) },
                                onDelete = { onRequestDelete(entry) },
                            )
                            HorizontalDivider(
                                color = MaterialTheme.colorScheme.outlineVariant,
                                thickness = 0.5.dp,
                            )
                        }
                    }
                }
            }
            is CollectionBodyState.Stale -> {
                val grouped = groupRowsByDate(body.rows)
                LazyColumn(modifier = Modifier.fillMaxSize()) {
                    item { Text(body.message, Modifier.padding(16.dp)) }
                    grouped.forEach { (dateLabel, entries) ->
                        stickyHeader(key = dateLabel) { DateGroupHeader(dateLabel) }
                        items(items = entries, key = HistoryEntity::id) { entry ->
                            HistoryItem(
                                entry = entry,
                                onClick = { onEntryTapped(entry.id) },
                                onDelete = { onRequestDelete(entry) },
                            )
                        }
                    }
                }
            }
            is CollectionBodyState.NoMatches -> FofNoMatchesState(
                activeFilterCount = body.activeFilterCount,
                onClearFilters = { onFilterChanged(FilterState()) },
            )
            is CollectionBodyState.Failed -> FofFailureState(body.message)
        }
    }

    when (val pending = state.pendingDeletion) {
        is PendingHistoryDeletion.Row -> AlertDialog(
            onDismissRequest = onDismissDeletion,
            title = { Text("Delete ${pending.label}?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("This permanently removes this detection from on-device history.")
                    state.deletionError?.let { error ->
                        Text(error, color = MaterialTheme.colorScheme.error)
                    }
                }
            },
            dismissButton = {
                TextButton(
                    onClick = onDismissDeletion,
                    enabled = !state.deletionInProgress,
                    modifier = Modifier.testTag("history_cancel_delete"),
                ) {
                    Text("Cancel")
                }
            },
            confirmButton = {
                TextButton(
                    onClick = onConfirmDeletion,
                    enabled = !state.deletionInProgress,
                    modifier = Modifier.testTag("history_confirm_delete_row"),
                ) {
                    Text(
                        when {
                            state.deletionInProgress -> "Deleting…"
                            state.deletionError != null -> "Retry"
                            else -> "Delete"
                        },
                    )
                }
            },
        )
        PendingHistoryDeletion.All -> AlertDialog(
            onDismissRequest = onDismissDeletion,
            title = { Text("Clear all history?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("This permanently removes every detection stored on this device.")
                    state.deletionError?.let { error ->
                        Text(error, color = MaterialTheme.colorScheme.error)
                    }
                }
            },
            dismissButton = {
                TextButton(
                    onClick = onDismissDeletion,
                    enabled = !state.deletionInProgress,
                    modifier = Modifier.testTag("history_cancel_delete"),
                ) {
                    Text("Cancel")
                }
            },
            confirmButton = {
                TextButton(
                    onClick = onConfirmDeletion,
                    enabled = !state.deletionInProgress,
                    modifier = Modifier.testTag("history_confirm_clear_all"),
                ) {
                    Text(
                        when {
                            state.deletionInProgress -> "Clearing…"
                            state.deletionError != null -> "Retry"
                            else -> "Clear all"
                        },
                    )
                }
            },
        )
        null -> Unit
    }
}

@Composable
private fun EmptyHistoryState() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        FofEmptyState(
            title = "No detections recorded yet",
            detail = "Past aircraft and drone detections will appear here",
        )
    }
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
        modifier = Modifier.fillMaxWidth().then(rowBackground).clickable(onClick = onClick)
            .testTag("history_row_${entry.id}").padding(start = 16.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(12.dp).clip(CircleShape).background(categoryColor(entry.category)))
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                entry.displayName,
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (!entry.description.isNullOrBlank()) {
                Text(
                    entry.description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(
                formatTime(entry.lastSeen),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            DetectionSourceBadge(entry.detectionSource)
        }
        IconButton(
            onClick = onDelete,
            modifier = Modifier.testTag("history_delete_${entry.id}"),
        ) {
            Icon(Icons.Default.DeleteOutline, contentDescription = "Delete ${entry.displayName}")
        }
    }
}

@Composable
private fun DetectionSourceBadge(source: String) {
    val (icon, label) = detectionSourceInfo(source)
    Surface(shape = RoundedCornerShape(4.dp), color = MaterialTheme.colorScheme.surfaceVariant) {
        Row(Modifier.padding(horizontal = 4.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, contentDescription = label, modifier = Modifier.size(12.dp))
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
    "wifi_nan" -> "WiFi NaN"
    "wifi_beacon" -> "WiFi Beacon"
    "wifi" -> "WiFi"
    else -> source
}

private fun detectionSourceInfo(source: String): Pair<ImageVector, String> = when (source.lowercase()) {
    "ads_b" -> Icons.Default.CellTower to detectionSourceLabel(source)
    "remote_id" -> Icons.Default.Bluetooth to detectionSourceLabel(source)
    "wifi_nan", "wifi_beacon", "wifi" -> Icons.Default.Wifi to detectionSourceLabel(source)
    else -> Icons.Default.CellTower to source
}

package com.friendorfoe.presentation.map

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.domain.model.cleared
import com.friendorfoe.presentation.filter.FilterModalSheet
import com.friendorfoe.presentation.filter.FilterSearchField

/** Keep the canvas prominent while making active restrictions visible. */
@Composable
internal fun MapWorkspaceControls(
    filter: FilterState,
    resultCount: Int,
    onFilterChange: (FilterState) -> Unit,
) {
    var searchOpen by rememberSaveable { mutableStateOf(filter.searchQuery.isNotBlank()) }
    var filtersOpen by rememberSaveable { mutableStateOf(false) }
    val filterCount = activeFilterCount(filter)
    val focus = LocalFocusManager.current
    Column(Modifier.fillMaxWidth().testTag("map_workspace_controls")) {
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Map", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
                Text(if (resultCount == 1) "1 result" else "$resultCount results",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = { searchOpen = !searchOpen; if (!searchOpen) focus.clearFocus() },
                modifier = Modifier.testTag("map_search_toggle")) {
                Icon(if (searchOpen) Icons.Default.Close else Icons.Default.Search,
                    contentDescription = if (searchOpen) "Hide search" else "Search map")
            }
            IconButton(onClick = { focus.clearFocus(); filtersOpen = true }, modifier = Modifier.testTag("filter_open")) {
                Icon(Icons.Default.FilterList,
                    tint = if (filterCount > 0) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                    contentDescription = if (filterCount == 0) "Filters" else "Filters, $filterCount active")
            }
        }
        AnimatedVisibility(searchOpen) {
            FilterSearchField(filter.searchQuery, { onFilterChange(filter.copy(searchQuery = it)) },
                Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        }
        if (filterCount > 0) {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(if (filterCount == 1) "1 filter active" else "$filterCount filters active",
                    Modifier.weight(1f), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                TextButton(onClick = { onFilterChange(filter.cleared()) }, modifier = Modifier.testTag("filter_clear")) {
                    Text("Clear filters")
                }
            }
        }
    }
    if (filtersOpen) {
        FilterModalSheet(filter, onFilterChange, onDismiss = { filtersOpen = false })
    }
}

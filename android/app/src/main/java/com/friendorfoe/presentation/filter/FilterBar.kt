package com.friendorfoe.presentation.filter

import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material3.AssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.domain.model.cleared

@Suppress("UNUSED_PARAMETER")
@Composable
fun FilterBar(
    filterState: FilterState,
    onFilterStateChange: (FilterState) -> Unit,
    resultCount: Int,
    modifier: Modifier = Modifier,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
) {
    var filtersOpen by rememberSaveable { mutableStateOf(filterState.isAdvancedExpanded) }
    val filterCount = activeFilterCount(filterState)

    CompactFilterBar(
        filterState = filterState,
        resultCount = resultCount,
        activeFilterCount = filterCount,
        onQueryChanged = { onFilterStateChange(filterState.copy(searchQuery = it)) },
        onOpenFilters = {
            filtersOpen = true
            if (!filterState.isAdvancedExpanded) {
                onFilterStateChange(filterState.copy(isAdvancedExpanded = true))
            }
        },
        onClearFilters = { onFilterStateChange(filterState.cleared()) },
        modifier = modifier,
    )

    if (filtersOpen) {
        FilterModalSheet(
            filterState = filterState,
            onFilterStateChange = onFilterStateChange,
            onDismiss = {
                filtersOpen = false
                if (filterState.isAdvancedExpanded) {
                    onFilterStateChange(filterState.copy(isAdvancedExpanded = false))
                }
            },
        )
    }
}

@Composable
fun CompactFilterBar(
    filterState: FilterState,
    resultCount: Int?,
    activeFilterCount: Int,
    onQueryChanged: (String) -> Unit,
    onOpenFilters: () -> Unit,
    onClearFilters: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        FilterSearchField(
            query = filterState.searchQuery,
            onQueryChange = onQueryChanged,
        )
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (resultCount != null) {
                Text(
                    text = if (resultCount == 1) "1 result" else "$resultCount results",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.weight(1f))
            if (activeFilterCount > 0) {
                TextButton(
                    onClick = onClearFilters,
                    modifier = Modifier.heightIn(min = 48.dp).testTag("filter_clear"),
                ) {
                    Text("Clear filters")
                }
            }
            AssistChip(
                onClick = onOpenFilters,
                label = {
                    Text(if (activeFilterCount == 0) "Filters" else "Filters $activeFilterCount")
                },
                leadingIcon = {
                    Icon(Icons.Default.FilterList, contentDescription = null)
                },
                modifier = Modifier.heightIn(min = 48.dp).testTag("filter_open"),
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FilterModalSheet(
    filterState: FilterState,
    onFilterStateChange: (FilterState) -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
    ) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState())) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Filters", style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.weight(1f))
                if (activeFilterCount(filterState) > 0) {
                    TextButton(
                        onClick = { onFilterStateChange(filterState.cleared()) },
                        modifier = Modifier.heightIn(min = 48.dp),
                    ) {
                        Text("Clear filters")
                    }
                }
            }
            HorizontalDivider()
            Text(
                text = "Category",
                style = MaterialTheme.typography.titleSmall,
                modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp),
            )
            FilterCategoryChips(
                selectedCategories = filterState.selectedCategories,
                onToggleCategory = { category ->
                    val categories = if (category in filterState.selectedCategories) {
                        filterState.selectedCategories - category
                    } else {
                        filterState.selectedCategories + category
                    }
                    onFilterStateChange(filterState.copy(selectedCategories = categories))
                },
            )
            FilterAdvancedSection(
                filterState = filterState.copy(isAdvancedExpanded = true),
                onFilterStateChange = onFilterStateChange,
                alwaysVisible = true,
                showClearAction = false,
            )
            Spacer(Modifier.height(24.dp))
        }
    }
}

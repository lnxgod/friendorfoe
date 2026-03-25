package com.friendorfoe.presentation.filter

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory

@Composable
fun FilterBar(
    filterState: FilterState,
    onFilterStateChange: (FilterState) -> Unit,
    resultCount: Int,
    modifier: Modifier = Modifier,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null
) {
    Column(modifier = modifier.fillMaxWidth()) {
        FilterSearchField(
            query = filterState.searchQuery,
            onQueryChange = { onFilterStateChange(filterState.copy(searchQuery = it)) }
        )

        FilterCategoryChips(
            selectedCategories = filterState.selectedCategories,
            onToggleCategory = { category ->
                val newCategories = if (category in filterState.selectedCategories) {
                    filterState.selectedCategories - category
                } else {
                    filterState.selectedCategories + category
                }
                onFilterStateChange(filterState.copy(selectedCategories = newCategories))
            }
        )

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "$resultCount results",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.weight(1f))
            Row(
                modifier = Modifier.clickable {
                    onFilterStateChange(
                        filterState.copy(isAdvancedExpanded = !filterState.isAdvancedExpanded)
                    )
                },
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Default.FilterList,
                    contentDescription = "Filters",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(modifier = Modifier.width(4.dp))
                Text(
                    text = "Filters",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            if (onNavigateToReferenceGuide != null || onNavigateToAbout != null) {
                var menuExpanded by remember { mutableStateOf(false) }
                Spacer(modifier = Modifier.width(4.dp))
                IconButton(onClick = { menuExpanded = true }) {
                    Icon(
                        imageVector = Icons.Default.MoreVert,
                        contentDescription = "More options",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                DropdownMenu(
                    expanded = menuExpanded,
                    onDismissRequest = { menuExpanded = false }
                ) {
                    onNavigateToReferenceGuide?.let { navigate ->
                        DropdownMenuItem(
                            text = { Text("Reference Guide") },
                            onClick = { menuExpanded = false; navigate() }
                        )
                    }
                    onNavigateToAbout?.let { navigate ->
                        DropdownMenuItem(
                            text = { Text("About") },
                            onClick = { menuExpanded = false; navigate() }
                        )
                    }
                }
            }
        }

        FilterAdvancedSection(
            filterState = filterState,
            onFilterStateChange = onFilterStateChange
        )
    }
}

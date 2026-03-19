package com.friendorfoe.presentation.filter

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.presentation.util.categoryColor

private val CHIP_LABELS = mapOf(
    ObjectCategory.COMMERCIAL to "Commercial",
    ObjectCategory.GENERAL_AVIATION to "GA",
    ObjectCategory.MILITARY to "Military",
    ObjectCategory.HELICOPTER to "Heli",
    ObjectCategory.GOVERNMENT to "Gov",
    ObjectCategory.EMERGENCY to "Emergency",
    ObjectCategory.CARGO to "Cargo",
    ObjectCategory.DRONE to "Drone",
    ObjectCategory.GROUND_VEHICLE to "Ground",
    ObjectCategory.UNKNOWN to "Unknown"
)

@Composable
fun FilterCategoryChips(
    selectedCategories: Set<ObjectCategory>,
    onToggleCategory: (ObjectCategory) -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        for ((category, label) in CHIP_LABELS) {
            val selected = category in selectedCategories
            val color = categoryColor(category)
            FilterChip(
                selected = selected,
                onClick = { onToggleCategory(category) },
                label = { Text(label) },
                colors = FilterChipDefaults.filterChipColors(
                    selectedContainerColor = color.copy(alpha = 0.2f),
                    selectedLabelColor = color
                )
            )
        }
    }
}

package com.friendorfoe.presentation.filter

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.FilterChip
import androidx.compose.material3.RangeSlider
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectTypeFilter
import com.friendorfoe.domain.model.SourceFilterGroup
import kotlin.math.roundToInt

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun FilterAdvancedSection(
    filterState: FilterState,
    onFilterStateChange: (FilterState) -> Unit,
    modifier: Modifier = Modifier
) {
    AnimatedVisibility(
        visible = filterState.isAdvancedExpanded,
        enter = expandVertically(),
        exit = shrinkVertically(),
        modifier = modifier
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp)
        ) {
            // Object type chips
            Text("Type", modifier = Modifier.padding(bottom = 4.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = filterState.objectTypeFilter == ObjectTypeFilter.AIRCRAFT,
                    onClick = {
                        val newType = if (filterState.objectTypeFilter == ObjectTypeFilter.AIRCRAFT) null
                            else ObjectTypeFilter.AIRCRAFT
                        onFilterStateChange(filterState.copy(objectTypeFilter = newType))
                    },
                    label = { Text("Aircraft") }
                )
                FilterChip(
                    selected = filterState.objectTypeFilter == ObjectTypeFilter.DRONE,
                    onClick = {
                        val newType = if (filterState.objectTypeFilter == ObjectTypeFilter.DRONE) null
                            else ObjectTypeFilter.DRONE
                        onFilterStateChange(filterState.copy(objectTypeFilter = newType))
                    },
                    label = { Text("Drone") }
                )
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Source chips
            Text("Source", modifier = Modifier.padding(bottom = 4.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (group in SourceFilterGroup.entries) {
                    val label = when (group) {
                        SourceFilterGroup.ADS_B -> "ADS-B"
                        SourceFilterGroup.REMOTE_ID -> "Remote ID"
                        SourceFilterGroup.WIFI -> "WiFi"
                    }
                    val selected = group in filterState.selectedSources
                    FilterChip(
                        selected = selected,
                        onClick = {
                            val newSources = if (selected) filterState.selectedSources - group
                                else filterState.selectedSources + group
                            onFilterStateChange(filterState.copy(selectedSources = newSources))
                        },
                        label = { Text(label) }
                    )
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Distance slider
            val distanceValue = filterState.maxDistanceNm ?: 100f
            Text("Max Distance: ${if (filterState.maxDistanceNm != null) "${distanceValue.roundToInt()} NM" else "No limit"}")
            Slider(
                value = distanceValue,
                onValueChange = { value ->
                    val snapped = (value / 5f).roundToInt() * 5f
                    onFilterStateChange(filterState.copy(maxDistanceNm = if (snapped >= 100f) null else snapped))
                },
                valueRange = 0f..100f,
                modifier = Modifier.fillMaxWidth()
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Altitude range slider
            val minAlt = (filterState.minAltitudeFt ?: 0).toFloat()
            val maxAlt = (filterState.maxAltitudeFt ?: 50000).toFloat()
            Text("Altitude: ${minAlt.roundToInt()}ft - ${if (filterState.maxAltitudeFt != null) "${maxAlt.roundToInt()}ft" else "No limit"}")
            RangeSlider(
                value = minAlt..maxAlt,
                onValueChange = { range ->
                    val snappedMin = (range.start / 1000f).roundToInt() * 1000
                    val snappedMax = (range.endInclusive / 1000f).roundToInt() * 1000
                    onFilterStateChange(
                        filterState.copy(
                            minAltitudeFt = if (snappedMin <= 0) null else snappedMin,
                            maxAltitudeFt = if (snappedMax >= 50000) null else snappedMax
                        )
                    )
                },
                valueRange = 0f..50000f,
                modifier = Modifier.fillMaxWidth()
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Clear All button
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically
            ) {
                TextButton(onClick = {
                    onFilterStateChange(FilterState())
                }) {
                    Text("Clear All")
                }
            }
        }
    }
}

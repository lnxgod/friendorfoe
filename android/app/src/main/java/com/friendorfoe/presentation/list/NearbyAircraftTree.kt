package com.friendorfoe.presentation.list

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.presentation.components.FofStaleBanner

@Composable
internal fun ListRows(
    rows: List<SkyObject>,
    actions: ListActions,
    activeVisualFocusIds: Set<String>,
    rangeMiles: Int,
    groupAircraftByType: Boolean,
    nowMs: Long,
    filter: FilterState,
    staleMessage: String? = null,
    staleAgeMs: Long? = null,
) {
    val sections = remember(rows, rangeMiles) { groupNearbyAircraft(rows, rangeMiles) }
    // Range changes reclassify the same observations without waiting for another scan.
    // Live updates preserve manual expansion; search opens every matching branch.
    var toggled by rememberSaveable(filter) { mutableStateOf(emptyList<String>()) }
    val searching = activeFilterCount(filter) > 0
    fun expanded(key: String, section: NearbyDistanceSection): Boolean =
        (searching || section != NearbyDistanceSection.FARTHER_AWAY) != (key in toggled)
    fun toggle(key: String) { toggled = if (key in toggled) toggled - key else toggled + key }

    LazyColumn(Modifier.fillMaxSize().testTag("list_results"), contentPadding = PaddingValues(bottom = 24.dp)) {
        if (staleMessage != null) item(key = "stale") {
            FofStaleBanner(staleMessage, staleAgeMs, Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        }
        sections.forEach { branch ->
            val section = branch.section
            val sectionKey = "nearby_section_${section.key}"
            item(key = sectionKey, contentType = "distance_section") {
                NearbyBranch(
                    label = section.label(rangeMiles), count = branch.rows.size, key = sectionKey,
                    description = when (section) {
                        NearbyDistanceSection.WITHIN_RANGE -> if (groupAircraftByType) "Groups and aircraft ordered by nearest distance" else "Closest detections first"
                        NearbyDistanceSection.FARTHER_AWAY -> "Beyond $rangeMiles mi · tap to explore"
                        NearbyDistanceSection.UNKNOWN -> "Proximity unconfirmed"
                    },
                    expanded = expanded(sectionKey, section), nested = false, onClick = { toggle(sectionKey) },
                )
            }
            if (expanded(sectionKey, section)) {
                if (branch.rows.isEmpty()) item(key = "no_nearby") {
                    Text("No detections within $rangeMiles mi", style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(20.dp))
                }
                if (groupAircraftByType) {
                    branch.groups.forEach { group ->
                        val groupKey = "nearby_type_${section.key}_${group.type.name}"
                        item(key = groupKey, contentType = "aircraft_type") {
                            NearbyBranch(
                                label = group.type.label, count = group.rows.size, key = groupKey,
                                description = if (group.nearestDistance.isFinite()) "Nearest ${listDistanceLabel(group.nearestDistance)}" else "Distance unavailable",
                                expanded = expanded(groupKey, section), nested = true, onClick = { toggle(groupKey) },
                            )
                        }
                        if (expanded(groupKey, section)) aircraftRows(group.rows, actions, activeVisualFocusIds, nowMs, nested = true)
                    }
                } else {
                    aircraftRows(branch.rows, actions, activeVisualFocusIds, nowMs, nested = false)
                }
            }
        }
    }
}

private fun LazyListScope.aircraftRows(
    rows: List<SkyObject>,
    actions: ListActions,
    focusIds: Set<String>,
    nowMs: Long,
    nested: Boolean,
) {
    items(rows, key = SkyObject::id, contentType = { "aircraft" }) { row ->
        Column(Modifier.padding(start = if (nested) 16.dp else 0.dp)) {
            SkyObjectItem(row, row.id in focusIds, nowMs) { actions.onOpenPeek(row) }
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant, thickness = 0.5.dp)
        }
    }
}

@Composable
private fun NearbyBranch(
    label: String,
    count: Int,
    description: String,
    key: String,
    expanded: Boolean,
    nested: Boolean,
    onClick: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth()
            .background(if (nested) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f))
            .heightIn(min = 64.dp).testTag(key)
            .semantics { stateDescription = if (expanded) "Expanded" else "Collapsed" }
            .clickable(role = Role.Button, onClickLabel = "${if (expanded) "Collapse" else "Expand"} $label", onClick = onClick)
            .padding(start = if (nested) 32.dp else 20.dp, end = 20.dp, top = 12.dp, bottom = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text(count.toString(), style = MaterialTheme.typography.labelLarge)
        Icon(if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore, contentDescription = null)
    }
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant, thickness = 0.5.dp)
}

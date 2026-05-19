package com.friendorfoe.presentation.badge

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeDisplayClassPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicyClasses

@Composable
fun BadgeDisplayFiltersSection(
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    policy: BadgeDisplayPolicy,
    displayPolicyHash: Long,
    filteredCounts: Map<String, Int>,
    onPolicyChange: (BadgeDisplayPolicy) -> Unit,
    onApply: () -> Unit,
    onReset: () -> Unit,
    onRefresh: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                RoundedCornerShape(8.dp)
            )
            .padding(8.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Display Filters",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "Badge-only LCD and scanner emission policy  #$displayPolicyHash",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            OutlinedButton(onClick = { onExpandedChange(!expanded) }) {
                Text(if (expanded) "Hide" else "Edit")
            }
        }

        if (!expanded) return@Column

        Spacer(modifier = Modifier.height(8.dp))
        BadgeDisplayPolicyClasses.forEach { info ->
            val config = policy.classes[info.key] ?: BadgeDisplayClassPolicy()
            val filtered = filteredCounts[info.key] ?: 0
            BadgeDisplayClassRow(
                label = info.label,
                filtered = filtered,
                config = config,
                onChange = { next ->
                    onPolicyChange(
                        policy.copy(classes = policy.classes + (info.key to next))
                    )
                }
            )
            HorizontalDivider(
                modifier = Modifier.padding(vertical = 6.dp),
                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.45f)
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Button(onClick = onApply) {
                Text("Apply")
            }
            OutlinedButton(onClick = onReset) {
                Text("Reset Defaults")
            }
            OutlinedButton(onClick = onRefresh) {
                Text("Refresh")
            }
        }
    }
}

@Composable
private fun BadgeDisplayClassRow(
    label: String,
    filtered: Int,
    config: BadgeDisplayClassPolicy,
    onChange: (BadgeDisplayClassPolicy) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = "Suppressed $filtered  |  Priority ${config.priority}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = { onChange(config.copy(enabled = it)) }
            )
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = "Lane",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("off", "lower", "top", "both").forEach { lane ->
                OutlinedButton(
                    onClick = { onChange(config.copy(lane = lane)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = lane.uppercase(),
                        style = MaterialTheme.typography.labelSmall,
                        color = if (config.lane == lane) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        maxLines = 1
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = "Minimum proximity",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("present", "near", "close").forEach { prox ->
                OutlinedButton(
                    onClick = { onChange(config.copy(minProximity = prox)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = prox.uppercase(),
                        style = MaterialTheme.typography.labelSmall,
                        color = if (config.minProximity == prox) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        maxLines = 1
                    )
                }
            }
        }
        Slider(
            value = config.priority.toFloat(),
            onValueChange = {
                onChange(config.copy(priority = it.toInt().coerceIn(0, 100)))
            },
            valueRange = 0f..100f
        )
    }
}

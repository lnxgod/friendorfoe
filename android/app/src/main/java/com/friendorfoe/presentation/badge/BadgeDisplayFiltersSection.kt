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
import com.friendorfoe.data.badge.BadgeDisplayLane
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicyClasses
import com.friendorfoe.data.badge.BadgeDisplayRule
import com.friendorfoe.data.badge.BadgeMinimumProximity
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicyClasses

private enum class BadgeRowDensityPreset(val label: String) {
    Focus("Focus"),
    Balanced("Balanced"),
    Full("Full")
}

private val highSignalClasses = setOf("drone", "meta", "wifi_attack", "skimmer", "flock")
private val mediumSignalClasses = setOf("tracker", "camera", "lock")

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
                    text = "Badge LCD row density and scanner emission policy  #$displayPolicyHash",
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
        Text(
            text = "Row density",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf(
                BadgeRowDensityPreset.Focus,
                BadgeRowDensityPreset.Balanced,
                BadgeRowDensityPreset.Full
            ).forEach { preset ->
                OutlinedButton(
                    onClick = { onPolicyChange(policy.withRowDensityPreset(preset)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = preset.label.uppercase(),
                        style = MaterialTheme.typography.labelSmall,
                        maxLines = 1
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        BadgeDisplayPolicyClasses.forEach { info ->
            val config = policy.classes.getValue(info.key)
            val filtered = filteredCounts[info.key] ?: 0
            BadgeDisplayClassRow(
                policyKey = info.key,
                label = info.label,
                filtered = filtered,
                config = config,
                onChange = { key, next ->
                    onPolicyChange(
                        if (next.enabled != config.enabled) {
                            policy.withEnabled(key, next.enabled)
                        } else {
                            policy.copy(classes = policy.classes + (key to next))
                        }
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

private fun BadgeDisplayPolicy.withRowDensityPreset(
    preset: BadgeRowDensityPreset
): BadgeDisplayPolicy {
    val defaults = defaultBadgeDisplayPolicyClasses()
    val next = when (preset) {
        BadgeRowDensityPreset.Balanced -> defaults
        BadgeRowDensityPreset.Focus -> defaults.mapValues { (key, config) ->
            when (key) {
                in highSignalClasses -> config.copy(
                    enabled = true,
                    lane = BadgeDisplayLane.BOTH,
                    minProximity = BadgeMinimumProximity.PRESENT,
                    priority = (config.priority + 5).coerceAtMost(100)
                )
                in mediumSignalClasses -> config.copy(
                    enabled = true,
                    lane = BadgeDisplayLane.LOWER,
                    minProximity = BadgeMinimumProximity.CLOSE,
                    priority = (config.priority + 10).coerceAtMost(100)
                )
                else -> config.copy(enabled = false, lane = BadgeDisplayLane.OFF)
            }
        }
        BadgeRowDensityPreset.Full -> defaults.mapValues { (key, config) ->
            config.copy(
                enabled = true,
                lane = if (key in highSignalClasses) {
                    BadgeDisplayLane.BOTH
                } else {
                    BadgeDisplayLane.LOWER
                },
                minProximity = BadgeMinimumProximity.PRESENT,
                priority = config.priority.coerceAtLeast(25)
            )
        }
    }
    return copy(classes = next)
}

@Composable
private fun BadgeDisplayClassRow(
    policyKey: String,
    label: String,
    filtered: Int,
    config: BadgeDisplayRule,
    onChange: (String, BadgeDisplayRule) -> Unit
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
                onCheckedChange = { onChange(policyKey, config.copy(enabled = it)) }
            )
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = "Lane",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            BadgeDisplayLane.entries.forEach { lane ->
                OutlinedButton(
                    onClick = {
                        onChange(
                            policyKey,
                            config.copy(
                                enabled = if (lane == BadgeDisplayLane.OFF) {
                                    false
                                } else {
                                    config.enabled
                                },
                                lane = lane
                            )
                        )
                    },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = lane.wireValue.uppercase(),
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
            BadgeMinimumProximity.entries.forEach { prox ->
                OutlinedButton(
                    onClick = { onChange(policyKey, config.copy(minProximity = prox)) },
                    modifier = Modifier.weight(1f)
                ) {
                    Text(
                        text = prox.wireValue.uppercase(),
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
                onChange(policyKey, config.copy(priority = it.toInt().coerceIn(0, 100)))
            },
            valueRange = 0f..100f
        )
    }
}

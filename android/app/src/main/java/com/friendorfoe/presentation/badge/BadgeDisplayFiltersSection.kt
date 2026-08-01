package com.friendorfoe.presentation.badge

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeDisplayClassPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicyClasses
import com.friendorfoe.data.badge.defaultBadgeDisplayPolicyClasses
import com.friendorfoe.data.badge.withClassEnabled

private val displayClassLabels = BadgeDisplayPolicyClasses.associate { info ->
    info.key to when (info.key) {
        "wifi_attack" -> "Wi-Fi Attack"
        "flock" -> "Flock / ALPR"
        else -> info.label
    }
}

/**
 * Compact editor for the dedicated Badge route. The route-level action card owns persistence.
 */
@Composable
fun BadgeDisplayFiltersSection(
    policy: BadgeDisplayPolicy?,
    policyHash: Long?,
    enabled: Boolean,
    unavailableReason: String?,
    onPolicyChange: ((BadgeDisplayPolicy) -> BadgeDisplayPolicy) -> Unit,
) {
    BadgeSectionCard {
        BadgeDisplayPolicyHeading(policyHash)
        BadgeDisplayPolicyTruthCopy()

        if (policy == null) {
            Text(
                text = unavailableReason ?: "Display policy readback is unavailable",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        } else {
            BadgeDisplayPolicyEditor(
                policy = policy,
                filteredCounts = emptyMap(),
                controlsEnabled = enabled,
                onPolicyChange = { next -> onPolicyChange { next } },
            )
            if (!enabled && unavailableReason != null) {
                Text(
                    text = unavailableReason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

/**
 * Expandable editor for the legacy control screen, sharing the same explicit firmware fields.
 */
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
    onRefresh: () -> Unit,
    remoteActionsEnabled: Boolean = true,
    refreshEnabled: Boolean = true,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surface.copy(alpha = 0.45f),
                RoundedCornerShape(8.dp),
            )
            .padding(8.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Display Filters",
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    text = "Exact badge policy · ${hashCode32(displayPolicyHash)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            OutlinedButton(
                onClick = { onExpandedChange(!expanded) },
                modifier = Modifier.testTag("badge_filters_toggle"),
            ) {
                Text(if (expanded) "Hide" else "Edit")
            }
        }

        if (!expanded) return@Column

        Spacer(modifier = Modifier.height(10.dp))
        BadgeDisplayPolicyTruthCopy()
        Spacer(modifier = Modifier.height(4.dp))
        BadgeDisplayPolicyEditor(
            policy = policy,
            filteredCounts = filteredCounts,
            controlsEnabled = true,
            onPolicyChange = onPolicyChange,
        )

        Spacer(modifier = Modifier.height(10.dp))
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .testTag("badge_filter_remote_actions"),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("badge_filter_apply_reset_row"),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Button(
                    onClick = onApply,
                    enabled = remoteActionsEnabled,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Apply")
                }
                OutlinedButton(
                    onClick = onReset,
                    enabled = remoteActionsEnabled,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Reset Defaults")
                }
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("badge_filter_refresh_row"),
            ) {
                OutlinedButton(
                    onClick = onRefresh,
                    enabled = refreshEnabled,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text("Refresh")
                }
            }
        }
    }
}

@Composable
private fun BadgeDisplayPolicyHeading(policyHash: Long?) {
    Text(
        text = "Display rules",
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.SemiBold,
    )
    Text(
        text = "Policy hash ${hashCode32(policyHash)}",
        style = MaterialTheme.typography.labelMedium,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun BadgeDisplayPolicyTruthCopy() {
    Text(
        text = "Close ≥ -60 dBm · Near ≥ -76 dBm · Present < -76 dBm",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(
        text = "Off is not an absolute suppression guarantee; firmware safety rules may still show high-confidence evidence.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.tertiary,
    )
}

@Composable
private fun BadgeDisplayPolicyEditor(
    policy: BadgeDisplayPolicy,
    filteredCounts: Map<String, Int>,
    controlsEnabled: Boolean,
    onPolicyChange: (BadgeDisplayPolicy) -> Unit,
) {
    val defaults = defaultBadgeDisplayPolicyClasses()
    BadgeDisplayPolicyClasses.forEachIndexed { index, info ->
        if (index > 0) {
            HorizontalDivider(
                modifier = Modifier.padding(vertical = 6.dp),
                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
            )
        }
        val config = policy.classes[info.key] ?: defaults.getValue(info.key)
        BadgeDisplayClassRow(
            policyKey = info.key,
            label = displayClassLabels[info.key] ?: info.label,
            filtered = filteredCounts[info.key],
            config = config,
            controlsEnabled = controlsEnabled,
            onEnabledChange = { isEnabled ->
                onPolicyChange(policy.withClassEnabled(info.key, isEnabled))
            },
            onChange = { next ->
                onPolicyChange(policy.copy(classes = policy.classes + (info.key to next)))
            },
        )
    }
}

@Composable
private fun BadgeDisplayClassRow(
    policyKey: String,
    label: String,
    filtered: Int?,
    config: BadgeDisplayClassPolicy,
    controlsEnabled: Boolean,
    onEnabledChange: (Boolean) -> Unit,
    onChange: (BadgeDisplayClassPolicy) -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
            .testTag("badge_rule_$policyKey"),
        verticalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    text = policyKey,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (filtered != null) {
                    Text(
                        text = "Suppressed by badge: $filtered",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            Switch(
                checked = config.enabled,
                onCheckedChange = onEnabledChange,
                enabled = controlsEnabled,
                modifier = Modifier
                    .heightIn(min = 48.dp)
                    .semantics { contentDescription = "$label display rule" }
                    .testTag("badge_rule_${policyKey}_toggle"),
            )
        }

        Text(
            text = "lane=${config.lane} · min_proximity=${config.minProximity} · priority=${config.priority}",
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (!config.enabled) {
            Text(
                text = "Off · wire lane=off",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@Column
        }

        Text(
            text = "Lane",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .testTag("badge_rule_${policyKey}_lanes"),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            listOf("lower", "top", "both").forEach { lane ->
                FilterChip(
                    selected = config.lane == lane,
                    onClick = { onChange(config.copy(lane = lane)) },
                    enabled = controlsEnabled,
                    label = { Text(lane.replaceFirstChar(Char::uppercase)) },
                    modifier = Modifier
                        .heightIn(min = 48.dp)
                        .semantics { contentDescription = "$label lane $lane" }
                        .testTag("badge_rule_${policyKey}_lane_$lane"),
                )
            }
        }

        Text(
            text = "Minimum proximity",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .testTag("badge_rule_${policyKey}_proximity"),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            listOf("present", "near", "close").forEach { proximity ->
                FilterChip(
                    selected = config.minProximity == proximity,
                    onClick = { onChange(config.copy(minProximity = proximity)) },
                    enabled = controlsEnabled,
                    label = { Text(proximity.replaceFirstChar(Char::uppercase)) },
                    modifier = Modifier
                        .heightIn(min = 48.dp)
                        .semantics {
                            contentDescription = "$label minimum proximity $proximity"
                        }
                        .testTag("badge_rule_${policyKey}_proximity_$proximity"),
                )
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Priority",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.weight(1f))
            Text(
                text = config.priority.toString(),
                style = MaterialTheme.typography.labelMedium,
                fontFamily = FontFamily.Monospace,
            )
        }
        Slider(
            value = config.priority.toFloat(),
            onValueChange = { value ->
                onChange(config.copy(priority = value.toInt().coerceIn(0, 100)))
            },
            enabled = controlsEnabled,
            valueRange = 0f..100f,
            steps = 99,
            modifier = Modifier
                .fillMaxWidth()
                .semantics { contentDescription = "$label priority ${config.priority}" }
                .testTag("badge_rule_${policyKey}_priority"),
        )
    }
}

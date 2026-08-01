package com.friendorfoe.presentation.badge

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.badge.BadgeDisplayLane
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayPolicyClasses
import com.friendorfoe.data.badge.BadgeDisplayRule
import com.friendorfoe.data.badge.BadgeMinimumProximity

private val displayClassLabels = BadgeDisplayPolicyClasses.associate { info ->
    info.key to when (info.key) {
        "wifi_attack" -> "Wi-Fi Attack"
        "flock" -> "Flock / ALPR"
        else -> info.label
    }
}

@Composable
fun BadgeDisplayFiltersSection(
    policy: BadgeDisplayPolicy?,
    policyHash: Long?,
    enabled: Boolean,
    unavailableReason: String?,
    onPolicyChange: ((BadgeDisplayPolicy) -> BadgeDisplayPolicy) -> Unit,
) {
    BadgeSectionCard {
        Text(
            "Display rules",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Policy hash ${hashCode32(policyHash)}",
            style = MaterialTheme.typography.labelMedium,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Close ≥ -60 dBm · Near ≥ -76 dBm · Present < -76 dBm",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Off is not an absolute suppression guarantee; firmware safety rules may still show high-confidence evidence.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.tertiary,
        )

        if (policy == null) {
            Text(
                unavailableReason ?: "Display policy readback is unavailable",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        } else {
            BadgeDisplayPolicy.classOrder.forEach { key ->
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f),
                )
                val rule = policy.classes.getValue(key)
                DisplayRuleRow(
                    firmwareKey = key,
                    label = displayClassLabels.getValue(key),
                    rule = rule,
                    controlsEnabled = enabled,
                    onEnabledChange = { nextEnabled ->
                        onPolicyChange { current -> current.withEnabled(key, nextEnabled) }
                    },
                    onLaneChange = { lane ->
                        onPolicyChange { current ->
                            val currentRule = current.classes.getValue(key)
                            current.copy(
                                classes = LinkedHashMap(current.classes).apply {
                                    put(key, currentRule.copy(lane = lane))
                                },
                            )
                        }
                    },
                    onProximityChange = { proximity ->
                        onPolicyChange { current ->
                            val currentRule = current.classes.getValue(key)
                            current.copy(
                                classes = LinkedHashMap(current.classes).apply {
                                    put(key, currentRule.copy(minProximity = proximity))
                                },
                            )
                        }
                    },
                )
            }
            if (!enabled && unavailableReason != null) {
                Text(
                    unavailableReason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

@Composable
private fun DisplayRuleRow(
    firmwareKey: String,
    label: String,
    rule: BadgeDisplayRule,
    controlsEnabled: Boolean,
    onEnabledChange: (Boolean) -> Unit,
    onLaneChange: (BadgeDisplayLane) -> Unit,
    onProximityChange: (BadgeMinimumProximity) -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().padding(vertical = 6.dp).testTag("badge_rule_$firmwareKey"),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
                Text(
                    firmwareKey,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = rule.enabled,
                onCheckedChange = onEnabledChange,
                enabled = controlsEnabled,
                modifier = Modifier
                    .heightIn(min = 48.dp)
                    .semantics { contentDescription = "$label display rule" }
                    .testTag("badge_rule_${firmwareKey}_toggle"),
            )
        }

        if (!rule.enabled) {
            Text(
                "Off",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            Text("Lane", style = MaterialTheme.typography.labelSmall)
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .testTag("badge_rule_${firmwareKey}_lanes"),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                listOf(BadgeDisplayLane.LOWER, BadgeDisplayLane.TOP, BadgeDisplayLane.BOTH)
                    .forEach { lane ->
                        FilterChip(
                            selected = rule.lane == lane,
                            onClick = { onLaneChange(lane) },
                            enabled = controlsEnabled,
                            label = { Text(lane.wireValue.replaceFirstChar(Char::uppercase)) },
                            modifier = Modifier
                                .heightIn(min = 48.dp)
                                .semantics {
                                    contentDescription = "$label ${lane.wireValue} lane"
                                }
                                .testTag("badge_rule_${firmwareKey}_lane_${lane.wireValue}"),
                        )
                    }
            }

            Text("Minimum proximity", style = MaterialTheme.typography.labelSmall)
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .testTag("badge_rule_${firmwareKey}_proximity"),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                BadgeMinimumProximity.entries.forEach { proximity ->
                    FilterChip(
                        selected = rule.minProximity == proximity,
                        onClick = { onProximityChange(proximity) },
                        enabled = controlsEnabled,
                        label = {
                            Text(proximity.wireValue.replaceFirstChar(Char::uppercase))
                        },
                        modifier = Modifier
                            .heightIn(min = 48.dp)
                            .semantics {
                                contentDescription =
                                    "$label ${proximity.wireValue} minimum proximity"
                            }
                            .testTag(
                                "badge_rule_${firmwareKey}_proximity_${proximity.wireValue}",
                            ),
                    )
                }
            }
        }
    }
}

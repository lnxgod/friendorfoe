package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

// Every branch and leaf is its own lazy item, including in fully expanded trees.
internal fun LazyListScope.privacyDeviceTree(
    groups: List<PrivacyGroupBranch>,
    expandedKeys: Set<String>,
    onToggle: (String) -> Unit,
    actions: PrivacyActions,
) {
    groups.forEach { branch ->
        val group = branch.group
        item(key = "tree_${group.key}", contentType = "device_group") {
            PrivacyTreeBranch(group.key, group.label, group.description, branch.findings,
                expanded = group.key in expandedKeys, nested = false, onToggle = { onToggle(group.key) })
        }
        if (group.key in expandedKeys) {
            branch.families.forEach { familyBranch ->
                val family = familyBranch.family
                item(key = "tree_${family.key}", contentType = "device_family") {
                    PrivacyTreeBranch(family.key, family.label,
                        description = if (family == PrivacyDeviceFamily.FIND_MY) "May include other Find My accessories" else null,
                        findings = familyBranch.findings, expanded = family.key in expandedKeys,
                        nested = true, onToggle = { onToggle(family.key) })
                }
                if (family.key in expandedKeys) {
                    if (familyBranch.networks.isNotEmpty()) {
                        familyBranch.networks.forEach { network ->
                            item(key = "tree_${network.key}", contentType = "beacon_network") {
                                PrivacyTreeBranch(network.key, "iBeacon network",
                                    network.uuid ?: "UUID unavailable", network.findings,
                                    network.key in expandedKeys, true, { onToggle(network.key) })
                            }
                            if (network.key in expandedKeys) {
                                items(network.findings, key = { it.observationKey.encoded }) { finding ->
                                    Column(Modifier.padding(start = 32.dp)) {
                                        PrivacyFindingRow(finding, actions, showObservationId = true)
                                    }
                                }
                            }
                        }
                    } else items(familyBranch.findings, key = { it.observationKey.encoded }, contentType = { "device_finding" }) { finding ->
                        val lineColor = MaterialTheme.colorScheme.outlineVariant
                        Column(Modifier.drawBehind {
                            drawLine(lineColor, Offset(24.dp.toPx(), 0f), Offset(24.dp.toPx(), size.height), 1.dp.toPx())
                        }.padding(start = 32.dp)) {
                            PrivacyFindingRow(finding, actions, showObservationId = true)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PrivacyTreeBranch(
    key: String,
    label: String,
    description: String?,
    findings: List<PrivacyFinding>,
    expanded: Boolean,
    nested: Boolean,
    onToggle: () -> Unit,
) {
    val live = findings.count { it.freshness == FindingFreshness.LIVE }
    val lineColor = MaterialTheme.colorScheme.outlineVariant
    Row(
        modifier = Modifier.fillMaxWidth()
            .background(if (nested) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f))
            .drawBehind {
                if (nested) drawLine(lineColor, Offset(24.dp.toPx(), 0f), Offset(24.dp.toPx(), size.height), 1.dp.toPx())
            }
            .heightIn(min = 64.dp)
            .testTag("privacy_tree_$key")
            .semantics { stateDescription = if (expanded) "Expanded" else "Collapsed" }
            .clickable(role = Role.Button, onClickLabel = if (expanded) "Collapse $label" else "Expand $label", onClick = onToggle)
            .padding(start = if (nested) 40.dp else 16.dp, end = 16.dp, top = 12.dp, bottom = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text("$label · ${findings.size}", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            description?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            Text("$live live · ${findings.size - live} cached", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Icon(if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore, contentDescription = null)
    }
    HorizontalDivider(color = lineColor)
}

package com.friendorfoe.presentation.detail

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.components.FofStatusStrip
import com.friendorfoe.presentation.components.FofTone

@Composable
fun DetailOverviewContent(
    model: DetailPresentation,
    modifier: Modifier = Modifier,
    onRetryDetails: (() -> Unit)? = null,
    referenceLabel: String? = null,
    onOpenReference: (() -> Unit)? = null,
) {
    val itemSaveKey = model.identifiers.firstOrNull()?.value ?: model.title
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        FofStatusStrip(
            label = if (model.isLive) "LIVE" else "SAVED",
            title = model.statusLabel,
            detail = if (model.isLive) {
                "Current local observation"
            } else {
                "Immutable snapshot from History"
            },
            tone = if (model.isLive) FofTone.Success else FofTone.Primary,
        )

        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                text = model.title,
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.SemiBold,
            )
            model.supportingMessage?.let { message ->
                Text(
                    text = message,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
                if (model.retryLabel != null && onRetryDetails != null) {
                    TextButton(
                        onClick = onRetryDetails,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("detail_retry"),
                    ) {
                        Text(model.retryLabel)
                    }
                }
            }
        }

        DetailFieldsSection(title = "At a glance", fields = model.summary)

        if (model.identifiers.isNotEmpty()) {
            IdentifierSection(model.identifiers)
        }

        if (referenceLabel != null && onOpenReference != null) {
            FofSection(title = "Reference") {
                FofActionRow(
                    title = referenceLabel,
                    description = "Compare this detection with the bundled identification guide.",
                    trailingLabel = "Open",
                    onClick = onOpenReference,
                )
            }
        }

        if (model.advanced.isNotEmpty()) {
            ExpandableDetailSection(
                title = "More details",
                fields = model.advanced,
                initiallyExpanded = false,
                testTag = "detail_advanced",
                itemSaveKey = itemSaveKey,
            )
        }

        if (model.raw.isNotEmpty()) {
            ExpandableDetailSection(
                title = "Raw fields",
                fields = model.raw,
                initiallyExpanded = model.rawExpandedByDefault,
                testTag = "detail_raw",
                itemSaveKey = itemSaveKey,
            )
        }
    }
}

@Composable
private fun IdentifierSection(identifiers: List<DetailIdentifier>) {
    val clipboard = LocalClipboardManager.current
    var copiedLabel by rememberSaveable { mutableStateOf<String?>(null) }
    FofSection(
        title = "Identifiers",
        subtitle = "Use Copy when you need an exact value.",
    ) {
        identifiers.forEachIndexed { index, identifier ->
            if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Row(
                modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = identifier.label,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        text = identifier.value,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.Medium,
                    )
                }
                if (identifier.copyable) {
                    TextButton(
                        onClick = {
                            clipboard.setText(AnnotatedString(identifier.value))
                            copiedLabel = identifier.label
                        },
                        modifier = Modifier
                            .heightIn(min = 48.dp)
                            .testTag("copy_${identifier.label.lowercase().replace(' ', '_')}"),
                    ) {
                        Icon(Icons.Default.ContentCopy, contentDescription = null)
                        Text("Copy", modifier = Modifier.padding(start = 6.dp))
                    }
                }
            }
        }
        copiedLabel?.let { label ->
            Text(
                text = "Copied $label",
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun DetailFieldsSection(
    title: String,
    fields: List<DetailField>,
) {
    FofSection(title = title) {
        DetailFieldRows(fields)
    }
}

@Composable
private fun ExpandableDetailSection(
    title: String,
    fields: List<DetailField>,
    initiallyExpanded: Boolean,
    testTag: String,
    itemSaveKey: String,
) {
    var expanded by rememberSaveable(itemSaveKey, title) { mutableStateOf(initiallyExpanded) }
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column {
            TextButton(
                onClick = { expanded = !expanded },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp)
                    .testTag(testTag),
            ) {
                Text(title, modifier = Modifier.weight(1f), fontWeight = FontWeight.Bold)
                Icon(
                    imageVector = if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    contentDescription = if (expanded) "Collapse $title" else "Expand $title",
                )
            }
            if (expanded) {
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                    DetailFieldRows(fields)
                }
            }
        }
    }
}

@Composable
private fun DetailFieldRows(fields: List<DetailField>) {
    fields.forEachIndexed { index, field ->
        if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Column(
            modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                text = field.label,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = field.value,
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.Medium,
            )
        }
    }
}

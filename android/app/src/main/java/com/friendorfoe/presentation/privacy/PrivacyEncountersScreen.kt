package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@HiltViewModel
class PrivacyEncountersViewModel @Inject constructor(private val repository: PrivacyFindingRepository) : ViewModel() {
    val entries = repository.encounters
    fun clear() = repository.clearEncounters()
}

@Composable
fun PrivacyEncountersScreen(
    onBack: () -> Unit,
    onOpenFinding: (PrivacyFindingKey) -> Unit,
    viewModel: PrivacyEncountersViewModel = hiltViewModel(),
) {
    val entries by viewModel.entries.collectAsStateWithLifecycle()
    PrivacyEncountersContent(entries, onBack, onOpenFinding, viewModel::clear)
}

@Composable
fun PrivacyEncountersContent(
    entries: List<PrivacyEncounter>,
    onBack: () -> Unit,
    onOpenFinding: (PrivacyFindingKey) -> Unit,
    onClear: () -> Unit,
) {
    var confirmClear by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxSize()) {
        FofSecondaryScreenHeader("Recent encounters", onBack)
        Text(
            "Last 30 minutes in this app session. Saved observations are not proof a device is still nearby.",
            Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium,
        )
        if (entries.isNotEmpty()) {
            TextButton(onClick = { confirmClear = true }, modifier = Modifier.padding(horizontal = 8.dp)) {
                Text("Clear recent encounters")
            }
        }
        LazyColumn(Modifier.fillMaxSize().testTag("privacy_encounters")) {
            if (entries.isEmpty()) item {
                Text("No recent encounters", Modifier.padding(24.dp), style = MaterialTheme.typography.titleMedium)
            }
            items(entries, key = { it.key.encoded }) { encounter ->
                Column(
                    Modifier.fillMaxWidth().clickable { onOpenFinding(encounter.key) }.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Text(encounter.finding.title, style = MaterialTheme.typography.titleMedium)
                    Text(encounter.finding.source.userLabel(), style = MaterialTheme.typography.labelMedium)
                    Text("Last observed ${formatEncounterTime(encounter.lastObservedWallMs)} · ${encounter.observationCount} updates")
                    Text("Review evidence", color = MaterialTheme.colorScheme.primary)
                }
                HorizontalDivider()
            }
        }
    }
    if (confirmClear) AlertDialog(
        onDismissRequest = { confirmClear = false },
        title = { Text("Clear recent encounters?") },
        text = { Text("Removes this session's saved observations. New observations can still appear while scanning.") },
        confirmButton = { TextButton(onClick = { onClear(); confirmClear = false }) { Text("Clear") } },
        dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("Cancel") } },
    )
}

@Composable
internal fun EncounterEvidence(encounter: PrivacyEncounter) {
    val clipboard = LocalClipboardManager.current
    var copied by remember(encounter.key) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Observation history", style = MaterialTheme.typography.titleLarge)
        TextButton(onClick = {
            clipboard.setText(AnnotatedString(encounterEvidenceText(encounter)))
            copied = true
        }) { Text(if (copied) "Evidence copied" else "Copy evidence") }
        Text("First observed: ${formatEncounterTime(encounter.firstObservedWallMs)}")
        Text("Last observed: ${formatEncounterTime(encounter.lastObservedWallMs)}")
        Text("${encounter.observationCount} updates observed this session")
        if (encounter.signalSamples.isNotEmpty()) {
            Text(signalTrend(encounter.signalSamples), style = MaterialTheme.typography.titleMedium)
            val strongest = encounter.signalSamples.maxOf { it.dbm }
            val weakest = encounter.signalSamples.minOf { it.dbm }
            Text("Signal range: $weakest to $strongest dBm · ${encounter.signalSamples.size} recent samples")
            if (encounter.signalSamples.size >= 2) SignalHistoryChart(encounter.signalSamples)
            Text(
                "Signal strength varies with walls, orientation, and interference. It does not establish distance or intent.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun SignalHistoryChart(samples: List<PrivacySignalSample>) {
    val color = MaterialTheme.colorScheme.primary
    Canvas(Modifier.fillMaxWidth().height(80.dp).testTag("privacy_signal_history")) {
        val first = samples.first().elapsedMs
        val span = (samples.last().elapsedMs - first).coerceAtLeast(1)
        val weakest = samples.minOf { it.dbm } - 3
        val strongest = samples.maxOf { it.dbm } + 3
        val offsets = samples.map { sample ->
            Offset(
                (sample.elapsedMs - first).toFloat() / span * size.width,
                (1f - (sample.dbm - weakest).toFloat() / (strongest - weakest)) * size.height,
            )
        }
        offsets.zipWithNext().forEach { (from, to) -> drawLine(color, from, to, strokeWidth = 3.dp.toPx()) }
    }
}

internal fun formatEncounterTime(wallMs: Long): String = Instant.ofEpochMilli(wallMs)
    .atZone(ZoneId.systemDefault()).format(DateTimeFormatter.ofPattern("HH:mm:ss"))

internal fun encounterEvidenceText(encounter: PrivacyEncounter): String = buildString {
    appendLine(encounter.finding.title)
    appendLine("Source: ${encounter.finding.source.userLabel()}")
    appendLine("Record: ${encounter.key.sourceRecordId}")
    appendLine("First observed: ${Instant.ofEpochMilli(encounter.firstObservedWallMs)}")
    appendLine("Last observed: ${Instant.ofEpochMilli(encounter.lastObservedWallMs)}")
    appendLine("Updates observed: ${encounter.observationCount}")
    encounter.finding.evidence?.let { appendLine("Evidence: $it") }
    encounter.finding.limitation?.let { appendLine("Limitations: $it") }
    append("Saved observation; does not establish current presence, identity, or intent.")
}

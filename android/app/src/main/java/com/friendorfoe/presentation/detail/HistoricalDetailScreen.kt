package com.friendorfoe.presentation.detail

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import com.friendorfoe.presentation.history.detectionSourceLabel
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun HistoricalDetailScreen(
    historyId: Long,
    onBack: () -> Unit,
    onReturnToHistory: () -> Unit,
    viewModel: DetailViewModel = hiltViewModel(),
) {
    val detailState by viewModel.detailState.collectAsStateWithLifecycle()

    LaunchedEffect(historyId) {
        viewModel.loadHistoricalDetail(historyId)
    }

    Scaffold(
        topBar = { FofSecondaryScreenHeader(title = "Historical detection", onBack = onBack) },
    ) { innerPadding ->
        Box(Modifier.fillMaxSize().padding(innerPadding), contentAlignment = Alignment.Center) {
            when (val state = detailState) {
                DetailState.Idle, DetailState.Loading -> CircularProgressIndicator()
                is DetailState.HistoricalLoaded -> HistoricalDetailContent(state.snapshot)
                is DetailState.Error -> HistoricalErrorContent(
                    message = state.message,
                    onBack = onBack,
                    onReturnToHistory = onReturnToHistory,
                )
                is DetailState.AircraftLoaded,
                is DetailState.DroneLoaded -> HistoricalErrorContent(
                    message = "Historical detection could not be displayed.",
                    onBack = onBack,
                    onReturnToHistory = onReturnToHistory,
                )
            }
        }
    }
}

@Composable
internal fun HistoricalDetailContent(snapshot: HistoryEntity) {
    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            text = "Historical detection",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Bold,
        )
        Text(snapshot.displayName, style = MaterialTheme.typography.headlineSmall)
        if (!snapshot.description.isNullOrBlank()) {
            Text(
                snapshot.description,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        FofSection(title = "Stored detection") {
            HistoricalRow("Detected", formatHistoricalTimestamp(snapshot.lastSeen))
            HistoricalRow("Source", detectionSourceLabel(snapshot.detectionSource))
            HistoricalRow(
                "Location at detection",
                "%.5f, %.5f".format(Locale.US, snapshot.latitude, snapshot.longitude),
            )
            HistoricalRow("Altitude at detection", "${snapshot.altitudeMeters.roundToInt()} m")
            HistoricalRow("Confidence", "${(snapshot.confidence * 100).roundToInt()}%")
        }
    }
}

@Composable
private fun HistoricalRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun HistoricalErrorContent(
    message: String,
    onBack: () -> Unit,
    onReturnToHistory: () -> Unit,
) {
    Column(
        modifier = Modifier.padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Historical detection unavailable", style = MaterialTheme.typography.titleMedium)
        Text(message, style = MaterialTheme.typography.bodyMedium)
        Button(onClick = onBack) { Text("Back") }
        TextButton(onClick = onReturnToHistory) { Text("Return to History") }
    }
}

private fun formatHistoricalTimestamp(timestamp: Long): String = Instant.ofEpochMilli(timestamp)
    .atZone(ZoneId.systemDefault())
    .format(DateTimeFormatter.ofPattern("MMM d, yyyy h:mm a", Locale.getDefault()))

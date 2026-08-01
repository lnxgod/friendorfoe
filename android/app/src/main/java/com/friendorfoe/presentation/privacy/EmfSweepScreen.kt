package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle

data class MagneticFieldActions(
    val onResetBaseline: () -> Unit = {},
    val onRetry: () -> Unit = {},
    val onBack: () -> Unit = {},
)

@Composable
fun EmfSweepScreen(
    onBack: () -> Unit,
    viewModel: EmfSweepViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val lifecycleOwner = LocalLifecycleOwner.current

    DisposableEffect(lifecycleOwner, viewModel) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> viewModel.start()
                Lifecycle.Event.ON_STOP -> viewModel.stop()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
            viewModel.start()
        }
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            viewModel.stop()
        }
    }

    MagneticFieldContent(
        state = state,
        actions = MagneticFieldActions(
            onResetBaseline = viewModel::resetBaseline,
            onRetry = viewModel::start,
            onBack = onBack,
        ),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MagneticFieldContent(
    state: MagneticFieldUiState,
    actions: MagneticFieldActions,
    modifier: Modifier = Modifier,
) {
    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Magnetic-field sweep") },
                navigationIcon = {
                    IconButton(onClick = actions.onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                ),
            )
        },
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            contentAlignment = Alignment.TopCenter,
        ) {
            Column(
                modifier = Modifier
                    .widthIn(max = 640.dp)
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 20.dp, vertical = 18.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                Text(
                    text = "Use the phone's magnetometer to compare field strength as you move it.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                when (state) {
                    MagneticFieldUiState.Initializing -> InitializingCard()
                    MagneticFieldUiState.SensorUnavailable -> SensorUnavailableCard(actions.onRetry)
                    is MagneticFieldUiState.AwaitingAccurateBaseline -> {
                        AwaitingBaselineCard(state, actions.onResetBaseline)
                    }
                    is MagneticFieldUiState.Live -> LiveMagneticFieldCard(state, actions.onResetBaseline)
                    is MagneticFieldUiState.Failed -> FailureCard(state.message, actions.onRetry)
                }
            }
        }
    }
}

@Composable
private fun InitializingCard() {
    EvidenceCard {
        Text(
            text = "Waiting for a reliable magnetometer sample",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            text = "A high-accuracy sample will set the baseline. The first reading can take a moment while the sensor calibrates.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SensorUnavailableCard(onRetry: () -> Unit) {
    EvidenceCard {
        Text(
            text = "Magnetometer unavailable",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            text = "This phone does not expose the magnetic-field sensor this tool needs.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(onClick = onRetry) { Text("Retry") }
    }
}

@Composable
private fun AwaitingBaselineCard(
    state: MagneticFieldUiState.AwaitingAccurateBaseline,
    onReset: () -> Unit,
) {
    EvidenceCard {
        Text(
            text = "Waiting for a high-accuracy baseline",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
        )
        AccuracyChip(state.accuracyLabel)
        Text(
            text = "Move the phone in a figure eight, then hold it still. Reset uses the next high-accuracy sample as the new baseline.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(onClick = onReset) { Text("Reset baseline") }
    }
}

@Composable
private fun LiveMagneticFieldCard(
    state: MagneticFieldUiState.Live,
    onReset: () -> Unit,
) {
    EvidenceCard {
        Text(
            text = formatMicroTesla(state.totalMicroTesla),
            style = MaterialTheme.typography.displayMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary,
        )
        Text(
            text = "Current field strength",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        AccuracyChip(state.accuracyLabel)

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f),
                    shape = RoundedCornerShape(14.dp),
                )
                .padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            MeasurementRow("Baseline", formatMicroTesla(state.baselineMicroTesla))
            MeasurementRow("Change", formatMicroTesla(state.deviationMicroTesla))
            MeasurementRow("Largest change", formatMicroTesla(state.peakDeviationMicroTesla))
        }

        Text(
            text = "A deviation is a magnetic-field change. It cannot identify electronics, cameras, or intent.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(onClick = onReset) { Text("Reset baseline") }
    }
}

@Composable
private fun FailureCard(message: String, onRetry: () -> Unit) {
    EvidenceCard {
        Text(
            text = "Magnetometer could not start",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            text = message,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(onClick = onRetry) { Text("Retry") }
    }
}

@Composable
private fun EvidenceCard(content: @Composable ColumnScope.() -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column(
            modifier = Modifier.padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
            horizontalAlignment = Alignment.Start,
            content = content,
        )
    }
}

@Composable
private fun AccuracyChip(label: String) {
    Text(
        text = label,
        modifier = Modifier
            .background(
                color = MaterialTheme.colorScheme.secondaryContainer,
                shape = RoundedCornerShape(50),
            )
            .padding(horizontal = 12.dp, vertical = 6.dp),
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSecondaryContainer,
    )
}

@Composable
private fun MeasurementRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyLarge,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.End,
        )
    }
}

private fun formatMicroTesla(value: Float): String = "%.1f µT".format(value)

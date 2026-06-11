package com.friendorfoe.presentation.privacy

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.detection.EmfDetector

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EmfSweepScreen(
    onBack: () -> Unit,
    viewModel: EmfSweepViewModel = hiltViewModel()
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val reading = state.reading

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("EMF Sweep") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(18.dp)
        ) {
            if (!state.sensorAvailable) {
                Text(
                    text = "Magnetometer unavailable",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.error
                )
                Text(
                    text = "This device does not expose the magnetic-field sensor needed for EMF sweep mode.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Button(onClick = viewModel::start) { Text("Retry") }
                return@Column
            }

            EmfGauge(
                magnitude = reading?.magnitudeUt ?: 0f,
                level = reading?.level ?: EmfDetector.EmfLevel.NORMAL,
                modifier = Modifier.size(260.dp)
            )

            Text(
                text = "${formatUt(reading?.magnitudeUt ?: 0f)} uT",
                style = MaterialTheme.typography.displayMedium,
                fontWeight = FontWeight.Bold,
                color = colorForLevel(reading?.level ?: EmfDetector.EmfLevel.NORMAL)
            )
            Text(
                text = "Peak ${formatUt(state.peakUt)} uT",
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            reading?.let {
                ComponentRow("X", it.x)
                ComponentRow("Y", it.y)
                ComponentRow("Z", it.z)
            }

            Spacer(modifier = Modifier.height(4.dp))
            Button(onClick = viewModel::resetPeak) {
                Text("Reset Peak")
            }
        }
    }
}

@Composable
private fun EmfGauge(
    magnitude: Float,
    level: EmfDetector.EmfLevel,
    modifier: Modifier = Modifier
) {
    val color = colorForLevel(level)
    val sweep = (magnitude / 400f).coerceIn(0f, 1f) * 270f
    Box(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.25f), RoundedCornerShape(8.dp)),
        contentAlignment = Alignment.Center
    ) {
        Canvas(modifier = Modifier.fillMaxSize().padding(24.dp)) {
            val strokeWidth = 18.dp.toPx()
            val topLeft = Offset(strokeWidth, strokeWidth)
            val size = Size(this.size.width - strokeWidth * 2, this.size.height - strokeWidth * 2)
            drawArc(
                color = Color(0xFF9E9E9E).copy(alpha = 0.25f),
                startAngle = 135f,
                sweepAngle = 270f,
                useCenter = false,
                topLeft = topLeft,
                size = size,
                style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
            )
            drawArc(
                color = color,
                startAngle = 135f,
                sweepAngle = sweep,
                useCenter = false,
                topLeft = topLeft,
                size = size,
                style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
            )
        }
        Text(
            text = level.name,
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold,
            color = color
        )
    }
}

@Composable
private fun ComponentRow(label: String, value: Float) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.18f), RoundedCornerShape(6.dp))
            .padding(horizontal = 14.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
        Text(
            text = "${formatUt(value)} uT",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

private fun formatUt(value: Float): String = "%.1f".format(value)

@Composable
private fun colorForLevel(level: EmfDetector.EmfLevel): Color = when (level) {
    EmfDetector.EmfLevel.NORMAL -> Color(0xFF2E7D32)
    EmfDetector.EmfLevel.LOW -> Color(0xFFF9A825)
    EmfDetector.EmfLevel.MEDIUM -> Color(0xFFEF6C00)
    EmfDetector.EmfLevel.HIGH -> MaterialTheme.colorScheme.error
}

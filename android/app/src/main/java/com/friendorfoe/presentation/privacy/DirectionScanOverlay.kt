package com.friendorfoe.presentation.privacy

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import kotlinx.coroutines.delay
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin

/**
 * Full-screen overlay for BLE direction finding.
 *
 * Phase 1: Guided 360° rotation scan — user spins slowly holding phone.
 *          Shows compass, rotation progress, speed guide, sample count.
 *
 * Phase 2: Tracking mode — arrow points toward device, signal strength
 *          meter helps user walk toward it.
 */
@Composable
fun DirectionScanOverlay(
    detection: GlassesDetection,
    viewModel: PrivacyViewModel,
    onDismiss: () -> Unit
) {
    var phase by remember { mutableStateOf(ScanPhase.SCANNING) }
    var result by remember { mutableStateOf<BleTracker.DirectionResult?>(null) }
    var sampleCount by remember { mutableIntStateOf(0) }
    var currentHeading by remember { mutableFloatStateOf(0f) }
    var rotationSpeed by remember { mutableFloatStateOf(0f) }
    var lastHeading by remember { mutableFloatStateOf(0f) }
    var coveredSectors by remember { mutableStateOf(BooleanArray(12)) } // 12 sectors of 30°
    var liveRssi by remember { mutableIntStateOf(detection.rssi) }

    val bleTracker = viewModel.bleTracker

    // Start the scan
    LaunchedEffect(detection.mac) {
        viewModel.startDirectionScan(detection.mac)
    }

    // Poll for updates during scan
    LaunchedEffect(phase) {
        if (phase == ScanPhase.SCANNING) {
            while (true) {
                delay(200)
                val heading = viewModel.sensorFusionEngine.orientation.value.azimuthDegrees
                val headingDelta = abs(heading - lastHeading).let {
                    if (it > 180) 360 - it else it
                }
                rotationSpeed = headingDelta / 0.2f // degrees per second
                lastHeading = heading
                currentHeading = heading

                // Track sector coverage
                val sector = ((heading / 30f).toInt()) % 12
                coveredSectors = coveredSectors.clone().also { it[sector] = true }

                sampleCount = bleTracker.getDirectionSampleCount()

                // Update live RSSI
                viewModel.getTrackedDeviceRssi(detection.mac)?.let { liveRssi = it }

                // Auto-finish when we have good coverage
                val coveredCount = coveredSectors.count { it }
                if (sampleCount >= 16 && coveredCount >= 10) {
                    result = viewModel.finishDirectionScan()
                    phase = ScanPhase.TRACKING
                }
            }
        } else if (phase == ScanPhase.TRACKING) {
            // Keep updating heading and RSSI in tracking mode
            while (true) {
                delay(200)
                currentHeading = viewModel.sensorFusionEngine.orientation.value.azimuthDegrees
                viewModel.getTrackedDeviceRssi(detection.mac)?.let { liveRssi = it }
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.97f))
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Header
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = "${detection.manufacturer} ${detection.deviceType}",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    detection.deviceName?.let {
                        Text(it, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                Text(
                    text = "Cancel",
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.clickable(onClick = onDismiss)
                )
            }

            Spacer(modifier = Modifier.height(16.dp))

            when (phase) {
                ScanPhase.SCANNING -> ScanningPhaseUI(
                    currentHeading = currentHeading,
                    rotationSpeed = rotationSpeed,
                    sampleCount = sampleCount,
                    coveredSectors = coveredSectors,
                    liveRssi = liveRssi,
                    onFinishEarly = {
                        result = viewModel.finishDirectionScan()
                        if (result != null) {
                            phase = ScanPhase.TRACKING
                        }
                    }
                )
                ScanPhase.TRACKING -> TrackingPhaseUI(
                    result = result,
                    currentHeading = currentHeading,
                    liveRssi = liveRssi,
                    peakRssi = result?.peakRssi ?: liveRssi,
                    onRescan = {
                        viewModel.startDirectionScan(detection.mac)
                        coveredSectors = BooleanArray(12)
                        sampleCount = 0
                        result = null
                        phase = ScanPhase.SCANNING
                    },
                    onDone = onDismiss
                )
            }
        }
    }
}

private enum class ScanPhase { SCANNING, TRACKING }

@Composable
private fun androidx.compose.foundation.layout.ColumnScope.ScanningPhaseUI(
    currentHeading: Float,
    rotationSpeed: Float,
    sampleCount: Int,
    coveredSectors: BooleanArray,
    liveRssi: Int,
    onFinishEarly: () -> Unit
) {
    val coveredCount = coveredSectors.count { it }
    val progress = (coveredCount / 12f).coerceIn(0f, 1f)
    val minSamples = 8

    // Pulsing animation for the scan ring
    val infiniteTransition = rememberInfiniteTransition(label = "pulse")
    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.3f, targetValue = 0.8f,
        animationSpec = infiniteRepeatable(tween(1000), RepeatMode.Reverse),
        label = "pulseAlpha"
    )

    Text(
        text = "Rotate slowly 360\u00B0",
        style = MaterialTheme.typography.headlineSmall,
        fontWeight = FontWeight.Bold,
        textAlign = TextAlign.Center
    )
    Text(
        text = "Hold your phone close to your body\nand spin in a complete circle",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Center
    )

    Spacer(modifier = Modifier.height(24.dp))

    // Compass rose with sector coverage
    Box(
        modifier = Modifier.size(240.dp),
        contentAlignment = Alignment.Center
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val center = Offset(size.width / 2, size.height / 2)
            val radius = size.minDimension / 2 - 20f

            // Draw covered sectors (green arcs)
            for (i in 0 until 12) {
                val startAngle = i * 30f - 90f // Canvas uses -90 = top
                val color = if (coveredSectors[i]) Color(0xFF4CAF50).copy(alpha = 0.3f)
                            else Color.Gray.copy(alpha = 0.1f)
                drawArc(
                    color = color,
                    startAngle = startAngle,
                    sweepAngle = 30f,
                    useCenter = true,
                    topLeft = Offset(center.x - radius, center.y - radius),
                    size = androidx.compose.ui.geometry.Size(radius * 2, radius * 2)
                )
            }

            // Outer ring
            drawCircle(
                color = Color(0xFF2196F3).copy(alpha = pulseAlpha),
                radius = radius,
                center = center,
                style = Stroke(width = 3f)
            )

            // Cardinal direction labels
            val labelRadius = radius + 12f
            // N/S/E/W handled by heading indicator below

            // Current heading indicator (red triangle at top, rotates with phone)
            rotate(-currentHeading, pivot = center) {
                val tipY = center.y - radius + 5f
                val path = Path().apply {
                    moveTo(center.x, tipY)
                    lineTo(center.x - 8f, tipY + 16f)
                    lineTo(center.x + 8f, tipY + 16f)
                    close()
                }
                drawPath(path, Color.Red)
            }
        }

        // Center text
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = "${currentHeading.toInt()}\u00B0",
                style = MaterialTheme.typography.headlineLarge,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = cardinalDirection(currentHeading),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }

    Spacer(modifier = Modifier.height(16.dp))

    // Speed indicator
    val speedText = when {
        rotationSpeed < 8f -> "\u23F8\uFE0F  Turn faster"
        rotationSpeed > 60f -> "\u26A0\uFE0F  Slow down!"
        else -> "\u2705  Good speed"
    }
    val speedColor = when {
        rotationSpeed < 8f -> Color(0xFFFFC107)
        rotationSpeed > 60f -> Color(0xFFFF5722)
        else -> Color(0xFF4CAF50)
    }
    Text(text = speedText, color = speedColor, fontWeight = FontWeight.Medium)

    Spacer(modifier = Modifier.height(12.dp))

    // Progress bar
    Text("Coverage: ${coveredCount}/12 sectors", style = MaterialTheme.typography.bodySmall)
    LinearProgressIndicator(
        progress = { progress },
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 32.dp)
            .height(8.dp),
        color = Color(0xFF4CAF50),
        trackColor = MaterialTheme.colorScheme.surfaceVariant,
    )

    Spacer(modifier = Modifier.height(8.dp))

    Text("Samples: $sampleCount (need $minSamples+)", style = MaterialTheme.typography.bodySmall)
    Text("Signal: ${liveRssi} dBm", style = MaterialTheme.typography.bodySmall,
        color = rssiColor(liveRssi))

    Spacer(modifier = Modifier.weight(1f))

    // Finish early button
    if (sampleCount >= minSamples) {
        Button(
            onClick = onFinishEarly,
            modifier = Modifier.fillMaxWidth(),
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF4CAF50))
        ) {
            Text("Get Direction ($sampleCount samples)")
        }
    }
}

@Composable
private fun androidx.compose.foundation.layout.ColumnScope.TrackingPhaseUI(
    result: BleTracker.DirectionResult?,
    currentHeading: Float,
    liveRssi: Int,
    peakRssi: Int,
    onRescan: () -> Unit,
    onDone: () -> Unit
) {
    if (result == null) {
        Text("Not enough data. Try scanning again.", color = MaterialTheme.colorScheme.error)
        Spacer(modifier = Modifier.height(16.dp))
        Button(onClick = onRescan) { Text("Rescan") }
        return
    }

    val bearing = result.estimatedBearing
    val confidence = result.confidence

    // Arrow rotation: relative to current heading
    // If device is at bearing 90° (East) and phone faces North (0°), arrow points right
    val relativeAngle = bearing - currentHeading

    Text(
        text = "Device located!",
        style = MaterialTheme.typography.headlineSmall,
        fontWeight = FontWeight.Bold,
        color = Color(0xFF4CAF50)
    )
    Text(
        text = "Confidence: ${(confidence * 100).toInt()}%",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant
    )

    Spacer(modifier = Modifier.height(16.dp))

    // Large direction arrow
    Box(
        modifier = Modifier.size(240.dp),
        contentAlignment = Alignment.Center
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val center = Offset(size.width / 2, size.height / 2)
            val radius = size.minDimension / 2 - 30f

            // Outer compass ring
            drawCircle(
                color = Color(0xFF4CAF50).copy(alpha = 0.2f),
                radius = radius + 20f,
                center = center,
                style = Stroke(width = 2f)
            )

            // Direction arrow (rotates relative to phone heading)
            rotate(relativeAngle, pivot = center) {
                // Arrow shaft
                drawLine(
                    color = Color(0xFF4CAF50),
                    start = Offset(center.x, center.y + radius * 0.3f),
                    end = Offset(center.x, center.y - radius * 0.7f),
                    strokeWidth = 6f,
                    cap = StrokeCap.Round
                )
                // Arrow head
                val tipY = center.y - radius * 0.7f
                val path = Path().apply {
                    moveTo(center.x, tipY - 20f)
                    lineTo(center.x - 18f, tipY + 10f)
                    lineTo(center.x + 18f, tipY + 10f)
                    close()
                }
                drawPath(path, Color(0xFF4CAF50))
            }

            // Center dot
            drawCircle(Color(0xFF2196F3), 8f, center)
        }

        // Bearing text in center
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = "${bearing.toInt()}\u00B0",
                style = MaterialTheme.typography.displaySmall,
                fontWeight = FontWeight.Bold,
                color = Color(0xFF4CAF50)
            )
            Text(
                text = cardinalDirection(bearing),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }

    Spacer(modifier = Modifier.height(20.dp))

    // Signal strength meter
    Text("Signal Strength", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
    Spacer(modifier = Modifier.height(8.dp))

    SignalStrengthMeter(rssi = liveRssi, peakRssi = peakRssi)

    Spacer(modifier = Modifier.height(12.dp))

    // RSSI text with proximity hint
    val proximity = when {
        liveRssi > -40 -> "Very Close!"
        liveRssi > -55 -> "Close"
        liveRssi > -70 -> "Medium Range"
        liveRssi > -85 -> "Far"
        else -> "Very Far"
    }
    val proximityColor = when {
        liveRssi > -40 -> Color(0xFF4CAF50)
        liveRssi > -55 -> Color(0xFF8BC34A)
        liveRssi > -70 -> Color(0xFFFFC107)
        liveRssi > -85 -> Color(0xFFFF9800)
        else -> Color(0xFFFF5722)
    }

    Text(
        text = "$proximity ($liveRssi dBm)",
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.Bold,
        color = proximityColor
    )

    Spacer(modifier = Modifier.weight(1f))

    // Action buttons
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        OutlinedButton(onClick = onRescan, modifier = Modifier.weight(1f)) {
            Text("Rescan")
        }
        Button(onClick = onDone, modifier = Modifier.weight(1f)) {
            Text("Done")
        }
    }
}

@Composable
private fun SignalStrengthMeter(rssi: Int, peakRssi: Int) {
    val barCount = 10
    // Normalize RSSI to 0-10 scale (-100 = 0, -30 = 10)
    val filled = ((rssi + 100) / 7f).coerceIn(0f, barCount.toFloat()).toInt()

    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.Bottom
    ) {
        for (i in 0 until barCount) {
            val barHeight = 16 + (i * 4)
            val isActive = i < filled
            val color = when {
                i < 3 -> Color(0xFFFF5722)   // weak = red
                i < 6 -> Color(0xFFFFC107)   // medium = yellow
                else -> Color(0xFF4CAF50)     // strong = green
            }
            Box(
                modifier = Modifier
                    .width(20.dp)
                    .height(barHeight.dp)
                    .padding(horizontal = 2.dp)
                    .background(
                        if (isActive) color else color.copy(alpha = 0.15f),
                        MaterialTheme.shapes.extraSmall
                    )
            )
        }
    }
}

private fun cardinalDirection(degrees: Float): String {
    val normalized = ((degrees % 360) + 360) % 360
    return when {
        normalized < 22.5 || normalized >= 337.5 -> "N"
        normalized < 67.5 -> "NE"
        normalized < 112.5 -> "E"
        normalized < 157.5 -> "SE"
        normalized < 202.5 -> "S"
        normalized < 247.5 -> "SW"
        normalized < 292.5 -> "W"
        else -> "NW"
    }
}

private fun rssiColor(rssi: Int): Color = when {
    rssi > -50 -> Color(0xFF4CAF50)
    rssi > -70 -> Color(0xFFFFC107)
    else -> Color(0xFFFF5722)
}

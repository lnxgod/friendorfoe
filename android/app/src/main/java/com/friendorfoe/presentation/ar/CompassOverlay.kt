package com.friendorfoe.presentation.ar

import android.hardware.SensorManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt

/**
 * Cardinal and intercardinal compass directions with their degree positions.
 */
private data class CompassPoint(val label: String, val degrees: Float)

private val compassPoints = listOf(
    CompassPoint("N", 0f),
    CompassPoint("NE", 45f),
    CompassPoint("E", 90f),
    CompassPoint("SE", 135f),
    CompassPoint("S", 180f),
    CompassPoint("SW", 225f),
    CompassPoint("W", 270f),
    CompassPoint("NW", 315f)
)

/**
 * Compass strip overlay for the AR view.
 *
 * Displays a horizontal compass band at the top of the screen that scrolls
 * based on the current device azimuth. Features:
 * - Current heading in cardinal direction + degrees (e.g. "NE 045°")
 * - Scrolling strip with tick marks and cardinal/intercardinal labels
 * - North highlighted in red/orange for quick orientation
 * - Center indicator triangle marking current heading
 * - Calibration warning when magnetometer accuracy is poor
 *
 * @param azimuthDegrees Current compass heading (0-360, 0 = north)
 * @param sensorAccuracy Magnetometer calibration status (SensorManager accuracy constants)
 */
@Composable
fun CompassOverlay(
    azimuthDegrees: Float,
    sensorAccuracy: Int,
    pitchDegrees: Float = 0f,
    modifier: Modifier = Modifier
) {
    val textMeasurer = rememberTextMeasurer()
    val needsCalibration = sensorAccuracy <= SensorManager.SENSOR_STATUS_ACCURACY_LOW

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(
                color = Color.Black.copy(alpha = 0.6f),
                shape = RoundedCornerShape(bottomStart = 12.dp, bottomEnd = 12.dp)
            )
            .padding(top = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Heading text: cardinal direction + degrees + pitch indicator
        val cardinalDir = getCardinalDirection(azimuthDegrees)
        val degreesText = "%03.0f".format(azimuthDegrees % 360f)
        val pitchAbs = kotlin.math.abs(pitchDegrees).roundToInt()
        val pitchArrow = if (pitchDegrees >= 0f) "\u2191" else "\u2193"  // ↑ or ↓
        val pitchText = "$pitchArrow ${pitchAbs}\u00B0"
        val pitchColor = if (pitchDegrees < -10f) Color(0xFFFFAB40) else Color.White.copy(alpha = 0.7f)

        Row(
            modifier = Modifier.padding(bottom = 2.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "$cardinalDir $degreesText\u00B0",
                color = Color.White,
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center
            )
            Text(
                text = pitchText,
                color = pitchColor,
                fontSize = 14.sp,
                fontWeight = FontWeight.Normal
            )
        }

        // Compass strip canvas
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(40.dp)
        ) {
            Canvas(modifier = Modifier.matchParentSize()) {
                drawCompassStrip(azimuthDegrees, textMeasurer)
            }
        }

        // Calibration warning
        if (needsCalibration) {
            Text(
                text = "⚠ Calibrate: move phone in figure-8",
                color = Color(0xFFFFAB40),
                fontSize = 11.sp,
                modifier = Modifier.padding(vertical = 2.dp)
            )
        }
    }
}

/**
 * Draw the scrolling compass strip on a Canvas.
 *
 * Renders a ~120° window of the compass centered on the current azimuth,
 * with tick marks every 10° and cardinal/intercardinal labels.
 */
private fun DrawScope.drawCompassStrip(
    azimuthDegrees: Float,
    textMeasurer: TextMeasurer
) {
    val width = size.width
    val height = size.height
    val centerX = width / 2f
    val visibleRange = 120f // degrees visible in the strip
    val pixelsPerDegree = width / visibleRange

    // Draw center indicator triangle at top
    val triangleSize = 8f
    val trianglePath = Path().apply {
        moveTo(centerX - triangleSize, 0f)
        lineTo(centerX + triangleSize, 0f)
        lineTo(centerX, triangleSize * 1.5f)
        close()
    }
    drawPath(trianglePath, Color.White)

    // Draw tick marks and labels
    // Iterate through all degrees that could be visible
    val startDeg = (azimuthDegrees - visibleRange / 2).toInt() - 5
    val endDeg = (azimuthDegrees + visibleRange / 2).toInt() + 5

    for (deg in startDeg..endDeg) {
        val normalizedDeg = ((deg % 360) + 360) % 360
        val offset = angleDifference(normalizedDeg.toFloat(), azimuthDegrees)

        // Skip if outside visible range
        if (offset < -visibleRange / 2 || offset > visibleRange / 2) continue

        val x = centerX + offset * pixelsPerDegree

        if (deg % 10 == 0) {
            // Major tick every 10°
            val isCardinal = normalizedDeg % 45 == 0
            val tickHeight = if (isCardinal) height * 0.45f else height * 0.25f
            val isNorth = normalizedDeg == 0
            val tickColor = if (isNorth) Color(0xFFFF6D00) else Color.White.copy(alpha = 0.7f)

            drawLine(
                color = tickColor,
                start = Offset(x, height - tickHeight),
                end = Offset(x, height),
                strokeWidth = if (isCardinal) 2f else 1f
            )

            // Draw cardinal/intercardinal label
            if (isCardinal) {
                val label = compassPoints.find { it.degrees == normalizedDeg.toFloat() }?.label ?: ""
                val labelColor = if (isNorth) Color(0xFFFF6D00) else Color.White
                val style = TextStyle(
                    color = labelColor,
                    fontSize = if (normalizedDeg % 90 == 0) 13.sp else 11.sp,
                    fontWeight = if (normalizedDeg % 90 == 0) FontWeight.Bold else FontWeight.Normal
                )
                val measured = textMeasurer.measure(label, style)
                drawText(
                    textLayoutResult = measured,
                    topLeft = Offset(
                        x - measured.size.width / 2f,
                        height - tickHeight - measured.size.height - 2f
                    )
                )
            }
        } else if (deg % 5 == 0) {
            // Minor tick every 5°
            drawLine(
                color = Color.White.copy(alpha = 0.3f),
                start = Offset(x, height - height * 0.15f),
                end = Offset(x, height),
                strokeWidth = 0.5f
            )
        }
    }

    // Draw center line
    drawLine(
        color = Color.White.copy(alpha = 0.5f),
        start = Offset(centerX, triangleSize * 1.5f),
        end = Offset(centerX, height),
        strokeWidth = 1f
    )
}

/**
 * Calculate the shortest angular difference between two angles,
 * handling the 0/360 wraparound. Returns value in range [-180, 180].
 */
private fun angleDifference(target: Float, current: Float): Float {
    var diff = target - current
    while (diff > 180f) diff -= 360f
    while (diff < -180f) diff += 360f
    return diff
}

/**
 * Get the cardinal/intercardinal direction label for a given azimuth.
 */
private fun getCardinalDirection(azimuth: Float): String {
    val normalized = ((azimuth % 360f) + 360f) % 360f
    return when {
        normalized < 22.5f || normalized >= 337.5f -> "N"
        normalized < 67.5f -> "NE"
        normalized < 112.5f -> "E"
        normalized < 157.5f -> "SE"
        normalized < 202.5f -> "S"
        normalized < 247.5f -> "SW"
        normalized < 292.5f -> "W"
        else -> "NW"
    }
}

package com.friendorfoe.presentation.trails

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.splitAircraftTrail
import kotlin.math.roundToInt

@Composable
fun FlightMetricChart(label: String, units: String, points: List<TrackingEntity>, selected: TrackingEntity?, value: (TrackingEntity) -> Double?) {
    val values = points.mapNotNull(value).filter { it.isFinite() }
    if (values.isEmpty()) return
    val min = values.min(); val max = values.max()
    val duration = ((points.lastOrNull()?.timestamp ?: 0) - (points.firstOrNull()?.timestamp ?: 0)).coerceAtLeast(1)
    val accent = MaterialTheme.colorScheme.primary
    val grid = MaterialTheme.colorScheme.outlineVariant
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text("$label · ${min.roundToInt()}–${max.roundToInt()} $units", style = MaterialTheme.typography.labelLarge)
        Canvas(Modifier.fillMaxWidth().height(88.dp).semantics { contentDescription = "$label over recorded time, ${min.roundToInt()} to ${max.roundToInt()} $units" }) {
            val inset = 4.dp.toPx()
            fun xy(p: TrackingEntity, v: Double) = Offset(
                inset + ((p.timestamp - points.first().timestamp).toDouble() / duration * (size.width - 2 * inset)).toFloat(),
                size.height - inset - ((v - min) / (max - min).coerceAtLeast(1.0) * (size.height - 2 * inset)).toFloat(),
            )
            drawLine(grid, Offset(inset, size.height - inset), Offset(size.width - inset, size.height - inset))
            splitAircraftTrail(points).forEach { segment ->
                val path = Path(); var first = true
                segment.forEach { p ->
                    val v = value(p)?.takeIf { it.isFinite() }
                    if (v == null) first = true else {
                        val pos = xy(p, v)
                        if (first) path.moveTo(pos.x, pos.y) else path.lineTo(pos.x, pos.y)
                        first = false
                    }
                }
                drawPath(path, accent, style = Stroke(2.dp.toPx()))
            }
            selected?.let { p -> value(p)?.takeIf { it.isFinite() }?.let { drawCircle(accent, 4.dp.toPx(), xy(p, it)) } }
        }
    }
}

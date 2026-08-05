package com.friendorfoe.presentation.map

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Point
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Overlay
import kotlin.math.roundToInt

private const val FORMATION_PIXEL_SIZE_DP = 7f
private const val FORMATION_PIXEL_COLOR = 0xFF00E5FF.toInt()
private const val FORMATION_PIXEL_BORDER_COLOR = 0xFF004D5A.toInt()

/** Draws every stationary formation pixel in one lightweight osmdroid overlay. */
internal class FormationOverlay(context: Context) : Overlay() {
    private val radiusPx =
        (FORMATION_PIXEL_SIZE_DP * context.resources.displayMetrics.density / 2f)
            .coerceAtLeast(1f)
    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = FORMATION_PIXEL_BORDER_COLOR
        style = Paint.Style.FILL
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = FORMATION_PIXEL_COLOR
        style = Paint.Style.FILL
    }

    @Volatile
    private var points: List<GeoPoint> = emptyList()

    internal val pointCount: Int
        get() = points.size

    internal fun replacePoints(newPoints: List<GeoPoint>) {
        if (points == newPoints) return
        points = newPoints.toList()
    }

    override fun draw(canvas: Canvas, mapView: MapView, shadow: Boolean) {
        if (shadow) return

        val snapshot = points
        if (snapshot.isEmpty()) return

        val projection = mapView.projection
        val screenPoint = Point()
        val borderRadius = radiusPx
        val fillRadius = (radiusPx - 1f).coerceAtLeast(0.5f)
        val margin = borderRadius.roundToInt()
        val minX = -margin
        val minY = -margin
        val maxX = canvas.width + margin
        val maxY = canvas.height + margin

        for (geoPoint in snapshot) {
            projection.toPixels(geoPoint, screenPoint)
            if (screenPoint.x !in minX..maxX || screenPoint.y !in minY..maxY) continue
            canvas.drawCircle(
                screenPoint.x.toFloat(),
                screenPoint.y.toFloat(),
                borderRadius,
                borderPaint,
            )
            canvas.drawCircle(
                screenPoint.x.toFloat(),
                screenPoint.y.toFloat(),
                fillRadius,
                fillPaint,
            )
        }
    }
}

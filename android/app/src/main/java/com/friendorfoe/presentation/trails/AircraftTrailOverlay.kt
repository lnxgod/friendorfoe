package com.friendorfoe.presentation.trails

import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.splitAircraftTrail
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Polyline

/** Own only the path overlays; leave markers, camera, and other map layers intact. */
class AircraftTrailOverlay(private val map: MapView) {
    private var rendered: List<TrackingEntity> = emptyList()
    private val lines = mutableListOf<Polyline>()

    fun render(points: List<TrackingEntity>) {
        if (rendered == points) return
        rendered = points
        map.overlays.removeAll(lines.toSet())
        lines.clear()
        splitAircraftTrail(points).filter { it.size >= 2 }.forEach { segment ->
            val line = Polyline(map).apply {
                setPoints(segment.map { GeoPoint(it.latitude, it.longitude) })
                outlinePaint.color = android.graphics.Color.rgb(0, 137, 194)
                outlinePaint.strokeWidth = 6f
                setOnClickListener { _, _, _ -> false }
            }
            lines.add(line)
            map.overlays.add(0, line)
        }
        map.invalidate()
    }
}

package com.friendorfoe.presentation.map

import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import com.friendorfoe.data.repository.splitAircraftTrail
import com.friendorfoe.presentation.trails.formatTrackTime
import org.osmdroid.util.BoundingBox
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.overlay.Marker
import org.osmdroid.views.overlay.Overlay
import org.osmdroid.views.overlay.Polyline

/** Independent historical layer: lines and recorded endpoints remain after live markers disappear. */
internal class MapFlightTrailOverlay(
    private val map: MapView,
    private val onOpenPath: (String) -> Unit,
) {
    private var disposed = false
    private val owned = mutableListOf<Overlay>()
    private var rendered: Pair<List<MapFlightTrail>, String?>? = null
    private val colors = intArrayOf(0xFF0089C2.toInt(), 0xFF8B50C7.toInt(), 0xFF008577.toInt(), 0xFFC05A00.toInt())

    fun render(trails: List<MapFlightTrail>, selectedId: String?) {
        if (disposed) return
        val next = trails to selectedId
        if (rendered == next) return
        rendered = next
        map.overlays.removeAll(owned.toSet())
        owned.clear()
        val density = map.resources.displayMetrics.density
        trails.forEach { trail ->
            val color = colors[(trail.objectId.hashCode() and Int.MAX_VALUE) % colors.size]
            splitAircraftTrail(trail.points).filter { it.size >= 2 }.forEach { segment ->
                val line = Polyline(map).apply {
                    setPoints(segment.map { GeoPoint(it.latitude, it.longitude) })
                    outlinePaint.color = color
                    outlinePaint.strokeWidth = (if (trail.objectId == selectedId) 4f else 2f) * density
                    setOnClickListener { _, _, _ -> false }
                }
                owned.add(line)
                map.overlays.add(0, line)
            }
            val last = trail.points.last()
            val marker = Marker(map).apply {
                position = GeoPoint(last.latitude, last.longitude)
                title = "${trail.label} · last recorded ${formatTrackTime(last.timestamp)}"
                setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER)
                icon = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(if (trail.live) color else Color.WHITE)
                    setStroke((2 * density).toInt(), color)
                    val size = ((if (trail.live) 10 else 14) * density).toInt()
                    setSize(size, size)
                }
                setOnMarkerClickListener { _, _ -> onOpenPath(trail.objectId); true }
            }
            owned.add(marker)
            // Keep the live aircraft markers above this layer.
            map.overlays.add(0, marker)
        }
        map.invalidate()
    }

    fun dispose() {
        disposed = true
        map.overlays.removeAll(owned.toSet())
        owned.clear()
        rendered = null
    }

    fun fit(trails: List<MapFlightTrail>) {
        if (disposed) return
        val locations = trails.flatMap { it.points }.map { GeoPoint(it.latitude, it.longitude) }
        if (locations.isEmpty()) return
        map.post {
            if (disposed) return@post
            if (locations.distinct().size == 1) {
                map.controller.setCenter(locations.first())
                map.controller.setZoom(12.0)
            } else map.zoomToBoundingBox(BoundingBox.fromGeoPointsSafe(locations), false, 80)
        }
    }
}

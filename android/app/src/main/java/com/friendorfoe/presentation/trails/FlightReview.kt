package com.friendorfoe.presentation.trails

import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.splitAircraftTrail
import java.time.Instant
import java.util.Locale

fun recordedPositionAt(points: List<TrackingEntity>, timeMs: Long): TrackingEntity? =
    points.lastOrNull { it.timestamp <= timeMs } ?: points.firstOrNull()

fun flightGpx(label: String, points: List<TrackingEntity>): String = buildString {
    fun escaped(text: String) = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;").replace("'", "&apos;")
    append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"Friend or Foe\" xmlns=\"http://www.topografix.com/GPX/1/1\"><trk><name>")
    append(escaped(label)); append("</name>")
    splitAircraftTrail(points).forEach { segment ->
        append("<trkseg>")
        segment.forEach { p ->
            append(String.format(Locale.US, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.2f</ele><time>%s</time></trkpt>", p.latitude, p.longitude, p.altitudeMeters, Instant.ofEpochMilli(p.timestamp)))
        }
        append("</trkseg>")
    }
    append("</trk></gpx>")
}

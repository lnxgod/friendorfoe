package com.friendorfoe.data.repository

import com.friendorfoe.data.local.TrackingEntity
import kotlin.math.*

const val AIRCRAFT_TRACK_RETENTION_MS = 24 * 60 * 60 * 1000L
const val MAX_AIRCRAFT_TRACK_POINTS = 3600

fun validTrackPoint(point: TrackingEntity): Boolean =
    point.latitude.isFinite() && point.latitude in -90.0..90.0 &&
        point.longitude.isFinite() && point.longitude in -180.0..180.0 &&
        (point.latitude != 0.0 || point.longitude != 0.0) &&
        point.altitudeMeters.isFinite() && point.timestamp > 0L

fun trackDistanceMeters(a: TrackingEntity, b: TrackingEntity): Double {
    val lat = Math.toRadians(b.latitude - a.latitude)
    val lon = Math.toRadians(b.longitude - a.longitude)
    val h = sin(lat / 2).pow(2) + cos(Math.toRadians(a.latitude)) *
        cos(Math.toRadians(b.latitude)) * sin(lon / 2).pow(2)
    return 6_371_000.0 * 2 * asin(sqrt(h.coerceIn(0.0, 1.0)))
}

internal fun shouldRecordAircraftPoint(
    previous: TrackingEntity?,
    next: TrackingEntity,
    nowMs: Long,
): Boolean {
    if (!validTrackPoint(next) || next.timestamp > nowMs || nowMs - next.timestamp > 120_000L) return false
    if (previous == null) return true
    val elapsed = next.timestamp - previous.timestamp
    if (elapsed < 5_000L) return false
    return elapsed >= 60_000L || trackDistanceMeters(previous, next) >= 20.0 ||
        abs(previous.altitudeMeters - next.altitudeMeters) >= 20.0
}

/** Separate missing coverage and impossible jumps instead of inventing a continuous flight. */
fun splitAircraftTrail(points: List<TrackingEntity>): List<List<TrackingEntity>> {
    val segments = mutableListOf<MutableList<TrackingEntity>>()
    points.filter(::validTrackPoint).sortedBy { it.timestamp }.distinctBy { it.objectId to it.timestamp }.forEach { point ->
        val previous = segments.lastOrNull()?.lastOrNull()
        val elapsed = previous?.let { point.timestamp - it.timestamp } ?: 0L
        val disconnected = previous == null || point.objectId != previous.objectId ||
            elapsed > 120_000L || abs(point.longitude - previous.longitude) > 180.0 ||
            trackDistanceMeters(previous, point) > elapsed / 1000.0 * 1000.0
        if (disconnected) segments.add(mutableListOf(point)) else segments.last().add(point)
    }
    return segments
}

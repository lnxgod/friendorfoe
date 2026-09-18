package com.friendorfoe.presentation.map

import com.friendorfoe.data.local.AircraftMapPoint
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.repository.trackDistanceMeters
import com.friendorfoe.data.repository.validTrackPoint
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.usecase.FilterEngine
import java.time.Instant

enum class FlightTrailWindow(val label: String, val durationMs: Long) {
    OFF("Off", 0), FIFTEEN_MINUTES("15 min", 15 * 60_000L),
    ONE_HOUR("1 hour", 60 * 60_000L), DAY("24 hours", 24 * 60 * 60_000L),
}

data class MapFlightTrail(
    val objectId: String,
    val label: String,
    val points: List<TrackingEntity>,
    val live: Boolean,
)

data class MapFlightTrailsState(
    val trails: List<MapFlightTrail> = emptyList(),
    val loading: Boolean = false,
    val error: Boolean = false,
)

/** Apply the map's live filters to recorded endpoints too; never promote history into live traffic. */
internal fun projectMapFlightTrails(
    records: List<AircraftMapPoint>,
    liveAircraft: List<Aircraft>,
    filter: FilterState,
    userPosition: Position?,
): List<MapFlightTrail> {
    val liveById = liveAircraft.associateBy { it.id }
    return records.filter { validTrackPoint(it.point) }.groupBy { it.point.objectId }
        .mapNotNull { (id, values) ->
            val ordered = values.distinctBy { it.point.timestamp }.sortedBy { it.point.timestamp }
            val latest = ordered.last()
            val point = latest.point
            val current = liveById[id]
            val saved = Aircraft(
                id = id, icaoHex = id, callsign = latest.label,
                position = Position(point.latitude, point.longitude, point.altitudeMeters),
                category = runCatching { ObjectCategory.valueOf(latest.category.uppercase()) }
                    .getOrDefault(ObjectCategory.UNKNOWN),
                firstSeen = Instant.ofEpochMilli(ordered.first().point.timestamp),
                lastUpdated = Instant.ofEpochMilli(point.timestamp),
                distanceMeters = userPosition?.takeIf { it.hasValidMapCoordinates() }?.let {
                    trackDistanceMeters(point, point.copy(latitude = it.latitude, longitude = it.longitude))
                },
            )
            if (FilterEngine.applyFilters(listOf(current ?: saved), filter).isEmpty()) return@mapNotNull null
            MapFlightTrail(id, current?.callsign ?: current?.registration ?: latest.label,
                ordered.map { it.point }, current != null)
        }
        .sortedWith(compareByDescending<MapFlightTrail> { it.points.last().timestamp }.thenBy { it.objectId })
        .take(40)
}

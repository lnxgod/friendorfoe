package com.friendorfoe.presentation.map

import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject

data class MapTarget(
    val id: String,
    val title: String,
    val sourceLabel: String,
    val latitude: Double,
    val longitude: Double,
    val skyObject: SkyObject?,
)

sealed interface MapSurfaceState {
    data object AwaitingLocation : MapSurfaceState
    data object Ready : MapSurfaceState
    data object LocationDenied : MapSurfaceState
    data class TileFailed(val message: String) : MapSurfaceState
    data class NetworkLimited(val cachedAgeMs: Long) : MapSurfaceState
}

sealed interface RemoteSearchState {
    data object Idle : RemoteSearchState
    data class Loading(val target: Position) : RemoteSearchState
    data class Results(val target: Position, val rows: List<MapTarget>) : RemoteSearchState
    data class Empty(val target: Position) : RemoteSearchState
    data class Failed(
        val target: Position,
        val message: String,
        val canRetry: Boolean = true,
    ) : RemoteSearchState
}

data class MapTileHealth(
    val lastSuccessElapsedMs: Long? = null,
    val consecutiveFailures: Int = 0,
    val cachedTileVisible: Boolean = false,
)

sealed interface MapTileEvent {
    data class Succeeded(val cachedTileVisible: Boolean) : MapTileEvent
    data class Failed(val cachedTileVisible: Boolean) : MapTileEvent
}

data class MapTileReduction(
    val health: MapTileHealth,
    val surface: MapSurfaceState,
)

data class MapUiState(
    val filter: FilterState = FilterState(),
    val activeFilterCount: Int = 0,
    val targets: List<MapTarget> = emptyList(),
    val userPosition: Position? = null,
    val mapCenter: Position? = null,
    val surface: MapSurfaceState = MapSurfaceState.AwaitingLocation,
    val remoteSearch: RemoteSearchState = RemoteSearchState.Idle,
)

fun mapSurfaceForLocation(
    userPosition: Position?,
    locationDenied: Boolean,
): MapSurfaceState = when {
    locationDenied -> MapSurfaceState.LocationDenied
    userPosition == null -> MapSurfaceState.AwaitingLocation
    else -> MapSurfaceState.Ready
}

fun beginRemoteSearch(mapCenter: Position?): RemoteSearchState =
    mapCenter?.let { RemoteSearchState.Loading(it) } ?: RemoteSearchState.Idle

fun completeRemoteSearch(
    target: Position,
    rows: List<MapTarget>,
): RemoteSearchState = if (rows.isEmpty()) {
    RemoteSearchState.Empty(target)
} else {
    RemoteSearchState.Results(target, rows)
}

fun failRemoteSearch(target: Position, message: String): RemoteSearchState =
    RemoteSearchState.Failed(target = target, message = message)

fun SkyObject.toMapTarget(sourceLabel: String): MapTarget = MapTarget(
    id = id,
    title = displayLabel(),
    sourceLabel = sourceLabel,
    latitude = position.latitude,
    longitude = position.longitude,
    skyObject = this,
)

fun LocatedDroneDto.toMapTarget(): MapTarget = MapTarget(
    id = droneId,
    title = listOfNotNull(manufacturer, model).joinToString(" ").ifBlank { droneId },
    sourceLabel = "Configured backend",
    latitude = lat,
    longitude = lon,
    skyObject = null,
)

fun reduceMapTileHealth(
    previous: MapTileHealth,
    event: MapTileEvent,
    nowElapsedMs: Long,
): MapTileReduction = when (event) {
    is MapTileEvent.Succeeded -> MapTileReduction(
        health = MapTileHealth(
            lastSuccessElapsedMs = nowElapsedMs,
            consecutiveFailures = 0,
            cachedTileVisible = event.cachedTileVisible,
        ),
        surface = MapSurfaceState.Ready,
    )
    is MapTileEvent.Failed -> {
        val failures = previous.consecutiveFailures + 1
        val health = previous.copy(
            consecutiveFailures = failures,
            cachedTileVisible = event.cachedTileVisible,
        )
        val surface = previous.lastSuccessElapsedMs?.let { lastSuccess ->
            MapSurfaceState.NetworkLimited(
                cachedAgeMs = (nowElapsedMs - lastSuccess).coerceAtLeast(0L),
            )
        } ?: if (failures >= TILE_FAILURE_THRESHOLD) {
            MapSurfaceState.TileFailed("Map tiles are unavailable")
        } else {
            MapSurfaceState.Ready
        }
        MapTileReduction(health = health, surface = surface)
    }
}

private const val TILE_FAILURE_THRESHOLD = 3

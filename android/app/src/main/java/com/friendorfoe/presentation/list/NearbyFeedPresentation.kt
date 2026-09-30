package com.friendorfoe.presentation.list

import com.friendorfoe.detection.AircraftFeedPhase
import com.friendorfoe.detection.AircraftFeedState
import com.friendorfoe.domain.model.SkyObject

data class NearbyFeedPresentation(val label: String, val detail: String?, val canRetry: Boolean, val body: ListBodyState)

fun nearbyFeedPresentation(
    feed: AircraftFeedState,
    aircraftEnabled: Boolean,
    raw: List<SkyObject>,
    visible: List<SkyObject>,
    filterCount: Int,
    nowMs: Long,
): NearbyFeedPresentation {
    val age = feed.lastSuccessMs?.let { (nowMs - it).coerceAtLeast(0) }
    val failure = aircraftEnabled && feed.phase in setOf(AircraftFeedPhase.RECONNECTING, AircraftFeedPhase.OFFLINE)
    val label = when {
        !aircraftEnabled -> "Aircraft feed paused"
        feed.phase == AircraftFeedPhase.WAITING -> "Waiting for location"
        feed.phase == AircraftFeedPhase.CONNECTING -> "Connecting"
        feed.phase == AircraftFeedPhase.LIVE -> "Aircraft live"
        feed.phase == AircraftFeedPhase.RECONNECTING -> "Reconnecting"
        else -> "Aircraft offline"
    }
    val detail = if (feed.rateLimited && feed.retryAtMs != null && feed.retryAtMs > nowMs)
        "Retrying in ${(feed.retryAtMs - nowMs + 999) / 1000}s" else age?.let { "Updated ${if (it < 60_000) "${it / 1000}s" else "${it / 60_000}m"} ago" }
    val resolved = !aircraftEnabled || raw.isNotEmpty() || feed.phase !in setOf(AircraftFeedPhase.WAITING, AircraftFeedPhase.CONNECTING)
    return NearbyFeedPresentation(label, detail, failure && !feed.rateLimited, reduceListBody(
        raw, visible, resolved,
        failure = if (failure) "Aircraft feed unavailable. Local radio detections can still update." else null,
        cacheAgeMs = age,
        activeFilterCount = filterCount,
    ))
}

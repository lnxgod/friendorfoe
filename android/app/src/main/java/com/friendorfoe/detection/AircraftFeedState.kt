package com.friendorfoe.detection

enum class AircraftFeedPhase { WAITING, CONNECTING, LIVE, RECONNECTING, OFFLINE }

data class AircraftFeedState(
    val phase: AircraftFeedPhase = AircraftFeedPhase.WAITING,
    val lastSuccessMs: Long? = null,
    val retryAtMs: Long? = null,
    val rateLimited: Boolean = false,
) {
    fun failed(nowMs: Long, failures: Int, retryMs: Long, rateLimited: Boolean = false) = copy(
        phase = if (lastSuccessMs != null && failures < 3) AircraftFeedPhase.RECONNECTING else AircraftFeedPhase.OFFLINE,
        retryAtMs = nowMs + retryMs,
        rateLimited = rateLimited,
    )
}

fun validAircraftFeedPosition(latitude: Double, longitude: Double): Boolean =
    latitude.isFinite() && latitude in -90.0..90.0 && longitude.isFinite() && longitude in -180.0..180.0 &&
        (latitude != 0.0 || longitude != 0.0)

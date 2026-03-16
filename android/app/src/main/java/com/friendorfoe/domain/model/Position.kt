package com.friendorfoe.domain.model

/**
 * Geographic position of a sky object.
 *
 * @property latitude WGS84 latitude in degrees
 * @property longitude WGS84 longitude in degrees
 * @property altitudeMeters Altitude in meters above mean sea level (AMSL)
 * @property heading Heading in degrees (0-360, true north), null if unknown
 * @property speedMps Ground speed in meters per second, null if unknown
 */
data class Position(
    val latitude: Double,
    val longitude: Double,
    val altitudeMeters: Double,
    val heading: Float? = null,
    val speedMps: Float? = null,
    val verticalRateMps: Float? = null
)

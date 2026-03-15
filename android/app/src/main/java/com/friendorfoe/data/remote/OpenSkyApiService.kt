package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Query

/**
 * Retrofit interface for the OpenSky Network REST API.
 *
 * Provides real-time ADS-B aircraft positions within a geographic bounding box.
 * No authentication required for anonymous access (limited to ~10s between requests).
 *
 * API docs: https://openskynetwork.github.io/opensky-api/rest.html
 */
interface OpenSkyApiService {

    /**
     * Get all state vectors (aircraft) within a bounding box.
     *
     * @param latMin Southern latitude of bounding box
     * @param latMax Northern latitude of bounding box
     * @param lonMin Western longitude of bounding box
     * @param lonMax Eastern longitude of bounding box
     * @return Response containing time and list of state vectors
     */
    @GET("states/all")
    suspend fun getStates(
        @Query("lamin") latMin: Double,
        @Query("lamax") latMax: Double,
        @Query("lomin") lonMin: Double,
        @Query("lomax") lonMax: Double
    ): OpenSkyResponse
}

/**
 * Response from the OpenSky /states/all endpoint.
 *
 * Each state vector is a heterogeneous JSON array with indices:
 *  0: icao24 (String)
 *  1: callsign (String?)
 *  2: origin_country (String)
 *  3: time_position (Number?)
 *  4: last_contact (Number)
 *  5: longitude (Number?)
 *  6: latitude (Number?)
 *  7: baro_altitude (Number?) - meters
 *  8: on_ground (Boolean)
 *  9: velocity (Number?) - m/s ground speed
 * 10: true_track (Number?) - degrees clockwise from north
 * 11: vertical_rate (Number?) - m/s
 * 12: sensors (array?)
 * 13: geo_altitude (Number?) - meters
 * 14: squawk (String?)
 * 15: spi (Boolean)
 * 16: position_source (Number)
 * 17: category (Number) - aircraft category 0-17
 */
data class OpenSkyResponse(
    val time: Long,
    val states: List<List<Any?>>?
)

package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Path
import retrofit2.http.Query

/**
 * Retrofit API service for communicating with the Friend or Foe backend.
 *
 * The backend proxies ADS-B data (OpenSky Network / ADS-B Exchange),
 * enriches aircraft data with photos and routes, and stores history.
 */
interface FriendOrFoeApiService {

    /**
     * Get aircraft within a bounding box around the user's location.
     *
     * @param latitude User's current latitude
     * @param longitude User's current longitude
     * @param radiusNm Search radius in nautical miles (default ~50nm)
     * @return List of aircraft DTOs with position and identity data
     */
    @GET("aircraft/nearby")
    suspend fun getNearbyAircraft(
        @Query("lat") latitude: Double,
        @Query("lon") longitude: Double,
        @Query("radius_nm") radiusNm: Int = 50
    ): List<AircraftDto>

    /**
     * Get detailed information about a specific aircraft by ICAO hex.
     *
     * @param icaoHex ICAO 24-bit hex address
     * @return Enriched aircraft data including photo, route, and operator info
     */
    @GET("aircraft/{icao_hex}")
    suspend fun getAircraftDetail(
        @Path("icao_hex") icaoHex: String
    ): AircraftDetailDto

    /**
     * Report a detected drone to the backend for enrichment.
     *
     * @param latitude User's latitude at time of detection
     * @param longitude User's longitude at time of detection
     * @param droneId Remote ID serial number or WiFi-derived ID
     * @param source Detection source ("remote_id" or "wifi")
     * @return Enriched drone data if available
     */
    @GET("drones/identify")
    suspend fun identifyDrone(
        @Query("lat") latitude: Double,
        @Query("lon") longitude: Double,
        @Query("drone_id") droneId: String,
        @Query("source") source: String
    ): DroneDetailDto?
}

package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName
import retrofit2.http.GET
import retrofit2.http.Path

/**
 * Aircraft data from hexdb.io lookup.
 */
data class HexDbAircraftResponse(
    @SerializedName("ICAOTypeCode") val icaoTypeCode: String?,
    @SerializedName("Manufacturer") val manufacturer: String?,
    @SerializedName("Type") val type: String?,
    @SerializedName("RegisteredOwners") val registeredOwners: String?,
    @SerializedName("Registration") val registration: String?
)

/**
 * Route data from hexdb.io lookup.
 */
data class HexDbRouteResponse(
    val route: String?,
    val callsign: String?
)

/**
 * Retrofit interface for the hexdb.io aircraft database API.
 *
 * Free, no API key required. Handles 1.1M+ requests/day.
 *
 * Base URL: https://hexdb.io/
 */
interface HexDbApiService {

    @GET("api/v1/aircraft/{hex}")
    suspend fun getAircraft(
        @Path("hex") icaoHex: String
    ): HexDbAircraftResponse

    @GET("api/v1/route/icao/{callsign}")
    suspend fun getRoute(
        @Path("callsign") callsign: String
    ): HexDbRouteResponse
}

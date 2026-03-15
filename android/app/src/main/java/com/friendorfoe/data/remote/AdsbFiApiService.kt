package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Path

/**
 * Retrofit interface for the adsb.fi ADS-B API.
 *
 * Free, no API key required. Rate limit ~1 req/sec.
 * Returns ADSBx v2 format with an `ac` array.
 *
 * Base URL: https://opendata.adsb.fi/api/
 */
interface AdsbFiApiService {

    @GET("v3/lat/{lat}/lon/{lon}/dist/{dist}")
    suspend fun getNearby(
        @Path("lat") lat: Double,
        @Path("lon") lon: Double,
        @Path("dist") distNm: Int
    ): AdsbxResponse
}

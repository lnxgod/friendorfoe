package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Path

/**
 * Retrofit interface for the ADSB One ADS-B API.
 *
 * Free, no API key required. Rate limit ~1 req/sec.
 * Returns ADSBx v2 format with an `ac` array.
 *
 * Base URL: https://api.adsb.one/
 */
interface AdsbOneApiService {

    @GET("v2/point/{lat}/{lon}/{radius}")
    suspend fun getNearby(
        @Path("lat") lat: Double,
        @Path("lon") lon: Double,
        @Path("radius") radiusNm: Int
    ): AdsbxResponse
}

package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Path

/**
 * Retrofit interface for the airplanes.live ADS-B API.
 *
 * Free community API, no API key required.
 * Returns ADSBx v2 format with an `ac` array.
 *
 * Base URL: https://api.airplanes.live/
 */
interface AirplanesLiveApiService {

    @GET("v2/point/{lat}/{lon}/{radius}")
    suspend fun getNearby(
        @Path("lat") lat: Double,
        @Path("lon") lon: Double,
        @Path("radius") radiusNm: Int
    ): AdsbxResponse
}

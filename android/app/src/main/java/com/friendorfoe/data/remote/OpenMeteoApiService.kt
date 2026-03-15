package com.friendorfoe.data.remote

import retrofit2.http.GET
import retrofit2.http.Query

/**
 * Retrofit interface for Open-Meteo weather API (free, no API key required).
 */
interface OpenMeteoApiService {

    @GET("v1/forecast")
    suspend fun getCurrentWeather(
        @Query("latitude") lat: Double,
        @Query("longitude") lon: Double,
        @Query("current") current: String = "weather_code,cloud_cover,wind_speed_10m"
    ): OpenMeteoResponse
}

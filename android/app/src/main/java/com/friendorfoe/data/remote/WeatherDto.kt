package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName

/** Raw Open-Meteo API response. */
data class OpenMeteoResponse(val current: OpenMeteoCurrentData?)

/** Current weather fields from Open-Meteo. */
data class OpenMeteoCurrentData(
    @SerializedName("weather_code") val weatherCode: Int,
    @SerializedName("cloud_cover") val cloudCover: Int,
    @SerializedName("wind_speed_10m") val windSpeed10m: Float
)

/** Normalized weather conditions used internally by the app. */
data class WeatherConditionsDto(
    val visibilityMeters: Int,
    val cloudCoverPercent: Int,
    val precipitationType: String?,
    val windSpeedMps: Float,
    val description: String
)

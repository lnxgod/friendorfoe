package com.friendorfoe.data.repository

import android.util.Log
import com.friendorfoe.data.remote.OpenMeteoApiService
import com.friendorfoe.data.remote.WeatherConditionsDto
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.abs

/**
 * On-device weather repository that fetches directly from Open-Meteo.
 *
 * Caches results in memory for 10 minutes / ~10 km to avoid redundant calls.
 * On network failure, returns cached value or null (caller falls back to manual override).
 */
@Singleton
class WeatherRepository @Inject constructor(
    private val openMeteoApiService: OpenMeteoApiService
) {
    companion object {
        private const val TAG = "WeatherRepository"
        private const val CACHE_TTL_MS = 10 * 60 * 1000L // 10 minutes
        private const val CACHE_DISTANCE_DEG = 0.1 // ~10 km
    }

    @Volatile private var cachedResult: WeatherConditionsDto? = null
    @Volatile private var cachedLat: Double = 0.0
    @Volatile private var cachedLon: Double = 0.0
    @Volatile private var cachedTimestamp: Long = 0L

    suspend fun getWeather(lat: Double, lon: Double): WeatherConditionsDto? {
        // Return cache if fresh and nearby
        val now = System.currentTimeMillis()
        val cached = cachedResult
        if (cached != null
            && (now - cachedTimestamp) < CACHE_TTL_MS
            && abs(lat - cachedLat) < CACHE_DISTANCE_DEG
            && abs(lon - cachedLon) < CACHE_DISTANCE_DEG
        ) {
            Log.d(TAG, "Weather cache hit")
            return cached
        }

        return try {
            val response = openMeteoApiService.getCurrentWeather(lat, lon)
            val current = response.current ?: return cached // stale cache better than nothing
            val result = parseWeather(current.weatherCode, current.cloudCover, current.windSpeed10m)

            // Update cache
            cachedResult = result
            cachedLat = lat
            cachedLon = lon
            cachedTimestamp = now
            result
        } catch (e: Exception) {
            Log.w(TAG, "Weather fetch failed, returning cached/null", e)
            cached // may be null
        }
    }

    // ----- WMO weather code mappings (ported from backend weather.py) -----

    private fun parseWeather(weatherCode: Int, cloudCover: Int, windSpeedKmh: Float): WeatherConditionsDto {
        val visibility = estimateVisibility(cloudCover, weatherCode)
        val precipitationType = WMO_PRECIP_MAP[weatherCode]
        val description = WMO_DESC_MAP[weatherCode] ?: "unknown"
        val windSpeedMps = windSpeedKmh / 3.6f

        return WeatherConditionsDto(
            visibilityMeters = visibility,
            cloudCoverPercent = cloudCover,
            precipitationType = precipitationType,
            windSpeedMps = windSpeedMps,
            description = description
        )
    }

    private fun estimateVisibility(cloudCover: Int, weatherCode: Int): Int = when {
        weatherCode in setOf(45, 48) -> 300          // fog
        weatherCode in setOf(65, 67, 75, 82, 86, 99) -> 1500  // heavy precip
        weatherCode in setOf(53, 55, 56, 57, 63, 66, 73, 81, 85, 95, 96) -> 4000 // moderate
        weatherCode in setOf(51, 61, 71, 77, 80) -> 6000  // light precip
        cloudCover > 90 -> 8000
        cloudCover > 60 -> 9000
        else -> 10000
    }

    private val WMO_PRECIP_MAP = mapOf(
        0 to null, 1 to null, 2 to null, 3 to null,
        45 to "Fog", 48 to "Fog",
        51 to "Drizzle", 53 to "Drizzle", 55 to "Drizzle",
        56 to "Drizzle", 57 to "Drizzle",
        61 to "Rain", 63 to "Rain", 65 to "Rain",
        66 to "Rain", 67 to "Rain",
        71 to "Snow", 73 to "Snow", 75 to "Snow",
        77 to "Snow",
        80 to "Rain", 81 to "Rain", 82 to "Rain",
        85 to "Snow", 86 to "Snow",
        95 to "Thunderstorm",
        96 to "Thunderstorm", 99 to "Thunderstorm"
    )

    private val WMO_DESC_MAP = mapOf(
        0 to "clear sky", 1 to "mainly clear", 2 to "partly cloudy", 3 to "overcast",
        45 to "fog", 48 to "depositing rime fog",
        51 to "light drizzle", 53 to "moderate drizzle", 55 to "dense drizzle",
        56 to "light freezing drizzle", 57 to "dense freezing drizzle",
        61 to "slight rain", 63 to "moderate rain", 65 to "heavy rain",
        66 to "light freezing rain", 67 to "heavy freezing rain",
        71 to "slight snow", 73 to "moderate snow", 75 to "heavy snow",
        77 to "snow grains",
        80 to "slight rain showers", 81 to "moderate rain showers", 82 to "violent rain showers",
        85 to "slight snow showers", 86 to "heavy snow showers",
        95 to "thunderstorm", 96 to "thunderstorm with slight hail", 99 to "thunderstorm with heavy hail"
    )
}

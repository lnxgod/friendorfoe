package com.friendorfoe.detection

/**
 * Converts weather visibility data into a confidence multiplier for visual detection.
 *
 * Visibility ranges:
 * - Clear (10km+): multiplier 1.0
 * - Haze (5-10km): multiplier 0.9
 * - Overcast/light rain (1-5km): multiplier 0.7
 * - Fog (<1km): multiplier 0.5
 * - Heavy fog (<200m): multiplier 0.3
 */
object WeatherAdjustment {

    @Suppress("UNUSED_PARAMETER")
    fun fromWeatherConditions(
        visibilityMeters: Int,
        cloudCoverPercent: Int,
        precipitationType: String?
    ): VisualDetectionRange {
        val (multiplier, rangeDesc) = when {
            visibilityMeters < 200 -> 0.3f to "Heavy fog"
            visibilityMeters < 1000 -> 0.5f to "Fog"
            visibilityMeters < 5000 -> 0.7f to "Reduced"
            visibilityMeters < 10000 -> 0.9f to "Haze"
            else -> 1.0f to "Clear"
        }

        // Further reduce for active precipitation
        val precipMultiplier = when (precipitationType?.lowercase()) {
            "rain", "drizzle" -> 0.9f
            "snow" -> 0.8f
            "thunderstorm" -> 0.7f
            else -> 1.0f
        }

        val finalMultiplier = (multiplier * precipMultiplier).coerceIn(0.2f, 1.0f)

        val description = if (precipitationType != null && precipitationType.lowercase() != "clear") {
            "$rangeDesc, $precipitationType"
        } else {
            rangeDesc
        }

        return VisualDetectionRange(
            effectiveRangeMeters = visibilityMeters,
            confidenceMultiplier = finalMultiplier,
            description = description
        )
    }
}

/** Visual detection range adjusted for current weather conditions. */
data class VisualDetectionRange(
    /** Effective visual detection range in meters */
    val effectiveRangeMeters: Int,
    /** Confidence multiplier for sky object filter (0.2-1.0) */
    val confidenceMultiplier: Float,
    /** Human-readable description */
    val description: String
)

package com.friendorfoe.detection

import java.time.Instant

object GlassesStalePolicy {
    private const val DEFAULT_TTL_SECONDS = 60L
    private const val HIGH_RISK_TTL_SECONDS = 300L

    fun ttlSeconds(detection: GlassesDetection): Long {
        val manufacturer = detection.manufacturer
        val type = detection.deviceType
        val highRisk = manufacturer.contains("Meta", ignoreCase = true) ||
            manufacturer.contains("Ray-Ban", ignoreCase = true) ||
            manufacturer.contains("Oakley", ignoreCase = true) ||
            manufacturer.contains("Luxottica", ignoreCase = true) ||
            type.contains("Quest", ignoreCase = true) ||
            type.contains("Smart Glasses", ignoreCase = true) ||
            type.contains("Flipper", ignoreCase = true) ||
            type.contains("Pwnagotchi", ignoreCase = true) ||
            type.contains("AirTag", ignoreCase = true) ||
            type.contains("Tile", ignoreCase = true) ||
            type.contains("SmartTag", ignoreCase = true)
        return if (highRisk) HIGH_RISK_TTL_SECONDS else DEFAULT_TTL_SECONDS
    }

    fun isStale(detection: GlassesDetection, now: Instant): Boolean {
        val deadline = detection.lastSeen.plusSeconds(ttlSeconds(detection))
        return !now.isBefore(deadline)
    }

    fun nextExpiryDelayMillis(
        detections: Collection<GlassesDetection>,
        now: Instant,
    ): Long? = detections.minOfOrNull { detection ->
        val deadline = detection.lastSeen.plusSeconds(ttlSeconds(detection))
        (deadline.toEpochMilli() - now.toEpochMilli()).coerceAtLeast(1L)
    }
}

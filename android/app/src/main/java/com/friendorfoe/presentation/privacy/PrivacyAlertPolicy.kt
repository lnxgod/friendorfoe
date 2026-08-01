package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant

data class PrivacyAlertCandidate(
    val key: String,
    val title: String,
    val body: String,
    val threatLevel: Int,
    val macs: Set<String> = emptySet(),
    val isBonded: Boolean = false
)

class PrivacyAlertPolicy(
    private val cooldownMs: Long = 10 * 60 * 1000L
) {
    private val lastNotifiedAt = mutableMapOf<String, Long>()

    fun shouldNotify(
        candidate: PrivacyAlertCandidate,
        ignoredMacs: Set<String>,
        nowMs: Long = System.currentTimeMillis()
    ): Boolean {
        if (candidate.threatLevel < 2) return false
        if (candidate.isBonded) return false
        if (candidate.macs.any { it in ignoredMacs }) return false
        val last = lastNotifiedAt[candidate.key]
        if (last != null && nowMs - last < cooldownMs) return false
        lastNotifiedAt[candidate.key] = nowMs
        return true
    }

    fun reset() {
        lastNotifiedAt.clear()
    }

    companion object {
        fun fromDetection(detection: GlassesDetection): PrivacyAlertCandidate? {
            val normalized = PrivacyFindingNormalizer.normalize(detection)
            if (normalized.category == PrivacyCategory.APPLE_CONTINUITY) return null
            if (normalized.category.threatLevel < 2 || normalized.isBonded) return null
            val label = normalized.deviceName?.takeIf { it.isNotBlank() }
                ?: normalized.deviceType
            return PrivacyAlertCandidate(
                key = listOf(
                    normalized.fingerprintKey.ifBlank { "mac:${normalized.mac}" },
                    normalized.category.name,
                    normalized.matchReason
                ).joinToString(":"),
                title = "${normalized.category.label} detected",
                body = "$label nearby (${normalized.rssi} dBm)",
                threatLevel = normalized.category.threatLevel,
                macs = normalized.seenMacs + normalized.mac,
                isBonded = normalized.isBonded
            )
        }

        fun wifiAnomaly(
            type: String,
            ssid: String,
            details: String,
            threatLevel: Int,
            bssids: List<String>
        ) = PrivacyAlertCandidate(
            key = "wifi:$type:${bssids.sorted().joinToString(",")}:$ssid",
            title = when (type) {
                "pwnagotchi" -> "Pwnagotchi detected"
                "evil_twin" -> "Evil twin WiFi detected"
                "karma_attack" -> "Karma WiFi attack detected"
                else -> "WiFi anomaly detected"
            },
            body = details,
            threatLevel = threatLevel,
            macs = bssids.toSet()
        )

        fun ultrasonic(
            frequencyHz: Float,
            snrDb: Float,
            persistenceFrames: Int,
            threatLevel: Int = 3
        ) = PrivacyAlertCandidate(
            key = "ultrasonic:${(frequencyHz / 100f).toInt()}",
            title = "Ultrasonic beacon detected",
            body = "${"%.0f".format(frequencyHz)} Hz, SNR ${"%.1f".format(snrDb)} dB, $persistenceFrames frames",
            threatLevel = threatLevel
        )

        fun stalker(
            mac: String,
            label: String,
            reason: String,
            threatLevel: Int
        ) = PrivacyAlertCandidate(
            key = "stalker:$reason:$mac",
            title = "BLE follower alert",
            body = "$label appears to be $reason",
            threatLevel = threatLevel,
            macs = setOf(mac)
        )

        fun timestampKey(prefix: String, timestamp: Instant): String =
            "$prefix:${timestamp.toEpochMilli()}"
    }
}

package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.canonicalPrivacyIdentities
import com.friendorfoe.detection.canonicalPrivacyIdentity
import com.friendorfoe.detection.canonicalPrivacyIdentityAliases
import com.friendorfoe.detection.privacyIdentityIsIgnored
import com.friendorfoe.presentation.components.FofTone
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

data class PrivacyAlertCandidate(
    val key: String,
    val title: String,
    val body: String,
    val threatLevel: Int,
    val macs: Set<String> = emptySet(),
    val isBonded: Boolean = false,
)

data class StalkerAlertPresentation(
    val title: String,
    val tone: FofTone,
)

internal fun GlassesDetection.isSupportedPrivacyFinding(): Boolean =
    !matchReason.startsWith("ble_behavioral:") || matchReason == "ble_behavioral:pairing_spam"

@Singleton
class PrivacyAlertPolicy internal constructor(
    private val cooldownMs: Long,
) {
    @Inject
    constructor() : this(DEFAULT_COOLDOWN_MS)

    private data class Published(val severity: FindingSeverity, val atElapsedMs: Long)

    private var activeKeys = emptySet<PrivacyFindingKey>()
    private val published = mutableMapOf<PrivacyFindingKey, Published>()
    private val lastNotifiedAt = mutableMapOf<String, Long>()

    @Synchronized
    fun shouldNotify(
        candidate: PrivacyAlertCandidate,
        ignoredMacs: Set<String>,
        nowMs: Long = System.currentTimeMillis(),
    ): Boolean {
        if (candidate.threatLevel < 2) return false
        if (candidate.isBonded) return false
        if (privacyIdentityIsIgnored(candidate.macs, ignoredMacs)) return false
        val last = lastNotifiedAt[candidate.key]
        if (last != null && elapsedSince(last, nowMs) < cooldownMs) return false
        lastNotifiedAt[candidate.key] = nowMs
        return true
    }

    @Synchronized
    fun newAlerts(
        alertEligible: List<PrivacyFinding>,
        nowElapsedMs: Long,
    ): List<PrivacyFinding> {
        val currentKeys = alertEligible.mapNotNull(PrivacyFinding::routableKey).toSet()
        val alerts = alertEligible.filter { finding ->
            val key = finding.routableKey ?: return@filter false
            val prior = published[key]
            val edge = key !in activeKeys
            val unpublished = prior == null
            val severityRose = prior != null && finding.severity.rank > prior.severity.rank
            val cooldownElapsed = prior == null ||
                elapsedSince(prior.atElapsedMs, nowElapsedMs) >= cooldownMs
            (edge || unpublished || severityRose) && cooldownElapsed
        }
        activeKeys = currentKeys
        return alerts
    }

    @Synchronized
    fun markPublished(finding: PrivacyFinding, nowElapsedMs: Long) {
        val key = finding.routableKey ?: return
        published[key] = Published(finding.severity, nowElapsedMs)
        activeKeys = activeKeys + key
    }

    @Synchronized
    fun reset() {
        activeKeys = emptySet()
        published.clear()
        lastNotifiedAt.clear()
    }

    private fun elapsedSince(then: Long, now: Long): Long =
        if (now >= then) now - then else Long.MAX_VALUE

    companion object {
        private const val DEFAULT_COOLDOWN_MS = 10 * 60 * 1_000L

        fun fromDetection(detection: GlassesDetection): PrivacyAlertCandidate? {
            if (!detection.isSupportedPrivacyFinding()) return null
            val normalized = PrivacyFindingNormalizer.normalize(detection)
            if (normalized.category.threatLevel < 2) return null
            if (normalized.category == PrivacyCategory.INFORMATIONAL) return null
            val label = normalized.deviceName?.takeIf { it.isNotBlank() }
                ?: normalized.deviceType
            return PrivacyAlertCandidate(
                key = listOf(
                    canonicalPrivacyIdentity(normalized.fingerprintKey)
                        ?: canonicalPrivacyIdentity(normalized.mac)
                        ?: normalized.mac,
                    normalized.category.name,
                    normalized.matchReason,
                ).joinToString(":"),
                title = "${normalized.category.label} detected",
                body = "$label nearby (${normalized.rssi} dBm)",
                threatLevel = normalized.category.threatLevel,
                macs = normalized.canonicalPrivacyIdentityAliases(),
                isBonded = normalized.isBonded,
            )
        }

        fun wifiAnomaly(
            type: String,
            ssid: String,
            details: String,
            threatLevel: Int,
            bssids: List<String>,
        ) = PrivacyAlertCandidate(
            key = "wifi:$type:${canonicalPrivacyIdentities(bssids).sorted().joinToString(",")}:$ssid",
            title = when (type) {
                "pwnagotchi" -> "Pwnagotchi detected"
                "evil_twin" -> "Evil twin WiFi detected"
                "karma_attack" -> "Karma WiFi attack detected"
                else -> "WiFi anomaly detected"
            },
            body = details,
            threatLevel = threatLevel,
            macs = canonicalPrivacyIdentities(bssids),
        )

        fun ultrasonic(
            frequencyHz: Float,
            snrDb: Float,
            persistenceFrames: Int,
            threatLevel: Int = 3,
        ) = PrivacyAlertCandidate(
            key = "ultrasonic:${(frequencyHz / 100f).toInt()}",
            title = "Ultrasonic beacon detected",
            body = "${"%.0f".format(frequencyHz)} Hz, " +
                "SNR ${"%.1f".format(snrDb)} dB, $persistenceFrames frames",
            threatLevel = threatLevel,
        )

        fun stalker(
            mac: String,
            label: String,
            reason: String,
            threatLevel: Int,
        ): PrivacyAlertCandidate {
            val presentation = stalkerPresentation(reason, threatLevel)
            val canonicalMac = canonicalPrivacyIdentity(mac) ?: mac
            val effectiveThreatLevel = if (reason == "following" && threatLevel in 2..3) {
                threatLevel
            } else {
                1
            }
            return PrivacyAlertCandidate(
                key = "stalker:$reason:$canonicalMac",
                title = presentation.title,
                body = "$label appears to be $reason",
                threatLevel = effectiveThreatLevel,
                macs = setOf(canonicalMac),
            )
        }

        fun stalkerPresentation(reason: String, threatLevel: Int): StalkerAlertPresentation =
            if (reason == "following" && threatLevel in 2..3) {
                StalkerAlertPresentation(
                    title = "Follower alert",
                    tone = FofTone.Danger,
                )
            } else {
                StalkerAlertPresentation(
                    title = "Nearby device",
                    tone = FofTone.Neutral,
                )
            }

        fun timestampKey(prefix: String, timestamp: Instant): String =
            "$prefix:${timestamp.toEpochMilli()}"
    }
}

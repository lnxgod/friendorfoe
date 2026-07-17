package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.components.FofTone
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class PrivacyAlertPolicyTest {

    @Test
    fun high_risk_detection_notifies_once_per_cooldown() {
        val policy = PrivacyAlertPolicy(cooldownMs = 600_000)
        val candidate = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.HIDDEN_CAMERA)
        )

        assertNotNull(candidate)
        assertTrue(policy.shouldNotify(candidate!!, ignoredMacs = emptySet(), nowMs = 1_000))
        assertFalse(policy.shouldNotify(candidate, ignoredMacs = emptySet(), nowMs = 2_000))
        assertTrue(policy.shouldNotify(candidate, ignoredMacs = emptySet(), nowMs = 602_000))
    }

    @Test
    fun suppresses_informational_bonded_and_ignored_devices() {
        assertNull(
            PrivacyAlertPolicy.fromDetection(
                detection(category = PrivacyCategory.INFORMATIONAL)
            )
        )

        val bonded = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.SMART_GLASSES, isBonded = true)
        )!!
        assertFalse(PrivacyAlertPolicy().shouldNotify(bonded, ignoredMacs = emptySet()))

        val ignored = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.ATTACK_TOOL)
        )!!
        assertFalse(
            PrivacyAlertPolicy().shouldNotify(
                ignored,
                ignoredMacs = setOf("AA:BB:CC:00:00:01")
            )
        )
    }

    @Test
    fun wifi_and_stalker_candidates_respect_severity_threshold() {
        val policy = PrivacyAlertPolicy()
        val wifi = PrivacyAlertPolicy.wifiAnomaly(
            type = "evil_twin",
            ssid = "Cafe",
            details = "Mixed OPEN + WPA2",
            threatLevel = 3,
            bssids = listOf("AA:BB:CC:00:00:02")
        )
        val stalkerLow = PrivacyAlertPolicy.stalker(
            mac = "AA:BB:CC:00:00:03",
            label = "Tag",
            reason = "following",
            threatLevel = 1
        )

        assertTrue(policy.shouldNotify(wifi, ignoredMacs = emptySet()))
        assertFalse(policy.shouldNotify(stalkerLow, ignoredMacs = emptySet()))
    }

    @Test
    fun lingering_candidate_is_nearby_neutral_and_never_notifies() {
        val presentation = PrivacyAlertPolicy.stalkerPresentation(
            reason = "lingering",
            threatLevel = 1,
        )
        val candidate = PrivacyAlertPolicy.stalker(
            mac = "AA:BB:CC:00:00:03",
            label = "Tag",
            reason = "lingering",
            threatLevel = 3,
        )

        assertEquals("Nearby device", presentation.title)
        assertEquals(FofTone.Neutral, presentation.tone)
        assertEquals("Nearby device", candidate.title)
        assertEquals(1, candidate.threatLevel)
        assertFalse(PrivacyAlertPolicy().shouldNotify(candidate, ignoredMacs = emptySet()))
    }

    @Test
    fun sustained_following_candidate_is_a_danger_alert() {
        listOf(2, 3).forEach { threatLevel ->
            val presentation = PrivacyAlertPolicy.stalkerPresentation(
                reason = "following",
                threatLevel = threatLevel,
            )
            val candidate = PrivacyAlertPolicy.stalker(
                mac = "AA:BB:CC:00:00:03",
                label = "Tag",
                reason = "following",
                threatLevel = threatLevel,
            )

            assertEquals("Follower alert", presentation.title)
            assertEquals(FofTone.Danger, presentation.tone)
            assertEquals("Follower alert", candidate.title)
            assertEquals(threatLevel, candidate.threatLevel)
        }
    }

    private fun detection(
        category: PrivacyCategory,
        isBonded: Boolean = false
    ) = GlassesDetection(
        mac = "AA:BB:CC:00:00:01",
        deviceName = "Test Device",
        deviceType = category.label,
        manufacturer = "Test",
        hasCamera = category.threatLevel >= 2,
        rssi = -50,
        confidence = 0.9f,
        matchReason = "test",
        firstSeen = Instant.parse("2026-06-11T12:00:00Z"),
        lastSeen = Instant.parse("2026-06-11T12:00:10Z"),
        category = category,
        isBonded = isBonded,
        fingerprintKey = "fp:test",
        seenMacs = setOf("AA:BB:CC:00:00:01")
    )
}

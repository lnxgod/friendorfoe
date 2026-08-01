package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
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

        assertNull(
            PrivacyAlertPolicy.fromDetection(
                detection(category = PrivacyCategory.SMART_GLASSES, isBonded = true)
            )
        )

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
    fun appleListeningClaimCannotBecomeAlertCandidate() {
        val candidate = PrivacyAlertPolicy.fromDetection(
            detection(
                category = PrivacyCategory.REMOTE_LISTENING,
                manufacturer = "Apple",
                deviceType = "Possible Remote Listening",
                matchReason = "backend:REMOTE_LISTENING",
                bleCompanyId = 0x004C,
            )
        )

        assertNull(candidate)
    }

    private fun detection(
        category: PrivacyCategory,
        isBonded: Boolean = false,
        manufacturer: String = "Test",
        deviceType: String = category.label,
        matchReason: String = "test",
        bleCompanyId: Int? = null,
    ) = GlassesDetection(
        mac = "AA:BB:CC:00:00:01",
        deviceName = "Test Device",
        deviceType = deviceType,
        manufacturer = manufacturer,
        hasCamera = category.threatLevel >= 2,
        rssi = -50,
        confidence = 0.9f,
        matchReason = matchReason,
        firstSeen = Instant.parse("2026-06-11T12:00:00Z"),
        lastSeen = Instant.parse("2026-06-11T12:00:10Z"),
        details = emptyMap(),
        category = category,
        isBonded = isBonded,
        bleCompanyId = bleCompanyId,
        bleAppleType = null,
        bleAppleFlags = null,
        bleAppleAction = null,
        bleAppleIosVersion = null,
        bleAdvFlags = null,
        bleDualModeHost = false,
        bleJa3Hash = null,
        bleServiceUuids = emptyList(),
        bleAppearance = null,
        bleLocalName = null,
        fingerprintKey = "fp:test",
        seenMacs = setOf("AA:BB:CC:00:00:01")
    )
}

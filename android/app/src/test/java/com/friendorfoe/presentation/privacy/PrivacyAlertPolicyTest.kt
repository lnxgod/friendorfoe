package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.components.FofTone
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyAlertPolicyTest {
    @Test
    fun unchangedEligibleFindingDoesNotRepeatOnFreshnessTicks() {
        val policy = PrivacyAlertPolicy(cooldownMs = 300_000L)
        val finding = finding("one", FindingSeverity.CRITICAL)

        assertEquals(listOf(finding), policy.newAlerts(listOf(finding), nowElapsedMs = 1_000L))
        policy.markPublished(finding, nowElapsedMs = 1_000L)

        assertTrue(policy.newAlerts(listOf(finding), nowElapsedMs = 2_000L).isEmpty())
        assertTrue(policy.newAlerts(listOf(finding), nowElapsedMs = 302_000L).isEmpty())
    }

    @Test
    fun reentryInsideCooldownIsSuppressedButLaterSeverityRiseCanPublish() {
        val policy = PrivacyAlertPolicy(cooldownMs = 300_000L)
        val awareness = finding("one", FindingSeverity.AWARENESS)
        val critical = awareness.copy(severity = FindingSeverity.CRITICAL)

        policy.markPublished(awareness, nowElapsedMs = 1_000L)
        policy.newAlerts(emptyList(), nowElapsedMs = 2_000L)
        assertTrue(policy.newAlerts(listOf(awareness), nowElapsedMs = 3_000L).isEmpty())
        assertTrue(policy.newAlerts(listOf(critical), nowElapsedMs = 4_000L).isEmpty())
        assertEquals(
            listOf(critical),
            policy.newAlerts(listOf(critical), nowElapsedMs = 301_000L),
        )
    }

    @Test
    fun highRiskDetectionNotifiesOncePerCooldown() {
        val policy = PrivacyAlertPolicy(cooldownMs = 600_000L)
        val candidate = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.HIDDEN_CAMERA),
        )

        assertNotNull(candidate)
        assertTrue(policy.shouldNotify(candidate!!, ignoredMacs = emptySet(), nowMs = 1_000L))
        assertFalse(policy.shouldNotify(candidate, ignoredMacs = emptySet(), nowMs = 2_000L))
        assertTrue(policy.shouldNotify(candidate, ignoredMacs = emptySet(), nowMs = 602_000L))
    }

    @Test
    fun legacySerialSkimmerDetectionCannotCreateAnAlertCandidate() {
        val legacySkimmer = detection(category = PrivacyCategory.ATTACK_TOOL).copy(
            deviceType = "Possible Serial Skimmer",
            matchReason = "ble_behavioral:serial_skimmer",
        )

        assertNull(PrivacyAlertPolicy.fromDetection(legacySkimmer))
    }

    @Test
    fun suppressesInformationalBondedAndCanonicalIgnoredDevices() {
        assertNull(
            PrivacyAlertPolicy.fromDetection(
                detection(category = PrivacyCategory.INFORMATIONAL),
            ),
        )

        val bonded = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.SMART_GLASSES, isBonded = true),
        )!!
        assertFalse(PrivacyAlertPolicy().shouldNotify(bonded, ignoredMacs = emptySet()))

        val ignored = PrivacyAlertPolicy.fromDetection(
            detection(category = PrivacyCategory.ATTACK_TOOL),
        )!!
        assertFalse(
            PrivacyAlertPolicy().shouldNotify(
                ignored,
                ignoredMacs = setOf("MAC:aa:bb:cc:00:00:01"),
            ),
        )
        assertFalse(
            PrivacyAlertPolicy().shouldNotify(
                ignored,
                ignoredMacs = setOf("FP:TEST"),
            ),
        )
    }

    @Test
    fun legacyAppleListeningDetectionCannotCreateAnAlertCandidate() {
        val apple = detection(category = PrivacyCategory.REMOTE_LISTENING).copy(
            deviceName = "Test iPhone",
            deviceType = "Possible iPhone listening",
            manufacturer = "Apple",
        )

        assertNull(PrivacyAlertPolicy.fromDetection(apple))
    }

    @Test
    fun wifiAndStalkerCandidatesRespectSeverityThreshold() {
        val policy = PrivacyAlertPolicy()
        val wifi = PrivacyAlertPolicy.wifiAnomaly(
            type = "evil_twin",
            ssid = "Cafe",
            details = "Mixed OPEN + WPA2",
            threatLevel = 3,
            bssids = listOf("AA:BB:CC:00:00:02"),
        )
        val stalkerLow = PrivacyAlertPolicy.stalker(
            mac = "AA:BB:CC:00:00:03",
            label = "Tag",
            reason = "following",
            threatLevel = 1,
        )

        assertTrue(policy.shouldNotify(wifi, ignoredMacs = emptySet()))
        assertFalse(policy.shouldNotify(stalkerLow, ignoredMacs = emptySet()))
    }

    @Test
    fun lingeringCandidateIsNearbyNeutralAndNeverNotifies() {
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
    fun sustainedFollowingCandidateIsADangerAlert() {
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

    private fun finding(id: String, severity: FindingSeverity) = PrivacyFinding(
        displayId = id,
        observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "observation:$id"),
        source = PrivacySourceKind.BACKEND,
        stableSourceId = "stable:$id",
        routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:$id"),
        title = "Critical privacy finding",
        evidence = "Current evidence",
        limitation = null,
        category = PrivacyCategory.HIDDEN_CAMERA,
        severity = severity,
        ownership = Ownership.UNKNOWN,
        signalDbm = null,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = false,
    )

    private fun detection(
        category: PrivacyCategory,
        isBonded: Boolean = false,
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
        seenMacs = setOf("AA:BB:CC:00:00:01"),
    )
}

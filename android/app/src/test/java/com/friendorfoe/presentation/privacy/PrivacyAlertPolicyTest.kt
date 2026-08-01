package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
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
}

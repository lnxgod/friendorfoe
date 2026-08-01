package com.friendorfoe.presentation.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PrivacyCapabilityTest {
    @Test
    fun livePhoneBleWithStableIdentityAndLocalSamplesEnablesAllActions() {
        assertEquals(
            PrivacyCapabilities(
                canIgnore = true,
                canTrack = true,
                canOpenDirectionSweep = true,
            ),
            capabilitiesFor(
                source = PrivacySourceKind.PHONE_BLE,
                stableId = "fp:one",
                hasLiveLocalSamples = true,
                freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE,
            ),
        )
    }

    @Test
    fun remoteRowWithoutStableIdentityHasNoActions() {
        assertEquals(
            PrivacyCapabilities(),
            capabilitiesFor(
                source = PrivacySourceKind.BACKEND,
                stableId = null,
                hasLiveLocalSamples = false,
                freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE,
            ),
        )
    }

    @Test
    fun badgeStableIdentityEnablesIgnoreButNotPhoneOnlyActions() {
        assertEquals(
            PrivacyCapabilities(canIgnore = true),
            capabilitiesFor(
                source = PrivacySourceKind.BADGE_USB,
                stableId = "entity:7",
                hasLiveLocalSamples = false,
                freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE,
            ),
        )
    }

    @Test
    fun stalePhoneRowKeepsRestoreableIgnoreIdentityButLosesLiveActions() {
        assertEquals(
            PrivacyCapabilities(canIgnore = true),
            capabilitiesFor(
                source = PrivacySourceKind.PHONE_BLE,
                stableId = "fp:one",
                hasLiveLocalSamples = true,
                freshness = FindingFreshness.STALE,
                sourceHealth = SourceHealthState.STALE,
            ),
        )
    }

    @Test
    fun blankStableIdentityDoesNotCreateAPreferenceKey() {
        val finding = finding(stableSourceId = "   ")

        assertNull(finding.ignoreKey)
        assertEquals(
            PrivacyCapabilities(),
            capabilitiesFor(
                source = PrivacySourceKind.PHONE_BLE,
                stableId = "   ",
                hasLiveLocalSamples = true,
                freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE,
            ),
        )
    }

    @Test
    fun malformedStableIdentityDoesNotAdvertiseIgnoreCapability() {
        val malformed = "stable\u001Fextra"
        val finding = finding(stableSourceId = malformed)

        assertNull(finding.ignoreKey)
        assertEquals(
            PrivacyCapabilities(),
            capabilitiesFor(
                source = PrivacySourceKind.PHONE_BLE,
                stableId = malformed,
                hasLiveLocalSamples = true,
                freshness = FindingFreshness.LIVE,
                sourceHealth = SourceHealthState.LIVE,
            ),
        )
    }

    private fun finding(stableSourceId: String?) = PrivacyFinding(
        displayId = "display",
        observationKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "observation"),
        source = PrivacySourceKind.PHONE_BLE,
        stableSourceId = stableSourceId,
        routableKey = null,
        title = "Phone row",
        evidence = null,
        limitation = null,
        category = com.friendorfoe.detection.PrivacyCategory.INFORMATIONAL,
        severity = FindingSeverity.INFO,
        ownership = Ownership.UNKNOWN,
        signalDbm = null,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 10_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = true,
    )
}

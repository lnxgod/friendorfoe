package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

class PrivacyFindingModelNormalizerTest {
    @Test
    fun sameRowAppleAirPodsAndListeningEvidenceBecomesInformational() {
        val input = finding(
            title = "Possible iPhone listening alert",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = true,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        val normalized = PrivacyFindingNormalizer.normalize(input)

        assertEquals("AirPods connection/activity nearby", normalized.title)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals(FindingSeverity.INFO, normalized.severity)
        assertEquals(
            "An Apple device reports connected AirPods and media, call, or video activity.",
            normalized.evidence,
        )
        assertEquals(
            "Live Listen and microphone use cannot be determined from BLE.",
            normalized.limitation,
        )
    }

    @Test
    fun appleListeningEvidenceWithoutAirPodsUsesGenericActivityWording() {
        val input = finding(
            title = "Possible listening",
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = false,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        val normalized = PrivacyFindingNormalizer.normalize(input)

        assertEquals("Apple device activity nearby", normalized.title)
        assertEquals(
            "An Apple device reports a nearby activity state; the specific activity is unavailable.",
            normalized.evidence,
        )
        assertEquals(FindingSeverity.INFO, normalized.severity)
    }

    @Test
    fun safeOwnedNameIsPreservedWithoutAListeningClaim() {
        val input = finding(
            title = "Bill's AirPods Pro",
            ownership = Ownership.OWNED,
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = true,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        assertEquals("Bill's AirPods Pro", PrivacyFindingNormalizer.normalize(input).title)
    }

    @Test
    fun ownedAirPodsLiveListenTitleIsReplacedWithNeutralActivityWording() {
        val input = finding(
            title = "Bill's AirPods Live Listen",
            ownership = Ownership.OWNED,
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = true,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        assertEquals(
            "AirPods connection/activity nearby",
            PrivacyFindingNormalizer.normalize(input).title,
        )
    }

    @Test
    fun ownedIPhoneMicrophoneTitleIsReplacedWithNeutralActivityWording() {
        val input = finding(
            title = "Bill's iPhone microphone active",
            ownership = Ownership.OWNED,
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = false,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        assertEquals(
            "Apple device activity nearby",
            PrivacyFindingNormalizer.normalize(input).title,
        )
    }

    @Test
    fun evidenceSplitAcrossDifferentRowsNeverCorrelates() {
        val appleOnly = finding(
            title = "Apple continuity",
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = true,
                listeningOrientedCategoryOrWording = false,
            ),
        )
        val listeningOnly = finding(
            sourceRecordId = "listening-only",
            title = "Possible listening",
            appleEvidence = PrivacyAppleListeningEvidence(
                appleFamilyEvidence = false,
                airPodsAssociationEvidence = false,
                listeningOrientedCategoryOrWording = true,
            ),
        )

        val normalized = listOf(appleOnly, listeningOnly).map(PrivacyFindingNormalizer::normalize)

        assertSame(appleOnly, normalized[0])
        assertSame(listeningOnly, normalized[1])
    }

    @Test
    fun sameRowLegacyAppleListeningWordingIsNeutralizedWithoutStructuredEvidence() {
        val input = finding(
            title = "Apple AirPods possible listening",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = null,
        )

        val normalized = PrivacyFindingNormalizer.normalize(input)

        assertEquals("AirPods connection/activity nearby", normalized.title)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals(FindingSeverity.INFO, normalized.severity)
        assertEquals(
            "Live Listen and microphone use cannot be determined from BLE.",
            normalized.limitation,
        )
    }

    @Test
    fun plainAppleHandoffWithoutListeningWordingIsPreserved() {
        val input = finding(
            title = "Apple Handoff nearby",
            category = PrivacyCategory.APPLE_CONTINUITY,
            severity = FindingSeverity.INFO,
            appleEvidence = null,
        )

        assertSame(input, PrivacyFindingNormalizer.normalize(input))
    }

    @Test
    fun plainAppleContinuityCannotKeepAnInflatedCriticalSeverity() {
        val input = finding(
            title = "Apple Handoff nearby",
            category = PrivacyCategory.APPLE_CONTINUITY,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = null,
        )

        val normalized = PrivacyFindingNormalizer.normalize(input)

        assertEquals("Apple Handoff nearby", normalized.title)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals(FindingSeverity.INFO, normalized.severity)
    }

    @Test
    fun nonAppleListeningFindingIsPreserved() {
        val input = finding(
            title = "Possible listening device",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = null,
        )

        assertSame(input, PrivacyFindingNormalizer.normalize(input))
    }

    @Test
    fun wordsContainingAppleOrIphoneFragmentsAreNotAppleEvidence() {
        val pineapple = finding(
            sourceRecordId = "pineapple",
            title = "Pineapple sensor possible listening",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = null,
        )
        val epiphone = finding(
            sourceRecordId = "epiphone",
            title = "Epiphone microphone monitor",
            category = PrivacyCategory.REMOTE_LISTENING,
            severity = FindingSeverity.CRITICAL,
            appleEvidence = null,
        )

        assertSame(pineapple, PrivacyFindingNormalizer.normalize(pineapple))
        assertSame(epiphone, PrivacyFindingNormalizer.normalize(epiphone))
    }

    private fun finding(
        sourceRecordId: String = "apple-row",
        title: String,
        category: PrivacyCategory = PrivacyCategory.REMOTE_LISTENING,
        severity: FindingSeverity = FindingSeverity.CRITICAL,
        ownership: Ownership = Ownership.UNKNOWN,
        appleEvidence: PrivacyAppleListeningEvidence?,
    ) = PrivacyFinding(
        displayId = "display:$sourceRecordId",
        observationKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, sourceRecordId),
        source = PrivacySourceKind.PHONE_BLE,
        stableSourceId = "stable:$sourceRecordId",
        routableKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "route:$sourceRecordId"),
        title = title,
        evidence = null,
        limitation = null,
        category = category,
        severity = severity,
        ownership = ownership,
        signalDbm = -48,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 99_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = true,
        appleEvidence = appleEvidence,
    )
}

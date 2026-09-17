package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.*
import org.junit.Test

class PrivacyEncounterLogTest {
    private fun finding(id: String = "one", seen: Long = 1_000, source: PrivacySourceKind = PrivacySourceKind.PHONE_BLE) = PrivacyFinding(
        displayId = id, observationKey = PrivacyFindingKey(source, "observation:$id"),
        source = source, stableSourceId = id, routableKey = PrivacyFindingKey(source, id),
        title = "Tracker $id", evidence = "Advertised tracker service", limitation = "Ownership unknown",
        category = PrivacyCategory.BLE_TRACKER, severity = FindingSeverity.NEARBY,
        ownership = Ownership.UNKNOWN, signalDbm = -65, firstSeenWallMs = 1_000,
        lastSeenWallMs = seen, lastObservedElapsedMs = seen, protocolTtlMs = null, hasLiveLocalSamples = true,
    )
    private fun state(vararg findings: PrivacyFinding) = PrivacyCurrentState(emptyList(), findings.toList(), 0, emptyList(), true)

    @Test fun clockTicksCannotCreateFakeObservationsAndExpiredEvidenceIsRetained() {
        val log = PrivacyEncounterLog()
        val row = finding()
        log.update(state(row), emptySet(), 1_000, 5_000)
        repeat(10) { log.update(state(row), emptySet(), 2_000 + it.toLong(), 6_000 + it.toLong()) }
        log.update(state(), emptySet(), 100_000, 105_000)
        val saved = log.entries.value.single()
        assertEquals(1, saved.observationCount)
        assertEquals(1, saved.signalSamples.size)
        assertEquals(5_000, saved.firstObservedWallMs)
        assertEquals(5_000, saved.lastObservedWallMs)
        log.update(state(), emptySet(), 1_801_000, 1_805_000)
        assertTrue(log.entries.value.isEmpty())
    }

    @Test fun clearDoesNotResurrectOldPacketsButAcceptsNewOnes() {
        val log = PrivacyEncounterLog()
        log.update(state(finding()), emptySet(), 1_000, 1_000)
        log.clear(2_000)
        log.update(state(finding()), emptySet(), 2_001, 2_001)
        assertTrue(log.entries.value.isEmpty())
        log.update(state(finding(seen = 2_002)), emptySet(), 2_002, 2_002)
        assertEquals(1, log.entries.value.single().observationCount)
    }

    @Test fun ignoredDevicesArePurgedAndExactSourcesNeverCollapse() {
        val log = PrivacyEncounterLog()
        val phone = finding()
        val backend = finding(source = PrivacySourceKind.BACKEND)
        log.update(state(phone, backend), emptySet(), 1_000, 1_000)
        assertEquals(2, log.entries.value.size)
        log.update(state(phone, backend), setOf(phone.ignoreKey!!.encoded), 1_001, 1_001)
        assertEquals(PrivacySourceKind.BACKEND, log.entries.value.single().key.source)
    }

    @Test fun boundsKeepMostRecentDevicesAndSamplesWithoutPacketDrivenReordering() {
        val log = PrivacyEncounterLog()
        log.update(state(*Array(250) { finding("id%03d".format(it)) }), emptySet(), 1_000, 1_000)
        assertEquals(200, log.entries.value.size)
        val retained = log.entries.value.first().finding
        val order = log.entries.value.map { it.key }
        repeat(100) { i ->
            log.update(state(retained.copy(lastObservedElapsedMs = 2_000L + i)), emptySet(), 2_000L + i, 2_000L + i)
        }
        assertEquals(order, log.entries.value.map { it.key })
        assertEquals(60, log.entries.value.first { it.key == retained.routableKey }.signalSamples.size)
    }

    @Test fun pausedStaleUnknownRoutesAndFutureSamplesAreNotFreshEvidence() {
        val log = PrivacyEncounterLog()
        log.update(state(finding().copy(freshness = FindingFreshness.STALE),
            finding("paused").copy(freshness = FindingFreshness.PAUSED_CACHED),
            finding("unknown").copy(routableKey = null), finding("future", seen = 20_000)),
            emptySet(), 1_000, 1_000)
        assertTrue(log.entries.value.isEmpty())
    }

    @Test fun signalTrendRequiresMultipleSamplesAndDoesNotClaimDistance() {
        fun samples(vararg dbm: Int) = dbm.mapIndexed { i, signal -> PrivacySignalSample(i.toLong(), i.toLong(), signal) }
        assertEquals("Collecting signal samples", signalTrend(samples(-60)))
        assertEquals("Signal strengthening", signalTrend(samples(-80, -81, -79, -60, -62, -61)))
        assertEquals("Signal broadly steady", signalTrend(samples(-60, -61, -59, -60, -62, -61)))
        assertEquals("Signal weakening", signalTrend(samples(-60, -61, -59, -80, -82, -81)))
    }
}

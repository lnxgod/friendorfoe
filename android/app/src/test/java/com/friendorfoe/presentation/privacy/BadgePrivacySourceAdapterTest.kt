package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeBleControlStatus
import com.friendorfoe.data.badge.BadgeCommand
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeConfigReadback
import com.friendorfoe.data.badge.BadgeConnectionEvidence
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeNetworkModeReadback
import com.friendorfoe.data.badge.BadgeReportingStatus
import com.friendorfoe.data.badge.BadgeRepositoryState
import com.friendorfoe.data.badge.BadgeThreatCounts
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeTransport
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class BadgePrivacySourceAdapterTest {

    @Test
    fun observesOnlyActiveTransportAndSwitchRemovesTheOldBadgeSnapshot() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)
        runCurrent()
        assertTrue(adapter.snapshots.value.isEmpty())

        port.state.value = badgeState(
            transport = BadgeTransport.USB_SERIAL,
            entity = badgeEntity(sourceId = 7),
        )
        runCurrent()
        assertEquals(listOf(PrivacySourceKind.BADGE_USB), adapter.snapshots.value.map { it.health.source })

        port.state.value = badgeState(
            transport = BadgeTransport.LOCAL_AP_HTTP,
            entity = badgeEntity(sourceId = 7),
        )
        runCurrent()
        assertEquals(listOf(PrivacySourceKind.BADGE_AP), adapter.snapshots.value.map { it.health.source })
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
        assertEquals(0, port.requestCalls)
        assertEquals(0, port.refreshCalls)
    }

    @Test
    fun provenEntityIdIsDurableButLabelsDisplayIdsAndBssidsAreNot() {
        val status = status(entity = badgeEntity(sourceId = 9))
        val durable = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 9),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "ephemeral:unused",
        )!!
        val ephemeral = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 0).copy(
                label = "Stable-looking label",
                displayId = "display-42",
                bssid = "AA:BB:CC:DD:EE:FF",
            ),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "ephemeral:1",
        )!!
        val negativeSentinel = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = -1),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "ephemeral:sentinel",
        )!!

        assertEquals("wifi_assoc:id:9", durable.stableSourceId)
        assertTrue(durable.observationKey.sourceRecordId.contains("wifi_assoc:id:9"))
        assertNull(ephemeral.stableSourceId)
        assertEquals("ephemeral:1", ephemeral.observationKey.sourceRecordId)
        assertNull(negativeSentinel.stableSourceId)
        assertEquals("ephemeral:sentinel", negativeSentinel.observationKey.sourceRecordId)
    }

    @Test
    fun entityUsesReceivedAndLastSeenTimestampsWithoutRejuvenatingOnRemap() {
        val status = status(
            receivedElapsed = 10_000L,
            receivedWall = 100_000L,
            entity = badgeEntity(sourceId = 1).copy(ageSeconds = 10, lastSeenSeconds = 2),
        )
        val first = BadgePrivacySourceAdapter.mapEntity(
            status.entities.single(),
            PrivacySourceKind.BADGE_BLE,
            status,
            "unused",
        )!!
        val remapped = BadgePrivacySourceAdapter.mapEntity(
            status.entities.single(),
            PrivacySourceKind.BADGE_BLE,
            status,
            "unused",
        )!!

        assertEquals(8_000L, first.lastObservedElapsedMs)
        assertEquals(98_000L, first.lastSeenWallMs)
        assertEquals(90_000L, first.firstSeenWallMs)
        assertEquals(first, remapped)
    }

    @Test
    fun sameEntityAppleListeningNormalizesButSplitEntitiesNeverCorrelate() {
        val status = status(entity = badgeEntity(sourceId = 1))
        val same = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 1).copy(
                label = "Apple AirPods",
                detail = "Possible listening activity",
                threatClass = "privacy",
                category = "REMOTE_LISTENING",
                code = "LISTEN",
            ),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "same",
        )!!
        val appleOnly = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 2).copy(
                label = "Apple AirPods",
                detail = "Nearby accessory",
                threatClass = "privacy",
                category = "AUDIO",
                code = "AUDIO",
            ),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "apple",
        )!!
        val listeningOnly = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 3).copy(
                label = "Unknown signal",
                detail = "Possible listening activity",
                threatClass = "privacy",
                category = "REMOTE_LISTENING",
                code = "LISTEN",
            ),
            source = PrivacySourceKind.BADGE_USB,
            status = status,
            ephemeralRecordId = "listening",
        )!!

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, same.category)
        assertEquals(FindingSeverity.INFO, same.severity)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, appleOnly.category)
        assertEquals(FindingSeverity.INFO, appleOnly.severity)
        assertTrue(reduced(appleOnly).alertEligible.isEmpty())
        assertEquals(PrivacyCategory.REMOTE_LISTENING, listeningOnly.category)
        assertEquals("Unknown signal", listeningOnly.title)
    }

    @Test
    fun successfulBadgeStatusWithNoEntitiesIsLiveEmpty() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)
        port.state.value = badgeState(
            transport = BadgeTransport.DEBUG_BRIDGE,
            entity = null,
        )
        runCurrent()

        val snapshot = adapter.snapshots.value.single()
        assertEquals(PrivacySourceKind.BADGE_DEBUG_BRIDGE, snapshot.health.source)
        assertEquals(SourceHealthState.LIVE, snapshot.health.state)
        assertTrue(snapshot.findings.isEmpty())
    }

    @Test
    fun allFourBadgeTransportsMapToTheirExactSourceKind() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)
        val expected = listOf(
            BadgeTransport.USB_SERIAL to PrivacySourceKind.BADGE_USB,
            BadgeTransport.LOCAL_AP_HTTP to PrivacySourceKind.BADGE_AP,
            BadgeTransport.BLE_GATT to PrivacySourceKind.BADGE_BLE,
            BadgeTransport.DEBUG_BRIDGE to PrivacySourceKind.BADGE_DEBUG_BRIDGE,
        )

        expected.forEach { (transport, source) ->
            port.state.value = badgeState(transport, badgeEntity(sourceId = 1))
            runCurrent()
            assertEquals(source, adapter.snapshots.value.single().health.source)
            assertTrue(adapter.snapshots.value.single().findings.all { it.source == source })
        }
    }

    @Test
    fun missingOrStaleStatusNeverPretendsTheBadgeSourceIsLive() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)
        port.state.value = BadgeRepositoryState(
            connection = BadgeConnectionEvidence(
                transport = BadgeTransport.USB_SERIAL,
                transportGeneration = 1L,
                phase = BadgeConnectionPhase.LIVE,
            ),
            controlStatus = null,
        )
        runCurrent()
        assertEquals(SourceHealthState.LOADING, adapter.snapshots.value.single().health.state)

        port.state.value = badgeState(BadgeTransport.USB_SERIAL, badgeEntity(1)).copy(
            connection = badgeState(BadgeTransport.USB_SERIAL, badgeEntity(1)).connection.copy(
                phase = BadgeConnectionPhase.STALE,
            ),
        )
        runCurrent()
        assertEquals(SourceHealthState.STALE, adapter.snapshots.value.single().health.state)
    }

    @Test
    fun badgeAgeArithmeticSaturatesAndNegativeAgesDoNotMoveIntoTheFuture() {
        val status = status(receivedElapsed = 10_000L, receivedWall = 100_000L, entity = null)
        val huge = BadgePrivacySourceAdapter.mapEntity(
            badgeEntity(1).copy(ageSeconds = Int.MAX_VALUE, lastSeenSeconds = Int.MAX_VALUE),
            PrivacySourceKind.BADGE_USB,
            status,
            "huge",
        )!!
        val negative = BadgePrivacySourceAdapter.mapEntity(
            badgeEntity(2).copy(ageSeconds = -5, lastSeenSeconds = -2),
            PrivacySourceKind.BADGE_USB,
            status,
            "negative",
        )!!

        assertEquals(0L, huge.firstSeenWallMs)
        assertEquals(0L, huge.lastSeenWallMs)
        assertEquals(0L, huge.lastObservedElapsedMs)
        assertEquals(100_000L, negative.firstSeenWallMs)
        assertEquals(100_000L, negative.lastSeenWallMs)
        assertEquals(10_000L, negative.lastObservedElapsedMs)
    }

    @Test
    fun repeatedUnprovenEntityWithinOneStatusLifetimeReusesEphemeralKey() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)
        val firstState = badgeState(
            BadgeTransport.BLE_GATT,
            badgeEntity(sourceId = 0).copy(displayId = "first", bssid = "AA:BB"),
        )
        port.state.value = firstState
        runCurrent()
        val first = adapter.snapshots.value.single().findings.single()

        port.state.value = firstState.copy(
            controlStatus = firstState.controlStatus!!.copy(
                entities = listOf(
                    badgeEntity(sourceId = 0).copy(
                        label = "Changed label",
                        displayId = "changed",
                        bssid = "CC:DD",
                    ),
                ),
            ),
        )
        runCurrent()
        val second = adapter.snapshots.value.single().findings.single()

        assertNull(first.stableSourceId)
        assertNull(second.stableSourceId)
        assertEquals(first.observationKey, second.observationKey)
    }

    @Test
    fun badgeRecoveryRequestsConnectionAndRefreshWithoutStartingOrStoppingTransport() = runTest {
        val port = FakeBadgePort()
        val adapter = BadgePrivacySourceAdapter(port, FakeClock(), backgroundScope)

        assertEquals(
            PrivacyRecoveryResult.Recovered(PrivacySourceKind.BADGE_BLE),
            adapter.recover(PrivacySourceKind.BADGE_BLE),
        )
        assertEquals(1, port.requestCalls)
        assertEquals(1, port.refreshCalls)
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
    }

    private fun badgeState(
        transport: BadgeTransport,
        entity: BadgeThreatEntity?,
    ) = BadgeRepositoryState(
        connection = BadgeConnectionEvidence(
            transport = transport,
            transportGeneration = 4L,
            phase = BadgeConnectionPhase.LIVE,
            lastValidStatusAtElapsedMs = 10_000L,
        ),
        controlStatus = status(entity = entity),
    )

    private fun reduced(finding: PrivacyFinding) = PrivacyCurrentReducer().reduce(
        sources = listOf(
            PrivacySourceSnapshot(
                health = PrivacySourceHealth(
                    source = finding.source,
                    state = SourceHealthState.LIVE,
                    lastSuccessElapsedMs = finding.lastObservedElapsedMs,
                    lastSuccessWallMs = finding.lastSeenWallMs,
                    recoveryLabel = null,
                    message = null,
                ),
                findings = listOf(finding),
                emittedAtElapsedMs = finding.lastObservedElapsedMs,
            ),
        ),
        ignoredKeys = emptySet(),
        nowElapsedMs = finding.lastObservedElapsedMs,
    )

    private fun status(
        receivedElapsed: Long = 10_000L,
        receivedWall: Long = 100_000L,
        entity: BadgeThreatEntity?,
    ) = BadgeControlStatus(
        version = "test",
        receivedAtElapsedMs = receivedElapsed,
        receivedAtWallClock = Instant.ofEpochMilli(receivedWall),
        themeReadback = BadgeConfigReadback(null, null, "fixture"),
        policyReadback = BadgeConfigReadback(null, null, "fixture"),
        networkModeReadback = BadgeNetworkModeReadback(null, "fixture"),
        entities = listOfNotNull(entity),
        scanners = emptyList(),
        displayState = null,
        debugBridge = null,
        reporting = BadgeReportingStatus(),
        counts = BadgeThreatCounts(),
        bleControl = BadgeBleControlStatus(),
        safeMode = false,
        safeReason = "",
        resetReason = "",
        crashCount = 0,
        recoveryMode = "",
        stackFreeBytes = emptyMap(),
        heapInternalFreeBytes = 0L,
        heapInternalMinimumFreeBytes = 0L,
        psramFreeBytes = 0L,
    )

    private fun badgeEntity(sourceId: Int) = BadgeThreatEntity(
        label = "Flock camera",
        detail = "Camera nearby",
        evidence = "Wi-Fi signature",
        threatClass = "flock",
        category = "FLOCK",
        code = "FLK",
        displayId = "display-$sourceId",
        source = "wifi_assoc",
        sourceId = sourceId,
        score = 90,
        confidencePct = 85,
        ageSeconds = 3,
        lastSeenSeconds = 1,
        rssi = -55,
        events = 2,
    )

    private class FakeBadgePort : BadgeControlPort {
        override val state = MutableStateFlow(BadgeRepositoryState())
        var startCalls = 0
        var stopCalls = 0
        var requestCalls = 0
        var refreshCalls = 0

        override fun start() { startCalls += 1 }
        override fun stop() { stopCalls += 1 }
        override fun requestConnection() { requestCalls += 1 }
        override fun refreshStatus() { refreshCalls += 1 }
        override suspend fun execute(command: BadgeCommand): BadgeCommandOutcome =
            BadgeCommandOutcome.Unsupported("fixture")
    }

    private class FakeClock : MonotonicClock {
        override fun nowElapsedMs(): Long = 10_000L
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(100_000L)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(10_000L)
    }
}

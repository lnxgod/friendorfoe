package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import kotlinx.coroutines.CoroutineScope
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
    fun absentBadgeIsImmediatelyUnavailableWithoutClaimingATransport() = runTest {
        val adapter = adapterFor(FakeBadgePort(), FakeClock(), backgroundScope)

        val snapshot = adapter.snapshots.value.single()
        assertEquals("badge", snapshot.health.source.preferenceId)
        assertEquals(SourceHealthState.UNSUPPORTED, snapshot.health.state)
        assertEquals("No badge connected", snapshot.health.message)
        assertNull(snapshot.health.lastSuccessElapsedMs)
        assertTrue(snapshot.findings.isEmpty())
    }

    @Test
    fun disconnectedKnownTransportResolvesImmediatelyInsteadOfCheckingForever() = runTest {
        val port = FakeBadgePort().apply {
            state.value = BadgeUsbState(
                status = BadgeUsbStatus.DISCONNECTED,
                message = "No badge connected",
                transportLabel = "USB-C",
            )
        }
        val adapter = adapterFor(port, FakeClock(), backgroundScope)

        val snapshot = adapter.snapshots.value.single()
        assertEquals(PrivacySourceKind.BADGE_USB, snapshot.health.source)
        assertEquals(SourceHealthState.PAUSED, snapshot.health.state)
        assertEquals("No badge connected", snapshot.health.message)
    }

    @Test
    fun usbPermissionBlockedOffersBadgeConnectionRecovery() = runTest {
        val port = FakeBadgePort().apply {
            state.value = BadgeUsbState(
                status = BadgeUsbStatus.PERMISSION_NEEDED,
                message = "Badge permission is required",
                transportLabel = "USB-C",
            )
        }
        val adapter = adapterFor(port, FakeClock(), backgroundScope)

        val snapshot = adapter.snapshots.value.single()
        assertEquals(PrivacySourceKind.BADGE_USB, snapshot.health.source)
        assertEquals(SourceHealthState.PERMISSION_BLOCKED, snapshot.health.state)
        assertEquals("Connect badge", snapshot.health.recoveryLabel)
    }

    @Test
    fun connectingPhasesKeepOneResolutionStartUntilTheReducerDeadline() = runTest {
        val port = FakeBadgePort()
        val clock = FakeClock(elapsed = 10_000L)
        val adapter = adapterFor(port, clock, backgroundScope)
        port.state.value = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTING,
            message = "Connecting to badge",
            transportLabel = "BLE",
        )
        runCurrent()
        assertEquals(10_000L, adapter.snapshots.value.single().emittedAtElapsedMs)

        clock.elapsed = 19_000L
        port.state.value = port.state.value.copy(
            message = "Opening badge control channel",
        )
        runCurrent()

        val loading = adapter.snapshots.value.single()
        assertEquals(SourceHealthState.LOADING, loading.health.state)
        assertEquals(10_000L, loading.emittedAtElapsedMs)
        val resolved = PrivacyCurrentReducer().reduce(
            sources = listOf(loading),
            ignoredKeys = emptySet(),
            nowElapsedMs = 30_000L,
        )
        assertEquals(SourceHealthState.FAILED, resolved.sources.single().state)
    }

    @Test
    fun observesOnlyActiveTransportAndSwitchRemovesTheOldBadgeSnapshot() = runTest {
        val port = FakeBadgePort()
        val adapter = adapterFor(port, FakeClock(), backgroundScope)
        runCurrent()

        port.state.value = badgeState(
            status = BadgeUsbStatus.CONNECTED,
            transportLabel = "USB-C",
            entity = badgeEntity(sourceId = 7),
        )
        runCurrent()
        assertEquals(listOf(PrivacySourceKind.BADGE_USB), adapter.snapshots.value.map { it.health.source })

        port.state.value = badgeState(
            status = BadgeUsbStatus.AP_CONNECTED,
            transportLabel = "Badge AP",
            entity = badgeEntity(sourceId = 7),
        )
        runCurrent()
        assertEquals(listOf(PrivacySourceKind.BADGE_AP), adapter.snapshots.value.map { it.health.source })
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
        assertEquals(0, port.requestCalls)
        assertEquals(0, port.statusCalls)
    }

    @Test
    fun provenEntityIdIsDurableButLabelsDisplayIdsAndBssidsAreNot() {
        val durable = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 9),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "ephemeral:unused",
        )!!
        val ephemeral = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 0).copy(
                label = "Stable-looking label",
                displayId = "display-42",
                bssid = "AA:BB:CC:DD:EE:FF",
            ),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "ephemeral:1",
        )!!
        val negativeSentinel = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = -1),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
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
        val entity = badgeEntity(sourceId = 1).copy(ageSeconds = 10, lastSeenSeconds = 2)
        val first = BadgePrivacySourceAdapter.mapEntity(
            entity = entity,
            source = PrivacySourceKind.BADGE_BLE,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "unused",
        )!!
        val remapped = BadgePrivacySourceAdapter.mapEntity(
            entity = entity,
            source = PrivacySourceKind.BADGE_BLE,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "unused",
        )!!

        assertEquals(8_000L, first.lastObservedElapsedMs)
        assertEquals(98_000L, first.lastSeenWallMs)
        assertEquals(90_000L, first.firstSeenWallMs)
        assertEquals(first, remapped)
    }

    @Test
    fun sameEntityAppleListeningNormalizesButSplitEntitiesNeverCorrelate() {
        val same = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(sourceId = 1).copy(
                label = "Apple AirPods",
                detail = "Possible listening activity",
                threatClass = "privacy",
                category = "REMOTE_LISTENING",
                code = "LISTEN",
            ),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
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
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
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
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
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
        val adapter = adapterFor(port, FakeClock(), backgroundScope)
        port.state.value = badgeState(
            status = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            transportLabel = "Debug Bridge",
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
        val adapter = adapterFor(port, FakeClock(), backgroundScope)
        val expected = listOf(
            Triple(BadgeUsbStatus.CONNECTED, "USB-C", PrivacySourceKind.BADGE_USB),
            Triple(BadgeUsbStatus.AP_CONNECTED, "Badge AP", PrivacySourceKind.BADGE_AP),
            Triple(BadgeUsbStatus.BLE_CONNECTED, "BLE", PrivacySourceKind.BADGE_BLE),
            Triple(
                BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
                "Debug Bridge",
                PrivacySourceKind.BADGE_DEBUG_BRIDGE,
            ),
        )

        expected.forEach { (status, label, source) ->
            port.state.value = badgeState(status, label, badgeEntity(sourceId = 1))
            runCurrent()
            assertEquals(source, adapter.snapshots.value.single().health.source)
            assertTrue(adapter.snapshots.value.single().findings.all { it.source == source })
        }
    }

    @Test
    fun missingOrFailedStatusNeverPretendsTheBadgeSourceIsLive() = runTest {
        val port = FakeBadgePort()
        val adapter = adapterFor(port, FakeClock(), backgroundScope)
        port.state.value = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTED,
            transportLabel = "USB-C",
            controlStatus = null,
        )
        runCurrent()
        assertEquals(SourceHealthState.LOADING, adapter.snapshots.value.single().health.state)

        port.state.value = badgeState(
            BadgeUsbStatus.ERROR,
            "USB-C",
            badgeEntity(1),
        )
        runCurrent()
        assertEquals(SourceHealthState.FAILED, adapter.snapshots.value.single().health.state)
    }

    @Test
    fun badgeAgeArithmeticSaturatesAndNegativeAgesDoNotMoveIntoTheFuture() {
        val huge = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(1).copy(
                ageSeconds = Int.MAX_VALUE,
                lastSeenSeconds = Int.MAX_VALUE,
            ),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "huge",
        )!!
        val negative = BadgePrivacySourceAdapter.mapEntity(
            entity = badgeEntity(2).copy(ageSeconds = -5, lastSeenSeconds = -2),
            source = PrivacySourceKind.BADGE_USB,
            snapshotAtElapsedMs = 10_000L,
            snapshotAtWallMs = 100_000L,
            ephemeralRecordId = "negative",
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
        val adapter = adapterFor(port, FakeClock(), backgroundScope)
        val firstState = badgeState(
            status = BadgeUsbStatus.BLE_CONNECTED,
            transportLabel = "BLE",
            entity = badgeEntity(sourceId = 0).copy(displayId = "first", bssid = "AA:BB"),
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
        val adapter = adapterFor(port, FakeClock(), backgroundScope)

        assertEquals(
            PrivacyRecoveryResult.Recovered(PrivacySourceKind.BADGE_BLE),
            adapter.recover(PrivacySourceKind.BADGE_BLE),
        )
        assertEquals(1, port.requestCalls)
        assertEquals(1, port.statusCalls)
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
    }

    private fun badgeState(
        status: BadgeUsbStatus,
        transportLabel: String,
        entity: BadgeThreatEntity?,
    ) = BadgeUsbState(
        status = status,
        transportLabel = transportLabel,
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

    private fun status(entity: BadgeThreatEntity?) = BadgeControlStatus(
        version = "test",
        entities = listOfNotNull(entity),
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
        snapshotAtElapsedMs = 10_000L,
        rssi = -55,
        events = 2,
    )

    private fun adapterFor(
        port: FakeBadgePort,
        clock: MonotonicClock,
        scope: CoroutineScope,
    ) = BadgePrivacySourceAdapter(
        state = port.state,
        clock = clock,
        scope = scope,
        requestConnection = port::requestConnection,
        requestStatus = port::requestStatus,
    )

    private class FakeBadgePort {
        val state = MutableStateFlow(BadgeUsbState())
        var startCalls = 0
        var stopCalls = 0
        var requestCalls = 0
        var statusCalls = 0

        fun start() { startCalls += 1 }
        fun stop() { stopCalls += 1 }
        fun requestConnection() { requestCalls += 1 }
        fun requestStatus() { statusCalls += 1 }
    }

    private class FakeClock(
        var elapsed: Long = 10_000L,
    ) : MonotonicClock {
        override fun nowElapsedMs(): Long = elapsed
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(100_000L)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }
}

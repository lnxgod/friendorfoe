package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PrivacyAlertCoordinatorTest {
    @Test
    fun ephemeralBleIdentityWithMacCannotRouteOrAlert() = runTest {
        val ephemeral = PhonePrivacySourceAdapter.mapBle(
            detection = GlassesDetection(
                mac = "AA:BB:CC:DD:EE:FF",
                deviceName = "Unidentified camera",
                deviceType = "Smart Glasses",
                manufacturer = "Unknown",
                hasCamera = true,
                rssi = -52,
                confidence = 0.92f,
                matchReason = "camera signature",
                firstSeen = Instant.ofEpochMilli(900L),
                lastSeen = Instant.ofEpochMilli(1_000L),
                category = PrivacyCategory.SMART_GLASSES,
                fingerprintKey = "",
            ),
            observedElapsedMs = 1_000L,
            observedWallMs = 1_000L,
            observationRecordId = "ephemeral:1",
        )
        val repository = PrivacyFindingRepository(
            sourceAdapters = setOf(FixedAdapter(livePhoneSnapshot(listOf(ephemeral)))),
            appPreferences = EmptyPreferences,
            clock = FakeClock(),
            scope = backgroundScope,
        )
        val publisher = RecordingPublisher()
        val coordinator = PrivacyAlertCoordinator(
            states = repository.currentState,
            policy = PrivacyAlertPolicy(),
            publisher = publisher,
            clock = FakeClock(),
            scope = backgroundScope,
        )

        coordinator.start()
        runCurrent()

        assertEquals(null, repository.currentState.value.findings.single().routableKey)
        assertTrue(repository.currentState.value.alertEligible.isEmpty())
        assertTrue(publisher.published.isEmpty())
    }

    @Test
    fun sameMacGenericAndFollowerRowsKeepExactRoutesAndDistinctNotificationIds() = runTest {
        val mac = "AA:BB:CC:DD:EE:FF"
        val generic = PhonePrivacySourceAdapter.mapBle(
            detection = GlassesDetection(
                mac = mac,
                deviceName = "Meta glasses",
                deviceType = "Smart Glasses",
                manufacturer = "Meta",
                hasCamera = true,
                rssi = -52,
                confidence = 0.92f,
                matchReason = "mfr",
                firstSeen = Instant.ofEpochMilli(900L),
                lastSeen = Instant.ofEpochMilli(1_000L),
                category = PrivacyCategory.SMART_GLASSES,
                fingerprintKey = "fp:camera",
            ),
            observedElapsedMs = 1_000L,
            observedWallMs = 1_000L,
            observationRecordId = "observation:fp:camera",
        )
        val follower = PhonePrivacySourceAdapter.mapFollower(
            alert = BleTracker.StalkerAlert(
                device = BleTracker.TrackedDevice(
                    mac = mac,
                    deviceName = "Tag",
                    deviceType = "BLE Tracker",
                    manufacturer = "Generic",
                    hasCamera = false,
                    firstSeen = Instant.ofEpochMilli(100L),
                    lastSeen = Instant.ofEpochMilli(1_000L),
                    peakRssi = -48,
                ),
                reason = "following",
                threatLevel = 3,
            ),
            observedElapsedMs = 1_000L,
            observedWallMs = 1_000L,
        )
        val adapter = FixedAdapter(livePhoneSnapshot(listOf(generic, follower)))
        val repository = PrivacyFindingRepository(
            sourceAdapters = setOf(adapter),
            appPreferences = EmptyPreferences,
            clock = FakeClock(),
            scope = backgroundScope,
        )
        runCurrent()

        val expectedKeys = setOf(
            PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "observation:fp:camera"),
            PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "follower:$mac"),
        )
        val reducedKeys = repository.currentState.value.alertEligible
            .mapNotNull(PrivacyFinding::routableKey)
        assertEquals(2, reducedKeys.size)
        assertEquals(expectedKeys, reducedKeys.toSet())
        reducedKeys.forEach { key ->
            assertTrue(repository.finding(key).first() is PrivacyFindingLookupState.Present)
        }

        val idStore = RecordingIdStore()
        val publishedRoutes = mutableListOf<PrivacyNotificationRoute>()
        val coordinator = PrivacyAlertCoordinator(
            states = repository.currentState,
            policy = PrivacyAlertPolicy(),
            publisher = PrivacyAlertPublisher { finding ->
                publishedRoutes += PrivacyNotificationRoute.from(
                    key = requireNotNull(finding.routableKey),
                    ids = idStore,
                )
                true
            },
            clock = FakeClock(),
            scope = backgroundScope,
        )

        coordinator.start()
        runCurrent()

        assertEquals(2, publishedRoutes.size)
        assertEquals(2, publishedRoutes.map { it.route }.toSet().size)
        assertEquals(2, publishedRoutes.map { it.pendingIntentId }.toSet().size)
    }

    @Test
    fun coordinatorPublishesOnlyReducerProvidedAlertEligibleRows() = runTest {
        val eligible = finding("eligible", PrivacyCategory.HIDDEN_CAMERA)
        val informationalApple = finding("apple", PrivacyCategory.APPLE_CONTINUITY).copy(
            severity = FindingSeverity.INFO,
        )
        val states = MutableStateFlow(current(findings = listOf(informationalApple)))
        val publisher = RecordingPublisher()
        val coordinator = PrivacyAlertCoordinator(
            states = states,
            policy = PrivacyAlertPolicy(),
            publisher = publisher,
            clock = FakeClock(),
            scope = backgroundScope,
        )

        coordinator.start()
        runCurrent()
        states.value = current(
            findings = listOf(informationalApple, eligible),
            alertEligible = listOf(eligible),
        )
        runCurrent()
        states.value = states.value.copy(threatCount = 2)
        runCurrent()

        assertEquals(listOf(eligible.routableKey), publisher.published.map { it.routableKey })
    }

    @Test
    fun failedPublisherPersistenceIsNotMarkedAndCanRetry() = runTest {
        val eligible = finding("retry", PrivacyCategory.HIDDEN_CAMERA)
        val states = MutableStateFlow(current(findings = emptyList()))
        var attempts = 0
        val publisher = PrivacyAlertPublisher {
            attempts += 1
            attempts > 1
        }
        val coordinator = PrivacyAlertCoordinator(
            states = states,
            policy = PrivacyAlertPolicy(),
            publisher = publisher,
            clock = FakeClock(),
            scope = backgroundScope,
        )

        coordinator.start()
        runCurrent()
        states.value = current(listOf(eligible), listOf(eligible))
        runCurrent()
        states.value = states.value.copy(threatCount = 2)
        runCurrent()
        states.value = states.value.copy(threatCount = 3)
        runCurrent()

        assertEquals(2, attempts)
    }

    private fun current(
        findings: List<PrivacyFinding>,
        alertEligible: List<PrivacyFinding> = emptyList(),
    ) = PrivacyCurrentState(
        sources = emptyList(),
        findings = findings,
        threatCount = alertEligible.size,
        alertEligible = alertEligible,
        initialResolutionComplete = true,
    )

    private fun livePhoneSnapshot(findings: List<PrivacyFinding>) = PrivacySourceSnapshot(
        health = PrivacySourceHealth(
            source = PrivacySourceKind.PHONE_BLE,
            state = SourceHealthState.LIVE,
            lastSuccessElapsedMs = 1_000L,
            lastSuccessWallMs = 1_000L,
            recoveryLabel = null,
            message = null,
        ),
        findings = findings,
        emittedAtElapsedMs = 1_000L,
    )

    private fun finding(id: String, category: PrivacyCategory) = PrivacyFinding(
        displayId = id,
        observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "observation:$id"),
        source = PrivacySourceKind.BACKEND,
        stableSourceId = "stable:$id",
        routableKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:$id"),
        title = "Finding $id",
        evidence = "Evidence",
        limitation = null,
        category = category,
        severity = FindingSeverity.CRITICAL,
        ownership = Ownership.UNKNOWN,
        signalDbm = null,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = false,
    )

    private class RecordingPublisher : PrivacyAlertPublisher {
        val published = mutableListOf<PrivacyFinding>()
        override fun publish(finding: PrivacyFinding): Boolean {
            published += finding
            return true
        }
    }

    private class RecordingIdStore : PrivacyNotificationIdStore {
        private val ids = linkedMapOf<PrivacyFindingKey, Int>()

        override fun idFor(key: PrivacyFindingKey): Int =
            ids.getOrPut(key) { ids.size + 1 }
    }

    private class FixedAdapter(
        snapshot: PrivacySourceSnapshot,
    ) : PrivacySourceAdapter {
        override val adapterId: String = "phone"
        override val representedSources: Set<PrivacySourceKind> =
            setOf(PrivacySourceKind.PHONE_BLE)
        override val snapshots: StateFlow<List<PrivacySourceSnapshot>> =
            MutableStateFlow(listOf(snapshot))

        override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult =
            PrivacyRecoveryResult.Recovered(source)
    }

    private data object EmptyPreferences : AppPreferences {
        override val launchState: Flow<AppLaunchState> = flowOf(AppLaunchState.NeedsOnboarding)
        override val ignoredFindingKeys: Flow<Set<String>> = flowOf(emptySet())

        override suspend fun setOnboardingComplete() = Unit
        override suspend fun setLastTopLevelRoute(route: String) = Unit
        override suspend fun ignoreFinding(key: FindingPreferenceKey) = Unit
        override suspend fun restoreFinding(key: FindingPreferenceKey) = Unit
    }

    private class FakeClock : MonotonicClock {
        override fun nowElapsedMs(): Long = 1_000L
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(1_000L)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(1_000L)
    }
}

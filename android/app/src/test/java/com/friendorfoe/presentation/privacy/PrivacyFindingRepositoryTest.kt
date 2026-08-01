package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.onStart
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.TestScope
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PrivacyFindingRepositoryTest {

    @Test
    fun duplicateAdapterIdsAreRejectedBeforeCollectionStarts() = runTest {
        val first = fakeAdapter("phone", setOf(PrivacySourceKind.PHONE_BLE))
        val second = fakeAdapter("phone", setOf(PrivacySourceKind.WIFI_ANALYSIS))

        val failure = assertThrows(IllegalArgumentException::class.java) {
            repository(setOf(first, second))
        }

        assertTrue(failure.message.orEmpty().contains("adapterId"))
    }

    @Test
    fun duplicateSourceOwnershipIsRejectedBeforeCollectionStarts() = runTest {
        val first = fakeAdapter("phone-a", setOf(PrivacySourceKind.PHONE_BLE))
        val second = fakeAdapter("phone-b", setOf(PrivacySourceKind.PHONE_BLE))

        val failure = assertThrows(IllegalArgumentException::class.java) {
            repository(setOf(first, second))
        }

        assertTrue(failure.message.orEmpty().contains("PHONE_BLE"))
    }

    @Test
    fun snapshotOutsideAdaptersDeclaredSourcesIsRejected() = runTest {
        val adapter = fakeAdapter(
            id = "phone",
            sources = setOf(PrivacySourceKind.PHONE_BLE),
            initial = listOf(liveSnapshot(finding(PrivacySourceKind.WIFI_ANALYSIS, "wifi-row"))),
        )

        val failure = assertThrows(IllegalArgumentException::class.java) {
            repository(setOf(adapter))
        }

        assertTrue(failure.message.orEmpty().contains("WIFI_ANALYSIS"))
    }

    @Test
    fun multipleCurrentStateCollectorsDoNotRestartAdapterUpstream() = runTest {
        var starts = 0
        val upstream = flowOf(listOf(liveSnapshot(finding(PrivacySourceKind.PHONE_BLE, "one"))))
            .onStart { starts += 1 }
            .stateIn(backgroundScope, kotlinx.coroutines.flow.SharingStarted.WhileSubscribed(), emptyList())
        val adapter = FakeAdapter(
            adapterId = "phone",
            representedSources = setOf(PrivacySourceKind.PHONE_BLE),
            snapshots = upstream,
        )
        val repository = repository(setOf(adapter))
        runCurrent()

        val first = backgroundScope.launch { repository.currentState.collect {} }
        val second = backgroundScope.launch { repository.currentState.collect {} }
        runCurrent()

        assertEquals(1, starts)
        first.cancel()
        second.cancel()
    }

    @Test
    fun exactLookupAndRecoveryRemainSourceQualified() = runTest {
        val sharedRecord = "shared"
        val phone = fakeAdapter(
            id = "phone",
            sources = setOf(PrivacySourceKind.PHONE_BLE),
            initial = listOf(liveSnapshot(finding(PrivacySourceKind.PHONE_BLE, sharedRecord))),
        )
        val wifi = fakeAdapter(
            id = "wifi",
            sources = setOf(PrivacySourceKind.WIFI_ANALYSIS),
            initial = listOf(liveSnapshot(finding(PrivacySourceKind.WIFI_ANALYSIS, sharedRecord))),
        )
        val repository = repository(setOf(phone, wifi))
        runCurrent()

        val phoneKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, sharedRecord)
        val wifiKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, sharedRecord)
        assertEquals(PrivacySourceKind.PHONE_BLE, repository.findFinding(phoneKey)?.source)
        assertEquals(PrivacySourceKind.WIFI_ANALYSIS, repository.findFinding(wifiKey)?.source)
        assertNull(repository.findFinding(PrivacyFindingKey(PrivacySourceKind.BACKEND, sharedRecord)))

        assertEquals(
            PrivacyRecoveryResult.Recovered(PrivacySourceKind.WIFI_ANALYSIS),
            repository.recover(PrivacySourceKind.WIFI_ANALYSIS),
        )
        assertEquals(0, phone.recoveryCalls)
        assertEquals(1, wifi.recoveryCalls)
    }

    @Test
    fun ignoreAndRestoreUseOnlyTheCurrentFindingsExactSourceKey() = runTest {
        val preferences = FakePreferences()
        val phoneFinding = finding(PrivacySourceKind.PHONE_BLE, "same", stableId = "same")
        val wifiFinding = finding(PrivacySourceKind.WIFI_ANALYSIS, "same", stableId = "same")
        val repository = repository(
            adapters = setOf(
                fakeAdapter("phone", setOf(PrivacySourceKind.PHONE_BLE), listOf(liveSnapshot(phoneFinding))),
                fakeAdapter("wifi", setOf(PrivacySourceKind.WIFI_ANALYSIS), listOf(liveSnapshot(wifiFinding))),
            ),
            preferences = preferences,
        )
        runCurrent()

        assertEquals(PrivacyPreferenceResult.Updated, repository.ignore(phoneFinding.observationKey))
        runCurrent()
        assertEquals(listOf(PrivacySourceKind.WIFI_ANALYSIS), repository.currentState.value.findings.map { it.source })
        assertEquals(
            setOf("phone_ble\u001Fsame"),
            preferences.ignored.value,
        )

        assertEquals(PrivacyPreferenceResult.Updated, repository.restore("phone_ble\u001Fsame"))
        runCurrent()
        assertEquals(2, repository.currentState.value.findings.size)
        assertEquals(
            PrivacyPreferenceResult.NotFound,
            repository.ignore(PrivacyFindingKey(PrivacySourceKind.BACKEND, "missing")),
        )
    }

    @Test
    fun routedLookupUsesOnlyTheExactRoutableKeyAndDistinguishesExpiry() = runTest {
        val observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "observation:42")
        val routeKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:42")
        val row = finding(PrivacySourceKind.BACKEND, "placeholder").copy(
            observationKey = observationKey,
            routableKey = routeKey,
        )
        val repository = repository(
            setOf(fakeAdapter("backend", setOf(PrivacySourceKind.BACKEND), listOf(liveSnapshot(row)))),
        )
        runCurrent()

        val present = repository.finding(routeKey).first()

        assertTrue(present is PrivacyFindingLookupState.Present)
        assertEquals(routeKey, (present as PrivacyFindingLookupState.Present).finding.routableKey)
        assertEquals(
            PrivacyFindingLookupState.Expired,
            repository.finding(observationKey).first(),
        )
    }

    @Test
    fun exactLookupNeverRegressesFromExpiredBackToLoadingDuringSourceRestart() = runTest {
        val routeKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:42")
        val row = finding(PrivacySourceKind.BACKEND, "observation:42").copy(
            routableKey = routeKey,
        )
        val snapshots = MutableStateFlow(listOf(liveSnapshot(row)))
        val repository = repository(
            setOf(FakeAdapter("backend", setOf(PrivacySourceKind.BACKEND), snapshots)),
        )
        val observed = mutableListOf<PrivacyFindingLookupState>()
        val collection = backgroundScope.launch {
            repository.finding(routeKey).collect(observed::add)
        }
        runCurrent()

        snapshots.value = emptyList()
        runCurrent()
        assertEquals(PrivacyFindingLookupState.Expired, observed.last())
        val afterExpiryCount = observed.size

        snapshots.value = listOf(
            liveSnapshot(row).copy(
                health = liveSnapshot(row).health.copy(state = SourceHealthState.LOADING),
                findings = emptyList(),
            ),
        )
        runCurrent()

        assertEquals(PrivacyFindingLookupState.Expired, observed.last())
        assertEquals(afterExpiryCount, observed.size)
        collection.cancel()
    }

    private fun TestScope.repository(
        adapters: Set<PrivacySourceAdapter>,
        preferences: FakePreferences = FakePreferences(),
    ) = PrivacyFindingRepository(
        sourceAdapters = adapters,
        appPreferences = preferences,
        clock = FakeClock(),
        scope = backgroundScope,
    )

    private fun fakeAdapter(
        id: String,
        sources: Set<PrivacySourceKind>,
        initial: List<PrivacySourceSnapshot> = emptyList(),
    ) = FakeAdapter(id, sources, MutableStateFlow(initial))

    private fun liveSnapshot(finding: PrivacyFinding) = PrivacySourceSnapshot(
        health = PrivacySourceHealth(
            source = finding.source,
            state = SourceHealthState.LIVE,
            lastSuccessElapsedMs = 1_000L,
            lastSuccessWallMs = 1_000L,
            recoveryLabel = null,
            message = null,
        ),
        findings = listOf(finding),
        emittedAtElapsedMs = 1_000L,
    )

    private fun finding(
        source: PrivacySourceKind,
        record: String,
        stableId: String? = record,
    ) = PrivacyFinding(
        displayId = record,
        observationKey = PrivacyFindingKey(source, record),
        source = source,
        stableSourceId = stableId,
        routableKey = PrivacyFindingKey(source, record),
        title = "$source:$record",
        evidence = null,
        limitation = null,
        category = PrivacyCategory.INFORMATIONAL,
        severity = FindingSeverity.INFO,
        ownership = Ownership.UNKNOWN,
        signalDbm = null,
        firstSeenWallMs = 1_000L,
        lastSeenWallMs = 1_000L,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = false,
    )

    private class FakeAdapter(
        override val adapterId: String,
        override val representedSources: Set<PrivacySourceKind>,
        override val snapshots: StateFlow<List<PrivacySourceSnapshot>>,
    ) : PrivacySourceAdapter {
        var recoveryCalls = 0

        override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult {
            recoveryCalls += 1
            return PrivacyRecoveryResult.Recovered(source)
        }
    }

    private class FakePreferences : AppPreferences {
        val ignored = MutableStateFlow<Set<String>>(emptySet())
        override val launchState: Flow<AppLaunchState> = flowOf(AppLaunchState.NeedsOnboarding)
        override val ignoredFindingKeys: Flow<Set<String>> = ignored
        override val requestedPermissions = MutableStateFlow(emptySet<String>())

        override suspend fun setOnboardingComplete() = Unit
        override suspend fun setLastTopLevelRoute(route: String) = Unit

        override suspend fun ignoreFinding(key: FindingPreferenceKey) {
            ignored.value += key.encoded
        }

        override suspend fun restoreFinding(key: FindingPreferenceKey) {
            ignored.value -= key.encoded
        }

        override suspend fun markPermissionsRequested(permissions: Set<String>) {
            requestedPermissions.value += permissions
        }
    }

    private class FakeClock : MonotonicClock {
        private val tick = MutableStateFlow(1_000L)
        override fun nowElapsedMs(): Long = tick.value
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(tick.value)
        override fun ticks(periodMs: Long): Flow<Long> = tick
    }
}

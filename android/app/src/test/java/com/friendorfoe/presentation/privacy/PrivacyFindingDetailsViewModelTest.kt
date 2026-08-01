package com.friendorfoe.presentation.privacy

import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.test.MainDispatcherRule
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PrivacyFindingDetailsViewModelTest {
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun expiredFindingCannotResurrectAfterUiUnsubscribesAndRestarts() = runTest {
        val routeKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:42")
        val row = finding(routeKey)
        val snapshots = MutableStateFlow(listOf(liveSnapshot(listOf(row))))
        val repository = PrivacyFindingRepository(
            sourceAdapters = setOf(FixedAdapter(snapshots)),
            appPreferences = EmptyPreferences,
            clock = FakeClock(),
            scope = backgroundScope,
        )
        val viewModel = PrivacyFindingDetailsViewModel(
            savedStateHandle = SavedStateHandle(
                mapOf(
                    "source" to "backend",
                    "record" to "entity:42",
                ),
            ),
            repository = repository,
        )
        val firstCollector = backgroundScope.launch { viewModel.state.collect { } }
        runCurrent()
        val present = viewModel.state.value as PrivacyFindingLookupState.Present
        assertEquals(routeKey, present.finding.routableKey)

        snapshots.value = listOf(liveSnapshot(emptyList()))
        runCurrent()
        assertEquals(PrivacyFindingLookupState.Expired, viewModel.state.value)

        firstCollector.cancel()
        advanceTimeBy(5_001L)
        snapshots.value = listOf(liveSnapshot(listOf(row)))
        runCurrent()

        val restartedCollector = backgroundScope.launch { viewModel.state.collect { } }
        runCurrent()

        assertEquals(PrivacyFindingLookupState.Expired, viewModel.state.value)
        restartedCollector.cancel()
    }

    private fun finding(routeKey: PrivacyFindingKey) = PrivacyFinding(
        displayId = "backend-42",
        observationKey = PrivacyFindingKey(PrivacySourceKind.BACKEND, "observation:42"),
        source = PrivacySourceKind.BACKEND,
        stableSourceId = "stable:42",
        routableKey = routeKey,
        title = "Backend finding",
        evidence = "Current backend evidence",
        limitation = null,
        category = PrivacyCategory.HIDDEN_CAMERA,
        severity = FindingSeverity.CRITICAL,
        ownership = Ownership.UNKNOWN,
        signalDbm = -50,
        firstSeenWallMs = 900L,
        lastSeenWallMs = 1_000L,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = false,
    )

    private fun liveSnapshot(findings: List<PrivacyFinding>) = PrivacySourceSnapshot(
        health = PrivacySourceHealth(
            source = PrivacySourceKind.BACKEND,
            state = SourceHealthState.LIVE,
            lastSuccessElapsedMs = 1_000L,
            lastSuccessWallMs = 1_000L,
            recoveryLabel = null,
            message = null,
        ),
        findings = findings,
        emittedAtElapsedMs = 1_000L,
    )

    private class FixedAdapter(
        override val snapshots: StateFlow<List<PrivacySourceSnapshot>>,
    ) : PrivacySourceAdapter {
        override val adapterId: String = "backend"
        override val representedSources: Set<PrivacySourceKind> = setOf(PrivacySourceKind.BACKEND)

        override suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult =
            PrivacyRecoveryResult.Recovered(source)
    }

    private data object EmptyPreferences : AppPreferences {
        override val launchState: Flow<AppLaunchState> = flowOf(AppLaunchState.NeedsOnboarding)
        override val ignoredFindingKeys: Flow<Set<String>> = flowOf(emptySet())
        override val requestedPermissions: Flow<Set<String>> = flowOf(emptySet())

        override suspend fun setOnboardingComplete() = Unit
        override suspend fun setLastTopLevelRoute(route: String) = Unit
        override suspend fun ignoreFinding(key: FindingPreferenceKey) = Unit
        override suspend fun restoreFinding(key: FindingPreferenceKey) = Unit
        override suspend fun markPermissionsRequested(permissions: Set<String>) = Unit
    }

    private class FakeClock : MonotonicClock {
        override fun nowElapsedMs(): Long = 1_000L
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(1_000L)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(1_000L)
    }
}

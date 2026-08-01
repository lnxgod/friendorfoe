package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.scan
import kotlinx.coroutines.flow.stateIn

sealed interface PrivacyFindingLookupState {
    data object Loading : PrivacyFindingLookupState
    data class Present(val finding: PrivacyFinding) : PrivacyFindingLookupState
    data object Expired : PrivacyFindingLookupState
}

@Singleton
class PrivacyFindingRepository @Inject constructor(
    sourceAdapters: Set<@JvmSuppressWildcards PrivacySourceAdapter>,
    private val appPreferences: AppPreferences,
    private val clock: MonotonicClock,
    @ApplicationScope scope: CoroutineScope,
) {
    private val adapters = sourceAdapters.sortedBy(PrivacySourceAdapter::adapterId)
    private val ownerBySource: Map<PrivacySourceKind, PrivacySourceAdapter>
    private val reducer = PrivacyCurrentReducer()

    init {
        require(adapters.all { it.adapterId.isNotBlank() }) {
            "Every privacy adapter requires a non-blank adapterId"
        }
        val duplicateIds = adapters.groupBy(PrivacySourceAdapter::adapterId)
            .filterValues { it.size > 1 }
            .keys
        require(duplicateIds.isEmpty()) {
            "Duplicate privacy adapterId values: ${duplicateIds.sorted()}"
        }

        val owners = linkedMapOf<PrivacySourceKind, PrivacySourceAdapter>()
        adapters.forEach { adapter ->
            require(adapter.representedSources.isNotEmpty()) {
                "Privacy adapter ${adapter.adapterId} represents no sources"
            }
            adapter.representedSources.forEach { source ->
                val previous = owners.putIfAbsent(source, adapter)
                require(previous == null) {
                    "$source is owned by both ${previous?.adapterId} and ${adapter.adapterId}"
                }
            }
            validateSnapshots(adapter, adapter.snapshots.value)
        }
        ownerBySource = owners
    }

    private val sourceSnapshots: Flow<List<PrivacySourceSnapshot>> =
        if (adapters.isEmpty()) {
            flowOf(emptyList())
        } else {
            combine(adapters.map(PrivacySourceAdapter::snapshots)) { lists ->
                adapters.zip(lists.asList()).flatMap { (adapter, snapshots) ->
                    validateSnapshots(adapter, snapshots)
                    snapshots.sortedBy { it.health.source.preferenceId }
                }
            }
        }

    val currentState = combine(
        sourceSnapshots,
        appPreferences.ignoredFindingKeys,
        clock.ticks(),
    ) { snapshots, ignoredKeys, nowElapsedMs ->
        reducer.reduce(snapshots, ignoredKeys, nowElapsedMs)
    }.stateIn(
        scope = scope,
        started = SharingStarted.Eagerly,
        initialValue = PrivacyCurrentState(
            sources = emptyList(),
            findings = emptyList(),
            threatCount = 0,
            alertEligible = emptyList(),
        ),
    )

    fun findFinding(key: PrivacyFindingKey): PrivacyFinding? =
        currentState.value.findings.firstOrNull { it.observationKey == key }

    fun finding(key: PrivacyFindingKey): Flow<PrivacyFindingLookupState> = currentState
        .scan<PrivacyCurrentState, PrivacyFindingLookupState?>(null) {
                previous,
                state,
            ->
            if (previous == PrivacyFindingLookupState.Expired) {
                PrivacyFindingLookupState.Expired
            } else {
                state.findings.singleOrNull { it.routableKey == key }
                    ?.let(PrivacyFindingLookupState::Present)
                    ?: if (
                        previous is PrivacyFindingLookupState.Present ||
                        state.initialResolutionComplete
                    ) {
                        PrivacyFindingLookupState.Expired
                    } else {
                        PrivacyFindingLookupState.Loading
                    }
            }
        }
        .filterNotNull()
        .distinctUntilChanged()

    suspend fun ignore(key: PrivacyFindingKey): PrivacyPreferenceResult {
        val finding = findFinding(key) ?: return PrivacyPreferenceResult.NotFound
        val preferenceKey = finding.ignoreKey ?: return PrivacyPreferenceResult.NotPersistable
        appPreferences.ignoreFinding(preferenceKey)
        return PrivacyPreferenceResult.Updated
    }

    suspend fun restore(encoded: String): PrivacyPreferenceResult {
        val key = FindingPreferenceKey.decode(encoded)
            ?: return PrivacyPreferenceResult.MalformedKey
        appPreferences.restoreFinding(key)
        return PrivacyPreferenceResult.Updated
    }

    suspend fun recover(source: PrivacySourceKind): PrivacyRecoveryResult =
        ownerBySource[source]?.recover(source)
            ?: PrivacyRecoveryResult.SourceUnavailable(source)

    suspend fun retryAllFailed(): List<PrivacyRecoveryResult> =
        currentState.value.sources
            .filter { it.state == SourceHealthState.FAILED }
            .map { recover(it.source) }

    private fun validateSnapshots(
        adapter: PrivacySourceAdapter,
        snapshots: List<PrivacySourceSnapshot>,
    ) {
        val unexpected = snapshots.map { it.health.source }
            .filterNot(adapter.representedSources::contains)
            .distinct()
        require(unexpected.isEmpty()) {
            "Adapter ${adapter.adapterId} emitted unowned sources: $unexpected"
        }
        val duplicates = snapshots.groupBy { it.health.source }
            .filterValues { it.size > 1 }
            .keys
        require(duplicates.isEmpty()) {
            "Adapter ${adapter.adapterId} emitted duplicate snapshots: $duplicates"
        }
    }
}

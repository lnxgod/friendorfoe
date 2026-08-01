package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.presentation.about.InfoSettingKey
import com.friendorfoe.presentation.about.InfoSettingsStore
import com.friendorfoe.sensor.SensorFusionEngine
import com.friendorfoe.sensor.SensorFusionLease
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

internal fun resolveDirectionSweepTarget(
    current: PrivacyCurrentState,
    key: PrivacyFindingKey,
): PrivacyFinding? = current.findings.singleOrNull { it.observationKey == key }
    ?.takeIf { finding ->
        finding.source == PrivacySourceKind.PHONE_BLE &&
            finding.freshness == FindingFreshness.LIVE &&
            finding.capabilities.canOpenDirectionSweep
    }

@HiltViewModel
class PrivacyViewModel @Inject constructor(
    private val repository: PrivacyFindingRepository,
    phonePrivacySourceAdapter: PhonePrivacySourceAdapter,
    private val sensorFusionEngine: SensorFusionEngine,
    private val settingsStore: InfoSettingsStore,
) : ViewModel() {
    private val filters = MutableStateFlow(PrivacyFilterState())
    private val directionController = RssiDirectionSweepController(
        sampleSource = phonePrivacySourceAdapter,
        scope = viewModelScope,
    )
    val directionSweepState = directionController.state
    val directionResultText = directionController.resultText
    private var sweepSensorLease: SensorFusionLease? = null

    init {
        viewModelScope.launch {
            directionSweepState.collect { state ->
                if (state !is DirectionSweepState.Sampling) {
                    sweepSensorLease?.close()
                    sweepSensorLease = null
                }
            }
        }
    }

    val uiState: StateFlow<PrivacyUiState> = combine(
        repository.currentState,
        filters,
    ) { current, activeFilters ->
        projectPrivacyUiState(current, activeFilters)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = projectPrivacyUiState(repository.currentState.value),
    )

    fun updateQuery(query: String) {
        filters.value = filters.value.copy(query = query)
    }

    fun toggleCategory(category: PrivacyCategory) {
        filters.value = filters.value.copy(
            categories = filters.value.categories.toggle(category),
        )
    }

    fun toggleSource(source: PrivacySourceKind) {
        filters.value = filters.value.copy(
            sources = filters.value.sources.toggle(source),
        )
    }

    fun clearFilters() {
        filters.value = PrivacyFilterState()
    }

    fun ignore(finding: PrivacyFinding) {
        if (!finding.capabilities.canIgnore) return
        viewModelScope.launch {
            repository.ignore(finding.observationKey)
        }
    }

    fun recover(source: PrivacySourceKind) {
        viewModelScope.launch {
            repository.recover(source)
        }
    }

    fun retryAllFailed() {
        viewModelScope.launch {
            repository.retryAllFailed()
        }
    }

    fun enablePhonePrivacyScanning() {
        phonePrivacyEnableWrites(settingsStore.settings.value).forEach { (key, enabled) ->
            settingsStore.set(key, enabled)
        }
        recover(PrivacySourceKind.PHONE_BLE)
    }

    fun onPrivacyPermissionResolved(source: PrivacySourceKind) {
        if (source == PrivacySourceKind.PHONE_BLE) {
            enablePhonePrivacyScanning()
        } else {
            recover(source)
        }
    }

    fun startDirectionSweep(finding: PrivacyFinding): Boolean {
        val currentFinding = resolveDirectionSweepTarget(
            current = repository.currentState.value,
            key = finding.observationKey,
        ) ?: return false
        if (sweepSensorLease == null) {
            sweepSensorLease = sensorFusionEngine.acquire()
        }
        directionController.start(currentFinding)
        val started = directionSweepState.value is DirectionSweepState.Sampling
        if (!started) {
            sweepSensorLease?.close()
            sweepSensorLease = null
        }
        return started
    }

    fun finishDirectionSweep() {
        directionController.finish()
        sweepSensorLease?.close()
        sweepSensorLease = null
    }

    fun cancelDirectionSweep() {
        directionController.cancel()
        sweepSensorLease?.close()
        sweepSensorLease = null
    }

    override fun onCleared() {
        directionController.cancel()
        sweepSensorLease?.close()
        sweepSensorLease = null
        super.onCleared()
    }

    private fun <T> Set<T>.toggle(value: T): Set<T> =
        if (value in this) this - value else this + value
}

internal fun phonePrivacyEnableWrites(
    settings: DetectionSettings,
): List<Pair<InfoSettingKey, Boolean>> = buildList {
    add(InfoSettingKey.PHONE_PRIVACY_SCAN to true)
    if (settings.backendOnlyMode) add(InfoSettingKey.BACKEND_ONLY to false)
}

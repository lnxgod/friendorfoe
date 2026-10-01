package com.friendorfoe.presentation.probes

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.time.MonotonicClock
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import retrofit2.HttpException
import javax.inject.Inject

@HiltViewModel
class ProbeViewModel internal constructor(
    settings: StateFlow<DetectionSettings>,
    private val api: SensorMapApiService,
    private val clock: MonotonicClock,
    private val savedState: SavedStateHandle = SavedStateHandle(),
) : ViewModel() {
    @Inject constructor(prefs: DetectionPrefs, api: SensorMapApiService, clock: MonotonicClock, savedState: SavedStateHandle) :
        this(prefs.settings, api, clock, savedState)

    private val _state = MutableStateFlow(ProbeUiState())
    val state: StateFlow<ProbeUiState> = _state
    private val active = MutableStateFlow(false)
    private val selected = savedState.getStateFlow<String?>("probe_sensor", null)
    private val refresh = Channel<Unit>(Channel.CONFLATED)
    private val changes = ProbeActivityChanges()

    init {
        viewModelScope.launch {
            combine(active, selected, settings.map { it.sensorBackendEnabled to it.backendUrl }.distinctUntilChanged()) {
                visible, sensor, backend -> Triple(visible, sensor, backend)
            }.collectLatest { (visible, sensor, backend) ->
                val previousBackend = savedState.get<String>("probe_backend")
                savedState["probe_backend"] = backend.second
                if (previousBackend != null && previousBackend != backend.second && sensor != null) {
                    _state.update { it.copy(snapshot = com.friendorfoe.data.remote.ProbeActivityDto(),
                        sensorId = null, snapshotElapsedMs = null, newActivity = null) }
                    savedState["probe_sensor"] = null
                    return@collectLatest
                }
                changes.reset()
                // Clear cached rows on source/URL/permission changes; never relabel another sensor's data.
                _state.update { it.copy(enabled = backend.first, sensorId = sensor,
                    snapshot = com.friendorfoe.data.remote.ProbeActivityDto(), snapshotElapsedMs = null,
                    loading = false, error = null, newActivity = null) }
                if (!visible || !backend.first) return@collectLatest
                while (isActive) {
                    _state.update { it.copy(loading = true) }
                    try {
                        val result = api.getProbeActivity(sensor, PROBE_WINDOW_SECONDS)
                        check(result.sensorId == sensor) { "Scanner response did not match the selected scanner" }
                        val count = changes.update(_state.value.copy(snapshot = result,
                            snapshotElapsedMs = clock.nowElapsedMs(), nowElapsedMs = clock.nowElapsedMs()).rows)
                        _state.update { it.copy(snapshot = result, snapshotElapsedMs = clock.nowElapsedMs(),
                            nowElapsedMs = clock.nowElapsedMs(), loading = false, error = null,
                            newActivity = if (it.announceNew && count > 0) "$count probe address${if (count == 1) "" else "es"} with new activity" else null) }
                    } catch (cancelled: CancellationException) { throw cancelled }
                    catch (failure: Exception) {
                        _state.update { it.copy(loading = false, error = if (failure is HttpException && failure.code() == 404)
                            "Update your backend to enable Wi-Fi probe activity."
                        else "Cannot reach probe activity. Check your backend connection and retry.", newActivity = null) }
                    }
                    withTimeoutOrNull(5_000) { refresh.receive() }
                }
            }
        }
        viewModelScope.launch {
            active.collectLatest { visible ->
                if (visible) clock.ticks().collect { now -> _state.update { it.copy(nowElapsedMs = now) } }
            }
        }
    }
    fun setActive(value: Boolean) { active.value = value }
    fun selectSensor(value: String) { savedState["probe_sensor"] = value }
    fun retry() { refresh.trySend(Unit) }
    fun query(value: String) { changes.reset(); _state.update { it.copy(query = value) } }
    fun minimumSignal(value: Int?) { changes.reset(); _state.update { it.copy(minimumRssi = value) } }
    fun directedOnly(value: Boolean) { changes.reset(); _state.update { it.copy(directedOnly = value) } }
    fun strongestFirst(value: Boolean) { _state.update { it.copy(strongestFirst = value) } }
    fun announce(value: Boolean) { changes.reset(); _state.update { it.copy(announceNew = value, newActivity = null) } }
}

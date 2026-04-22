package com.friendorfoe.presentation.calibrate

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.remote.CalibrationModelDto
import com.friendorfoe.data.remote.EventDto
import com.friendorfoe.data.remote.EventStatsDto
import com.friendorfoe.data.remote.NodeDto
import com.friendorfoe.data.remote.ProbeDeviceDto
import com.friendorfoe.data.remote.SensorMapApiService
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class CalibrateConsoleViewModel @Inject constructor(
    private val sensorMapApiService: SensorMapApiService,
) : ViewModel() {

    data class State(
        val loading: Boolean = false,
        val lastRefreshMs: Long? = null,
        val errorMessage: String? = null,
        val nodes: List<NodeDto> = emptyList(),
        val probes: List<ProbeDeviceDto> = emptyList(),
        val events: List<EventDto> = emptyList(),
        val eventStats: EventStatsDto = EventStatsDto(),
        val calibrationModel: CalibrationModelDto? = null,
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var pollJob: Job? = null

    fun startPolling() {
        if (pollJob != null) return
        refreshNow()
        pollJob = viewModelScope.launch {
            while (isActive) {
                delay(5_000L)
                refreshOnce()
            }
        }
    }

    fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
    }

    fun refreshNow() {
        viewModelScope.launch {
            refreshOnce()
        }
    }

    fun ackEvent(eventId: Int) {
        viewModelScope.launch {
            try {
                sensorMapApiService.ackEvent(eventId)
                refreshOnce()
            } catch (e: Exception) {
                _state.value = _state.value.copy(errorMessage = e.message ?: "Ack failed")
            }
        }
    }

    private suspend fun refreshOnce() {
        _state.value = _state.value.copy(loading = true, errorMessage = null)
        try {
            val nodes = sensorMapApiService.getNodesStatus().nodes
            val probes = sensorMapApiService.getProbeDevices(maxAgeS = 86400).devices
            val events = sensorMapApiService.getEvents(
                types = PROBE_INTEL_EVENT_TYPES,
                acknowledged = false,
                sinceHours = 24f,
                limit = 200,
            ).events
            val eventStats = sensorMapApiService.getEventStats()
            val calibrationModel = sensorMapApiService.getCalibrationModel()

            _state.value = State(
                loading = false,
                lastRefreshMs = System.currentTimeMillis(),
                nodes = sortNodesForDiagnostics(nodes),
                probes = probes.sortedByDescending { it.lastSeen ?: 0.0 },
                events = events,
                eventStats = eventStats,
                calibrationModel = calibrationModel,
            )
        } catch (e: Exception) {
            _state.value = _state.value.copy(
                loading = false,
                errorMessage = e.message ?: "Diagnostics refresh failed",
            )
        }
    }
}

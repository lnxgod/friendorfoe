package com.friendorfoe.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.presentation.calibrate.probeIntelBadgeCount
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class AppChromeViewModel @Inject constructor(
    private val sensorMapApiService: SensorMapApiService,
) : ViewModel() {

    private val _calibrateBadgeCount = MutableStateFlow(0)
    val calibrateBadgeCount: StateFlow<Int> = _calibrateBadgeCount.asStateFlow()

    private var pollJob: Job? = null

    fun start() {
        if (pollJob != null) return
        refreshNow()
        pollJob = viewModelScope.launch {
            while (isActive) {
                delay(15_000L)
                fetchBadgeCount()
            }
        }
    }

    fun stop() {
        pollJob?.cancel()
        pollJob = null
    }

    fun refreshNow() {
        viewModelScope.launch {
            fetchBadgeCount()
        }
    }

    private suspend fun fetchBadgeCount() {
        try {
            val stats = sensorMapApiService.getEventStats()
            _calibrateBadgeCount.value = probeIntelBadgeCount(stats)
        } catch (_: Exception) {
            _calibrateBadgeCount.value = 0
        }
    }
}

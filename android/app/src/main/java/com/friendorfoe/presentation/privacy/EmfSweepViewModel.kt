package com.friendorfoe.presentation.privacy

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.detection.EmfDetector
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class EmfSweepUiState(
    val reading: EmfDetector.EmfReading? = null,
    val peakUt: Float = 0f,
    val sensorAvailable: Boolean = true,
    val scanning: Boolean = true
)

@HiltViewModel
class EmfSweepViewModel @Inject constructor(
    private val emfDetector: EmfDetector
) : ViewModel() {
    private val _uiState = MutableStateFlow(EmfSweepUiState())
    val uiState: StateFlow<EmfSweepUiState> = _uiState.asStateFlow()

    private var scanJob: Job? = null

    init {
        start()
    }

    fun start() {
        scanJob?.cancel()
        scanJob = viewModelScope.launch {
            var receivedReading = false
            _uiState.value = EmfSweepUiState(scanning = true)
            emfDetector.startMonitoring().collect { reading ->
                receivedReading = true
                _uiState.update { state ->
                    state.copy(
                        reading = reading,
                        peakUt = maxOf(state.peakUt, reading.magnitudeUt),
                        sensorAvailable = true,
                        scanning = true
                    )
                }
            }
            if (!receivedReading) {
                _uiState.update { it.copy(sensorAvailable = false, scanning = false) }
            }
        }
    }

    fun resetPeak() {
        _uiState.update { it.copy(peakUt = it.reading?.magnitudeUt ?: 0f) }
    }
}

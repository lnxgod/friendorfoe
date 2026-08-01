package com.friendorfoe.presentation.privacy

import android.hardware.SensorManager
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.detection.EmfDetector
import com.friendorfoe.detection.MagneticSample
import com.friendorfoe.detection.MagneticSensorEvent
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlin.math.abs
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

fun sensorAccuracyLabel(accuracy: Int): String = when (accuracy) {
    SensorManager.SENSOR_STATUS_ACCURACY_HIGH -> "High accuracy"
    SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM -> "Medium accuracy"
    SensorManager.SENSOR_STATUS_ACCURACY_LOW -> "Low accuracy"
    else -> "Unreliable"
}

sealed interface MagneticFieldUiState {
    data object Initializing : MagneticFieldUiState
    data object SensorUnavailable : MagneticFieldUiState
    data class AwaitingAccurateBaseline(val accuracyLabel: String) : MagneticFieldUiState
    data class Live(
        val totalMicroTesla: Float,
        val baselineMicroTesla: Float,
        val deviationMicroTesla: Float,
        val peakDeviationMicroTesla: Float,
        val accuracyLabel: String,
    ) : MagneticFieldUiState

    data class Failed(val message: String) : MagneticFieldUiState
}

class MagneticFieldReducer {
    private var resetPending = true
    private var baseline: Float? = null
    private var peakDeviation = 0f
    private val _state = MutableStateFlow<MagneticFieldUiState>(MagneticFieldUiState.Initializing)
    val state: StateFlow<MagneticFieldUiState> = _state.asStateFlow()

    fun beginSession() {
        resetPending = true
        baseline = null
        peakDeviation = 0f
        _state.value = MagneticFieldUiState.Initializing
    }

    fun onUnavailable() {
        _state.value = MagneticFieldUiState.SensorUnavailable
    }

    fun onFailure(message: String) {
        _state.value = MagneticFieldUiState.Failed(message)
    }

    fun requestBaselineReset() {
        resetPending = true
        baseline = null
        peakDeviation = 0f
        _state.value = MagneticFieldUiState.AwaitingAccurateBaseline(
            accuracyLabel = "Waiting for high accuracy",
        )
    }

    fun onSample(sample: MagneticSample) {
        if ((resetPending || baseline == null) &&
            sample.accuracy != SensorManager.SENSOR_STATUS_ACCURACY_HIGH
        ) {
            _state.value = MagneticFieldUiState.AwaitingAccurateBaseline(
                accuracyLabel = sensorAccuracyLabel(sample.accuracy),
            )
            return
        }

        if (resetPending || baseline == null) {
            baseline = sample.totalMicroTesla
            peakDeviation = 0f
            resetPending = false
        }
        val currentBaseline = requireNotNull(baseline)
        val deviation = abs(sample.totalMicroTesla - currentBaseline)
        peakDeviation = maxOf(peakDeviation, deviation)
        _state.value = MagneticFieldUiState.Live(
            totalMicroTesla = sample.totalMicroTesla,
            baselineMicroTesla = currentBaseline,
            deviationMicroTesla = deviation,
            peakDeviationMicroTesla = peakDeviation,
            accuracyLabel = sensorAccuracyLabel(sample.accuracy),
        )
    }
}

@HiltViewModel
class EmfSweepViewModel @Inject constructor(
    private val emfDetector: EmfDetector,
) : ViewModel() {
    private val reducer = MagneticFieldReducer()
    val uiState: StateFlow<MagneticFieldUiState> = reducer.state

    private var scanJob: Job? = null

    fun start() {
        if (scanJob?.isActive == true) return
        reducer.beginSession()
        scanJob = viewModelScope.launch {
            emfDetector.startMonitoring().collect { event ->
                when (event) {
                    MagneticSensorEvent.Unavailable -> reducer.onUnavailable()
                    is MagneticSensorEvent.Failed -> reducer.onFailure(event.message)
                    is MagneticSensorEvent.Sample -> reducer.onSample(event.value)
                }
            }
        }
    }

    fun stop() {
        scanJob?.cancel()
        scanJob = null
    }

    fun resetBaseline() {
        reducer.requestBaselineReset()
    }

    override fun onCleared() {
        stop()
        super.onCleared()
    }
}

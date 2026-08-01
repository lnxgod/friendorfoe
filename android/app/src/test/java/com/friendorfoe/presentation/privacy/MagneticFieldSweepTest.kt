package com.friendorfoe.presentation.privacy

import android.hardware.SensorManager
import com.friendorfoe.detection.MagneticSample
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MagneticFieldSweepTest {
    @Test
    fun initialStateDoesNotInventZeroOrNormal() {
        val reducer = MagneticFieldReducer()

        assertTrue(reducer.state.value is MagneticFieldUiState.Initializing)
    }

    @Test
    fun resetUsesNextAccurateSampleAsBaseline() {
        val reducer = MagneticFieldReducer()
        reducer.onSample(MagneticSample(42f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))

        reducer.requestBaselineReset()

        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)
        reducer.onSample(MagneticSample(55f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))
        val live = reducer.state.value as MagneticFieldUiState.Live
        assertEquals(55f, live.baselineMicroTesla, 0.01f)
        assertEquals(0f, live.deviationMicroTesla, 0.01f)
    }

    @Test
    fun lowAndMediumAccuracyCannotSetBaseline() {
        val reducer = MagneticFieldReducer()

        reducer.onSample(MagneticSample(42f, SensorManager.SENSOR_STATUS_ACCURACY_LOW))
        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)

        reducer.onSample(MagneticSample(45f, SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM))
        assertTrue(reducer.state.value is MagneticFieldUiState.AwaitingAccurateBaseline)
    }

    @Test
    fun unreliableAccuracyCannotSetBaseline() {
        val reducer = MagneticFieldReducer()

        reducer.onSample(MagneticSample(42f, SensorManager.SENSOR_STATUS_UNRELIABLE))

        val waiting = reducer.state.value as MagneticFieldUiState.AwaitingAccurateBaseline
        assertEquals("Unreliable", waiting.accuracyLabel)
    }

    @Test
    fun liveStateTracksAbsoluteDeviationAndPeakWithoutRebaselining() {
        val reducer = MagneticFieldReducer()
        reducer.onSample(MagneticSample(40f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))

        reducer.onSample(MagneticSample(52f, SensorManager.SENSOR_STATUS_ACCURACY_MEDIUM))
        reducer.onSample(MagneticSample(35f, SensorManager.SENSOR_STATUS_ACCURACY_HIGH))

        val live = reducer.state.value as MagneticFieldUiState.Live
        assertEquals(35f, live.totalMicroTesla, 0.01f)
        assertEquals(40f, live.baselineMicroTesla, 0.01f)
        assertEquals(5f, live.deviationMicroTesla, 0.01f)
        assertEquals(12f, live.peakDeviationMicroTesla, 0.01f)
        assertEquals("High accuracy", live.accuracyLabel)
    }

    @Test
    fun unavailableAndFailureEventsAreVisibleStates() {
        val reducer = MagneticFieldReducer()

        reducer.onUnavailable()
        assertTrue(reducer.state.value is MagneticFieldUiState.SensorUnavailable)

        reducer.onFailure("Could not read magnetometer")
        assertEquals(
            MagneticFieldUiState.Failed("Could not read magnetometer"),
            reducer.state.value,
        )
    }
}

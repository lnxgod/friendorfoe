package com.friendorfoe.presentation.ar

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.detection.VisualDetection
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ArBackendPollingTest {
    @Test
    fun endpointReplacementRejectsLateResultAndDisableClearsOnlyRemoteState() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val state = ArBackendIntegrationState()
        val localObservation = VisualDetection(
            trackingId = 7,
            centerX = 0.5f,
            centerY = 0.5f,
            width = 0.1f,
            height = 0.1f,
            labels = listOf("local"),
            timestampMs = 1L,
        )
        state.localObservations.value = listOf(localObservation)
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<Int>? = null
        val job = launch {
            collectArBackend(
                settings = settings,
                intervalMs = 100,
                state = state,
                fetchDroneCount = {
                    fetchCount++
                    when (fetchCount) {
                        1 -> 1
                        2 -> suspendCoroutine<Int> { oldContinuation = it }.also {
                            oldFetchObservedCancellation = !currentCoroutineContext().isActive
                        }
                        else -> 2
                    }
                },
            )
        }

        runCurrent()
        assertTrue(state.sensorBackendOnline.value)
        assertEquals(1, state.sensorDroneCount.value)
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(99)
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertTrue(state.sensorBackendOnline.value)
        assertEquals(2, state.sensorDroneCount.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertFalse(state.sensorBackendOnline.value)
        assertEquals(0, state.sensorDroneCount.value)
        assertEquals(listOf(localObservation), state.localObservations.value)
        job.cancel()
    }
}

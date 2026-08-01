package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.detection.GlassesDetection
import java.time.Instant
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
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PrivacyBackendPollingTest {
    @Test
    fun endpointReplacementRejectsLateResultAndDisableClearsOnlyRemoteState() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val backend = MutableStateFlow<List<GlassesDetection>>(emptyList())
        val pollState = MutableStateFlow<PrivacyBackendPollState>(PrivacyBackendPollState.Disabled)
        val local = MutableStateFlow(listOf(detection("local")))
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<List<GlassesDetection>>? = null
        val job = launch {
            collectPrivacyBackend(
                settings = settings,
                intervalMs = 100,
                backendPrivacyDetections = backend,
                backendPollState = pollState,
                fetchDetections = {
                    fetchCount++
                    when (fetchCount) {
                        1 -> listOf(detection("first"))
                        2 -> suspendCoroutine<List<GlassesDetection>> { oldContinuation = it }.also {
                            oldFetchObservedCancellation = !currentCoroutineContext().isActive
                        }
                        else -> listOf(detection("replacement"))
                    }
                },
            )
        }

        runCurrent()
        assertEquals(listOf("first"), backend.value.map { it.mac })
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(listOf(detection("stale")))
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertEquals(listOf("replacement"), backend.value.map { it.mac })
        assertEquals(PrivacyBackendPollState.Connected, pollState.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertTrue(backend.value.isEmpty())
        assertEquals(PrivacyBackendPollState.Disabled, pollState.value)
        assertEquals(listOf("local"), local.value.map { it.mac })
        job.cancel()
    }

    private fun detection(id: String) = GlassesDetection(
        mac = id,
        deviceName = id,
        deviceType = "test",
        manufacturer = "test",
        hasCamera = false,
        rssi = -50,
        confidence = 1f,
        matchReason = "test",
        firstSeen = Instant.EPOCH,
        lastSeen = Instant.EPOCH,
    )
}

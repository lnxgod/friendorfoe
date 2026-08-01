package com.friendorfoe.presentation.ar

import com.friendorfoe.data.DetectionSettings
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
        val online = MutableStateFlow(false)
        val remoteCount = MutableStateFlow(0)
        val localCount = MutableStateFlow(7)
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<Int>? = null
        val job = launch {
            collectArBackend(
                settings = settings,
                intervalMs = 100,
                sensorBackendOnline = online,
                sensorDroneCount = remoteCount,
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
        assertTrue(online.value)
        assertEquals(1, remoteCount.value)
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(99)
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertTrue(online.value)
        assertEquals(2, remoteCount.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertFalse(online.value)
        assertEquals(0, remoteCount.value)
        assertEquals(7, localCount.value)
        job.cancel()
    }
}

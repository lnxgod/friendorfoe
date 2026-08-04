package com.friendorfoe.presentation

import com.friendorfoe.data.DetectionSettings
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
class BackendPollingGateTest {
    @Test
    fun disablingBackendCancelsPollingAndClearsRemoteState() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(sensorBackendEnabled = true),
        )
        var fetches = 0
        var clears = 0
        val job = launch {
            collectBackendWhileEnabled(
                settings = settings,
                intervalMs = 5_000,
                clear = { clears++ },
                fetch = { ++fetches },
                publish = {},
            )
        }

        runCurrent()
        assertEquals(1, fetches)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()
        advanceTimeBy(10_000)

        assertEquals(1, fetches)
        assertEquals(2, clears)
        job.cancel()
    }

    @Test
    fun changingEndpointCancelsOldLoopAndClearsBeforeRefetch() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(sensorBackendEnabled = true),
        )
        val events = mutableListOf<String>()
        var fetches = 0
        val job = launch {
            collectBackendWhileEnabled(
                settings = settings,
                intervalMs = 5_000,
                clear = { events += "clear" },
                fetch = {
                    fetches++
                    if (fetches == 1) {
                        try {
                            awaitCancellation()
                        } finally {
                            events += "cancel"
                        }
                    }
                    "fresh"
                },
                publish = { events += it },
            )
        }

        runCurrent()
        settings.value = settings.value.copy(backendUrl = "http://field-kit:8000/")
        runCurrent()

        assertEquals(listOf("clear", "cancel", "clear", "fresh"), events)
        job.cancel()
    }

    @Test
    fun lateNonCooperativeFetchCannotPublishAfterEndpointChanges() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(sensorBackendEnabled = true),
        )
        val continuations = mutableListOf<Continuation<String>>()
        val published = mutableListOf<String>()
        var clears = 0
        val job = launch {
            collectBackendWhileEnabled(
                settings = settings,
                intervalMs = 5_000,
                clear = { clears++ },
                fetch = { suspendCoroutine { continuations += it } },
                publish = { published += it },
            )
        }

        runCurrent()
        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        continuations.single().resume("stale")
        runCurrent()

        assertEquals(emptyList<String>(), published)
        assertEquals(2, clears)
        assertEquals(2, continuations.size)
        job.cancel()
        continuations.last().resume("cleanup")
        runCurrent()
    }

    @Test
    fun lateNonCooperativeFailureCannotPublishAfterEndpointChanges() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(sensorBackendEnabled = true),
        )
        val continuations = mutableListOf<Continuation<String>>()
        val failures = mutableListOf<String>()
        val job = launch {
            collectBackendWhileEnabled(
                settings = settings,
                intervalMs = 5_000,
                clear = {},
                fetch = { suspendCoroutine { continuations += it } },
                publish = {},
                onFailure = { failures += it.message.orEmpty() },
            )
        }

        runCurrent()
        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        continuations.single().resumeWith(Result.failure(IOException("stale failure")))
        runCurrent()

        assertEquals(emptyList<String>(), failures)
        assertEquals(2, continuations.size)
        job.cancel()
        continuations.last().resume("cleanup")
        runCurrent()
    }

    @Test
    fun initiallyDisabledBackendDoesNotFetchUntilExplicitlyEnabled() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        var fetches = 0
        val job = launch {
            collectBackendWhileEnabled(
                settings = settings,
                intervalMs = 5_000,
                clear = {},
                fetch = { ++fetches },
                publish = {},
            )
        }

        runCurrent()
        assertEquals(0, fetches)

        settings.value = settings.value.copy(sensorBackendEnabled = true)
        runCurrent()
        assertEquals(1, fetches)
        job.cancel()
    }
}

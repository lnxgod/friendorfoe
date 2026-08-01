package com.friendorfoe.presentation.about

import com.friendorfoe.data.BackendEndpoint
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class BackendConnectionTesterTest {
    @Test
    fun replacementTestRejectsLateResultFromOldEndpoint() = runTest {
        val first = BackendEndpoint.parse("http://first.example/").getOrThrow()
        val replacement = BackendEndpoint.parse("https://replacement.example/").getOrThrow()
        val status = MutableStateFlow<ConnectionTestState>(ConnectionTestState.Idle)
        var fetches = 0
        var oldContinuation: Continuation<String?>? = null
        val tester = BackendConnectionTester(
            scope = this,
            connectionStatus = status,
            fetchServerVersion = {
                fetches++
                if (fetches == 1) {
                    suspendCoroutine<String?> { oldContinuation = it }
                } else {
                    "2.0"
                }
            },
        )

        tester.test(first)
        runCurrent()
        assertEquals(ConnectionTestState.Checking(first), status.value)
        tester.test(replacement)
        runCurrent()
        assertEquals(ConnectionTestState.Connected(replacement, "2.0"), status.value)

        oldContinuation!!.resume("stale")
        runCurrent()

        assertEquals(ConnectionTestState.Connected(replacement, "2.0"), status.value)
    }

    @Test
    fun resetRejectsLateResultAndLeavesConnectionIdle() = runTest {
        val endpoint = BackendEndpoint.parse("http://first.example/").getOrThrow()
        val status = MutableStateFlow<ConnectionTestState>(ConnectionTestState.Idle)
        var continuation: Continuation<String?>? = null
        val tester = BackendConnectionTester(
            scope = this,
            connectionStatus = status,
            fetchServerVersion = { suspendCoroutine<String?> { continuation = it } },
        )

        tester.test(endpoint)
        runCurrent()
        tester.reset()
        continuation!!.resume("stale")
        runCurrent()

        assertEquals(ConnectionTestState.Idle, status.value)
    }
}

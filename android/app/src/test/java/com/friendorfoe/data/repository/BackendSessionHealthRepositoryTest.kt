package com.friendorfoe.data.repository

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.remote.BackendHealthClient
import com.friendorfoe.data.remote.BackendHealthResponse
import kotlin.coroutines.Continuation
import kotlin.coroutines.CoroutineContext
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

@OptIn(ExperimentalCoroutinesApi::class)
class BackendSessionHealthRepositoryTest {
    @Test
    fun concurrentlyStartedChecksSerializeTheirSessionTransition() {
        val endpointA = BackendEndpoint.parse("http://badge-lab:8000/").getOrThrow()
        val endpointB = BackendEndpoint.parse("https://field-kit.example/").getOrThrow()
        val scope = BlockingFirstLaunchScope()
        val repository = BackendSessionHealthRepository(
            healthClient = BackendHealthClient { awaitCancellation() },
            scope = scope,
        )
        val executor = Executors.newFixedThreadPool(2)
        val firstReturned = CountDownLatch(1)
        val secondReturned = CountDownLatch(1)

        try {
            executor.execute {
                repository.check(endpointA, enabled = true)
                firstReturned.countDown()
            }
            assertTrue("first check never entered its transition", scope.firstLaunchEntered.await(2, TimeUnit.SECONDS))

            executor.execute {
                repository.check(endpointB, enabled = true)
                secondReturned.countDown()
            }

            assertFalse(
                "a concurrent check entered while the first session transition was incomplete",
                secondReturned.await(150, TimeUnit.MILLISECONDS),
            )

            scope.releaseFirstLaunch.countDown()
            assertTrue(firstReturned.await(2, TimeUnit.SECONDS))
            assertTrue(secondReturned.await(2, TimeUnit.SECONDS))
            assertEquals(SessionHealth.Checking(endpointB), repository.health.value)
        } finally {
            scope.releaseFirstLaunch.countDown()
            repository.invalidate()
            executor.shutdownNow()
        }
    }

    @Test
    fun lateResponseFromReplacedEndpointCannotPublish() = runTest {
        val endpointA = BackendEndpoint.parse("http://badge-lab:8000/").getOrThrow()
        val endpointB = BackendEndpoint.parse("https://field-kit.example/").getOrThrow()
        var endpointAContinuation: Continuation<BackendHealthResponse>? = null
        val repository = BackendSessionHealthRepository(
            healthClient = BackendHealthClient { endpoint ->
                if (endpoint == endpointA) {
                    suspendCoroutine { endpointAContinuation = it }
                } else {
                    BackendHealthResponse(status = "ok", version = "2.0")
                }
            },
            scope = this,
        )

        repository.check(endpointA, enabled = true)
        runCurrent()
        assertEquals(SessionHealth.Checking(endpointA), repository.health.value)

        repository.check(endpointB, enabled = true)
        runCurrent()
        assertEquals(SessionHealth.Healthy(endpointB), repository.health.value)
        assertEquals("2.0", repository.serverVersion.value)

        endpointAContinuation!!.resume(BackendHealthResponse(status = "ok", version = "stale"))
        runCurrent()

        assertEquals(SessionHealth.Healthy(endpointB), repository.health.value)
        assertEquals("2.0", repository.serverVersion.value)
    }

    @Test
    fun disabledCheckClearsSessionEvidenceWithoutCallingBackend() = runTest {
        var calls = 0
        val endpoint = BackendEndpoint.parse("http://badge-lab:8000/").getOrThrow()
        val repository = BackendSessionHealthRepository(
            healthClient = BackendHealthClient {
                calls += 1
                BackendHealthResponse(status = "ok", version = "1.0")
            },
            scope = this,
        )
        repository.recordConnected(endpoint, serverVersion = "1.0")

        repository.check(endpoint, enabled = false)
        runCurrent()

        assertEquals(SessionHealth.Untested, repository.health.value)
        assertNull(repository.serverVersion.value)
        assertEquals(0, calls)
    }
}

private class BlockingFirstLaunchScope : CoroutineScope {
    private val firstRead = AtomicBoolean(true)
    val firstLaunchEntered = CountDownLatch(1)
    val releaseFirstLaunch = CountDownLatch(1)

    override val coroutineContext: CoroutineContext
        get() {
            if (firstRead.compareAndSet(true, false)) {
                firstLaunchEntered.countDown()
                check(releaseFirstLaunch.await(2, TimeUnit.SECONDS)) {
                    "timed out waiting to release the first launch"
                }
            }
            return SupervisorJob() + Dispatchers.Unconfined
        }
}

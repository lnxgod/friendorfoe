package com.friendorfoe.data.badge

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeTransportRaceCoordinatorTest {

    @Test
    fun capturedHttpAuthorityCannotStartPostAfterTransportStop() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val token = transportGate.claim(
            transportGate.captureSession(session),
            BadgeTransport.LOCAL_AP_HTTP,
        )!!
        val authority = BadgeHttpCommandAuthority(token, BADGE_AP_ENDPOINT)
        val commandStarts = BadgeHttpCommandStartGate(transportGate)
        var postStarts = 0

        transportGate.stopSession()

        assertFalse(
            commandStarts.startIfAuthorized(
                expected = authority,
                current = { authority },
                start = { postStarts += 1 },
            ),
        )
        assertEquals(0, postStarts)
    }

    @Test
    fun capturedApAuthorityCannotStartPostAfterDirectUsbWins() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val sessionToken = transportGate.captureSession(session)
        val apToken = transportGate.claim(
            sessionToken,
            BadgeTransport.LOCAL_AP_HTTP,
        )!!
        val authority = BadgeHttpCommandAuthority(apToken, BADGE_AP_ENDPOINT)
        val commandStarts = BadgeHttpCommandStartGate(transportGate)
        var postStarts = 0

        transportGate.claimNewConnection(sessionToken, BadgeTransport.USB_SERIAL)!!

        assertFalse(
            commandStarts.startIfAuthorized(
                expected = authority,
                current = { authority },
                start = { postStarts += 1 },
            ),
        )
        assertEquals(0, postStarts)
    }

    @Test
    fun debugPostStartRequiresExactCurrentPhysicalTarget() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val token = transportGate.claim(
            transportGate.captureSession(session),
            BadgeTransport.DEBUG_BRIDGE,
        )!!
        val targetA = BadgeHttpCommandAuthority(token, "/dev/cu.usbmodem-A")
        val targetB = BadgeHttpCommandAuthority(token, "/dev/cu.usbmodem-B")
        val commandStarts = BadgeHttpCommandStartGate(transportGate)
        var postStarts = 0

        assertFalse(
            commandStarts.startIfAuthorized(
                expected = targetA,
                current = { targetB },
                start = { postStarts += 1 },
            ),
        )
        assertEquals(0, postStarts)
    }

    @Test
    fun inFlightNewerDebugStatusBlocksOlderTargetPostStart() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val tokenA = transportGate.claim(
            transportGate.captureSession(session),
            BadgeTransport.DEBUG_BRIDGE,
        )!!
        val authorityA = BadgeHttpCommandAuthority(tokenA, "/dev/cu.usbmodem-A")
        val commandStarts = BadgeHttpCommandStartGate(transportGate)
        val statusRequests = BadgeHttpStatusRequestGate()
        statusRequests.startSession(session)
        val pendingTargetB = statusRequests.begin(
            sessionGeneration = session,
            transport = BadgeTransport.DEBUG_BRIDGE,
        )!!
        var postStarts = 0

        assertFalse(
            statusRequests.runIfNoActiveRequest(BadgeTransport.DEBUG_BRIDGE) {
                commandStarts.startIfAuthorized(
                    expected = authorityA,
                    current = { authorityA },
                    start = { postStarts += 1 },
                )
            },
        )
        assertEquals(0, postStarts)

        statusRequests.finish(pendingTargetB)
        assertTrue(
            statusRequests.runIfNoActiveRequest(BadgeTransport.DEBUG_BRIDGE) {
                commandStarts.startIfAuthorized(
                    expected = authorityA,
                    current = { authorityA },
                    start = { postStarts += 1 },
                )
            },
        )
        assertEquals(1, postStarts)
    }

    @Test
    fun slowOlderDebugStatusCannotReclaimAfterFastNewerStatus() {
        val requests = BadgeHttpStatusRequestGate()
        requests.startSession(sessionGeneration = 41L)
        val slowTargetA = requests.begin(
            sessionGeneration = 41L,
            transport = BadgeTransport.DEBUG_BRIDGE,
        )!!
        val fastTargetB = requests.begin(
            sessionGeneration = 41L,
            transport = BadgeTransport.DEBUG_BRIDGE,
        )!!
        var publishedTarget: String? = null

        assertTrue(requests.runIfLatest(fastTargetB) { publishedTarget = "B" })
        assertFalse(requests.runIfLatest(slowTargetA) { publishedTarget = "A" })
        assertEquals("B", publishedTarget)
    }

    @Test
    fun stoppedHttpRequestGateCannotAuthorizeCommandStart() {
        val requests = BadgeHttpStatusRequestGate()
        requests.startSession(sessionGeneration = 52L)
        requests.stopSession()
        var commandStartRan = false

        assertFalse(
            requests.runIfNoActiveRequest(BadgeTransport.LOCAL_AP_HTTP) {
                commandStartRan = true
            },
        )
        assertFalse(commandStartRan)
    }

    @Test
    fun staleHttpStatusCannotReplaceNewerTransportToken() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val sessionToken = transportGate.captureSession(session)
        val slowStatusToken = transportGate.claim(
            sessionToken,
            BadgeTransport.DEBUG_BRIDGE,
        )!!
        var fastStatusToken: BadgeActiveTransportToken? = null

        assertTrue(
            transportGate.replaceAndRunIfCurrent(slowStatusToken) { replacement ->
                fastStatusToken = replacement
            },
        )
        var staleReplacementRan = false

        assertFalse(
            transportGate.replaceAndRunIfCurrent(slowStatusToken) {
                staleReplacementRan = true
            },
        )
        assertFalse(staleReplacementRan)
        assertTrue(transportGate.isCurrent(fastStatusToken!!))
    }

    @Test
    fun stoppedSessionRejectsPreviouslyCapturedBleScanStart() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val callback = Any()
        var scanStarts = 0

        transportGate.stopSession()

        assertFalse(
            transportGate.runIfSessionHasNoActiveTransport(session) {
                scanLeases.startIfIdle(callback) { scanStarts += 1 }
            },
        )
        assertEquals(0, scanStarts)
        assertFalse(scanLeases.isCurrent(callback))
    }

    @Test
    fun scanThatStartsFirstIsStoppedBeforeStopReturns() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val callback = Any()
        val events = Collections.synchronizedList(mutableListOf<String>())
        val startEntered = CountDownLatch(1)
        val allowStartToReturn = CountDownLatch(1)
        val startThread = thread(start = true) {
            transportGate.runIfSessionHasNoActiveTransport(session) {
                scanLeases.startIfIdle(callback) {
                    startEntered.countDown()
                    allowStartToReturn.await()
                    events += "startScan"
                }
            }
        }
        assertTrue(startEntered.await(5, TimeUnit.SECONDS))
        val stopThread = thread(start = true) {
            transportGate.stopSession()
            scanLeases.stopCurrent { events += "stopScan" }
            events += "stopReturned"
        }

        allowStartToReturn.countDown()
        startThread.join()
        stopThread.join()

        assertEquals(listOf("startScan", "stopScan", "stopReturned"), events)
    }

    @Test
    fun concurrentBleScanAttemptsStartExactlyOneLease() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val scanStarts = AtomicInteger(0)
        val results = Collections.synchronizedList(mutableListOf<Boolean>())
        val attemptsReady = CountDownLatch(2)
        val beginAttempts = CountDownLatch(1)
        val attempts = List(2) {
            thread(start = true) {
                attemptsReady.countDown()
                beginAttempts.await()
                var acquired = false
                transportGate.runIfSessionHasNoActiveTransport(session) {
                    acquired = scanLeases.startIfIdle(Any()) { scanStarts.incrementAndGet() }
                }
                results += acquired
            }
        }

        assertTrue(attemptsReady.await(5, TimeUnit.SECONDS))
        beginAttempts.countDown()
        attempts.forEach { it.join() }

        assertEquals(1, results.count { it })
        assertEquals(1, scanStarts.get())
    }

    @Test
    fun staleBleScanTimeoutCannotClearNewerLease() {
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val oldCallback = Any()
        val newCallback = Any()

        assertTrue(scanLeases.startIfIdle(oldCallback) {})
        assertTrue(scanLeases.stopIfCurrent(oldCallback) {})
        assertTrue(scanLeases.startIfIdle(newCallback) {})

        assertFalse(scanLeases.stopIfCurrent(oldCallback) {})
        assertTrue(scanLeases.isCurrent(newCallback))
    }

    @Test
    fun lifecycleStopWaitsForExactBleStopAttemptBeforeReturning() {
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val callback = Any()
        val stopAttemptEntered = CountDownLatch(1)
        val allowStopAttemptToReturn = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        assertTrue(scanLeases.startIfIdle(callback) {})
        val callbackThread = thread(start = true) {
            scanLeases.stopIfCurrent(callback) {
                stopAttemptEntered.countDown()
                allowStopAttemptToReturn.await()
                events += "stopScan"
            }
        }
        assertTrue(stopAttemptEntered.await(5, TimeUnit.SECONDS))
        val lifecycleThread = thread(start = true) {
            scanLeases.stopCurrent { events += "duplicateStopScan" }
            events += "stopReturned"
        }

        allowStopAttemptToReturn.countDown()
        callbackThread.join()
        lifecycleThread.join()

        assertEquals(listOf("stopScan", "stopReturned"), events)
    }

    @Test
    fun scanFailureCannotPublishAfterSessionStop() {
        val transportGate = BadgeTransportGenerationGate()
        val session = transportGate.startSession()
        val scanLeases = BadgeBleScanLeaseCoordinator<Any>()
        val callback = Any()
        var failurePublished = false
        assertTrue(scanLeases.startIfIdle(callback) {})
        assertTrue(scanLeases.completeIfCurrent(callback))

        transportGate.stopSession()

        assertFalse(
            transportGate.runIfSessionCurrent(session) { failurePublished = true },
        )
        assertFalse(failurePublished)
    }
}

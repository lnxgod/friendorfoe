package com.friendorfoe.data.badge

import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbLifecycleGateTest {

    @Test
    fun `begin and end are idempotent`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        assertFalse(gate.begin())
        assertTrue(gate.end())
        assertFalse(gate.end())
        assertTrue(gate.begin())
    }

    @Test
    fun `stop invalidates a queued connect from its session`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        val queuedConnectSession = requireNotNull(gate.activeSession())

        assertTrue(gate.end(queuedConnectSession))
        assertFalse(gate.isActive(queuedConnectSession))
    }

    @Test
    fun `stop then start invalidates stale cleanup`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        val staleCleanupSession = requireNotNull(gate.activeSession())
        assertTrue(gate.end(staleCleanupSession))
        assertTrue(gate.canClean(staleCleanupSession))

        assertTrue(gate.begin())
        val currentSession = requireNotNull(gate.activeSession())
        assertFalse(gate.canClean(staleCleanupSession))
        assertTrue(gate.isActive(currentSession))
    }

    @Test
    fun `reader errors require the active session and exact connection`() {
        assertTrue(badgeUsbReaderOwnsSession(lifecycleActive = true, activeConnectionMatches = true))
        assertFalse(badgeUsbReaderOwnsSession(lifecycleActive = false, activeConnectionMatches = true))
        assertFalse(badgeUsbReaderOwnsSession(lifecycleActive = true, activeConnectionMatches = false))
    }

    @Test
    @OptIn(ExperimentalCoroutinesApi::class)
    fun `stale reader action queued behind USB mutex is skipped`() = runTest {
        val mutex = Mutex()
        val ownsReader = AtomicBoolean(true)
        val actionRan = AtomicBoolean(false)

        mutex.lock()
        val queuedAction = launch {
            mutex.withBadgeUsbReaderOwner(ownsReader::get) {
                actionRan.set(true)
            }
        }
        runCurrent()
        ownsReader.set(false)
        mutex.unlock()
        queuedAction.join()

        assertFalse(actionRan.get())
    }

    @Test
    fun `current reader owner executes action under USB mutex`() = runTest {
        val actionRan = AtomicBoolean(false)

        val executed = Mutex().withBadgeUsbReaderOwner({ true }) {
            actionRan.set(true)
        }

        assertTrue(executed)
        assertTrue(actionRan.get())
    }
}

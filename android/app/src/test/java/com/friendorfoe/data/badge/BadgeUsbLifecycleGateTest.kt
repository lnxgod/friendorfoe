package com.friendorfoe.data.badge

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
}

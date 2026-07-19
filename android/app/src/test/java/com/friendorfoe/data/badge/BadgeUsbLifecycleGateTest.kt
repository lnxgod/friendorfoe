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
}

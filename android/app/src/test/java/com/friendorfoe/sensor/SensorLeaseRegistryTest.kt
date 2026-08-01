package com.friendorfoe.sensor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class SensorLeaseRegistryTest {
    @Test
    fun hardwareStopsOnlyAfterTheLastIndependentLeaseCloses() {
        val events = mutableListOf<String>()
        val registry = SensorLeaseRegistry(
            onFirstLease = { events += "start" },
            onLastLease = { events += "stop" },
        )

        val arLease = registry.acquire()
        val privacyLease = registry.acquire()
        assertEquals(listOf("start"), events)

        privacyLease.close()
        assertEquals(listOf("start"), events)

        arLease.close()
        assertEquals(listOf("start", "stop"), events)
    }

    @Test
    fun closingTheSameLeaseTwiceIsHarmless() {
        var running = false
        val registry = SensorLeaseRegistry(
            onFirstLease = { running = true },
            onLastLease = { running = false },
        )

        val lease = registry.acquire()
        lease.close()
        lease.close()

        assertFalse(running)
    }

    @Test
    fun aFailedFirstStartDoesNotLeaveAPhantomLease() {
        var attempts = 0
        val registry = SensorLeaseRegistry(
            onFirstLease = {
                attempts += 1
                if (attempts == 1) error("sensor registration failed")
            },
            onLastLease = {},
        )

        runCatching { registry.acquire() }
        val recovered = registry.acquire()

        assertEquals(2, attempts)
        recovered.close()
    }
}

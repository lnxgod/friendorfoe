package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class DetectionSessionGateTest {

    @Test
    fun ended_generation_rejects_late_callbacks_and_restart_is_distinct() {
        val gate = DetectionSessionGate()
        val events = mutableListOf<String>()
        val firstGeneration = gate.ensureSession(
            onStarted = { events += "start:$it" },
            onActive = { fail("first session unexpectedly reused") },
        )

        assertTrue(
            gate.runIfActive(firstGeneration) {
                events += "record:$firstGeneration"
            }
        )
        val secondGeneration = gate.restartSession(
            onEnded = { events += "clear:$firstGeneration" },
            onStarted = { events += "start:$it" },
        )
        requireNotNull(secondGeneration)
        assertFalse(
            gate.runIfActive(firstGeneration) {
                events += "late:$firstGeneration"
            }
        )
        assertNotEquals(firstGeneration, secondGeneration)
        assertTrue(
            gate.runIfActive(secondGeneration) {
                events += "publish:$secondGeneration"
            }
        )
        assertEquals(
            listOf(
                "start:$firstGeneration",
                "record:$firstGeneration",
                "clear:$firstGeneration",
                "start:$secondGeneration",
                "publish:$secondGeneration",
            ),
            events,
        )
    }
}

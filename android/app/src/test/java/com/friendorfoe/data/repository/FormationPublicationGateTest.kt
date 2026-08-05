package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FormationPublicationGateTest {

    @Test
    fun firstChangedPixelPublishesImmediately() {
        val gate = FormationPublicationGate(minimumIntervalMs = 500)

        assertTrue(gate.shouldPublish(mapChanged = true, nowMs = 1_000))
    }

    @Test
    fun unchangedCarouselRefreshesStayQuiet() {
        val gate = FormationPublicationGate(minimumIntervalMs = 500)

        assertTrue(gate.shouldPublish(mapChanged = true, nowMs = 1_000))
        repeat(240) { index ->
            assertFalse(
                gate.shouldPublish(mapChanged = false, nowMs = 1_100L + index * 200L),
            )
        }
    }

    @Test
    fun reportsDelayForTimerDrivenTrailingPublication() {
        val gate = FormationPublicationGate(minimumIntervalMs = 500)

        assertTrue(gate.shouldPublish(mapChanged = true, nowMs = 1_000))
        assertNull(gate.pendingDelayMs(nowMs = 1_100))
        assertFalse(gate.shouldPublish(mapChanged = true, nowMs = 1_200))
        assertEquals(300L, gate.pendingDelayMs(nowMs = 1_200))
        assertTrue(gate.shouldPublish(mapChanged = false, nowMs = 1_500))
        assertNull(gate.pendingDelayMs(nowMs = 1_500))
    }
}

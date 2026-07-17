package com.friendorfoe.presentation.map

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MapHeadingStabilizerTest {

    @Test
    fun `first heading emits immediately and jitter is suppressed`() {
        val filter = MapHeadingStabilizer()

        assertEquals(10f, requireNotNull(filter.update(10f, 0)), 0.001f)
        assertNull(filter.update(11f, 100))
        assertNull(filter.update(12.9f, 200))
    }

    @Test
    fun `heading crosses north by the shortest circular path`() {
        val filter = MapHeadingStabilizer()
        filter.update(359f, 0)

        val next = requireNotNull(filter.update(5f, 250))
        assertTrue(next > 359f || next < 5f)
    }

    @Test
    fun `heading output is rate limited and follows the time constant`() {
        val filter = MapHeadingStabilizer()
        filter.update(0f, 0)

        assertNull(filter.update(90f, 99))
        assertEquals(90f * (1f - kotlin.math.exp(-100.0 / 250.0)).toFloat(), filter.update(90f, 100)!!, 0.001f)
    }

    @Test
    fun `first heading is normalized and reset clears state`() {
        val filter = MapHeadingStabilizer()

        assertEquals(350f, filter.update(-10f, 0))
        filter.reset()

        assertEquals(10f, filter.update(370f, 10))
    }
}

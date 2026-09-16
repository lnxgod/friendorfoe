package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ListLocationPolicyTest {
    private val now = 100_000_000_000L
    private fun fix(ageSeconds: Long, accuracy: Float = 10f) = ListLocationFix(
        Position(32.7, -117.1, 0.0), now - ageSeconds * 1_000_000_000L, accuracy,
    )

    @Test
    fun freshNetworkFixBeatsStaleGpsAndDelayedCallbacks() {
        val network = fix(2)
        assertEquals(network, selectListLocationFix(listOf(fix(60), network), now))
        assertEquals(network, selectListLocationFix(listOf(network, fix(10)), now))
    }

    @Test
    fun staleFutureAndInvalidFixesCannotSeedScanning() {
        listOf(fix(31), fix(-1), fix(0).copy(position = Position(Double.NaN, 0.0, 0.0)),
            fix(0).copy(position = Position(91.0, 0.0, 0.0))).forEach {
            assertNull(selectListLocationFix(listOf(it), now))
        }
        assertEquals(fix(30), selectListLocationFix(listOf(fix(30)), now))
    }

    @Test
    fun simultaneousFixesPreferAccuracyAndZeroCoordinatesAreValid() {
        val precise = fix(1, 5f)
        assertEquals(precise, selectListLocationFix(listOf(precise, fix(1, 500f)), now))
        val zero = precise.copy(position = Position(0.0, 0.0, 0.0))
        assertEquals(zero, selectListLocationFix(listOf(zero), now))
    }
}

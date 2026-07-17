package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LocationEvidenceTest {

    @Test
    fun finite_nonnegative_accuracy_is_preserved() {
        assertEquals(0f, validatedLocationAccuracyMeters(true, 0f))
        assertEquals(12.5f, validatedLocationAccuracyMeters(true, 12.5f))
    }

    @Test
    fun unavailable_invalid_or_negative_accuracy_becomes_unknown() {
        listOf(
            validatedLocationAccuracyMeters(false, 5f),
            validatedLocationAccuracyMeters(true, -1f),
            validatedLocationAccuracyMeters(true, Float.NaN),
            validatedLocationAccuracyMeters(true, Float.POSITIVE_INFINITY),
            validatedLocationAccuracyMeters(true, Float.NEGATIVE_INFINITY),
        ).forEach { accuracy ->
            assertTrue(accuracy.isInfinite())
            assertTrue(accuracy > 0f)
        }
    }

    @Test
    fun scanner_only_start_does_not_replace_an_active_real_fix() {
        val realFix = UserLocationFix(
            latitude = 37.7749,
            longitude = -122.4194,
            accuracyMeters = 8f,
        )
        val scannerOnly = userLocationFixForStart(
            latitude = 0.0,
            longitude = 0.0,
            accuracyMeters = Float.POSITIVE_INFINITY,
        )

        assertNull(scannerOnly)
        assertEquals(realFix, mergeUserLocationFix(realFix, scannerOnly))
    }

    @Test
    fun zero_coordinates_with_valid_accuracy_are_a_real_fix() {
        val fix = userLocationFixForStart(
            latitude = 0.0,
            longitude = 0.0,
            accuracyMeters = 6f,
        )

        assertFalse(fix == null)
        assertEquals(6f, fix?.accuracyMeters)
    }
}

package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.AircraftRange
import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Test

class ListDistanceLabelTest {
    @Test fun missingOrInvalidDistanceNeverLooksNearby() {
        listOf(null, Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, -1.0).forEach {
            assertEquals("Unknown", listDistanceLabel(it))
        }
    }

    @Test fun validDistancesKeepTenthsOfAMileInsteadOfRoundingAwayUsefulDistance() {
        val previous = Locale.getDefault()
        try {
            Locale.setDefault(Locale.US)
            assertEquals("0 m", listDistanceLabel(0.0))
            assertEquals("150 m", listDistanceLabel(150.0))
            assertEquals("18.2 mi", listDistanceLabel(18.2 * AircraftRange.METERS_PER_MILE))
        } finally { Locale.setDefault(previous) }
    }
}

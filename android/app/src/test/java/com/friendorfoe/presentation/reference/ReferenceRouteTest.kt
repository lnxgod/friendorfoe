package com.friendorfoe.presentation.reference

import org.junit.Assert.assertEquals
import org.junit.Test

class ReferenceRouteTest {

    @Test
    fun routeEncodesTabAndQueryExactlyOnce() {
        assertEquals(
            "reference_guide?tab=drones&query=DJI%20Mini%2F4",
            referenceGuideRoute(ReferenceTab.DRONES, "DJI Mini/4"),
        )
    }

    @Test
    fun routePreservesPercentPlusSpaceAndSlashAsDistinctInput() {
        assertEquals(
            "reference_guide?tab=aircraft&query=100%25%20%2B%20A%2FB",
            referenceGuideRoute(ReferenceTab.AIRCRAFT, "100% + A/B"),
        )
    }
}

package com.friendorfoe.presentation.util

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class AircraftPhotosTest {
    @Test
    fun normalizesExactBundledType() {
        assertEquals(
            expectedReferenceUrl("aircraft/B738.jpg"),
            getAircraftPhotoUrl(" b738 "),
        )
    }

    @Test
    fun databaseAliasUsesItsRepresentativeBundledPhoto() {
        assertEquals(
            expectedReferenceUrl("aircraft/B737.jpg"),
            getAircraftPhotoUrl("B736"),
        )
    }

    @Test
    fun missingTypeDoesNotInventAPhoto() {
        assertNull(getAircraftPhotoUrl(null))
        assertNull(getAircraftPhotoUrl("ZZZZ"))
    }
}

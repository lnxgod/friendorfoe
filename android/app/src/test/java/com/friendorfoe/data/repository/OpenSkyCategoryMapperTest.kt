package com.friendorfoe.data.repository

import com.friendorfoe.domain.model.ObjectCategory
import org.junit.Assert.assertEquals
import org.junit.Test

class OpenSkyCategoryMapperTest {

    @Test
    fun highPerformanceCategoryDoesNotMapToMilitary() {
        assertEquals(ObjectCategory.GENERAL_AVIATION, mapOpenSkyCategory(7))
    }

    @Test
    fun mapsRotorcraftUavAndSurfaceVehicleCategories() {
        assertEquals(ObjectCategory.HELICOPTER, mapOpenSkyCategory(8))
        assertEquals(ObjectCategory.DRONE, mapOpenSkyCategory(14))
        assertEquals(ObjectCategory.EMERGENCY, mapOpenSkyCategory(16))
        assertEquals(ObjectCategory.GROUND_VEHICLE, mapOpenSkyCategory(17))
    }
}

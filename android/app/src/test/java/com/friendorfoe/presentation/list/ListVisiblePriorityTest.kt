package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.Instant

class ListVisiblePriorityTest {

    @Test
    fun `active visible objects sort before confidence and distance`() {
        val highConfidence = aircraft("HIGH", confidence = 0.99f, distanceMeters = 500.0)
        val visibleLowerConfidence = aircraft("VISIBLE", confidence = 0.50f, distanceMeters = 2000.0)
        val mediumConfidence = aircraft("MED", confidence = 0.80f, distanceMeters = 100.0)

        val sorted = sortSkyObjectsForList(
            listOf(highConfidence, visibleLowerConfidence, mediumConfidence),
            activeVisualFocusIds = setOf("VISIBLE")
        )

        assertEquals(listOf("VISIBLE", "HIGH", "MED"), sorted.map { it.id })
    }

    @Test
    fun `objects within the same visible group keep confidence then distance ordering`() {
        val visibleFarHighConfidence = aircraft("VISIBLE_HIGH", confidence = 0.90f, distanceMeters = 2000.0)
        val visibleNearLowConfidence = aircraft("VISIBLE_LOW", confidence = 0.70f, distanceMeters = 100.0)
        val hiddenHighConfidence = aircraft("HIDDEN_HIGH", confidence = 0.95f, distanceMeters = 50.0)
        val hiddenLowerConfidence = aircraft("HIDDEN_LOW", confidence = 0.60f, distanceMeters = 10.0)

        val sorted = sortSkyObjectsForList(
            listOf(hiddenLowerConfidence, visibleNearLowConfidence, hiddenHighConfidence, visibleFarHighConfidence),
            activeVisualFocusIds = setOf("VISIBLE_HIGH", "VISIBLE_LOW")
        )

        assertEquals(
            listOf("VISIBLE_HIGH", "VISIBLE_LOW", "HIDDEN_HIGH", "HIDDEN_LOW"),
            sorted.map { it.id }
        )
    }

    @Test
    fun `public safety aircraft sort before ordinary aircraft in the list`() {
        val ordinaryNearby = aircraft(
            id = "NORM",
            confidence = 0.99f,
            distanceMeters = 100.0,
            category = ObjectCategory.COMMERCIAL
        )
        val sheriffHelicopter = aircraft(
            id = "SHERIFF",
            confidence = 0.80f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.GOVERNMENT,
            aircraftType = "AS50",
            aircraftModel = "Eurocopter AS350",
            operatorName = "SAN DIEGO COUNTY SHERIFF",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )

        val sorted = sortSkyObjectsForList(
            listOf(ordinaryNearby, sheriffHelicopter),
            activeVisualFocusIds = emptySet()
        )

        assertEquals(listOf("SHERIFF", "NORM"), sorted.map { it.id })
    }

    @Test
    fun `sheriff rotorcraft row text calls out sheriff helicopter`() {
        val sheriffHelicopter = aircraft(
            id = "ABC123",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.GOVERNMENT,
            aircraftType = "AS50",
            aircraftModel = "Eurocopter AS350",
            operatorName = "SAN DIEGO COUNTY SHERIFF",
            registration = "N123SD",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )

        assertEquals("SHERIFF HELICOPTER  ABC123", listPrimaryText(sheriffHelicopter))
        assertEquals("SAN DIEGO COUNTY SHERIFF - Eurocopter AS350 - N123SD", listSecondaryText(sheriffHelicopter))
        assertEquals("LAW", listBadgeText(sheriffHelicopter))
    }

    private fun aircraft(
        id: String,
        confidence: Float,
        distanceMeters: Double,
        category: ObjectCategory = ObjectCategory.COMMERCIAL,
        aircraftType: String? = null,
        aircraftModel: String? = null,
        operatorName: String? = null,
        registration: String? = null,
        classificationSignals: List<String>? = null
    ): Aircraft {
        return Aircraft(
            id = id,
            position = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 1000.0),
            source = DetectionSource.ADS_B,
            category = category,
            confidence = confidence,
            firstSeen = Instant.EPOCH,
            lastUpdated = Instant.EPOCH,
            distanceMeters = distanceMeters,
            icaoHex = id,
            registration = registration,
            aircraftType = aircraftType,
            aircraftModel = aircraftModel,
            operatorName = operatorName,
            classificationSignals = classificationSignals
        )
    }
}

package com.friendorfoe.presentation.list

import androidx.compose.ui.graphics.Color
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.time.Instant

class ListVisiblePriorityTest {

    @Test
    fun `nearby objects sort before visual focus and confidence`() {
        val highConfidence = aircraft("HIGH", confidence = 0.99f, distanceMeters = 500.0)
        val visibleLowerConfidence = aircraft("VISIBLE", confidence = 0.50f, distanceMeters = 2000.0)
        val mediumConfidence = aircraft("MED", confidence = 0.80f, distanceMeters = 100.0)

        val sorted = sortSkyObjectsForList(
            listOf(highConfidence, visibleLowerConfidence, mediumConfidence),
            activeVisualFocusIds = setOf("VISIBLE")
        )

        assertEquals(listOf("MED", "HIGH", "VISIBLE"), sorted.map { it.id })
    }

    @Test
    fun `distance takes priority across visible groups`() {
        val visibleFarHighConfidence = aircraft("VISIBLE_HIGH", confidence = 0.90f, distanceMeters = 2000.0)
        val visibleNearLowConfidence = aircraft("VISIBLE_LOW", confidence = 0.70f, distanceMeters = 100.0)
        val hiddenHighConfidence = aircraft("HIDDEN_HIGH", confidence = 0.95f, distanceMeters = 50.0)
        val hiddenLowerConfidence = aircraft("HIDDEN_LOW", confidence = 0.60f, distanceMeters = 10.0)

        val sorted = sortSkyObjectsForList(
            listOf(hiddenLowerConfidence, visibleNearLowConfidence, hiddenHighConfidence, visibleFarHighConfidence),
            activeVisualFocusIds = setOf("VISIBLE_HIGH", "VISIBLE_LOW")
        )

        assertEquals(
            listOf("HIDDEN_LOW", "HIDDEN_HIGH", "VISIBLE_LOW", "VISIBLE_HIGH"),
            sorted.map { it.id }
        )
    }

    @Test
    fun `nearby ordinary aircraft sort before farther public safety aircraft`() {
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

        assertEquals(listOf("NORM", "SHERIFF"), sorted.map { it.id })
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

    @Test
    fun `public safety badge visuals distinguish law fire ems and generic signals`() {
        val sheriff = aircraft(
            id = "SHERIFF",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.GOVERNMENT,
            operatorName = "SAN DIEGO COUNTY SHERIFF",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )
        val fire = aircraft(
            id = "FIRE",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.EMERGENCY,
            operatorName = "CALFIRE",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )
        val ems = aircraft(
            id = "EMS",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.EMERGENCY,
            operatorName = "COUNTY MEDEVAC",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )
        val publicSafety = aircraft(
            id = "PS",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.COMMERCIAL,
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )

        assertEquals(ListBadgeVisual("LAW", Color(0xFFE65100)), listBadgeVisual(sheriff))
        assertEquals(ListBadgeVisual("FIRE", Color(0xFFD32F2F)), listBadgeVisual(fire))
        assertEquals(ListBadgeVisual("EMS", Color(0xFFE91E63)), listBadgeVisual(ems))
        assertEquals(ListBadgeVisual("PS", Color(0xFF1565C0)), listBadgeVisual(publicSafety))
    }

    @Test
    fun `public safety rows get attention color even when category is ordinary`() {
        val signaledAircraft = aircraft(
            id = "PS",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.COMMERCIAL,
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY")
        )

        assertNotNull(listAttentionColor(signaledAircraft))
        assertEquals(Color(0xFF1565C0), listAttentionColor(signaledAircraft))
    }

    @Test
    fun `rows expose exact human source labels`() {
        assertEquals("ADS-B", listSourceLabel(DetectionSource.ADS_B))
        assertEquals("Remote ID", listSourceLabel(DetectionSource.REMOTE_ID))
        assertEquals("Remote ID · Wi-Fi", listSourceLabel(DetectionSource.WIFI_NAN))
        assertEquals("Remote ID · Wi-Fi", listSourceLabel(DetectionSource.WIFI_BEACON))
        assertEquals("Phone", listSourceLabel(DetectionSource.WIFI))
    }

    @Test
    fun `rows expose category and attention as text not color alone`() {
        val sheriff = aircraft(
            id = "SHERIFF",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.GOVERNMENT,
            operatorName = "SAN DIEGO COUNTY SHERIFF",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY"),
        )
        val military = aircraft(
            id = "MIL",
            confidence = 0.95f,
            distanceMeters = 2_000.0,
            category = ObjectCategory.MILITARY,
        )

        assertEquals("General aviation", listCategoryLabel(ObjectCategory.GENERAL_AVIATION))
        assertEquals("Law enforcement", listAttentionLabel(sheriff))
        assertEquals("Military", listAttentionLabel(military))
    }

    @Test
    fun `fifty mile helicopter cannot outrank nearby aircraft even in visual focus`() {
        val helicopter = aircraft("HELI", 1f, 50 * 1609.344, ObjectCategory.HELICOPTER)
        val nearby = aircraft("NEAR", 0.5f, 300.0)
        assertEquals(
            listOf("NEAR", "HELI"),
            sortSkyObjectsForList(listOf(helicopter, nearby), setOf("HELI")).map { it.id },
        )
    }

    @Test
    fun `invalid and unknown distances follow known distances with stable ties`() {
        val near = aircraft("NEAR", 0.5f, 100.0)
        val a = aircraft("A", 0.9f, 200.0)
        val b = aircraft("B", 0.9f, 200.0)
        val invalid = listOf(null, Double.NaN, Double.POSITIVE_INFINITY, -1.0).mapIndexed { i, d ->
            aircraft("UNKNOWN$i", 1f, 0.0).copy(distanceMeters = d)
        }
        assertEquals(
            listOf("NEAR", "A", "B") + invalid.map { it.id },
            sortSkyObjectsForList(invalid.reversed() + listOf(b, a, near), emptySet()).map { it.id },
        )
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

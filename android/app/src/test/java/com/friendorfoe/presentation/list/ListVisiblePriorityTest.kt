package com.friendorfoe.presentation.list

import androidx.compose.ui.graphics.Color
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.time.Instant

class ListVisiblePriorityTest {

    @Test
    fun `active visible objects sort before distance and confidence`() {
        val highConfidence = aircraft("HIGH", confidence = 0.99f, distanceMeters = 500.0)
        val visibleLowerConfidence = aircraft("VISIBLE", confidence = 0.50f, distanceMeters = 2000.0)
        val mediumConfidence = aircraft("MED", confidence = 0.80f, distanceMeters = 100.0)

        val sorted = sortSkyObjectsForList(
            listOf(highConfidence, visibleLowerConfidence, mediumConfidence),
            activeVisualFocusIds = setOf("VISIBLE")
        )

        assertEquals(listOf("VISIBLE", "MED", "HIGH"), sorted.map { it.id })
    }

    @Test
    fun `objects within the same visible group sort by distance before confidence`() {
        val visibleFarHighConfidence = aircraft("VISIBLE_HIGH", confidence = 0.90f, distanceMeters = 2000.0)
        val visibleNearLowConfidence = aircraft("VISIBLE_LOW", confidence = 0.70f, distanceMeters = 100.0)
        val hiddenHighConfidence = aircraft("HIDDEN_HIGH", confidence = 0.95f, distanceMeters = 50.0)
        val hiddenLowerConfidence = aircraft("HIDDEN_LOW", confidence = 0.60f, distanceMeters = 10.0)

        val sorted = sortSkyObjectsForList(
            listOf(hiddenLowerConfidence, visibleNearLowConfidence, hiddenHighConfidence, visibleFarHighConfidence),
            activeVisualFocusIds = setOf("VISIBLE_HIGH", "VISIBLE_LOW")
        )

        assertEquals(
            listOf("VISIBLE_LOW", "VISIBLE_HIGH", "HIDDEN_LOW", "HIDDEN_HIGH"),
            sorted.map { it.id }
        )
    }

    @Test
    fun `nearby public safety aircraft sort before ordinary aircraft in the list`() {
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
    fun `helicopters fifty miles away do not outrank nearer traffic`() {
        val nearby = aircraft("NEAR", confidence = 0.60f, distanceMeters = METERS_PER_MILE)
        val middle = aircraft("MIDDLE", confidence = 0.70f, distanceMeters = 20.0 * METERS_PER_MILE)
        val helicopter = aircraft(
            "HELI", confidence = 0.99f, distanceMeters = 50.0 * METERS_PER_MILE,
            category = ObjectCategory.HELICOPTER,
        )
        val sheriff = helicopter.copy(
            id = "SHERIFF",
            category = ObjectCategory.GOVERNMENT,
            aircraftType = "AS50",
            operatorName = "COUNTY SHERIFF",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY"),
            distanceMeters = 51.0 * METERS_PER_MILE,
        )

        val sorted = sortSkyObjectsForList(listOf(sheriff, helicopter, middle, nearby), emptySet())

        assertEquals(listOf("NEAR", "MIDDLE", "HELI", "SHERIFF"), sorted.map { it.id })
    }

    @Test
    fun `aircraft category priority stops just beyond ten statute miles by default`() {
        val nearby = aircraft("NEAR", confidence = 0.99f, distanceMeters = METERS_PER_MILE)
        val boundary = aircraft("BOUNDARY", confidence = 0.60f, distanceMeters = 10.0 * METERS_PER_MILE)
        val priorityAircraft = listOf(
            boundary.copy(category = ObjectCategory.HELICOPTER),
            boundary.copy(category = ObjectCategory.MILITARY),
            boundary.copy(category = ObjectCategory.GOVERNMENT),
            boundary.copy(category = ObjectCategory.EMERGENCY),
            boundary.copy(
                category = ObjectCategory.GOVERNMENT,
                aircraftType = "AS50",
                operatorName = "COUNTY SHERIFF",
            ),
            boundary.copy(classificationSignals = listOf("OWNER:PUBLIC_SAFETY")),
        )

        for (atBoundary in priorityAircraft) {
            val justOutside = atBoundary.copy(
                id = "OUTSIDE",
                confidence = 1.0f,
                distanceMeters = 10.0 * METERS_PER_MILE + 0.01,
            )
            val sorted = sortSkyObjectsForList(listOf(justOutside, nearby, atBoundary), emptySet())

            assertEquals(
                "Category ${atBoundary.category}, type ${atBoundary.aircraftType}",
                listOf("BOUNDARY", "NEAR", "OUTSIDE"),
                sorted.map { it.id },
            )
        }
    }

    @Test
    fun `nearby helicopters with equal priority sort nearest first despite confidence`() {
        val nearer = aircraft(
            "NEAR_HELI", confidence = 0.60f, distanceMeters = METERS_PER_MILE,
            category = ObjectCategory.HELICOPTER,
        )
        val farther = nearer.copy(id = "FAR_HELI", confidence = 0.99f, distanceMeters = 10.0 * METERS_PER_MILE)
        val ordinary = aircraft("ORDINARY", confidence = 1.0f, distanceMeters = 100.0)

        val sorted = sortSkyObjectsForList(listOf(farther, ordinary, nearer), emptySet())

        assertEquals(listOf("NEAR_HELI", "FAR_HELI", "ORDINARY"), sorted.map { it.id })
    }

    @Test
    fun `visual focus cannot boost a helicopter outside the selected range`() {
        val nearby = aircraft("NEAR", confidence = 0.60f, distanceMeters = METERS_PER_MILE)
        val helicopter = aircraft(
            "HELI", confidence = 0.99f, distanceMeters = 50.0 * METERS_PER_MILE,
            category = ObjectCategory.HELICOPTER,
        )
        val objects = listOf(helicopter, nearby)

        assertEquals(
            listOf("NEAR", "HELI"),
            sortSkyObjectsForList(objects, setOf("HELI")).map { it.id },
        )
        assertEquals(
            listOf("NEAR", "HELI"),
            sortSkyObjectsForList(objects, emptySet()).map { it.id },
        )
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun `changing saved range reorders existing rows without a new detection`() = runTest {
        val nearby = aircraft("NEAR", confidence = 0.60f, distanceMeters = METERS_PER_MILE)
        val helicopter = aircraft(
            "HELI", confidence = 0.99f, distanceMeters = 12.0 * METERS_PER_MILE,
            category = ObjectCategory.HELICOPTER,
        )
        val settings = MutableStateFlow(DetectionSettings.defaults())
        var sortedIds = emptyList<String>()
        backgroundScope.launch {
            observeSortedSkyObjectsForList(
                objects = MutableStateFlow<List<SkyObject>>(listOf(helicopter, nearby)),
                filter = MutableStateFlow(FilterState()),
                activeVisualFocusIds = MutableStateFlow(setOf("HELI")),
                settings = settings,
            ).collect { sortedIds = it.map(SkyObject::id) }
        }
        runCurrent()
        assertEquals(listOf("NEAR", "HELI"), sortedIds)

        settings.value = settings.value.copy(aircraftRangeMiles = 15)
        runCurrent()
        assertEquals(listOf("HELI", "NEAR"), sortedIds)

        settings.value = settings.value.copy(aircraftRangeMiles = 5)
        runCurrent()
        assertEquals(listOf("NEAR", "HELI"), sortedIds)
    }

    @Test
    fun `unknown and invalid distances cannot give aircraft category priority`() {
        val known = aircraft("KNOWN", confidence = 0.50f, distanceMeters = 50.0 * METERS_PER_MILE)
        val unknown = aircraft(
            "UNKNOWN", confidence = 1.0f, distanceMeters = null,
            category = ObjectCategory.GOVERNMENT,
            aircraftType = "AS50",
            classificationSignals = listOf("OWNER:PUBLIC_SAFETY"),
        )

        for (distance in listOf(null, -1.0, Double.NaN, Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY)) {
            val sorted = sortSkyObjectsForList(listOf(unknown.copy(distanceMeters = distance), known), emptySet())

            assertEquals("Distance $distance", listOf("KNOWN", "UNKNOWN"), sorted.map { it.id })
        }
    }

    @Test
    fun `zero distance is a valid nearby distance`() {
        val helicopter = aircraft(
            "HERE", confidence = 0.60f, distanceMeters = 0.0,
            category = ObjectCategory.HELICOPTER,
        )
        val ordinary = aircraft("NEAR", confidence = 0.99f, distanceMeters = 1.0)

        assertEquals(
            listOf("HERE", "NEAR"),
            sortSkyObjectsForList(listOf(ordinary, helicopter), emptySet()).map { it.id },
        )
    }

    @Test
    fun `drones keep priority with a known distance and go last without one`() {
        val ordinary = aircraft("AIRCRAFT", confidence = 0.99f, distanceMeters = 100.0)
        val drone = Drone(
            id = "DRONE",
            droneId = "DRONE",
            position = ordinary.position,
            source = DetectionSource.REMOTE_ID,
            confidence = 0.80f,
            firstSeen = Instant.EPOCH,
            lastUpdated = Instant.EPOCH,
            distanceMeters = 200.0,
        )
        val unknown = drone.copy(id = "UNKNOWN", distanceMeters = null, confidence = 1.0f)

        assertEquals(
            listOf("DRONE", "AIRCRAFT", "UNKNOWN"),
            sortSkyObjectsForList(listOf(unknown, ordinary, drone), emptySet()).map { it.id },
        )
    }

    @Test
    fun `confidence breaks equal distance ties and ids keep refresh order stable`() {
        val lowerConfidence = aircraft("LOW", confidence = 0.50f, distanceMeters = 100.0)
        val first = lowerConfidence.copy(id = "A", confidence = 0.99f)
        val second = first.copy(id = "B")
        val objects = listOf(second, lowerConfidence, first)

        assertEquals(listOf("A", "B", "LOW"), sortSkyObjectsForList(objects, emptySet()).map { it.id })
        assertEquals(listOf("A", "B", "LOW"), sortSkyObjectsForList(objects.reversed(), emptySet()).map { it.id })
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
        distanceMeters: Double?,
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

    private companion object {
        const val METERS_PER_MILE = 1609.344
    }
}

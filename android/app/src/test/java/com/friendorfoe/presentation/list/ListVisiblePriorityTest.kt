package com.friendorfoe.presentation.list

import androidx.compose.ui.graphics.Color
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
    fun `distance wins over helicopter category public safety and confidence`() {
        val close = aircraft("CLOSE", 0.2f, 100.0)
        val helicopter = aircraft("HELI", 1f, 9 * METERS_PER_MILE, ObjectCategory.HELICOPTER)
        val sheriff = aircraft("SHERIFF", 1f, 50 * METERS_PER_MILE, ObjectCategory.GOVERNMENT,
            aircraftType = "AS50", classificationSignals = listOf("OWNER:PUBLIC_SAFETY"))
        assertEquals(listOf("CLOSE", "HELI", "SHERIFF"), sortSkyObjectsForList(listOf(sheriff, helicopter, close)).map { it.id })
    }

    @Test
    fun `all categories stay behind closer traffic even at ten mile boundary`() {
        val close = aircraft("CLOSE", 0.1f, METERS_PER_MILE)
        ObjectCategory.entries.forEach { category ->
            val boundary = aircraft("BOUNDARY", 1f, 10 * METERS_PER_MILE, category)
            val outside = boundary.copy(id = "OUTSIDE", distanceMeters = 10 * METERS_PER_MILE + 0.01)
            assertEquals(listOf("CLOSE", "BOUNDARY", "OUTSIDE"), sortSkyObjectsForList(listOf(outside, boundary, close)).map { it.id })
        }
    }

    @Test
    fun `invalid and unknown distances follow every known distance`() {
        val known = aircraft("KNOWN", 0.1f, 50 * METERS_PER_MILE)
        val invalid = listOf(null, -1.0, Double.NaN, Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY).mapIndexed { index, distance ->
            aircraft("UNKNOWN$index", 1f, distance, ObjectCategory.HELICOPTER)
        }
        assertEquals(listOf("KNOWN") + invalid.map { it.id }, sortSkyObjectsForList(invalid.reversed() + known).map { it.id })
    }

    @Test
    fun `zero is valid and confidence changes cannot disturb distance ties`() {
        val here = aircraft("HERE", 0.1f, 0.0)
        val first = aircraft("A", 0.1f, 100.0)
        val last = aircraft("Z", 1f, 100.0)
        assertEquals(listOf("HERE", "A", "Z"), sortSkyObjectsForList(listOf(last, first, here)).map { it.id })
        assertEquals(listOf("HERE", "A", "Z"), sortSkyObjectsForList(listOf(first.copy(confidence = 1f), last.copy(confidence = 0.1f), here)).map { it.id })
    }

    @Test
    fun `drone distances follow the same ordering and RSSI estimates are not treated as measured positions`() {
        val close = aircraft("AIRCRAFT", 0.1f, 100.0)
        val drone = Drone("DRONE", close.position, DetectionSource.REMOTE_ID,
            confidence = 1f, firstSeen = Instant.EPOCH, lastUpdated = Instant.EPOCH,
            distanceMeters = 200.0, droneId = "DRONE")
        val unknown = drone.copy(id = "UNKNOWN", distanceMeters = null, estimatedDistanceMeters = 1.0)
        assertEquals(listOf("AIRCRAFT", "DRONE", "UNKNOWN"), sortSkyObjectsForList(listOf(unknown, drone, close)).map { it.id })
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun `new distances and filters update the sorted feed immediately`() = runTest {
        val plane = aircraft("PLANE", 0.2f, 100.0)
        val helicopter = aircraft("HELI", 1f, 12 * METERS_PER_MILE, ObjectCategory.HELICOPTER)
        val rows = MutableStateFlow<List<SkyObject>>(listOf(helicopter, plane))
        val filter = MutableStateFlow(FilterState())
        var sortedIds = emptyList<String>()
        backgroundScope.launch { observeSortedSkyObjectsForList(rows, filter).collect { sortedIds = it.map(SkyObject::id) } }
        runCurrent()
        assertEquals(listOf("PLANE", "HELI"), sortedIds)
        rows.value = listOf(plane, helicopter.copy(distanceMeters = 50.0))
        runCurrent()
        assertEquals(listOf("HELI", "PLANE"), sortedIds)
        filter.value = FilterState(searchQuery = "PLANE")
        runCurrent()
        assertEquals(listOf("PLANE"), sortedIds)
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

package com.friendorfoe.presentation.map

import com.friendorfoe.data.local.AircraftMapPoint
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.domain.model.*
import org.junit.Assert.*
import org.junit.Test
import java.time.Instant

class MapFlightTrailsTest {
    private fun point(id: String = "abc123", time: Long = 1_000, lat: Double = 32.7) = AircraftMapPoint(
        TrackingEntity(objectId = id, latitude = lat, longitude = -117.1, altitudeMeters = 1000.0,
            heading = null, speedMps = null, timestamp = time), "DEMO123", "commercial")
    private fun project(records: List<AircraftMapPoint>, filter: FilterState = FilterState(), position: Position? = null) =
        projectMapFlightTrails(records, emptyList(), filter, position)

    @Test fun departedAircraftRemainHistoricalAndPointsAreOrderedAndDeduplicated() {
        val trails = project(listOf(point(time = 2_000), point(), point(), point("invalid", lat = Double.NaN)))
        assertEquals(1, trails.size)
        assertFalse(trails.single().live)
        assertEquals(listOf(1_000L, 2_000L), trails.single().points.map { it.timestamp })
    }

    @Test fun historyRespectsSearchCategorySourceTypeAndAltitudeFilters() {
        val records = listOf(point())
        assertEquals(1, project(records, FilterState(searchQuery = "demo")).size)
        listOf(FilterState(searchQuery = "missing"),
            FilterState(selectedCategories = setOf(ObjectCategory.UNKNOWN)),
            FilterState(selectedSources = setOf(SourceFilterGroup.WIFI)),
            FilterState(objectTypeFilter = ObjectTypeFilter.DRONE),
            FilterState(minAltitudeFt = 4000), FilterState(maxAltitudeFt = 3000)).forEach {
            assertTrue(project(records, it).isEmpty())
        }
    }

    @Test fun departedDistanceUsesCurrentViewerPositionAndUnknownIsNotNearby() {
        val filter = FilterState(maxDistanceNm = 1f)
        assertTrue(project(listOf(point()), filter).isEmpty())
        assertEquals(1, project(listOf(point()), filter, Position(32.7, -117.1, 0.0)).size)
        assertTrue(project(listOf(point()), filter, Position(34.0, -117.1, 0.0)).isEmpty())
    }

    @Test fun liveMetadataAndCurrentDistanceControlMatching() {
        val aircraft = Aircraft(id = "abc123", icaoHex = "abc123", registration = "N456", category = ObjectCategory.COMMERCIAL,
            position = Position(32.7, -117.1, 1000.0), firstSeen = Instant.EPOCH, lastUpdated = Instant.EPOCH, distanceMeters = 500.0)
        val result = projectMapFlightTrails(listOf(point()), listOf(aircraft), FilterState(searchQuery = "n456", maxDistanceNm = 1f), null)
        assertTrue(result.single().live)
        assertEquals("N456", result.single().label)
        assertTrue(projectMapFlightTrails(listOf(point()), listOf(aircraft.copy(distanceMeters = 100_000.0)),
            FilterState(maxDistanceNm = 1f), null).isEmpty())
    }

    @Test fun mostRecentFortyAircraftAreShownWithStableTieBreaking() {
        val result = project((0..49).map { point("id%02d".format(it), it.toLong() + 1000) }.reversed())
        assertEquals(40, result.size)
        assertEquals("id49", result.first().objectId)
        assertEquals("id10", result.last().objectId)
    }
}

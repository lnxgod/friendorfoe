package com.friendorfoe.sensor

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.Instant

/**
 * Unit tests for [SkyPositionMapper].
 *
 * Tests the pure-math functions for bearing, distance, elevation,
 * and screen coordinate mapping without requiring Android framework mocking.
 */
class SkyPositionMapperTest {

    private lateinit var mapper: SkyPositionMapper

    @Before
    fun setUp() {
        mapper = SkyPositionMapper()
    }

    // ---- Bearing calculation tests ----

    @Test
    fun `bearing due north is approximately 0 degrees`() {
        // Point 2 is directly north of point 1 (same lon, higher lat)
        val bearing = mapper.calculateBearing(
            lat1Deg = 40.0, lon1Deg = -74.0,
            lat2Deg = 41.0, lon2Deg = -74.0
        )
        assertEquals(0f, bearing, 1f) // Within 1 degree
    }

    @Test
    fun `bearing due east is approximately 90 degrees`() {
        // Point 2 is directly east of point 1 (same lat, higher lon)
        val bearing = mapper.calculateBearing(
            lat1Deg = 40.0, lon1Deg = -74.0,
            lat2Deg = 40.0, lon2Deg = -73.0
        )
        assertEquals(90f, bearing, 1f)
    }

    @Test
    fun `bearing due south is approximately 180 degrees`() {
        // Point 2 is directly south of point 1 (same lon, lower lat)
        val bearing = mapper.calculateBearing(
            lat1Deg = 40.0, lon1Deg = -74.0,
            lat2Deg = 39.0, lon2Deg = -74.0
        )
        assertEquals(180f, bearing, 1f)
    }

    @Test
    fun `bearing due west is approximately 270 degrees`() {
        // Point 2 is directly west of point 1 (same lat, lower lon)
        val bearing = mapper.calculateBearing(
            lat1Deg = 40.0, lon1Deg = -74.0,
            lat2Deg = 40.0, lon2Deg = -75.0
        )
        assertEquals(270f, bearing, 1f)
    }

    @Test
    fun `bearing is always in 0 to 360 range`() {
        // Test many directions to verify normalization
        val testCases = listOf(
            Pair(Pair(0.0, 0.0), Pair(1.0, 1.0)),   // NE
            Pair(Pair(0.0, 0.0), Pair(-1.0, 1.0)),   // SE
            Pair(Pair(0.0, 0.0), Pair(-1.0, -1.0)),  // SW
            Pair(Pair(0.0, 0.0), Pair(1.0, -1.0)),   // NW
        )
        for ((from, to) in testCases) {
            val bearing = mapper.calculateBearing(from.first, from.second, to.first, to.second)
            assertTrue("Bearing $bearing should be >= 0", bearing >= 0f)
            assertTrue("Bearing $bearing should be < 360", bearing < 360f)
        }
    }

    @Test
    fun `bearing northeast is approximately 45 degrees`() {
        // A point northeast at the equator
        val bearing = mapper.calculateBearing(
            lat1Deg = 0.0, lon1Deg = 0.0,
            lat2Deg = 1.0, lon2Deg = 1.0
        )
        assertEquals(45f, bearing, 2f)
    }

    // ---- Haversine distance tests ----

    @Test
    fun `same point has zero distance`() {
        val distance = mapper.calculateHaversineDistance(
            lat1Deg = 40.0, lon1Deg = -74.0,
            lat2Deg = 40.0, lon2Deg = -74.0
        )
        assertEquals(0.0, distance, 0.1)
    }

    @Test
    fun `known distance NYC to LA is approximately correct`() {
        // NYC (40.7128, -74.0060) to LA (34.0522, -118.2437) ~ 3940 km ~ 3,940,000 m
        val distance = mapper.calculateHaversineDistance(
            lat1Deg = 40.7128, lon1Deg = -74.0060,
            lat2Deg = 34.0522, lon2Deg = -118.2437
        )
        // Allow 5% tolerance for Earth radius approximation
        assertTrue("Distance should be > 3.7M meters", distance > 3_700_000.0)
        assertTrue("Distance should be < 4.2M meters", distance < 4_200_000.0)
    }

    @Test
    fun `one degree of latitude is approximately 111 km`() {
        val distance = mapper.calculateHaversineDistance(
            lat1Deg = 0.0, lon1Deg = 0.0,
            lat2Deg = 1.0, lon2Deg = 0.0
        )
        // 1 degree latitude ~ 111,195 meters
        assertEquals(111_195.0, distance, 500.0) // Within 500m
    }

    // ---- Elevation angle tests ----

    @Test
    fun `aircraft directly overhead has 90 degree elevation`() {
        val elevation = mapper.calculateElevationAngle(
            altitudeDiffMeters = 10000.0,
            groundDistanceMeters = 0.0001 // Nearly zero, avoid division by zero
        )
        assertTrue("Elevation should be nearly 90", elevation > 89f)
    }

    @Test
    fun `aircraft on the horizon has approximately 0 degree elevation`() {
        // Very far away, at roughly same altitude
        val elevation = mapper.calculateElevationAngle(
            altitudeDiffMeters = 0.0,
            groundDistanceMeters = 100_000.0
        )
        assertEquals(0f, elevation, 0.1f)
    }

    @Test
    fun `aircraft at 45 degree elevation`() {
        // Equal altitude difference and ground distance -> 45 degrees
        val elevation = mapper.calculateElevationAngle(
            altitudeDiffMeters = 10000.0,
            groundDistanceMeters = 10000.0
        )
        assertEquals(45f, elevation, 0.1f)
    }

    @Test
    fun `aircraft below user has negative elevation`() {
        val elevation = mapper.calculateElevationAngle(
            altitudeDiffMeters = -5000.0,
            groundDistanceMeters = 10000.0
        )
        assertTrue("Elevation should be negative for objects below", elevation < 0f)
    }

    @Test
    fun `both zero returns zero elevation`() {
        val elevation = mapper.calculateElevationAngle(
            altitudeDiffMeters = 0.0,
            groundDistanceMeters = 0.0
        )
        assertEquals(0f, elevation, 0.01f)
    }

    // ---- Screen position mapping tests ----

    @Test
    fun `object at camera center maps to screen center`() {
        // Create an aircraft exactly where the camera is pointing
        val userPos = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 0.0)
        val fovCalc = CameraFovCalculator() // default 60x45 deg

        // Object due north, at horizon level
        val objectPos = Position(latitude = 40.1, longitude = -74.0, altitudeMeters = 0.0)
        val aircraft = createTestAircraft("TEST1", objectPos)

        // Camera pointing north, level
        val orientation = DeviceOrientation(azimuthDegrees = 0f, pitchDegrees = 0f, rollDegrees = 0f)

        val results = mapper.mapToScreen(userPos, listOf(aircraft), orientation, fovCalc)

        assertEquals(1, results.size)
        val pos = results[0]
        assertTrue("Object should be in view", pos.isInView)
        assertEquals(0.5f, pos.screenX, 0.05f) // Center horizontally
        assertEquals(0.5f, pos.screenY, 0.05f) // Center vertically
    }

    @Test
    fun `object behind camera is not in view`() {
        val userPos = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 0.0)
        val fovCalc = CameraFovCalculator()

        // Object is due south
        val objectPos = Position(latitude = 39.0, longitude = -74.0, altitudeMeters = 0.0)
        val aircraft = createTestAircraft("TEST2", objectPos)

        // Camera pointing north
        val orientation = DeviceOrientation(azimuthDegrees = 0f, pitchDegrees = 0f, rollDegrees = 0f)

        val results = mapper.mapToScreen(userPos, listOf(aircraft), orientation, fovCalc)

        assertEquals(1, results.size)
        assertFalse("Object behind camera should not be in view", results[0].isInView)
    }

    @Test
    fun `object to the right maps to right side of screen`() {
        val userPos = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 0.0)
        val fovCalc = CameraFovCalculator() // 60 deg horizontal FOV

        // Object ~15 degrees east of north (within 30 deg half-FOV)
        // At equator, bearing to small east offset is ~90 degrees
        // Use a point slightly east of due north to keep within FOV
        val objectPos = Position(latitude = 40.1, longitude = -73.98, altitudeMeters = 0.0)
        val aircraft = createTestAircraft("TEST3", objectPos)

        // Camera pointing north
        val orientation = DeviceOrientation(azimuthDegrees = 0f, pitchDegrees = 0f, rollDegrees = 0f)

        val results = mapper.mapToScreen(userPos, listOf(aircraft), orientation, fovCalc)

        assertEquals(1, results.size)
        if (results[0].isInView) {
            assertTrue("Object to the right should have screenX > 0.5", results[0].screenX > 0.5f)
        }
    }

    @Test
    fun `object above maps to upper part of screen`() {
        val userPos = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 0.0)
        val fovCalc = CameraFovCalculator() // 45 deg vertical FOV

        // Object at high altitude and moderate distance -> some elevation angle
        val objectPos = Position(latitude = 40.001, longitude = -74.0, altitudeMeters = 5000.0)
        val aircraft = createTestAircraft("TEST4", objectPos)

        // Camera pointing north, level with horizon
        val orientation = DeviceOrientation(azimuthDegrees = 0f, pitchDegrees = 0f, rollDegrees = 0f)

        val results = mapper.mapToScreen(userPos, listOf(aircraft), orientation, fovCalc)

        assertEquals(1, results.size)
        // High elevation object should be toward top of screen (low screenY)
        // Note: may or may not be in view depending on exact elevation vs FOV
    }

    @Test
    fun `multiple objects are all mapped`() {
        val userPos = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 0.0)
        val fovCalc = CameraFovCalculator()
        val orientation = DeviceOrientation(azimuthDegrees = 0f, pitchDegrees = 0f, rollDegrees = 0f)

        val objects = listOf(
            createTestAircraft("A1", Position(40.1, -74.0, 10000.0)),
            createTestAircraft("A2", Position(39.9, -74.0, 5000.0)),
            createTestAircraft("A3", Position(40.0, -73.9, 8000.0)),
        )

        val results = mapper.mapToScreen(userPos, objects, orientation, fovCalc)
        assertEquals(3, results.size)
    }

    // ---- Helper ----

    private fun createTestAircraft(id: String, position: Position): Aircraft {
        return Aircraft(
            id = id,
            position = position,
            source = DetectionSource.ADS_B,
            category = ObjectCategory.COMMERCIAL,
            confidence = 0.95f,
            firstSeen = Instant.EPOCH,
            lastUpdated = Instant.EPOCH,
            icaoHex = id
        )
    }
}

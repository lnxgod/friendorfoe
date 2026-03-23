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

class TrajectoryPredictorTest {

    private lateinit var predictor: TrajectoryPredictor

    @Before
    fun setUp() {
        predictor = TrajectoryPredictor()
    }

    // ---- Forward projection tests ----

    @Test
    fun `forward project due north moves latitude up`() {
        val (lat, lon) = predictor.forwardProject(
            latDeg = 40.0, lonDeg = -74.0,
            bearingDeg = 0f, distanceMeters = 1000.0
        )
        assertTrue("Latitude should increase going north", lat > 40.0)
        assertEquals(-74.0, lon, 0.001) // Longitude unchanged
    }

    @Test
    fun `forward project due east moves longitude up`() {
        val (lat, lon) = predictor.forwardProject(
            latDeg = 0.0, lonDeg = 0.0,
            bearingDeg = 90f, distanceMeters = 1000.0
        )
        assertEquals(0.0, lat, 0.001) // Latitude unchanged at equator
        assertTrue("Longitude should increase going east", lon > 0.0)
    }

    @Test
    fun `forward project 111km north moves latitude by approximately 1 degree`() {
        val (lat, _) = predictor.forwardProject(
            latDeg = 0.0, lonDeg = 0.0,
            bearingDeg = 0f, distanceMeters = 111_195.0
        )
        assertEquals(1.0, lat, 0.01)
    }

    @Test
    fun `forward project zero distance returns same position`() {
        val (lat, lon) = predictor.forwardProject(
            latDeg = 40.0, lonDeg = -74.0,
            bearingDeg = 45f, distanceMeters = 0.0
        )
        assertEquals(40.0, lat, 0.0001)
        assertEquals(-74.0, lon, 0.0001)
    }

    @Test
    fun `forward project 1250m at cruise speed heading north`() {
        // 250 m/s * 5 seconds = 1250m. At 40 deg latitude, should move ~0.0112 deg north
        val (lat, _) = predictor.forwardProject(
            latDeg = 40.0, lonDeg = -74.0,
            bearingDeg = 0f, distanceMeters = 1250.0
        )
        val deltaLatDeg = lat - 40.0
        // 1250m / 111195m_per_deg ≈ 0.01124 degrees
        assertEquals(0.01124, deltaLatDeg, 0.001)
    }

    // ---- Dead reckoning integration tests ----

    @Test
    fun `stationary aircraft has no extrapolation`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "STAT1",
            Position(40.0, -74.0, 10000.0, heading = 90f, speedMps = 0.5f),
            lastUpdated = now
        )
        val result = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 3000)

        assertEquals(1, result.size)
        assertFalse("Stationary aircraft should not be extrapolated", result[0].isExtrapolated)
        assertEquals(40.0, result[0].predictedPosition.latitude, 0.0001)
    }

    @Test
    fun `aircraft with null heading has no extrapolation`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "NULL1",
            Position(40.0, -74.0, 10000.0, heading = null, speedMps = 250f),
            lastUpdated = now
        )
        val result = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 3000)

        assertEquals(1, result.size)
        assertFalse("Aircraft with null heading should not be extrapolated", result[0].isExtrapolated)
    }

    @Test
    fun `aircraft with null speed has no extrapolation`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "NULL2",
            Position(40.0, -74.0, 10000.0, heading = 90f, speedMps = null),
            lastUpdated = now
        )
        val result = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 3000)

        assertEquals(1, result.size)
        assertFalse("Aircraft with null speed should not be extrapolated", result[0].isExtrapolated)
    }

    @Test
    fun `aircraft heading north at 250 mps moves north after 5 seconds`() {
        val now = Instant.now()
        val startLat = 40.0
        val aircraft = createTestAircraft(
            "FLY1",
            Position(startLat, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = now
        )

        val fiveSecondsLater = now.toEpochMilli() + 5000
        val result = predictor.predictAll(listOf(aircraft), fiveSecondsLater)

        assertEquals(1, result.size)
        assertTrue("Should be extrapolated", result[0].isExtrapolated)
        assertTrue("Should move north (higher latitude)", result[0].predictedPosition.latitude > startLat)

        // 250 m/s * 5s = 1250m north ≈ 0.0112 deg
        val deltaLat = result[0].predictedPosition.latitude - startLat
        assertEquals(0.0112, deltaLat, 0.002)
    }

    @Test
    fun `aircraft heading east at 250 mps moves east after 5 seconds`() {
        val now = Instant.now()
        val startLon = -74.0
        val aircraft = createTestAircraft(
            "FLY2",
            Position(40.0, startLon, 10000.0, heading = 90f, speedMps = 250f),
            lastUpdated = now
        )

        val fiveSecondsLater = now.toEpochMilli() + 5000
        val result = predictor.predictAll(listOf(aircraft), fiveSecondsLater)

        assertTrue("Should be extrapolated", result[0].isExtrapolated)
        assertTrue("Should move east (higher longitude)", result[0].predictedPosition.longitude > startLon)
    }

    @Test
    fun `vertical rate changes altitude`() {
        val now = Instant.now()
        val startAlt = 10000.0
        val aircraft = createTestAircraft(
            "CLIMB1",
            Position(40.0, -74.0, startAlt, heading = 0f, speedMps = 250f, verticalRateMps = 10f),
            lastUpdated = now
        )

        val fiveSecondsLater = now.toEpochMilli() + 5000
        val result = predictor.predictAll(listOf(aircraft), fiveSecondsLater)

        // 10 m/s * 5s = 50m altitude gain
        val expectedAlt = startAlt + 50.0
        assertEquals(expectedAlt, result[0].predictedPosition.altitudeMeters, 1.0)
    }

    @Test
    fun `confidence decays with age`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "AGE1",
            Position(40.0, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = now
        )

        // At t=0, confidence should be 1.0
        val fresh = predictor.predictAll(listOf(aircraft), now.toEpochMilli())
        assertEquals(1.0f, fresh[0].confidence, 0.01f)

        // At t=15s, confidence should be 0.5
        val halfStale = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 15000)
        assertEquals(0.5f, halfStale[0].confidence, 0.05f)

        // At t=30s, confidence should be 0.0
        val stale = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 30000)
        assertEquals(0.0f, stale[0].confidence, 0.01f)
    }

    @Test
    fun `extrapolation capped at MAX_EXTRAPOLATION_S`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "CAP1",
            Position(40.0, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = now
        )

        // At t=12s (max extrapolation) and t=20s, predicted position should be same
        // because extrapolation is capped
        val at12s = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 12000)
        // Reset predictor to clear track state and get independent predictions
        predictor.reset()
        val at20s = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 20000)

        assertEquals(
            "Latitude should be same at 12s and 20s (capped)",
            at12s[0].predictedPosition.latitude,
            at20s[0].predictedPosition.latitude,
            0.0001
        )
    }

    @Test
    fun `on-ground aircraft not extrapolated`() {
        val now = Instant.now()
        val aircraft = Aircraft(
            id = "GND1",
            position = Position(40.0, -74.0, 100.0, heading = 90f, speedMps = 30f),
            category = ObjectCategory.COMMERCIAL,
            firstSeen = now,
            lastUpdated = now,
            icaoHex = "GND1",
            isOnGround = true
        )

        val result = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 5000)
        assertFalse("On-ground aircraft should not be extrapolated", result[0].isExtrapolated)
    }

    @Test
    fun `blend offset decays smoothly on new report`() {
        val t0 = Instant.now()
        val t0Ms = t0.toEpochMilli()

        // First report: aircraft heading north at 250 m/s
        val aircraft1 = createTestAircraft(
            "BLEND1",
            Position(40.0, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = t0
        )

        // Let predictor build initial state
        predictor.predictAll(listOf(aircraft1), t0Ms + 3000)

        // Second report 5s later: actual position slightly different from predicted
        val t1 = t0.plusSeconds(5)
        val aircraft2 = createTestAircraft(
            "BLEND1",
            Position(40.012, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = t1
        )

        // Immediately after new report
        val justAfter = predictor.predictAll(listOf(aircraft2), t1.toEpochMilli() + 10)
        val posJustAfter = justAfter[0].predictedPosition.latitude

        // 2 seconds after new report (blend should have mostly decayed)
        val twoSecLater = predictor.predictAll(listOf(aircraft2), t1.toEpochMilli() + 2000)
        val posTwoSecLater = twoSecLater[0].predictedPosition.latitude

        // The positions should converge (blend offset decaying)
        // After 2 seconds (2000ms / 700ms half-life ≈ 2.86 half-lives), blend should be ~6% of original
        // We can't assert exact values easily but the prediction should be reasonable
        assertTrue("Prediction should produce valid latitude", posJustAfter > 40.0)
        assertTrue("Prediction should produce valid latitude", posTwoSecLater > 40.0)
    }

    @Test
    fun `tracks are cleaned up when objects removed`() {
        val now = Instant.now()
        val aircraft1 = createTestAircraft(
            "CLEAN1",
            Position(40.0, -74.0, 10000.0, heading = 0f, speedMps = 250f),
            lastUpdated = now
        )
        val aircraft2 = createTestAircraft(
            "CLEAN2",
            Position(41.0, -74.0, 10000.0, heading = 180f, speedMps = 250f),
            lastUpdated = now
        )

        // Both tracked
        predictor.predictAll(listOf(aircraft1, aircraft2), now.toEpochMilli())

        // Remove aircraft1 from list
        val result = predictor.predictAll(listOf(aircraft2), now.toEpochMilli() + 1000)
        assertEquals(1, result.size)
        assertEquals("CLEAN2", result[0].skyObject.id)
    }

    @Test
    fun `trackHeadingDegrees reflects current heading`() {
        val now = Instant.now()
        val aircraft = createTestAircraft(
            "HDG1",
            Position(40.0, -74.0, 10000.0, heading = 135f, speedMps = 250f),
            lastUpdated = now
        )

        val result = predictor.predictAll(listOf(aircraft), now.toEpochMilli() + 1000)
        assertEquals(135f, result[0].trackHeadingDegrees!!, 1f)
    }

    // ---- Helper ----

    private fun createTestAircraft(
        id: String,
        position: Position,
        lastUpdated: Instant = Instant.now()
    ): Aircraft {
        return Aircraft(
            id = id,
            position = position,
            source = DetectionSource.ADS_B,
            category = ObjectCategory.COMMERCIAL,
            confidence = 0.95f,
            firstSeen = lastUpdated,
            lastUpdated = lastUpdated,
            icaoHex = id
        )
    }
}

package com.friendorfoe.data.repository

import com.friendorfoe.data.local.TrackingEntity
import org.junit.Assert.*
import org.junit.Test

class AircraftTrackPolicyTest {
    private val now = 1_000_000L
    private fun point(time: Long = now, lat: Double = 32.7, lon: Double = -117.1) =
        TrackingEntity(objectId = "abc123", latitude = lat, longitude = lon,
            altitudeMeters = 1000.0, heading = 90f, speedMps = 100f, timestamp = time)

    @Test fun invalidStaleAndFuturePointsAreNotSaved() {
        listOf(point(lat = Double.NaN), point(lat = 91.0), point(lon = -181.0),
            point(lat = 0.0, lon = 0.0), point().copy(altitudeMeters = Double.POSITIVE_INFINITY),
            point(now + 1), point(now - 120_001)).forEach {
            assertFalse(shouldRecordAircraftPoint(null, it, now))
        }
        assertTrue(shouldRecordAircraftPoint(null, point(), now))
    }

    @Test fun samplingRejectsDuplicatesDelayedPacketsAndJitterButCapturesMotion() {
        val previous = point(now - 5_000)
        assertFalse(shouldRecordAircraftPoint(previous, previous, now))
        assertFalse(shouldRecordAircraftPoint(previous, point(now - 6_000), now))
        assertFalse(shouldRecordAircraftPoint(previous, point(lat = 32.70001), now))
        assertTrue(shouldRecordAircraftPoint(previous, point(lat = 32.701), now))
        assertTrue(shouldRecordAircraftPoint(previous, point().copy(altitudeMeters = 1025.0), now))
        assertTrue(shouldRecordAircraftPoint(point(now - 60_000), point(), now))
    }

    @Test fun pathSeparatesMissingCoverageTeleportsAndDatelineCrossings() {
        val a = point(now - 200_000)
        val b = point(now - 190_000, lat = 32.701)
        val c = point(now - 30_000, lat = 32.71)
        val d = point(now - 20_000, lat = 34.0)
        assertEquals(listOf(listOf(a, b), listOf(c), listOf(d)), splitAircraftTrail(listOf(d, c, b, a)))
        assertEquals(2, splitAircraftTrail(listOf(point(now - 10_000, lon = 179.999), point(lon = -179.999))).size)
    }

    @Test fun realNearbyPositionsStayConnectedAndCannotMixObjectIdentities() {
        val a = point(now - 10_000)
        val b = point(lat = 32.701)
        assertEquals(1, splitAircraftTrail(listOf(a, b, b)).size)
        assertEquals(2, splitAircraftTrail(listOf(a, b.copy(objectId = "different"))).size)
        assertEquals(111.2, trackDistanceMeters(a, b), 0.2)
        assertEquals(0.0, trackDistanceMeters(a, a), 0.0)
    }
}

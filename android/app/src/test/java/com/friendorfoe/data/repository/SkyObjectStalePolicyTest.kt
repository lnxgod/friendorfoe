package com.friendorfoe.data.repository

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class SkyObjectStalePolicyTest {

    @Test
    fun `ordinary remote id expires after 120 seconds while formation remains`() {
        val now = Instant.parse("2026-07-04T12:05:00Z")
        val ordinary = drone("SERIAL123", now.minusSeconds(121))
        val formation = drone("FOF-C5-A1B2C3-001", now.minusSeconds(121))

        assertTrue(isSkyObjectStale(ordinary, now))
        assertFalse(isSkyObjectStale(formation, now))
    }

    @Test
    fun `formation retention covers two full worst-case sweeps and expires after 300 seconds`() {
        val now = Instant.parse("2026-07-04T12:05:00Z")

        assertFalse(isSkyObjectStale(
            drone("FOF-C5-A1B2C3-001", now.minusSeconds(240)),
            now,
        ))
        assertFalse(isSkyObjectStale(
            drone("FOF-C5-A1B2C3-002", now.minusSeconds(300)),
            now,
        ))
        assertTrue(isSkyObjectStale(
            drone("FOF-C5-A1B2C3-003", now.minusSeconds(301)),
            now,
        ))
    }

    private fun drone(droneId: String, lastUpdated: Instant): Drone = Drone(
        id = "rid_$droneId",
        position = Position(36.13094, -115.15064, 120.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.95f,
        firstSeen = lastUpdated,
        lastUpdated = lastUpdated,
        droneId = droneId,
    )
}

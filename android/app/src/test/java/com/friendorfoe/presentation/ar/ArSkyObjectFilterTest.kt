package com.friendorfoe.presentation.ar

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.Instant

class ArSkyObjectFilterTest {

    @Test
    fun `large formation is excluded while ordinary drones remain`() {
        val formation = (0 until 240).map { index ->
            drone(
                id = "formation-$index",
                droneId = "FOF-C5-ABCDEF-${index.toString().padStart(3, '0')}",
            )
        }
        val ordinary = drone(id = "ordinary", droneId = "RID-ABCDEF")

        assertEquals(listOf(ordinary), (formation + ordinary).withoutFormationDrones())
    }

    private fun drone(id: String, droneId: String): Drone {
        val now = Instant.parse("2026-01-01T00:00:00Z")
        return Drone(
            id = id,
            position = Position(37.0, -122.0, 100.0),
            source = DetectionSource.REMOTE_ID,
            confidence = 0.9f,
            firstSeen = now,
            lastUpdated = now,
            droneId = droneId,
        )
    }
}

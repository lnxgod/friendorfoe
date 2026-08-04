package com.friendorfoe.presentation.map

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.Instant

class FormationMapRoutingTest {

    @Test
    fun `large formation uses shared layer and ordinary drone keeps marker`() {
        val formationTracks = (0 until 240).map { index ->
            track(
                id = "formation-$index",
                droneId = "FOF-C5-ABCDEF-${index.toString().padStart(3, '0')}",
            )
        }
        val ordinaryTrack = track(id = "ordinary", droneId = "RID-ABCDEF")

        val layers = splitLocalMapTracks(formationTracks + ordinaryTrack)

        assertEquals(240, layers.formationTracks.size)
        assertEquals(listOf(ordinaryTrack), layers.markerTracks)
    }

    private fun track(id: String, droneId: String): MapTrack {
        val now = Instant.parse("2026-01-01T00:00:00Z")
        val position = Position(37.0, -122.0, 100.0)
        val drone = Drone(
            id = id,
            position = position,
            source = DetectionSource.REMOTE_ID,
            confidence = 0.9f,
            firstSeen = now,
            lastUpdated = now,
            droneId = droneId,
        )
        return MapTrack(
            skyObject = drone,
            position = position,
            ageSeconds = 0f,
            confidence = 0.9f,
            isExtrapolated = false,
            headingDegrees = null,
        )
    }
}

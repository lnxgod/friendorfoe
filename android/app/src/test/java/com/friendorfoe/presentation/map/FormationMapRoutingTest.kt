package com.friendorfoe.presentation.map

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class FormationMapRoutingTest {

    @Test
    fun `large formation bypasses trajectory projection and ordinary drone keeps track`() {
        val formationDrones = (0 until 240).map { index ->
            drone(
                id = "formation-$index",
                droneId = "FOF-C5-ABCDEF-${index.toString().padStart(3, '0')}",
            )
        }
        val ordinaryDrone = drone(id = "ordinary", droneId = "RID-ABCDEF")

        val sources = splitMapProjectionSources(formationDrones + ordinaryDrone)

        assertEquals(240, sources.formationPoints.size)
        assertEquals(listOf(ordinaryDrone), sources.trackObjects)
        assertEquals("formation-0", sources.formationPoints.first().objectId)
        assertEquals(37.0, sources.formationPoints.first().latitude, 0.0)
        assertEquals(-122.0, sources.formationPoints.first().longitude, 0.0)
    }

    @Test
    fun `equal formation snapshots do not rebuild geo points`() {
        val original = listOf(FormationMapPoint("formation-1", 37.0, -122.0))

        assertTrue(formationPointsChanged(previous = null, next = original))
        assertFalse(formationPointsChanged(previous = original, next = original))
        assertFalse(formationPointsChanged(previous = original, next = original.toList()))
        assertTrue(
            formationPointsChanged(
                previous = original,
                next = listOf(FormationMapPoint("formation-1", 37.0001, -122.0)),
            )
        )
    }

    @Test
    fun `radio timestamp refresh does not change stationary formation snapshot`() {
        val first = drone("formation-1", "FOF-C5-ABCDEF-001")
        val refreshed = first.copy(lastUpdated = first.lastUpdated.plusSeconds(30))

        assertEquals(
            splitMapProjectionSources(listOf(first)).formationPoints,
            splitMapProjectionSources(listOf(refreshed)).formationPoints,
        )
    }

    private fun drone(id: String, droneId: String): Drone {
        val now = Instant.parse("2026-01-01T00:00:00Z")
        val position = Position(37.0, -122.0, 100.0)
        return Drone(
            id = id,
            position = position,
            source = DetectionSource.REMOTE_ID,
            confidence = 0.9f,
            firstSeen = now,
            lastUpdated = now,
            droneId = droneId,
        )
    }
}

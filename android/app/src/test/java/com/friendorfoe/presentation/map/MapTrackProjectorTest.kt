package com.friendorfoe.presentation.map

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class MapTrackProjectorTest {

    @Test
    fun `moving aircraft advances without mutating report`() {
        val report = aircraft(lastUpdated = Instant.ofEpochMilli(1_000), speedMps = 100f, heading = 90f)
        val frame = MapTrackProjector().project(listOf(report), 6_000).single()

        assertTrue(frame.position.longitude > report.position.longitude)
        assertEquals(report.position, frame.skyObject.position)
        assertTrue(frame.isExtrapolated)
    }

    @Test
    fun `stale aircraft freezes with zero confidence`() {
        val report = aircraft(lastUpdated = Instant.ofEpochMilli(1_000), speedMps = 100f, heading = 90f)
        val frame = MapTrackProjector().project(listOf(report), 32_000).single()

        assertEquals(0f, frame.confidence)
        assertFalse(frame.isExtrapolated)
    }

    private fun aircraft(
        lastUpdated: Instant,
        speedMps: Float,
        heading: Float,
    ) = Aircraft(
        id = "abc123",
        position = Position(
            latitude = 37.0,
            longitude = -122.0,
            altitudeMeters = 1_000.0,
            heading = heading,
            speedMps = speedMps,
        ),
        category = ObjectCategory.GENERAL_AVIATION,
        firstSeen = lastUpdated,
        lastUpdated = lastUpdated,
        icaoHex = "abc123",
    )
}

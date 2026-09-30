package com.friendorfoe.presentation.list

import com.friendorfoe.detection.*
import com.friendorfoe.domain.model.*
import java.time.Instant
import org.junit.Assert.*
import org.junit.Test

class NearbyFeedPresentationTest {
    private fun aircraft(id: String, miles: Double?, category: ObjectCategory = ObjectCategory.COMMERCIAL) = Aircraft(
        id = id, icaoHex = id, category = category, position = Position(32.7, -117.1, 1000.0),
        firstSeen = Instant.EPOCH, lastUpdated = Instant.EPOCH, distanceMeters = miles?.times(1609.344),
    )
    @Test fun failedFetchCannotMasqueradeAsAnEmptySky() {
        val state = nearbyFeedPresentation(AircraftFeedState(AircraftFeedPhase.OFFLINE), true, emptyList(), emptyList(), 0, 5000)
        assertTrue(state.body is ListBodyState.Failed)
        assertTrue(state.canRetry)
        assertNull(state.detail)
    }
    @Test fun successfulEmptyFetchAndConnectingAreDifferent() {
        assertEquals(ListBodyState.NoDetections, nearbyFeedPresentation(AircraftFeedState(AircraftFeedPhase.LIVE, 1000), true, emptyList(), emptyList(), 0, 5000).body)
        assertEquals(ListBodyState.Loading, nearbyFeedPresentation(AircraftFeedState(AircraftFeedPhase.CONNECTING), true, emptyList(), emptyList(), 0, 5000).body)
    }
    @Test fun outageRetainsRowsAndShowsLastSuccessAge() {
        val rows = listOf(aircraft("cached", 5.0))
        val feed = AircraftFeedState(AircraftFeedPhase.LIVE, 1000).failed(5000, 1, 5000)
        val state = nearbyFeedPresentation(feed, true, rows, rows, 0, 9000)
        assertEquals(AircraftFeedPhase.RECONNECTING, feed.phase)
        assertEquals(8000L, (state.body as ListBodyState.StaleResults).ageMs)
        assertEquals(rows, (state.body as ListBodyState.StaleResults).rows)
    }
    @Test fun providerBackoffIsVisibleAndCannotBeBypassedByRetry() {
        val state = nearbyFeedPresentation(AircraftFeedState(AircraftFeedPhase.OFFLINE, retryAtMs = 35_000, rateLimited = true), true, emptyList(), emptyList(), 0, 5000)
        assertEquals("Retrying in 30s", state.detail)
        assertFalse(state.canRetry)
    }
    @Test fun missingAndInvalidCoordinatesCannotStartAnAircraftQuery() {
        assertFalse(validAircraftFeedPosition(0.0, 0.0))
        assertFalse(validAircraftFeedPosition(Double.NaN, -117.1))
        assertFalse(validAircraftFeedPosition(91.0, -117.1))
        assertTrue(validAircraftFeedPosition(0.0, 25.0))
    }
    @Test fun pausedAircraftFeedDoesNotProduceAFalseOutage() {
        val state = nearbyFeedPresentation(AircraftFeedState(AircraftFeedPhase.OFFLINE), false, emptyList(), emptyList(), 0, 5000)
        assertEquals(ListBodyState.NoDetections, state.body)
        assertFalse(state.canRetry)
    }
    @Test fun nearestFirstOverridesCategoryAndCameraPriorityButKeepsUnknownDistancesLast() {
        val rows = listOf(aircraft("HELI", 9.0, ObjectCategory.HELICOPTER), aircraft("CLOSE", 1.0), aircraft("UNKNOWN", null))
        assertEquals(listOf("HELI", "CLOSE", "UNKNOWN"), sortSkyObjectsForList(rows, setOf("HELI")).map { it.id })
        assertEquals(listOf("CLOSE", "HELI", "UNKNOWN"), sortSkyObjectsForList(rows, setOf("HELI"), nearestFirst = true).map { it.id })
    }
}

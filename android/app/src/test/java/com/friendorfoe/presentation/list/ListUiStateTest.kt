package com.friendorfoe.presentation.list

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Test

class ListUiStateTest {
    @Test
    fun unresolvedFeedIsLoadingEvenBeforeAnErrorBecomesTerminal() {
        assertEquals(
            ListBodyState.Loading,
            reduceListBody(
                raw = emptyList(),
                visible = emptyList(),
                resolved = false,
                failure = "Source unavailable",
            ),
        )
    }

    @Test
    fun emptyFeedAndNoFilterMatchesAreDifferent() {
        val row = drone("one")

        assertEquals(
            ListBodyState.NoDetections,
            reduceListBody(
                raw = emptyList(),
                visible = emptyList(),
                resolved = true,
                failure = null,
            ),
        )
        assertEquals(
            ListBodyState.NoMatches(activeFilterCount = 2),
            reduceListBody(
                raw = listOf(row),
                visible = emptyList(),
                resolved = true,
                failure = null,
                activeFilterCount = 2,
            ),
        )
    }

    @Test
    fun failureKeepsCachedVisibleRowsOnlyAsStale() {
        val row = drone("cached")

        assertEquals(
            ListBodyState.StaleResults(
                rows = listOf(row),
                ageMs = 20_000L,
                message = "Remote ID unavailable",
            ),
            reduceListBody(
                raw = listOf(row),
                visible = listOf(row),
                resolved = true,
                failure = "Remote ID unavailable",
                cacheAgeMs = 20_000L,
            ),
        )
    }

    @Test
    fun failureWithVisibleRowsDoesNotInventAnUnknownCacheAge() {
        val row = drone("cached-with-unknown-age")

        assertEquals(
            ListBodyState.StaleResults(
                rows = listOf(row),
                ageMs = null,
                message = "Remote ID unavailable",
            ),
            reduceListBody(
                raw = listOf(row),
                visible = listOf(row),
                resolved = true,
                failure = "Remote ID unavailable",
                cacheAgeMs = null,
            ),
        )
    }

    @Test
    fun terminalFailureWithoutVisibleRowsDoesNotPretendTheFeedIsEmpty() {
        val row = drone("filtered")

        assertEquals(
            ListBodyState.Failed("ADS-B unavailable"),
            reduceListBody(
                raw = listOf(row),
                visible = emptyList(),
                resolved = true,
                failure = "ADS-B unavailable",
                activeFilterCount = 1,
            ),
        )
    }

    @Test
    fun resolvedVisibleRowsAreResults() {
        val row = drone("current")

        assertEquals(
            ListBodyState.Results(listOf(row)),
            reduceListBody(
                raw = listOf(row),
                visible = listOf(row),
                resolved = true,
                failure = null,
            ),
        )
    }

    private fun drone(id: String) = Drone(
        id = id,
        position = Position(latitude = 32.7, longitude = -117.1, altitudeMeters = 100.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        droneId = id,
    )
}

package com.friendorfoe.presentation.ar

import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Test

class ArLocationSeedPolicyTest {

    @Test
    fun `resume without a usable seed clears a prior valid AR position`() {
        val firstStartPosition = arPositionForLastKnownLocationSeed(
            ArLocationFix(
                position = Position(36.1699, -115.1398, 620.0),
                accuracyMeters = 35f,
                elapsedRealtimeNanos = 115_000_000_000L,
            ),
        )

        val resumedPosition = arPositionForLastKnownLocationSeed(null)

        assertEquals(Position(36.1699, -115.1398, 620.0), firstStartPosition)
        assertEquals(Position(0.0, 0.0, 0.0), resumedPosition)
    }

    @Test
    fun `stale GPS does not hide a fresh network location`() {
        val freshNetwork = Position(36.1699, -115.1398, 620.0)

        assertEquals(
            freshNetwork,
            selectFreshestArLastKnownLocationFix(
                gps = ArLocationFix(
                    position = Position(37.0, -122.0, 0.0),
                    accuracyMeters = 8f,
                    elapsedRealtimeNanos = 1_000_000_000L,
                ),
                network = ArLocationFix(
                    position = freshNetwork,
                    accuracyMeters = 35f,
                    elapsedRealtimeNanos = 115_000_000_000L,
                ),
                nowElapsedRealtimeNanos = 120_000_000_000L,
            )?.position,
        )
    }

    @Test
    fun `invalid newer fix does not hide a fresh valid location`() {
        val freshNetwork = Position(36.1699, -115.1398, 620.0)

        assertEquals(
            freshNetwork,
            selectFreshestArLastKnownLocationFix(
                gps = ArLocationFix(
                    position = Position(0.0, 0.0, 0.0),
                    accuracyMeters = 8f,
                    elapsedRealtimeNanos = 119_000_000_000L,
                ),
                network = ArLocationFix(
                    position = freshNetwork,
                    accuracyMeters = 35f,
                    elapsedRealtimeNanos = 115_000_000_000L,
                ),
                nowElapsedRealtimeNanos = 120_000_000_000L,
            )?.position,
        )
    }
}

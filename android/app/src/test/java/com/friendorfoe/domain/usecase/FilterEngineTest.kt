package com.friendorfoe.domain.usecase

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.FilterState
import com.friendorfoe.domain.model.Position
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Test

class FilterEngineTest {
    @Test
    fun activeDistanceLimitExcludesLiveObjectsWithUnknownDistance() {
        val unknown = drone("unknown", distanceMeters = null)
        val nearby = drone("nearby", distanceMeters = 1_000.0)
        val boundary = drone("boundary", distanceMeters = 1_852.0)
        val far = drone("far", distanceMeters = 1_853.0)

        assertEquals(
            listOf(nearby, boundary),
            FilterEngine.applyFilters(
                listOf(unknown, nearby, boundary, far),
                FilterState(maxDistanceNm = 1f),
            ),
        )
    }

    @Test
    fun activeDistanceLimitExcludesHistoryRowsWithUnknownDistance() {
        val unknown = history("unknown", distanceMeters = null)
        val nearby = history("nearby", distanceMeters = 1_000.0)

        assertEquals(
            listOf(nearby),
            FilterEngine.applyFilters(
                listOf(unknown, nearby),
                FilterState(maxDistanceNm = 1f),
            ),
        )
    }

    @Test
    fun unknownDistanceRemainsVisibleWhenDistanceFilterIsInactive() {
        val unknown = drone("unknown", distanceMeters = null)

        assertEquals(listOf(unknown), FilterEngine.applyFilters(listOf(unknown), FilterState()))
    }

    private fun drone(id: String, distanceMeters: Double?) = Drone(
        id = id,
        position = Position(latitude = 32.7, longitude = -117.1, altitudeMeters = 100.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        distanceMeters = distanceMeters,
        droneId = id,
    )

    private fun history(id: String, distanceMeters: Double?) = HistoryEntity(
        objectId = id,
        objectType = "drone",
        detectionSource = "remote_id",
        category = "drone",
        displayName = id,
        description = null,
        latitude = 32.7,
        longitude = -117.1,
        altitudeMeters = 100.0,
        userLatitude = 32.7,
        userLongitude = -117.1,
        distanceMeters = distanceMeters,
        confidence = 0.9f,
        firstSeen = 0L,
        lastSeen = 1L,
    )
}

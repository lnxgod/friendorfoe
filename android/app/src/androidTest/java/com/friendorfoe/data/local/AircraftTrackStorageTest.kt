package com.friendorfoe.data.local

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.friendorfoe.data.repository.AIRCRAFT_TRACK_RETENTION_MS
import com.friendorfoe.data.repository.HistoryRepository
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

class AircraftTrackStorageTest {
    private val database = Room.inMemoryDatabaseBuilder(ApplicationProvider.getApplicationContext(), FriendOrFoeDatabase::class.java).build()
    private val dao = database.trackingDao()
    private val now = 1_800_000_000_000L
    private fun point(time: Long, lat: Double = 32.7) = TrackingEntity(
        objectId = "abc123", latitude = lat, longitude = -117.1, altitudeMeters = 1000.0,
        heading = 90f, speedMps = 100f, timestamp = time,
    )
    private fun history() = HistoryEntity(
        objectId = "abc123", objectType = "aircraft", detectionSource = "ads_b", category = "commercial",
        displayName = "N123", description = null, latitude = 32.7, longitude = -117.1,
        altitudeMeters = 1000.0, userLatitude = 32.6, userLongitude = -117.0, distanceMeters = 1000.0,
        confidence = 0.95f, firstSeen = now, lastSeen = now,
    )

    @After fun close() = database.close()

    @Test fun observedPointsAreDeduplicatedOrderedBoundedAndRetainedAcrossRecorderCalls() = runBlocking {
        dao.insert(point(now - AIRCRAFT_TRACK_RETENTION_MS - 1))
        val initial = point(now - 10_000)
        val next = point(now, lat = 32.701)
        dao.recordAircraftBatch(listOf(initial, next), now, true)
        dao.recordAircraftBatch(listOf(initial, next, point(now + 60_000)), now, false)
        assertEquals(listOf(initial.timestamp, next.timestamp), dao.getTrailForObject("abc123").map { it.timestamp })
        assertEquals(listOf(next.timestamp), dao.observeTrailForObject("abc123", 1).first().map { it.timestamp })
    }

    @Test fun deletingLastHistoryRowDeletesPathButEarlierSnapshotDeletionDoesNot() = runBlocking {
        val repository = HistoryRepository(database.historyDao(), database)
        val first = repository.save(history())
        val second = repository.save(history().copy(firstSeen = now + 1, lastSeen = now + 1))
        dao.insert(point(now))
        repository.deleteById(first)
        assertEquals(1, dao.getTrailForObject("abc123").size)
        repository.deleteById(second)
        assertTrue(dao.getTrailForObject("abc123").isEmpty())
    }

    @Test fun clearingHistoryAlsoClearsPositionStorage() = runBlocking {
        val repository = HistoryRepository(database.historyDao(), database)
        repository.save(history())
        dao.insert(point(now))
        repository.clearAll()
        assertEquals(0, database.historyDao().getCount())
        assertTrue(dao.getTrailForObject("abc123").isEmpty())
    }
    @Test fun mapQueryUsesLatestAircraftMetadataWithoutDuplicatingPaths() = runBlocking {
        val repository = HistoryRepository(database.historyDao(), database)
        repository.save(history())
        repository.save(history().copy(displayName = "LATEST", lastSeen = now + 1))
        repository.save(history().copy(objectId = "drone", objectType = "drone"))
        dao.insert(point(now - 1000))
        dao.insert(point(now))
        dao.insert(point(now).copy(objectId = "drone"))
        dao.insert(point(now).copy(objectId = "unknown"))
        val records = dao.observeAircraftMapPoints(now - 1000, 100).first()
        assertEquals(2, records.size)
        assertTrue(records.all { it.label == "LATEST" && it.point.objectId == "abc123" })
        assertEquals(listOf(now), dao.observeAircraftMapPoints(now, 100).first().map { it.point.timestamp })
        assertEquals(1, dao.observeAircraftMapPoints(0, 1).first().size)
        repository.clearAll()
        assertTrue(dao.observeAircraftMapPoints(0, 100).first().isEmpty())
    }

}

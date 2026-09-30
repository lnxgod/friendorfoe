package com.friendorfoe.data.local

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.friendorfoe.data.repository.SavedFlightRepository
import com.friendorfoe.data.repository.decodedPoints
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

class SavedFlightStorageTest {
    private val db = Room.inMemoryDatabaseBuilder(ApplicationProvider.getApplicationContext(), FriendOrFoeDatabase::class.java).build()
    @After fun close() = db.close()
    @Test fun savedCopySurvivesTrailCleanupAndCanBeExplicitlyDeleted() = runBlocking {
        val point = TrackingEntity(objectId = "abc", latitude = 32.7, longitude = -117.1, altitudeMeters = 1000.0,
            heading = null, speedMps = null, timestamp = 1000)
        db.trackingDao().insert(point)
        val repository = SavedFlightRepository(db)
        val id = repository.save("abc", "N123", listOf(point))
        db.trackingDao().deleteOlderThan(System.currentTimeMillis())
        assertTrue(db.trackingDao().getTrailForObject("abc").isEmpty())
        val saved = requireNotNull(repository.observe(id).first())
        assertEquals(listOf(point), saved.decodedPoints())
        assertEquals("N123", repository.observeAll().first().single().label)
        repository.delete(id)
        assertNull(repository.observe(id).first())
    }
}

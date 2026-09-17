package com.friendorfoe.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import androidx.room.Transaction
import kotlinx.coroutines.flow.Flow
import com.friendorfoe.data.repository.shouldRecordAircraftPoint
import com.friendorfoe.data.repository.AIRCRAFT_TRACK_RETENTION_MS

@Dao
interface TrackingDao {
    @Query("SELECT * FROM position_tracking WHERE object_id = :objectId ORDER BY timestamp ASC")
    suspend fun getTrailForObject(objectId: String): List<TrackingEntity>

    @Query("SELECT * FROM (SELECT * FROM position_tracking WHERE object_id = :objectId ORDER BY timestamp DESC, id DESC LIMIT :limit) ORDER BY timestamp ASC, id ASC")
    fun observeTrailForObject(objectId: String, limit: Int): Flow<List<TrackingEntity>>

    @Query("SELECT * FROM position_tracking WHERE object_id = :objectId ORDER BY timestamp DESC, id DESC LIMIT 1")
    suspend fun latestForObject(objectId: String): TrackingEntity?

    @Query("DELETE FROM position_tracking WHERE object_id = :objectId")
    suspend fun deleteForObject(objectId: String)

    @Query("DELETE FROM position_tracking")
    suspend fun deleteAll()

    @Query("DELETE FROM position_tracking WHERE id IN (SELECT id FROM position_tracking ORDER BY timestamp DESC, id DESC LIMIT -1 OFFSET 100000)")
    suspend fun trimToStorageLimit()

    @Transaction
    suspend fun recordAircraftBatch(points: List<TrackingEntity>, nowMs: Long, prune: Boolean) {
        points.forEach { point ->
            if (shouldRecordAircraftPoint(latestForObject(point.objectId), point, nowMs)) insert(point)
        }
        if (prune) deleteOlderThan(nowMs - AIRCRAFT_TRACK_RETENTION_MS)
        trimToStorageLimit()
    }

    @Insert
    suspend fun insert(entity: TrackingEntity)

    @Query("DELETE FROM position_tracking WHERE timestamp < :beforeTime")
    suspend fun deleteOlderThan(beforeTime: Long)
}

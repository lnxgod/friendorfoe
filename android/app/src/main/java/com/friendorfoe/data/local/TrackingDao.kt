package com.friendorfoe.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query

@Dao
interface TrackingDao {
    @Query("SELECT * FROM position_tracking WHERE object_id = :objectId ORDER BY timestamp ASC")
    suspend fun getTrailForObject(objectId: String): List<TrackingEntity>

    @Insert
    suspend fun insert(entity: TrackingEntity)

    @Query("DELETE FROM position_tracking WHERE timestamp < :beforeTime")
    suspend fun deleteOlderThan(beforeTime: Long)
}

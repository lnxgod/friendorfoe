package com.friendorfoe.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

/**
 * Data Access Object for detection history records.
 *
 * Provides reactive queries (Flow) for the History screen and
 * insert/delete operations for managing history.
 */
@Dao
interface HistoryDao {

    /** Get all history entries ordered by most recent first. */
    @Query("SELECT * FROM detection_history ORDER BY last_seen DESC")
    fun getAllHistory(): Flow<List<HistoryEntity>>

    /** Get history entries for a specific date range. */
    @Query("SELECT * FROM detection_history WHERE last_seen BETWEEN :startTime AND :endTime ORDER BY last_seen DESC")
    fun getHistoryBetween(startTime: Long, endTime: Long): Flow<List<HistoryEntity>>

    /** Get history entries by object type (aircraft or drone). */
    @Query("SELECT * FROM detection_history WHERE object_type = :objectType ORDER BY last_seen DESC")
    fun getHistoryByType(objectType: String): Flow<List<HistoryEntity>>

    /** Get a specific history entry by object ID. */
    @Query("SELECT * FROM detection_history WHERE object_id = :objectId ORDER BY last_seen DESC LIMIT 1")
    suspend fun getByObjectId(objectId: String): HistoryEntity?

    /** Insert or update a history entry. */
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insert(entity: HistoryEntity): Long

    /** Insert multiple history entries. */
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(entities: List<HistoryEntity>)

    /** Delete a specific history entry. */
    @Query("DELETE FROM detection_history WHERE id = :id")
    suspend fun deleteById(id: Long)

    /** Delete all history older than a given timestamp. */
    @Query("DELETE FROM detection_history WHERE last_seen < :beforeTime")
    suspend fun deleteOlderThan(beforeTime: Long)

    /** Delete all history entries. */
    @Query("DELETE FROM detection_history")
    suspend fun deleteAll()

    /** Get total count of history entries. */
    @Query("SELECT COUNT(*) FROM detection_history")
    suspend fun getCount(): Int
}

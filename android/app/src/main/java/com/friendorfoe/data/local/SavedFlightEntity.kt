package com.friendorfoe.data.local

import androidx.room.*
import kotlinx.coroutines.flow.Flow

@Entity(tableName = "saved_flights")
data class SavedFlightEntity(
    @PrimaryKey val id: String,
    val objectId: String,
    val label: String,
    val savedAt: Long,
    val pointCount: Int,
    val pointsJson: String,
)

data class SavedFlightSummary(val id: String, val label: String, val savedAt: Long, val pointCount: Int)

@Dao
interface SavedFlightDao {
    @Query("SELECT id, label, savedAt, pointCount FROM saved_flights ORDER BY savedAt DESC")
    fun observeAll(): Flow<List<SavedFlightSummary>>
    @Query("SELECT * FROM saved_flights WHERE id = :id")
    fun observe(id: String): Flow<SavedFlightEntity?>
    @Insert suspend fun insert(flight: SavedFlightEntity)
    @Query("DELETE FROM saved_flights WHERE id = :id") suspend fun delete(id: String)
}

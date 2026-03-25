package com.friendorfoe.data.local

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

@Entity(
    tableName = "position_tracking",
    indices = [
        Index(value = ["object_id"]),
        Index(value = ["timestamp"])
    ]
)
data class TrackingEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    @ColumnInfo(name = "object_id") val objectId: String,
    @ColumnInfo(name = "latitude") val latitude: Double,
    @ColumnInfo(name = "longitude") val longitude: Double,
    @ColumnInfo(name = "altitude_meters") val altitudeMeters: Double,
    @ColumnInfo(name = "heading") val heading: Float?,
    @ColumnInfo(name = "speed_mps") val speedMps: Float?,
    @ColumnInfo(name = "timestamp") val timestamp: Long
)

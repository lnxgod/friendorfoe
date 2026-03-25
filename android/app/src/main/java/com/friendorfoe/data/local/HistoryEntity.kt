package com.friendorfoe.data.local

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

/**
 * Room entity representing a historical detection record.
 *
 * Stores every unique sky object detected, with timestamps and
 * enriched data, for the History screen.
 */
@Entity(
    tableName = "detection_history",
    indices = [
        Index(value = ["object_id"]),
        Index(value = ["object_type"]),
        Index(value = ["last_seen"])
    ]
)
data class HistoryEntity(
    @PrimaryKey(autoGenerate = true)
    val id: Long = 0,

    /** Unique object ID (ICAO hex for aircraft, drone serial for drones) */
    @ColumnInfo(name = "object_id")
    val objectId: String,

    /** "aircraft" or "drone" */
    @ColumnInfo(name = "object_type")
    val objectType: String,

    /** Detection source: "ads_b", "remote_id", "wifi" */
    @ColumnInfo(name = "detection_source")
    val detectionSource: String,

    /** Category: "commercial", "general_aviation", "military", "drone", "unknown" */
    @ColumnInfo(name = "category")
    val category: String,

    /** Display name (callsign for aircraft, manufacturer for drones) */
    @ColumnInfo(name = "display_name")
    val displayName: String,

    /** Short description */
    @ColumnInfo(name = "description")
    val description: String?,

    /** Latitude at time of detection */
    @ColumnInfo(name = "latitude")
    val latitude: Double,

    /** Longitude at time of detection */
    @ColumnInfo(name = "longitude")
    val longitude: Double,

    /** Altitude in meters at time of detection */
    @ColumnInfo(name = "altitude_meters")
    val altitudeMeters: Double,

    /** User's latitude at time of detection */
    @ColumnInfo(name = "user_latitude")
    val userLatitude: Double,

    /** User's longitude at time of detection */
    @ColumnInfo(name = "user_longitude")
    val userLongitude: Double,

    /** Distance from user in meters at time of detection */
    @ColumnInfo(name = "distance_meters")
    val distanceMeters: Double?,

    /** Detection confidence (0.0 to 1.0) */
    @ColumnInfo(name = "confidence")
    val confidence: Float,

    /** Timestamp of first detection (epoch millis) */
    @ColumnInfo(name = "first_seen")
    val firstSeen: Long,

    /** Timestamp of last update (epoch millis) */
    @ColumnInfo(name = "last_seen")
    val lastSeen: Long,

    /** Photo URL if available */
    @ColumnInfo(name = "photo_url")
    val photoUrl: String? = null
)

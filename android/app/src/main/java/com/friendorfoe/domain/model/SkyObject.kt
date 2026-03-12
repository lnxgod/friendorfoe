package com.friendorfoe.domain.model

import java.time.Instant

/**
 * Base sealed class for all detectable objects in the sky.
 *
 * All detection sources (ADS-B, Remote ID, WiFi) produce objects
 * that extend this class. The sensor fusion engine unifies them
 * into a single list for the AR overlay.
 *
 * @property id Unique identifier for this detection instance
 * @property position Current geographic position
 * @property source How this object was detected
 * @property category Classification of the object type
 * @property confidence Detection confidence (0.0 to 1.0)
 * @property firstSeen When this object was first detected
 * @property lastUpdated When position was last updated
 * @property distanceMeters Calculated distance from user, null if not yet computed
 * @property screenX Projected screen X coordinate, null if not in view
 * @property screenY Projected screen Y coordinate, null if not in view
 */
sealed class SkyObject {
    abstract val id: String
    abstract val position: Position
    abstract val source: DetectionSource
    abstract val category: ObjectCategory
    abstract val confidence: Float
    abstract val firstSeen: Instant
    abstract val lastUpdated: Instant
    abstract val distanceMeters: Double?
    abstract val screenX: Float?
    abstract val screenY: Float?

    /** Human-readable label for AR overlay display */
    abstract fun displayLabel(): String

    /** Short summary for list view */
    abstract fun displaySummary(): String
}

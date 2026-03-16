package com.friendorfoe.sensor

import com.friendorfoe.detection.VisualDetection
import com.friendorfoe.domain.model.SkyObject

/**
 * Represents the projected screen position of a sky object.
 *
 * Screen coordinates are normalized to 0.0-1.0 where (0,0) is top-left
 * and (1,1) is bottom-right of the camera preview.
 *
 * @property skyObject The original sky object being projected
 * @property screenX Normalized X coordinate (0.0 = left edge, 1.0 = right edge)
 * @property screenY Normalized Y coordinate (0.0 = top edge, 1.0 = bottom edge)
 * @property isInView Whether the object is within the camera's field of view
 * @property bearingDegrees Compass bearing from user to object in degrees (0-360)
 * @property elevationDegrees Elevation angle from user to object in degrees
 * @property distanceMeters Ground distance from user to object in meters
 */
data class ScreenPosition(
    val skyObject: SkyObject,
    val screenX: Float,
    val screenY: Float,
    val isInView: Boolean,
    val bearingDegrees: Float = 0f,
    val elevationDegrees: Float = 0f,
    val distanceMeters: Double = 0.0,
    /** Ground distance (horizontal only, no altitude component) in meters */
    val groundDistanceMeters: Double = 0.0,
    /** True when a visual detection has been matched to this radio position */
    val visuallyConfirmed: Boolean = false,
    /** The matched visual detection, if any */
    val matchedDetection: VisualDetection? = null
)

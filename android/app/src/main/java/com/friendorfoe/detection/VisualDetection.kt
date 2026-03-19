package com.friendorfoe.detection

/**
 * Represents a single object detected by ML Kit visual object detection.
 *
 * All coordinates are normalized to 0-1 range, matching the convention
 * used by [com.friendorfoe.sensor.ScreenPosition].
 */
data class VisualDetection(
    /** ML Kit cross-frame tracking ID (null if tracking not available) */
    val trackingId: Int?,
    /** Normalized center X coordinate (0-1) */
    val centerX: Float,
    /** Normalized center Y coordinate (0-1) */
    val centerY: Float,
    /** Normalized bounding box width (0-1) */
    val width: Float,
    /** Normalized bounding box height (0-1) */
    val height: Float,
    /** ML Kit base model labels (may be empty for unknown objects) */
    val labels: List<String>,
    /** Confidence scores parallel to [labels] (0.0-1.0) */
    val labelConfidences: List<Float> = emptyList(),
    /** Timestamp in milliseconds when this detection was produced */
    val timestampMs: Long,
    /** Sky-relevance score from [SkyObjectFilter] (0-1, higher = more likely sky object) */
    val skyScore: Float = 0f,
    /** Motion consistency score from [SkyObjectFilter] (0-1, higher = consistent movement) */
    val motionScore: Float = 0f,
    /** Heuristic classification from [SkyObjectFilter] */
    val visualClassification: VisualClassification? = null,
    /** True when a strobe pattern has been correlated with this detection */
    val strobeConfirmed: Boolean = false
)

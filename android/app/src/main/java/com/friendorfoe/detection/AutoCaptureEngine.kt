package com.friendorfoe.detection

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.sensor.ScreenPosition
import kotlin.math.sqrt

/**
 * Stateless engine that correlates ML Kit visual detections with tracked
 * radio-detected sky objects to find auto-capture candidates.
 *
 * A candidate is found when an ML Kit bounding box overlaps a nearby
 * (< 3km) sky object's projected screen position.
 */
object AutoCaptureEngine {

    /** Maximum ground distance (meters) for auto-capture eligibility */
    private const val MAX_DISTANCE_METERS = 3000.0

    /** Maximum screen-space distance (normalized 0-1) to correlate visual + radio */
    private const val MAX_CORRELATION_DISTANCE = 0.10f

    /** Minimum cooldown between auto-captures (ms) */
    const val COOLDOWN_MS = 5000L

    /**
     * Find the best auto-capture candidate from current detections.
     *
     * @param screenPositions Current radio-detected objects with screen projections
     * @param visualDetections Current ML Kit visual detections (enriched with classification)
     * @param capturedIds Set of object IDs already captured this session
     * @param lastCaptureTimeMs Timestamp of the last auto-capture
     * @param isLockedOn Whether the user is manually locked onto an object
     * @return Best capture candidate, or null if none qualifies
     */
    fun findCaptureCandidate(
        screenPositions: List<ScreenPosition>,
        visualDetections: List<VisualDetection>,
        capturedIds: Set<String>,
        lastCaptureTimeMs: Long,
        isLockedOn: Boolean
    ): CaptureCandidate? {
        // Don't interfere with manual lock-on
        if (isLockedOn) return null

        // Enforce cooldown
        if (System.currentTimeMillis() - lastCaptureTimeMs < COOLDOWN_MS) return null

        // Filter to eligible visual detections (flying objects only)
        val flyingVisuals = visualDetections.filter { det ->
            det.visualClassification == VisualClassification.LIKELY_AIRCRAFT ||
            det.visualClassification == VisualClassification.LIKELY_DRONE ||
            det.visualClassification == VisualClassification.UNKNOWN_FLYING
        }
        if (flyingVisuals.isEmpty()) return null

        // Filter to eligible sky objects: close enough, not already captured, has position
        val eligiblePositions = screenPositions.filter { sp ->
            sp.skyObject.id !in capturedIds &&
            sp.groundDistanceMeters < MAX_DISTANCE_METERS &&
            sp.groundDistanceMeters > 0.0 &&
            isEligibleSource(sp.skyObject)
        }
        if (eligiblePositions.isEmpty()) return null

        // Find best correlation: visual detection ↔ radio sky object
        var bestCandidate: CaptureCandidate? = null
        var bestCorrelation = Float.MAX_VALUE

        for (sp in eligiblePositions) {
            for (vis in flyingVisuals) {
                val dx = vis.centerX - sp.screenX
                val dy = vis.centerY - sp.screenY
                val dist = sqrt(dx * dx + dy * dy)

                if (dist < MAX_CORRELATION_DISTANCE && dist < bestCorrelation) {
                    bestCorrelation = dist
                    bestCandidate = CaptureCandidate(
                        skyObject = sp.skyObject,
                        visualDetection = vis,
                        distanceMeters = sp.groundDistanceMeters,
                        correlationDistanceNorm = dist,
                        suggestedZoom = suggestZoom(sp.groundDistanceMeters)
                    )
                }
            }
        }

        // Also try correlation using ScreenPosition's own visual match
        // (VisualCorrelationEngine may have already matched them)
        for (sp in eligiblePositions) {
            if (sp.visuallyConfirmed && sp.skyObject.id !in capturedIds) {
                val dist = sp.groundDistanceMeters
                if (dist < MAX_DISTANCE_METERS && dist > 0.0) {
                    val correlationDist = 0f // already correlated
                    if (bestCandidate == null || correlationDist < bestCorrelation) {
                        bestCorrelation = correlationDist
                        bestCandidate = CaptureCandidate(
                            skyObject = sp.skyObject,
                            visualDetection = sp.matchedDetection ?: continue,
                            distanceMeters = dist,
                            correlationDistanceNorm = correlationDist,
                            suggestedZoom = suggestZoom(dist)
                        )
                    }
                }
            }
        }

        return bestCandidate
    }

    /** Only auto-capture objects with reliable positioning (ADS-B or Remote ID with GPS) */
    private fun isEligibleSource(obj: SkyObject): Boolean = when (obj) {
        is Aircraft -> true // ADS-B always has position
        is Drone -> {
            // Only drones with actual GPS position (not WiFi-only)
            obj.source != DetectionSource.WIFI &&
            (obj.position.latitude != 0.0 || obj.position.longitude != 0.0)
        }
    }

    private fun suggestZoom(groundDistanceMeters: Double): Float = when {
        groundDistanceMeters < 500 -> 1.5f
        groundDistanceMeters < 2000 -> 2.0f
        groundDistanceMeters < 5000 -> 3.0f
        else -> 4.0f
    }

}

/**
 * A candidate for auto-capture: a sky object visually confirmed by ML Kit.
 */
data class CaptureCandidate(
    val skyObject: SkyObject,
    val visualDetection: VisualDetection,
    val distanceMeters: Double,
    val correlationDistanceNorm: Float,
    val suggestedZoom: Float
)

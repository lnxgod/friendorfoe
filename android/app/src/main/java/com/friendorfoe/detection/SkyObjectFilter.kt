package com.friendorfoe.detection

import javax.inject.Inject
import javax.inject.Singleton

/**
 * Heuristic filter applied after ML Kit object detection to score and classify
 * detections as sky objects vs. ground clutter.
 *
 * Uses sky-region bias, size heuristics, aspect ratio, and frame-to-frame motion
 * tracking to produce [ScoredVisualDetection] with sky/motion scores and classification.
 */
@Singleton
class SkyObjectFilter @Inject constructor() {

    companion object {
        /** Ring buffer capacity for motion tracking */
        private const val MOTION_BUFFER_SIZE = 10
        /** Frames stationary before classifying as static */
        private const val STATIC_FRAME_THRESHOLD = 10
        /** Minimum movement between frames to count as motion (normalized coords) */
        private const val MOTION_EPSILON = 0.005f
    }

    /** Ring buffer of recent detection positions keyed by trackingId */
    private val motionHistory = mutableMapOf<Int, ArrayDeque<PositionFrame>>()

    /**
     * Filter and score raw ML Kit detections.
     *
     * @param detections Raw detections from ML Kit
     * @param confidenceMultiplier Weather-based scaling factor (1.0 = clear, lower = poor visibility)
     * @param strobeDetections Optional strobe detections from nighttime analysis
     * @return Scored and classified detections
     */
    fun filter(
        detections: List<VisualDetection>,
        confidenceMultiplier: Float = 1.0f,
        strobeDetections: List<StrobeDetection> = emptyList(),
        devicePitchDegrees: Float = 0f
    ): List<ScoredVisualDetection> {
        val results = mutableListOf<ScoredVisualDetection>()

        for (detection in detections) {
            val skyScore = computeSkyScore(detection, devicePitchDegrees) * confidenceMultiplier
            val motionScore = computeMotionScore(detection)

            // Check for nearby strobe correlation
            val nearbyStrobe = findNearbyStrobe(detection, strobeDetections)
            val classification = if (nearbyStrobe != null) {
                classifyWithStrobe(nearbyStrobe)
            } else {
                classify(detection, skyScore, motionScore)
            }

            // Boost sky score if correlated with a strobe detection
            val adjustedSkyScore = if (nearbyStrobe != null) {
                (skyScore + nearbyStrobe.confidence * 0.3f).coerceAtMost(1f)
            } else {
                skyScore
            }

            val shapeClass = if (classification != VisualClassification.LIKELY_STATIC) {
                inferShapeClass(detection)
            } else {
                ShapeClass.INDETERMINATE
            }

            results.add(
                ScoredVisualDetection(
                    detection = detection,
                    skyScore = adjustedSkyScore,
                    motionScore = motionScore,
                    classification = classification,
                    shapeClass = shapeClass
                )
            )
        }

        // Add strobe-only detections that don't correlate with any ML Kit detection
        // (nighttime: ML Kit often finds nothing, but strobes are visible)
        for (strobe in strobeDetections) {
            val alreadyCorrelated = results.any { scored ->
                val dx = scored.detection.centerX - strobe.centerX
                val dy = scored.detection.centerY - strobe.centerY
                kotlin.math.sqrt(dx * dx + dy * dy) < STROBE_CORRELATION_DISTANCE
            }
            if (!alreadyCorrelated && strobe.classification != StrobeClassification.STATIC &&
                strobe.classification != StrobeClassification.NOISE) {
                val classification = classifyWithStrobe(strobe)
                results.add(
                    ScoredVisualDetection(
                        detection = VisualDetection(
                            trackingId = strobe.trackingId + 10000, // offset to avoid ID collision
                            centerX = strobe.centerX,
                            centerY = strobe.centerY,
                            width = 0.02f,
                            height = 0.02f,
                            labels = listOf("strobe"),
                            timestampMs = System.currentTimeMillis()
                        ),
                        skyScore = strobe.confidence,
                        motionScore = if (strobe.hasAngularMotion) 0.7f else 0.3f,
                        classification = classification
                    )
                )
            }
        }

        // Prune motion history for trackingIds no longer present
        val activeIds = detections.mapNotNull { it.trackingId }.toSet()
        motionHistory.keys.retainAll(activeIds)

        return results
    }

    /** Distance threshold (normalized coords) to correlate ML Kit detection with strobe */
    private val STROBE_CORRELATION_DISTANCE = 0.08f

    /**
     * Find a strobe detection near an ML Kit detection.
     */
    private fun findNearbyStrobe(
        detection: VisualDetection,
        strobes: List<StrobeDetection>
    ): StrobeDetection? {
        return strobes.minByOrNull { strobe ->
            val dx = detection.centerX - strobe.centerX
            val dy = detection.centerY - strobe.centerY
            kotlin.math.sqrt(dx * dx + dy * dy)
        }?.takeIf { strobe ->
            val dx = detection.centerX - strobe.centerX
            val dy = detection.centerY - strobe.centerY
            kotlin.math.sqrt(dx * dx + dy * dy) < STROBE_CORRELATION_DISTANCE
        }
    }

    /**
     * Classify based on strobe flash rate.
     */
    private fun classifyWithStrobe(strobe: StrobeDetection): VisualClassification {
        return when (strobe.classification) {
            StrobeClassification.DRONE_LED -> VisualClassification.LIKELY_DRONE
            StrobeClassification.AIRCRAFT_STROBE -> VisualClassification.LIKELY_AIRCRAFT
            StrobeClassification.STATIC -> VisualClassification.LIKELY_STATIC
            StrobeClassification.NOISE -> VisualClassification.LIKELY_STATIC
        }
    }

    /**
     * Compute sky-region score based on position, size, and aspect ratio.
     * Higher = more likely to be a sky object.
     */
    private fun computeSkyScore(detection: VisualDetection, pitchDegrees: Float = 0f): Float {
        var score = 0.5f

        // Pitch-aware sky region bias: adjust ground region based on device elevation angle
        val groundRegionStart = when {
            pitchDegrees > 30f -> 0.90f   // High angle: bottom 10% is ground
            pitchDegrees > 10f -> 0.80f   // Medium angle: bottom 20% is ground
            pitchDegrees > -10f -> 0.65f  // Near horizon: bottom 35% is ground
            else -> 1.0f                   // Below horizon: everything is ground
        }
        when {
            detection.centerY <= groundRegionStart * 0.8f -> score += 0.2f  // sky region
            detection.centerY >= groundRegionStart -> score -= 0.4f          // ground region
        }

        // Size heuristics
        val area = detection.width * detection.height
        when {
            area > 0.25f -> score -= 0.5f  // >25% frame area — likely ground clutter
            area < 0.005f -> score += 0.1f  // <0.5% — small, could be distant object
        }

        // Aspect ratio: very tall/narrow → likely poles/buildings
        val aspectRatio = if (detection.height > 0f) detection.width / detection.height else 1f
        if (aspectRatio < 0.3f || aspectRatio > 3.5f) {
            score -= 0.2f  // extreme aspect ratio — unlikely flying object
        }

        return score.coerceIn(0f, 1f)
    }

    /**
     * Compute motion score by tracking position across frames via ring buffer.
     * Consistent velocity = boost, stationary = demote.
     */
    private fun computeMotionScore(detection: VisualDetection): Float {
        val trackingId = detection.trackingId ?: return 0.5f  // no tracking — neutral

        val history = motionHistory.getOrPut(trackingId) { ArrayDeque() }
        history.addLast(PositionFrame(detection.centerX, detection.centerY, detection.timestampMs))
        while (history.size > MOTION_BUFFER_SIZE) {
            history.removeFirst()
        }

        if (history.size < 3) return 0.5f  // not enough history

        // Count frames with motion
        var motionFrames = 0
        var totalVelocity = 0f
        val velocities = mutableListOf<Float>()

        for (i in 1 until history.size) {
            val prev = history[i - 1]
            val curr = history[i]
            val dx = curr.x - prev.x
            val dy = curr.y - prev.y
            val dist = kotlin.math.sqrt(dx * dx + dy * dy)
            val dt = (curr.timestampMs - prev.timestampMs).coerceAtLeast(1L)
            val velocity = dist / dt * 1000f  // normalized units per second

            if (dist > MOTION_EPSILON) {
                motionFrames++
            }
            totalVelocity += velocity
            velocities.add(velocity)
        }

        // Stationary for too long → demote
        val stationaryFrames = (history.size - 1) - motionFrames
        if (stationaryFrames >= STATIC_FRAME_THRESHOLD) {
            return 0.1f
        }

        // Consistent velocity → boost (low variance in velocity)
        if (velocities.size >= 3) {
            val avgVelocity = totalVelocity / velocities.size
            if (avgVelocity > 0.01f) {
                val variance = velocities.map { (it - avgVelocity) * (it - avgVelocity) }.average().toFloat()
                val cv = if (avgVelocity > 0f) kotlin.math.sqrt(variance) / avgVelocity else 0f
                return when {
                    cv < 0.3f -> 0.9f  // very consistent velocity — likely aircraft
                    cv < 0.7f -> 0.7f  // somewhat consistent — could be drone
                    else -> 0.6f       // erratic — could be bird
                }
            }
        }

        return if (motionFrames > 0) 0.6f else 0.3f
    }

    /**
     * Classify detection based on combined sky and motion scores plus size/behavior.
     */
    private fun classify(
        detection: VisualDetection,
        skyScore: Float,
        motionScore: Float
    ): VisualClassification {
        // Definitely ground clutter
        if (skyScore < 0.2f) return VisualClassification.LIKELY_STATIC

        // Stationary in sky region — static object (antenna, building edge)
        if (motionScore < 0.2f && skyScore < 0.6f) return VisualClassification.LIKELY_STATIC

        val area = detection.width * detection.height
        val trackingId = detection.trackingId

        // Use motion pattern and size for flying object classification
        if (trackingId != null) {
            val history = motionHistory[trackingId]
            if (history != null && history.size >= 3) {
                val avgVelocity = computeAverageVelocity(history)
                val erraticism = computeErraticism(history)

                return when {
                    // Small, slow/hovering → drone
                    area < 0.03f && avgVelocity < 0.05f -> VisualClassification.LIKELY_DRONE
                    // Small, erratic → bird
                    area < 0.02f && erraticism > 0.5f -> VisualClassification.LIKELY_BIRD
                    // Medium, fast, straight → aircraft
                    avgVelocity > 0.08f && erraticism < 0.3f -> VisualClassification.LIKELY_AIRCRAFT
                    // Has motion but doesn't match patterns
                    motionScore > 0.4f -> VisualClassification.UNKNOWN_FLYING
                    else -> VisualClassification.LIKELY_STATIC
                }
            }
        }

        // Not enough data to classify specifically
        return if (skyScore > 0.5f) VisualClassification.UNKNOWN_FLYING
        else VisualClassification.LIKELY_STATIC
    }

    private fun computeAverageVelocity(history: ArrayDeque<PositionFrame>): Float {
        if (history.size < 2) return 0f
        var total = 0f
        for (i in 1 until history.size) {
            val dx = history[i].x - history[i - 1].x
            val dy = history[i].y - history[i - 1].y
            val dt = (history[i].timestampMs - history[i - 1].timestampMs).coerceAtLeast(1L)
            total += kotlin.math.sqrt(dx * dx + dy * dy) / dt * 1000f
        }
        return total / (history.size - 1)
    }

    /**
     * Measure erraticism as direction variance (0 = perfectly straight, 1 = random).
     */
    private fun computeErraticism(history: ArrayDeque<PositionFrame>): Float {
        if (history.size < 3) return 0f
        val angles = mutableListOf<Float>()
        for (i in 1 until history.size) {
            val dx = history[i].x - history[i - 1].x
            val dy = history[i].y - history[i - 1].y
            angles.add(kotlin.math.atan2(dy, dx))
        }
        // Compute angular variance
        if (angles.size < 2) return 0f
        val diffs = mutableListOf<Float>()
        for (i in 1 until angles.size) {
            var diff = angles[i] - angles[i - 1]
            // Normalize to -PI..PI
            while (diff > Math.PI) diff -= (2 * Math.PI).toFloat()
            while (diff < -Math.PI) diff += (2 * Math.PI).toFloat()
            diffs.add(kotlin.math.abs(diff))
        }
        val avgDiff = diffs.average().toFloat()
        return (avgDiff / Math.PI.toFloat()).coerceIn(0f, 1f)
    }

    /**
     * Infer physical shape class from bounding box aspect ratio and motion history.
     * Uses averaged aspect ratio over the motion buffer to smooth jitter.
     */
    private fun inferShapeClass(detection: VisualDetection): ShapeClass {
        val trackingId = detection.trackingId ?: return ShapeClass.INDETERMINATE
        val history = motionHistory[trackingId]
        if (history == null || history.size < 3) return ShapeClass.INDETERMINATE

        val avgVelocity = computeAverageVelocity(history)
        val erraticism = computeErraticism(history)

        // Use current detection aspect ratio (width/height)
        val aspectRatio = if (detection.height > 0f) detection.width / detection.height else 1f
        val area = detection.width * detection.height

        return when {
            // Fixed-wing: elongated, fast, straight path
            aspectRatio > 1.5f && avgVelocity > 0.06f && erraticism < 0.4f ->
                ShapeClass.FIXED_WING
            // Helicopter: moderately elongated, moderate speed and erraticism
            aspectRatio in 1.3f..2.0f && avgVelocity in 0.02f..0.07f && erraticism in 0.2f..0.6f ->
                ShapeClass.HELICOPTER
            // Multirotor (hex/octo): near-square, slow, larger bounding box
            aspectRatio in 0.8f..1.5f && avgVelocity < 0.05f && area > 0.01f ->
                ShapeClass.MULTIROTOR
            // Quadcopter: near-square, slow/hovering, small
            aspectRatio in 0.7f..1.3f && avgVelocity < 0.05f && erraticism < 0.4f ->
                ShapeClass.QUADCOPTER
            else -> ShapeClass.INDETERMINATE
        }
    }

    /** Reset all motion tracking history. */
    fun reset() {
        motionHistory.clear()
    }
}

/** Position sample for motion tracking ring buffer. */
private data class PositionFrame(
    val x: Float,
    val y: Float,
    val timestampMs: Long
)

/** A visual detection scored and classified by [SkyObjectFilter]. */
data class ScoredVisualDetection(
    val detection: VisualDetection,
    val skyScore: Float,
    val motionScore: Float,
    val classification: VisualClassification,
    val shapeClass: ShapeClass = ShapeClass.INDETERMINATE
)

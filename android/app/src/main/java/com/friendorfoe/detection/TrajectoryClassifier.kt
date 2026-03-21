package com.friendorfoe.detection

import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

/**
 * Classifies flight behavior from visual detection motion history.
 *
 * Pure geometric analysis of position ring buffers — no ML required.
 * Consumes scored detections that already have motion history tracked
 * by [SkyObjectFilter], and classifies their trajectory behavior.
 */
@Singleton
class TrajectoryClassifier @Inject constructor() {

    companion object {
        /** Minimum frames to attempt trajectory classification (~1s at 30fps) */
        private const val MIN_FRAMES = 30

        /** Heading variance threshold for straight-line transit (radians) */
        private const val TRANSIT_HEADING_VARIANCE = 0.09f // ~5 degrees

        /** Speed increase ratio to detect terminal dive */
        private const val DIVE_SPEED_RATIO = 2.5f

        /** Minimum hover stability (normalized movement per frame) */
        private const val HOVER_THRESHOLD = 0.001f
    }

    /**
     * Classify the trajectory behavior of a scored visual detection.
     *
     * @param scored The scored detection with motion history context
     * @param motionHistory The position ring buffer from SkyObjectFilter
     * @return Classified behavior, or UNKNOWN if insufficient data
     */
    fun classify(motionHistory: List<PositionFrame>): BehaviorClass {
        if (motionHistory.size < MIN_FRAMES) return BehaviorClass.UNKNOWN

        val headings = computeHeadings(motionHistory)
        if (headings.size < 5) return BehaviorClass.UNKNOWN

        val headingVariance = computeHeadingVariance(headings)
        val avgSpeed = computeAverageSpeed(motionHistory)

        // Check for hover (nearly stationary)
        if (avgSpeed < HOVER_THRESHOLD) return BehaviorClass.HOVER

        // Check for straight-line transit (low heading variance)
        if (headingVariance < TRANSIT_HEADING_VARIANCE && avgSpeed > 0.03f) {
            return BehaviorClass.TRANSIT
        }

        // Check for loitering (heading rotates ~360 degrees)
        val totalHeadingChange = computeTotalHeadingChange(headings)
        if (abs(totalHeadingChange) > 5.0f && avgSpeed > 0.01f) { // >~290 degrees
            return BehaviorClass.LOITERING
        }

        // Check for terminal dive: speed in last 1/3 is much higher than first 2/3
        val splitIdx = motionHistory.size * 2 / 3
        if (splitIdx > 5 && motionHistory.size - splitIdx > 5) {
            val earlySpeed = computeAverageSpeed(motionHistory.subList(0, splitIdx))
            val lateSpeed = computeAverageSpeed(motionHistory.subList(splitIdx, motionHistory.size))
            if (earlySpeed > 0.005f && lateSpeed / earlySpeed > DIVE_SPEED_RATIO) {
                // Also check downward motion (increasing Y in screen coords)
                val earlyY = motionHistory.subList(0, splitIdx).map { it.y }.average()
                val lateY = motionHistory.subList(splitIdx, motionHistory.size).map { it.y }.average()
                if (lateY > earlyY + 0.02f) {
                    return BehaviorClass.TERMINAL_DIVE
                }
            }
        }

        // High heading variance = erratic
        if (headingVariance > 0.5f) return BehaviorClass.ERRATIC

        return BehaviorClass.UNKNOWN
    }

    private fun computeHeadings(history: List<PositionFrame>): List<Float> {
        val headings = mutableListOf<Float>()
        // Sample every 3rd frame to reduce noise
        val step = 3.coerceAtMost(history.size / 5).coerceAtLeast(1)
        for (i in step until history.size step step) {
            val dx = history[i].x - history[i - step].x
            val dy = history[i].y - history[i - step].y
            if (sqrt(dx * dx + dy * dy) > 0.002f) {
                headings.add(atan2(dy, dx))
            }
        }
        return headings
    }

    private fun computeHeadingVariance(headings: List<Float>): Float {
        if (headings.size < 2) return 0f
        val diffs = mutableListOf<Float>()
        for (i in 1 until headings.size) {
            var diff = headings[i] - headings[i - 1]
            while (diff > PI) diff -= (2 * PI).toFloat()
            while (diff < -PI) diff += (2 * PI).toFloat()
            diffs.add(diff * diff)
        }
        return diffs.average().toFloat()
    }

    private fun computeTotalHeadingChange(headings: List<Float>): Float {
        if (headings.size < 2) return 0f
        var total = 0f
        for (i in 1 until headings.size) {
            var diff = headings[i] - headings[i - 1]
            while (diff > PI) diff -= (2 * PI).toFloat()
            while (diff < -PI) diff += (2 * PI).toFloat()
            total += diff
        }
        return total
    }

    private fun computeAverageSpeed(history: List<PositionFrame>): Float {
        if (history.size < 2) return 0f
        var total = 0f
        for (i in 1 until history.size) {
            val dx = history[i].x - history[i - 1].x
            val dy = history[i].y - history[i - 1].y
            val dt = (history[i].timestampMs - history[i - 1].timestampMs).coerceAtLeast(1L)
            total += sqrt(dx * dx + dy * dy) / dt * 1000f
        }
        return total / (history.size - 1)
    }
}

/** Classified flight behavior pattern. */
enum class BehaviorClass(val label: String) {
    /** Straight-line flight, low heading variance — autonomous GPS-guided */
    TRANSIT("Transit"),
    /** Circular flight pattern — surveillance or target acquisition */
    LOITERING("Loitering"),
    /** Sudden speed increase with downward trajectory — attack run */
    TERMINAL_DIVE("Diving"),
    /** Nearly stationary — multirotor hover */
    HOVER("Hover"),
    /** High heading variance — bird or manual FPV */
    ERRATIC("Erratic"),
    /** Insufficient data for classification */
    UNKNOWN("Unknown")
}

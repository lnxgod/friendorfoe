package com.friendorfoe.sensor

import com.friendorfoe.detection.ScoredVisualDetection
import com.friendorfoe.detection.UnknownObjectClassifier
import com.friendorfoe.detection.VisualDetection
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.hypot

/**
 * Matches visual detections (from ML Kit) with radio-projected screen positions
 * and blends them for more accurate label placement.
 *
 * Correlation logic:
 * - For each in-view radio position, finds the nearest visual detection within a threshold
 * - One-to-one matching: each visual detection used at most once
 * - Blends: position = radioPos * 0.4 + visualPos * 0.6 (visual gets priority)
 * - Temporal smoothing: exponential moving average (alpha=0.3) to prevent snapping
 * - When visual match lost: position decays back toward compass-math position
 */
@Singleton
class VisualCorrelationEngine @Inject constructor(
    private val unknownObjectClassifier: UnknownObjectClassifier
) {

    companion object {
        /** Base match threshold — adapted per-object by position confidence */
        private const val MATCH_THRESHOLD_BASE = 0.035f
        /** Additional threshold allowance when data is stale (scales with 1-confidence) */
        private const val MATCH_THRESHOLD_STALE_BONUS = 0.045f

        /** Looser threshold for a visible blob rescuing a just-off-screen radio projection. */
        private const val RESCUE_MATCH_THRESHOLD = 0.45f

        /** Blend weight for radio position (visual = 1 - RADIO_WEIGHT) */
        private const val RADIO_WEIGHT = 0.4f

        /** Exponential moving average factor for temporal smoothing */
        private const val SMOOTHING_ALPHA = 0.3f

        /** Maximum number of in-view radio positions to attempt visual matching against */
        private const val MAX_VISUAL_MATCH_CANDIDATES = 5
    }

    /** Smoothed position history keyed by sky object ID */
    private val smoothedPositions = mutableMapOf<String, Pair<Float, Float>>()

    /**
     * Correlate visual detections with radio-projected positions.
     *
     * @param radioPositions Screen positions computed from compass-math / radio data
     * @param visualDetections Current frame's ML Kit detections
     * @param scoredDetections Scored detections from [SkyObjectFilter] for classification
     * @return Updated screen positions with visual correlation applied
     */
    fun correlate(
        radioPositions: List<ScreenPosition>,
        visualDetections: List<VisualDetection>,
        scoredDetections: List<ScoredVisualDetection> = emptyList()
    ): CorrelationResult {
        if (radioPositions.isEmpty()) return CorrelationResult(
            positions = radioPositions,
            unmatchedVisuals = visualDetections
        )
        if (visualDetections.isEmpty()) {
            // No visual data — decay smoothed positions toward radio
            return CorrelationResult(
                positions = radioPositions.map { pos -> decayToRadio(pos) },
                unmatchedVisuals = emptyList()
            )
        }

        val result = mutableListOf<ScreenPosition>()

        // Sort radio positions by distance to nearest detection (greedy closest-first matching).
        // Near-viewport positions use raw coordinates so visual detections can rescue
        // labels that projection math placed just outside the camera frame.
        val positionsWithMinDist = radioPositions.map { pos ->
            val canMatch = pos.isInView || pos.isNearViewport
            val minDist = if (canMatch) {
                visualDetections.indices.minOfOrNull { idx ->
                    matchDistance(pos, visualDetections[idx])
                } ?: Float.MAX_VALUE
            } else Float.MAX_VALUE
            pos to minDist
        }.sortedBy { it.second }

        // Only attempt visual matching for the closest in-view or near-viewport radio positions.
        val matchCandidates = positionsWithMinDist
            .filter { it.first.isInView || it.first.isNearViewport }
            .take(MAX_VISUAL_MATCH_CANDIDATES)
        val candidateIds = matchCandidates.map { it.first.skyObject.id }.toSet()
        val remainingPositions = positionsWithMinDist.filter { it.first.skyObject.id !in candidateIds }

        // Add remaining positions without visual matching
        for ((radioPos, _) in remainingPositions) {
            result.add(decayToRadio(radioPos))
        }

        // Match only top candidates
        val matchedDetectionIndices = mutableSetOf<Int>()

        for ((radioPos, _) in matchCandidates) {
            // Adaptive threshold: tighter when data is fresh, looser when stale/extrapolated
            val effectiveThreshold = if (radioPos.isInView) {
                MATCH_THRESHOLD_BASE +
                    MATCH_THRESHOLD_STALE_BONUS * (1f - radioPos.positionConfidence) +
                    if (radioPos.trackHeadingDegrees != null && radioPos.isExtrapolated) 0.02f else 0f
            } else {
                RESCUE_MATCH_THRESHOLD +
                    MATCH_THRESHOLD_STALE_BONUS * (1f - radioPos.positionConfidence)
            }

            // Find nearest unmatched visual detection
            var bestIdx = -1
            var bestDist = Float.MAX_VALUE
            for (idx in visualDetections.indices) {
                if (idx in matchedDetectionIndices) continue
                val dist = matchDistance(radioPos, visualDetections[idx])
                if (dist < bestDist) {
                    bestDist = dist
                    bestIdx = idx
                }
            }

            if (bestIdx >= 0 && bestDist <= effectiveThreshold) {
                // Match found — blend radio and visual positions
                matchedDetectionIndices.add(bestIdx)
                val visual = visualDetections[bestIdx]

                if (!radioPos.isInView && radioPos.isNearViewport) {
                    smoothedPositions[radioPos.skyObject.id] = visual.centerX to visual.centerY
                    result.add(radioPos.copy(
                        screenX = visual.centerX.coerceIn(0f, 1f),
                        screenY = visual.centerY.coerceIn(0f, 1f),
                        isInView = true,
                        isNearViewport = false,
                        visuallyConfirmed = true,
                        matchedDetection = visual
                    ))
                    continue
                }

                // Dynamic blend weight: trust radio more when prediction is confident,
                // trust visual more when data is stale
                val dynamicRadioWeight = RADIO_WEIGHT * radioPos.positionConfidence
                val blendedX = radioPos.screenX * dynamicRadioWeight + visual.centerX * (1f - dynamicRadioWeight)
                val blendedY = radioPos.screenY * dynamicRadioWeight + visual.centerY * (1f - dynamicRadioWeight)

                // Apply temporal smoothing
                val (smoothedX, smoothedY) = applySmoothing(
                    radioPos.skyObject.id, blendedX, blendedY
                )

                result.add(radioPos.copy(
                    screenX = smoothedX,
                    screenY = smoothedY,
                    visuallyConfirmed = true,
                    matchedDetection = visual
                ))
            } else {
                // No visual match — decay toward radio position
                result.add(decayToRadio(radioPos))
            }
        }

        val unmatchedVisuals = visualDetections.filterIndexed { idx, _ ->
            idx !in matchedDetectionIndices
        }

        // Classify unmatched visuals for alert escalation
        val classifiedUnknowns = unknownObjectClassifier.classify(unmatchedVisuals, scoredDetections)

        return CorrelationResult(
            positions = result,
            unmatchedVisuals = unmatchedVisuals,
            classifiedUnknowns = classifiedUnknowns
        )
    }

    /**
     * Apply exponential moving average to prevent snapping.
     */
    private fun applySmoothing(
        objectId: String,
        newX: Float,
        newY: Float
    ): Pair<Float, Float> {
        val prev = smoothedPositions[objectId]
        return if (prev != null) {
            val smoothedX = prev.first + SMOOTHING_ALPHA * (newX - prev.first)
            val smoothedY = prev.second + SMOOTHING_ALPHA * (newY - prev.second)
            smoothedPositions[objectId] = smoothedX to smoothedY
            smoothedX to smoothedY
        } else {
            smoothedPositions[objectId] = newX to newY
            newX to newY
        }
    }

    /**
     * When no visual match exists, decay smoothed position back toward radio position.
     */
    private fun decayToRadio(pos: ScreenPosition): ScreenPosition {
        if (!pos.isInView) {
            smoothedPositions.remove(pos.skyObject.id)
            return pos
        }

        val prev = smoothedPositions[pos.skyObject.id]
        return if (prev != null) {
            val decayedX = prev.first + SMOOTHING_ALPHA * (pos.screenX - prev.first)
            val decayedY = prev.second + SMOOTHING_ALPHA * (pos.screenY - prev.second)
            smoothedPositions[pos.skyObject.id] = decayedX to decayedY
            pos.copy(screenX = decayedX, screenY = decayedY)
        } else {
            pos
        }
    }

    /**
     * Clear all correlation history. Call when sensors stop.
     */
    fun reset() {
        smoothedPositions.clear()
        unknownObjectClassifier.reset()
    }

    private fun matchDistance(pos: ScreenPosition, visual: VisualDetection): Float {
        val baseX = if (pos.isInView) pos.screenX else pos.rawScreenX
        val baseY = if (pos.isInView) pos.screenY else pos.rawScreenY
        return hypot(baseX - visual.centerX, baseY - visual.centerY)
    }
}

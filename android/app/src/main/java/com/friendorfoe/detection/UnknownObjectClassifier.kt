package com.friendorfoe.detection

import javax.inject.Inject
import javax.inject.Singleton

/**
 * Classifies unmatched visual detections (no radio correlation) based on
 * temporal persistence and movement behavior.
 *
 * Escalates alert level for persistent unknowns:
 * - >3s tracked → INTEREST
 * - >10s tracked → ALERT
 */
@Singleton
class UnknownObjectClassifier @Inject constructor() {

    /** First-seen timestamps keyed by trackingId */
    private val firstSeen = mutableMapOf<Int, Long>()

    /**
     * Classify unmatched visuals with persistence and behavior heuristics.
     *
     * @param unmatchedVisuals Visual detections that did not match any radio position
     * @param allScored All scored detections from [SkyObjectFilter] (for accessing scores)
     * @return Classified detections with alert levels
     */
    fun classify(
        unmatchedVisuals: List<VisualDetection>,
        allScored: List<ScoredVisualDetection>
    ): List<ClassifiedVisualDetection> {
        val now = System.currentTimeMillis()
        val scoredMap = allScored.associateBy { it.detection.trackingId }

        val results = unmatchedVisuals.mapNotNull { visual ->
            val trackingId = visual.trackingId ?: return@mapNotNull null

            // Track first-seen time
            val firstSeenMs = firstSeen.getOrPut(trackingId) { now }
            val persistenceMs = now - firstSeenMs
            val persistenceSeconds = persistenceMs / 1000f

            // Get scored data if available
            val scored = scoredMap[trackingId]
            val classification = scored?.classification ?: visual.visualClassification
                ?: VisualClassification.UNKNOWN_FLYING

            // Skip static detections
            if (classification == VisualClassification.LIKELY_STATIC) {
                return@mapNotNull null
            }

            // Determine alert level based on persistence
            val alertLevel = when {
                persistenceSeconds >= 10f -> AlertLevel.ALERT
                persistenceSeconds >= 3f -> AlertLevel.INTEREST
                else -> AlertLevel.NORMAL
            }

            ClassifiedVisualDetection(
                detection = visual,
                classification = classification,
                alertLevel = alertLevel,
                persistenceSeconds = persistenceSeconds
            )
        }

        // Prune first-seen entries for trackingIds no longer present
        val activeIds = unmatchedVisuals.mapNotNull { it.trackingId }.toSet()
        firstSeen.keys.retainAll(activeIds)

        return results
    }

    /** Reset all tracking state. */
    fun reset() {
        firstSeen.clear()
    }
}

/** A visual detection with classification and alert level from [UnknownObjectClassifier]. */
data class ClassifiedVisualDetection(
    val detection: VisualDetection,
    val classification: VisualClassification,
    val alertLevel: AlertLevel,
    val persistenceSeconds: Float
)

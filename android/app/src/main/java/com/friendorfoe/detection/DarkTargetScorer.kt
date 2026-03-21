package com.friendorfoe.detection

import javax.inject.Inject
import javax.inject.Singleton

/**
 * Stateless scoring engine for "dark targets" — visual detections with no radio
 * correlation (no ADS-B, no Remote ID, no WiFi).
 *
 * The absence of signal IS the signal. Any airborne object that our radio stack
 * cannot explain gets scored based on visual characteristics, persistence,
 * trajectory behavior, and optional acoustic confirmation.
 *
 * Score thresholds:
 * - >= 40: UNIDENTIFIED AIRCRAFT (yellow)
 * - >= 60: UNIDENTIFIED - NO TRANSPONDER (orange)
 * - >= 80: THREAT WARNING (red pulsing)
 */
@Singleton
class DarkTargetScorer @Inject constructor(
    private val trajectoryClassifier: TrajectoryClassifier,
    private val skyObjectFilter: SkyObjectFilter
) {

    companion object {
        /** Score threshold for basic unidentified alert */
        const val THRESHOLD_UNIDENTIFIED = 40
        /** Score threshold for no-transponder warning */
        const val THRESHOLD_NO_TRANSPONDER = 60
        /** Score threshold for threat warning */
        const val THRESHOLD_THREAT = 80
    }

    /**
     * Score a classified visual detection that has NO radio correlation.
     *
     * @param classified The classified unknown visual detection
     * @param isDarkMode True if nighttime (strobe detection active)
     * @param acousticResult Latest acoustic analysis result (optional)
     * @return Dark target score and threat level
     */
    fun score(
        classified: ClassifiedVisualDetection,
        isDarkMode: Boolean = false,
        acousticResult: AcousticResult = AcousticResult.NONE
    ): DarkTargetScore {
        var score = 0

        val detection = classified.detection
        val persistenceSeconds = classified.persistenceSeconds

        // --- Factor 1: No radio correlation over time ---
        if (persistenceSeconds >= 3f) score += 30   // Flying with no transponder for 3s
        if (persistenceSeconds >= 10f) score += 25  // Persistent unidentified for 10s+

        // --- Factor 2: Shape classification ---
        when (classified.shapeClass) {
            ShapeClass.DELTA_WING -> score += 25    // Delta-wing is highly unusual civilian
            ShapeClass.FIXED_WING -> score += 15    // Fixed-wing threats (Shahed, Lancet)
            ShapeClass.QUADCOPTER,
            ShapeClass.MULTIROTOR -> score += 5     // Consumer drone likely
            ShapeClass.HELICOPTER -> score += 10    // Could be anything
            ShapeClass.INDETERMINATE -> score += 0
        }

        // --- Factor 3: Motion characteristics ---
        val trackingId = detection.trackingId
        if (trackingId != null) {
            val history = skyObjectFilter.getMotionHistory(trackingId)

            // Speed: fast objects are more concerning
            if (detection.motionScore > 0.7f) score += 10

            // Trajectory behavior
            val behavior = trajectoryClassifier.classify(history)
            when (behavior) {
                BehaviorClass.TRANSIT -> score += 15        // Autonomous GPS flight
                BehaviorClass.LOITERING -> score += 20      // Surveillance pattern
                BehaviorClass.TERMINAL_DIVE -> score += 30  // IMMEDIATE THREAT
                BehaviorClass.HOVER -> score += 0           // Likely consumer drone
                BehaviorClass.ERRATIC -> score -= 5         // Likely bird or FPV
                BehaviorClass.UNKNOWN -> score += 0
            }
        }

        // --- Factor 4: Size (small bounding box = distant or small) ---
        val area = detection.width * detection.height
        if (area < 0.005f) score += 5

        // --- Factor 5: Low in frame (below horizon line approach) ---
        if (detection.centerY > 0.6f && detection.centerY < 0.85f) score += 5

        // --- Factor 6: Nighttime with no strobe ---
        if (isDarkMode && !detection.strobeConfirmed) score += 10

        // --- Factor 7: Acoustic confirmation ---
        if (acousticResult.engineType != EngineType.NONE && acousticResult.confidence > 0.3f) {
            when (acousticResult.engineType) {
                EngineType.PISTON -> score += 20       // Piston engine = Shahed-like
                EngineType.ELECTRIC_MOTOR -> score += 10 // Electric = consumer drone or Lancet
                EngineType.TURBINE -> score -= 10      // Turbine = commercial aircraft (less threat)
                EngineType.NONE -> {}
            }
        }

        val level = when {
            score >= THRESHOLD_THREAT -> ThreatLevel.THREAT
            score >= THRESHOLD_NO_TRANSPONDER -> ThreatLevel.NO_TRANSPONDER
            score >= THRESHOLD_UNIDENTIFIED -> ThreatLevel.UNIDENTIFIED
            else -> ThreatLevel.NONE
        }

        return DarkTargetScore(
            score = score.coerceIn(0, 100),
            level = level,
            shapeClass = classified.shapeClass,
            behaviorClass = if (trackingId != null) {
                trajectoryClassifier.classify(skyObjectFilter.getMotionHistory(trackingId))
            } else BehaviorClass.UNKNOWN,
            acousticType = acousticResult.engineType,
            persistenceSeconds = persistenceSeconds
        )
    }
}

/** Threat escalation level for dark targets. */
enum class ThreatLevel(val label: String) {
    NONE(""),
    /** Score >= 40: Basic unidentified aircraft */
    UNIDENTIFIED("UNIDENTIFIED AIRCRAFT"),
    /** Score >= 60: No transponder warning */
    NO_TRANSPONDER("UNIDENTIFIED - NO TRANSPONDER"),
    /** Score >= 80: Active threat warning */
    THREAT("THREAT WARNING")
}

/** Scored dark target with all contributing factors. */
data class DarkTargetScore(
    val score: Int,
    val level: ThreatLevel,
    val shapeClass: ShapeClass,
    val behaviorClass: BehaviorClass,
    val acousticType: EngineType,
    val persistenceSeconds: Float
)

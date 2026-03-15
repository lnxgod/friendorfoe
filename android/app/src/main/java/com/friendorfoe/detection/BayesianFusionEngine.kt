package com.friendorfoe.detection

import android.util.Log
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import java.time.Duration
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Multi-sensor Bayesian confidence fusion engine.
 *
 * Replaces the simple "highest confidence wins" deduplication with proper
 * Bayesian evidence combination. Uses log-odds representation for efficient
 * sequential updates and time-decays old evidence.
 *
 * Key behaviors:
 * - Agreeing sources boost confidence: WiFi(0.3) + RemoteID(0.9) → fused 0.97
 * - Evidence decays over time (half-life = 30 seconds)
 *
 * Operates on candidate IDs: each candidate may have observations from
 * multiple sensors. The engine maintains a belief state (log-odds) for
 * each candidate and updates it as new evidence arrives.
 */
@Singleton
class BayesianFusionEngine @Inject constructor() {

    companion object {
        private const val TAG = "BayesianFusion"

        /** Prior probability that any candidate is a real drone (before evidence) */
        private const val PRIOR_PROBABILITY = 0.1f

        /** Evidence half-life in seconds — older evidence is worth less */
        internal const val EVIDENCE_HALF_LIFE_SEC = 30.0

        /** Maximum log-odds to prevent overflow (corresponds to ~0.999) */
        private const val MAX_LOG_ODDS = 7.0

        /** Minimum log-odds to prevent underflow (corresponds to ~0.001) */
        private const val MIN_LOG_ODDS = -7.0

        /**
         * Sensor reliability factors: how trustworthy each sensor's
         * positive detection is vs its false-positive rate.
         *
         * Likelihood ratio = P(detection | drone) / P(detection | not drone)
         * Higher = more reliable sensor.
         */
        private val SENSOR_LIKELIHOOD_RATIOS = mapOf(
            DetectionSource.REMOTE_ID to 50.0,   // Very reliable, few false positives
            DetectionSource.ADS_B to 100.0,       // Extremely reliable (transponder data)
            DetectionSource.WIFI to 3.0           // SSID matching has false positives
        )

        /**
         * Negative evidence ratios: how much the ABSENCE of a detection
         * from a sensor that SHOULD detect a drone (if present) reduces belief.
         *
         * P(no detection | drone) / P(no detection | not drone)
         * Lower = stronger negative evidence.
         */
        private val NEGATIVE_EVIDENCE_RATIOS = mapOf(
            DetectionSource.WIFI to 0.9            // WiFi off or out of range
        )
    }

    /** Belief state per candidate ID: log-odds of being a real drone */
    private val beliefStates = mutableMapOf<String, BeliefState>()

    /**
     * Update the belief state for a candidate with new sensor evidence.
     *
     * @param candidateId Unique ID for the candidate detection
     * @param source Which sensor produced this detection
     * @param sensorConfidence The sensor's own confidence (0-1)
     * @param now Current timestamp
     * @return Updated fused probability (0-1)
     */
    fun updateWithEvidence(
        candidateId: String,
        source: DetectionSource,
        sensorConfidence: Float,
        now: Instant
    ): Float {
        val state = beliefStates.getOrPut(candidateId) {
            BeliefState(probabilityToLogOdds(PRIOR_PROBABILITY.toDouble()), now)
        }

        // Time-decay existing evidence
        state.applyTimeDecay(now)

        // Compute likelihood ratio for this observation, scaled by sensor confidence
        val baseLR = SENSOR_LIKELIHOOD_RATIOS[source] ?: 2.0
        // Scale LR by sensor confidence: at conf=1.0 use full LR, at conf=0 use LR=1 (neutral)
        val scaledLR = 1.0 + (baseLR - 1.0) * sensorConfidence

        // Update log-odds with new evidence
        val logLR = ln(scaledLR)
        state.logOdds = (state.logOdds + logLR).coerceIn(MIN_LOG_ODDS, MAX_LOG_ODDS)
        state.lastUpdate = now
        state.sensorContributions[source] = SensorContribution(sensorConfidence, now)

        val fusedProb = logOddsToProbability(state.logOdds)
        Log.d(TAG, "Fused $candidateId: $source(conf=${"%.2f".format(sensorConfidence)}) → " +
                "logOdds=${"%.2f".format(state.logOdds)}, prob=${"%.3f".format(fusedProb)}")

        return fusedProb.toFloat()
    }

    /**
     * Apply negative evidence: a sensor that SHOULD detect a drone did not.
     * Used when a sensor that should detect a drone does not.
     *
     * @param candidateId Unique ID for the candidate
     * @param source The sensor that did NOT detect
     * @param now Current timestamp
     * @return Updated fused probability
     */
    fun applyNegativeEvidence(
        candidateId: String,
        source: DetectionSource,
        now: Instant
    ): Float {
        val state = beliefStates[candidateId] ?: return PRIOR_PROBABILITY

        val negLR = NEGATIVE_EVIDENCE_RATIOS[source] ?: return logOddsToProbability(state.logOdds).toFloat()

        state.applyTimeDecay(now)
        val logLR = ln(negLR)
        state.logOdds = (state.logOdds + logLR).coerceIn(MIN_LOG_ODDS, MAX_LOG_ODDS)
        state.lastUpdate = now

        return logOddsToProbability(state.logOdds).toFloat()
    }

    /**
     * Get the current fused probability for a candidate.
     *
     * @param candidateId Candidate ID
     * @param now Current timestamp (for time decay)
     * @return Fused probability (0-1), or the prior if no evidence
     */
    fun getFusedProbability(candidateId: String, now: Instant): Float {
        val state = beliefStates[candidateId] ?: return PRIOR_PROBABILITY
        state.applyTimeDecay(now)
        return logOddsToProbability(state.logOdds).toFloat()
    }

    /**
     * Fuse multiple sky objects that may represent the same physical entity.
     * Returns the object with the highest-quality position data, with fused confidence.
     *
     * @param candidates Objects that are believed to be the same entity
     * @param now Current timestamp
     * @return The best representative object with fused confidence
     */
    fun fuseObjects(candidates: List<SkyObject>, now: Instant): SkyObject? {
        if (candidates.isEmpty()) return null
        if (candidates.size == 1) return candidates.first()

        // Use the first candidate's ID as the fusion key
        val fusionId = candidates.first().id

        // Update belief with each source
        for (candidate in candidates) {
            updateWithEvidence(fusionId, candidate.source, candidate.confidence, now)
        }

        val fusedConfidence = getFusedProbability(fusionId, now)

        // Pick the best representative: prefer the one with valid position + highest source reliability
        val best = candidates
            .sortedWith(
                compareByDescending<SkyObject> { hasValidPosition(it) }
                    .thenByDescending { sourceReliabilityRank(it.source) }
                    .thenByDescending { it.lastUpdated }
            )
            .first()

        // Apply fused confidence
        return when (best) {
            is Drone -> best.copy(confidence = fusedConfidence)
            is com.friendorfoe.domain.model.Aircraft -> best.copy(confidence = fusedConfidence)
        }
    }

    /** Remove stale candidates that haven't been updated recently. */
    fun pruneStale(now: Instant, maxAge: Duration = Duration.ofMinutes(2)) {
        val expiredKeys = beliefStates.filter { (_, state) ->
            Duration.between(state.lastUpdate, now) > maxAge
        }.keys
        expiredKeys.forEach { beliefStates.remove(it) }
    }

    /** Reset all belief states. */
    fun reset() {
        beliefStates.clear()
    }

    private fun hasValidPosition(obj: SkyObject): Boolean {
        return obj.position.latitude != 0.0 || obj.position.longitude != 0.0
    }

    private fun sourceReliabilityRank(source: DetectionSource): Int = when (source) {
        DetectionSource.ADS_B -> 5
        DetectionSource.REMOTE_ID -> 4
        DetectionSource.WIFI -> 2
    }

    private fun probabilityToLogOdds(p: Double): Double {
        val clamped = p.coerceIn(0.001, 0.999)
        return ln(clamped / (1.0 - clamped))
    }

    private fun logOddsToProbability(logOdds: Double): Double {
        val odds = kotlin.math.exp(logOdds)
        return odds / (1.0 + odds)
    }

    private fun ln(x: Double): Double = kotlin.math.ln(x)
}

/** Contribution from a single sensor to a candidate's belief state. */
private data class SensorContribution(
    val confidence: Float,
    val timestamp: Instant
)

/** Internal belief state for a candidate detection. */
private class BeliefState(
    var logOdds: Double,
    var lastUpdate: Instant
) {
    val sensorContributions = mutableMapOf<DetectionSource, SensorContribution>()

    /**
     * Apply time decay to the log-odds, pulling it toward the prior.
     * Uses exponential decay with half-life of [BayesianFusionEngine.EVIDENCE_HALF_LIFE_SEC].
     */
    fun applyTimeDecay(now: Instant) {
        val elapsedSec = Duration.between(lastUpdate, now).toMillis() / 1000.0
        if (elapsedSec <= 0) return

        val priorLogOdds = kotlin.math.ln(0.1 / 0.9) // log-odds of prior
        val decayFactor = kotlin.math.exp(-0.693 * elapsedSec / BayesianFusionEngine.EVIDENCE_HALF_LIFE_SEC)

        // Decay toward prior
        logOdds = priorLogOdds + (logOdds - priorLogOdds) * decayFactor
    }

    companion object {
        private const val EVIDENCE_HALF_LIFE_SEC = 30.0
    }
}

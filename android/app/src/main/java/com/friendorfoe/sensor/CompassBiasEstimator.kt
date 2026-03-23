package com.friendorfoe.sensor

/**
 * Estimates systematic compass azimuth bias by comparing radio-projected positions
 * with visually confirmed positions across multiple aircraft.
 *
 * When 2+ aircraft have both radio projections and ML Kit visual matches, the
 * systematic horizontal offset between radio and visual positions reveals compass drift.
 * A slow P-controller corrects this bias over time.
 *
 * Requirements for a valid observation:
 * - At least [MIN_MATCHES] visually confirmed matches
 * - Matches must be within inner 80% of FOV (not edge, where distortion is highest)
 * - Data must be fresh (positionConfidence > 0.5)
 */
class CompassBiasEstimator {

    companion object {
        /** Minimum visually confirmed matches needed to estimate bias */
        private const val MIN_MATCHES = 3

        /** Maximum bias correction in degrees (prevents runaway) */
        private const val MAX_BIAS_DEG = 15f

        /** Only use matches within this fraction of the FOV from center (inner 80%) */
        private const val FOV_INNER_FRACTION = 0.4f
    }

    /** Current estimated compass bias in degrees. Add this to raw azimuth. */
    var biasDegrees: Float = 0f
        private set

    /**
     * Observe correlation results and update bias estimate.
     *
     * @param positions Screen positions from the current frame (after visual correlation)
     * @param horizontalFovDegrees Camera horizontal FOV in degrees
     */
    fun observe(positions: List<ScreenPosition>, horizontalFovDegrees: Double) {
        // Collect azimuth residuals from visually confirmed, high-confidence, in-view matches
        val residuals = mutableListOf<Float>()

        for (pos in positions) {
            if (!pos.visuallyConfirmed || !pos.isInView) continue
            if (pos.positionConfidence < 0.5f) continue

            val matchedVisual = pos.matchedDetection ?: continue

            // Only use matches near center of FOV (inner 80%)
            val distFromCenter = kotlin.math.abs(pos.screenX - 0.5f)
            if (distFromCenter > FOV_INNER_FRACTION) continue

            // Convert screen-space horizontal offset (radio vs visual) to degrees
            // screenX is normalized 0-1, so delta * hFOV gives degrees
            val radioScreenX = pos.screenX
            val visualScreenX = matchedVisual.centerX
            val offsetDeg = ((radioScreenX - visualScreenX) * horizontalFovDegrees).toFloat()

            residuals.add(offsetDeg)
        }

        if (residuals.size < MIN_MATCHES) return

        // Robust median of residuals (less sensitive to outliers than mean)
        residuals.sort()
        val median = if (residuals.size % 2 == 0) {
            (residuals[residuals.size / 2 - 1] + residuals[residuals.size / 2]) / 2f
        } else {
            residuals[residuals.size / 2]
        }

        // Adaptive P-controller: faster convergence when more matches available
        val adaptiveGain = 0.05f + 0.15f * (residuals.size / 5f).coerceAtMost(1f)
        biasDegrees = (biasDegrees - median * adaptiveGain).coerceIn(-MAX_BIAS_DEG, MAX_BIAS_DEG)
    }

    fun reset() {
        biasDegrees = 0f
    }
}

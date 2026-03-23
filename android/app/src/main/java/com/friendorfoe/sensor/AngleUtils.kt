package com.friendorfoe.sensor

/**
 * Shared angle math utilities used by SkyPositionMapper, SensorFusionEngine,
 * TrajectoryPredictor, and CompassBiasEstimator.
 */
object AngleUtils {

    /**
     * Normalize an angle difference to the range -180 to 180 degrees.
     *
     * Correctly handles the 360/0 degree boundary. For example,
     * if the camera points at 350 degrees and the object is at 10 degrees,
     * the difference should be +20, not -340.
     */
    fun normalizeAngleDifference(diff: Float): Float {
        var result = diff % 360f
        if (result > 180f) result -= 360f
        if (result < -180f) result += 360f
        return result
    }

    /**
     * Normalize an angle to the 0-360 degree range.
     */
    fun normalizeAngle360(degrees: Float): Float {
        var result = degrees % 360f
        if (result < 0f) result += 360f
        return result
    }

    /**
     * Circular linear interpolation for angular values (handles 0/360 wrap).
     * Interpolates from [current] toward [target] by [alpha] fraction.
     */
    fun circularLerp(current: Float, target: Float, alpha: Float): Float {
        var diff = target - current
        if (diff > 180f) diff -= 360f
        if (diff < -180f) diff += 360f
        var result = current + alpha * diff
        if (result < 0f) result += 360f
        if (result >= 360f) result -= 360f
        return result
    }
}

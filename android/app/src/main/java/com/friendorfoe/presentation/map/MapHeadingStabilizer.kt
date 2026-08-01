package com.friendorfoe.presentation.map

import com.friendorfoe.sensor.AngleUtils
import kotlin.math.abs
import kotlin.math.exp

class MapHeadingStabilizer {

    private var headingDegrees: Float? = null
    private var lastOutputMs: Long? = null

    fun update(inputDegrees: Float, nowMs: Long): Float? {
        if (!inputDegrees.isFinite()) return null

        val normalizedInput = AngleUtils.normalizeAngle360(inputDegrees)
        val currentHeading = headingDegrees
        if (currentHeading == null) {
            headingDegrees = normalizedInput
            lastOutputMs = nowMs
            return normalizedInput
        }

        val elapsedMs = (nowMs - (lastOutputMs ?: nowMs)).coerceAtLeast(0L)
        if (elapsedMs < MIN_OUTPUT_INTERVAL_MS) return null

        val delta = AngleUtils.normalizeAngleDifference(normalizedInput - currentHeading)
        if (abs(delta) < MIN_RESIDUAL_DELTA_DEGREES) return null

        val alpha = 1.0 - exp(-elapsedMs.toDouble() / TIME_CONSTANT_MS)
        val nextHeading = AngleUtils.normalizeAngle360(
            currentHeading + delta * alpha.toFloat()
        )
        headingDegrees = nextHeading
        lastOutputMs = nowMs
        return nextHeading
    }

    fun reset() {
        headingDegrees = null
        lastOutputMs = null
    }

    private companion object {
        const val MIN_OUTPUT_INTERVAL_MS = 100L
        const val MIN_RESIDUAL_DELTA_DEGREES = 3f
        const val TIME_CONSTANT_MS = 250.0
    }
}

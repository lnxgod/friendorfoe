package com.friendorfoe.sensor

import android.hardware.camera2.CameraCharacteristics
import android.util.SizeF
import kotlin.math.atan

/**
 * Calculates camera field of view (FOV) from camera hardware characteristics.
 *
 * Uses the camera sensor physical size and focal length to compute
 * the horizontal and vertical FOV angles. These angles define the
 * cone of sky that is visible in the camera viewfinder, which is needed
 * to determine whether a given sky object should be drawn on screen.
 *
 * FOV formula: 2 * atan(sensorDimension / (2 * focalLength))
 *
 * Typical smartphone FOV values:
 * - Horizontal: ~60-70 degrees (wide angle) or ~25-35 degrees (telephoto)
 * - Vertical: ~45-55 degrees (wide angle) or ~18-25 degrees (telephoto)
 */
class CameraFovCalculator {

    /** Horizontal field of view in radians (default for portrait: narrower) */
    var horizontalFovRadians: Double = Math.toRadians(45.0)
        private set

    /** Vertical field of view in radians (default for portrait: wider) */
    var verticalFovRadians: Double = Math.toRadians(60.0)
        private set

    /** Whether portrait swap has been applied */
    private var portraitSwapped = false

    /** Horizontal FOV in degrees */
    val horizontalFovDegrees: Double get() = Math.toDegrees(horizontalFovRadians)

    /** Vertical FOV in degrees */
    val verticalFovDegrees: Double get() = Math.toDegrees(verticalFovRadians)

    /**
     * Calculate FOV from [CameraCharacteristics].
     *
     * Extracts the physical sensor size and available focal lengths from the
     * camera characteristics, then computes FOV using the widest focal length
     * (shortest focal length = widest field of view).
     *
     * @param characteristics Camera characteristics from CameraManager
     * @return true if FOV was successfully calculated, false if required data was missing
     */
    fun calculateFromCharacteristics(characteristics: CameraCharacteristics): Boolean {
        val sensorSize = characteristics.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
            ?: return false
        val focalLengths = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
            ?: return false

        if (focalLengths.isEmpty()) return false

        // Use the shortest focal length (widest FOV), which is typically the default lens
        val focalLength = focalLengths.min()

        calculateFromFocalLengthAndSensorSize(focalLength, sensorSize)
        return true
    }

    /**
     * Calculate FOV from explicit focal length and sensor dimensions.
     *
     * @param focalLengthMm Focal length in millimeters
     * @param sensorSize Physical sensor size (width x height) in millimeters
     */
    fun calculateFromFocalLengthAndSensorSize(focalLengthMm: Float, sensorSize: SizeF) {
        horizontalFovRadians = 2.0 * atan(sensorSize.width / (2.0 * focalLengthMm))
        verticalFovRadians = 2.0 * atan(sensorSize.height / (2.0 * focalLengthMm))
    }

    /**
     * Calculate FOV from explicit focal length and sensor width/height.
     *
     * @param focalLengthMm Focal length in millimeters
     * @param sensorWidthMm Sensor physical width in millimeters
     * @param sensorHeightMm Sensor physical height in millimeters
     */
    fun calculateFromFocalLengthAndSensorSize(
        focalLengthMm: Float,
        sensorWidthMm: Float,
        sensorHeightMm: Float
    ) {
        horizontalFovRadians = 2.0 * atan(sensorWidthMm / (2.0 * focalLengthMm))
        verticalFovRadians = 2.0 * atan(sensorHeightMm / (2.0 * focalLengthMm))
    }

    /**
     * Swap horizontal and vertical FOV for portrait mode.
     *
     * Camera sensors report FOV in landscape orientation (wider = horizontal).
     * When the app is locked to portrait, the narrower sensor dimension maps
     * to screen horizontal and the wider to screen vertical. This method
     * swaps the values so label positioning matches what the user sees.
     *
     * Safe to call multiple times; only swaps once.
     */
    fun swapForPortrait() {
        if (portraitSwapped) return
        val tmp = horizontalFovRadians
        horizontalFovRadians = verticalFovRadians
        verticalFovRadians = tmp
        portraitSwapped = true
    }

    /**
     * Check if an offset from the camera center is within the field of view.
     *
     * @param azimuthOffsetRadians Horizontal offset from camera center in radians
     * @param elevationOffsetRadians Vertical offset from camera center in radians
     * @return true if the point is within the FOV
     */
    fun isInFieldOfView(azimuthOffsetRadians: Double, elevationOffsetRadians: Double): Boolean {
        val halfHFov = horizontalFovRadians / 2.0
        val halfVFov = verticalFovRadians / 2.0
        return azimuthOffsetRadians in -halfHFov..halfHFov &&
            elevationOffsetRadians in -halfVFov..halfVFov
    }

    /**
     * Get the visible azimuth range given the current device orientation.
     *
     * @param centerAzimuthDegrees The azimuth the camera is pointing at (0-360)
     * @return Pair of (minAzimuth, maxAzimuth) in degrees. May wrap around 360/0 boundary.
     */
    fun getVisibleAzimuthRange(centerAzimuthDegrees: Double): Pair<Double, Double> {
        val halfFovDeg = horizontalFovDegrees / 2.0
        return Pair(
            normalizeAngle(centerAzimuthDegrees - halfFovDeg),
            normalizeAngle(centerAzimuthDegrees + halfFovDeg)
        )
    }

    /**
     * Get the visible elevation range given the current device orientation.
     *
     * @param centerElevationDegrees The elevation the camera is pointing at
     * @return Pair of (minElevation, maxElevation) in degrees, clamped to -90..90
     */
    fun getVisibleElevationRange(centerElevationDegrees: Double): Pair<Double, Double> {
        val halfFovDeg = verticalFovDegrees / 2.0
        return Pair(
            (centerElevationDegrees - halfFovDeg).coerceIn(-90.0, 90.0),
            (centerElevationDegrees + halfFovDeg).coerceIn(-90.0, 90.0)
        )
    }

    /**
     * Normalize an angle to the 0-360 degree range.
     */
    private fun normalizeAngle(degrees: Double): Double {
        var result = degrees % 360.0
        if (result < 0) result += 360.0
        return result
    }
}

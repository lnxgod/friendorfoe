package com.friendorfoe.sensor

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Maps sky object geographic positions to screen coordinates.
 *
 * Given the user's GPS position, a list of [SkyObject]s, the current device
 * orientation, and camera FOV parameters, this class:
 * 1. Calculates the bearing (azimuth) from user to each object
 * 2. Calculates the elevation angle from user to each object
 * 3. Determines whether each object falls within the camera's field of view
 * 4. Maps in-view objects to normalized screen coordinates (0.0 to 1.0)
 *
 * Math:
 * - Bearing uses the forward azimuth formula from spherical trigonometry
 * - Elevation uses atan2(altitude_difference, ground_distance)
 * - Ground distance uses the haversine formula
 * - Screen projection is a linear mapping of angular offset to screen position
 */
class SkyPositionMapper {

    companion object {
        /** Mean Earth radius in meters (WGS84 approximation) */
        private const val EARTH_RADIUS_METERS = 6_371_000.0

        /** Max visual range for aircraft: ~7 nm (13 km) — beyond this, aircraft are specks */
        private const val MAX_AIRCRAFT_VISUAL_RANGE_M = 13_000.0

        /** Max visual range for drones: ~2 km — small objects invisible further */
        private const val MAX_DRONE_VISUAL_RANGE_M = 2_000.0
    }

    /**
     * Map a list of sky objects to screen positions.
     *
     * @param userPosition The user's current GPS position
     * @param skyObjects List of detected sky objects to project
     * @param orientation Current device orientation from [SensorFusionEngine]
     * @param fovCalculator Camera FOV calculator with current FOV values
     * @return List of [ScreenPosition] for each sky object, with isInView indicating visibility
     */
    fun mapToScreen(
        userPosition: Position,
        skyObjects: List<SkyObject>,
        orientation: DeviceOrientation,
        fovCalculator: CameraFovCalculator
    ): List<ScreenPosition> {
        return skyObjects.map { skyObject ->
            mapSingleObject(userPosition, skyObject, orientation, fovCalculator)
        }
    }

    /**
     * Map a single sky object to a screen position.
     */
    private fun mapSingleObject(
        userPosition: Position,
        skyObject: SkyObject,
        orientation: DeviceOrientation,
        fovCalculator: CameraFovCalculator
    ): ScreenPosition {
        val objectPosition = skyObject.position

        // Step 1: Calculate bearing from user to object
        val bearingDeg = calculateBearing(
            userPosition.latitude, userPosition.longitude,
            objectPosition.latitude, objectPosition.longitude
        )

        // Step 2: Calculate ground distance (haversine)
        val groundDistanceMeters = calculateHaversineDistance(
            userPosition.latitude, userPosition.longitude,
            objectPosition.latitude, objectPosition.longitude
        )

        // Step 3: Calculate elevation angle
        val altitudeDiff = objectPosition.altitudeMeters - userPosition.altitudeMeters
        val elevationDeg = calculateElevationAngle(altitudeDiff, groundDistanceMeters)

        // Step 4: Calculate angular offsets from camera center
        val azimuthOffsetDeg = normalizeAngleDifference(bearingDeg - orientation.azimuthDegrees)
        val elevationOffsetDeg = elevationDeg - orientation.pitchDegrees

        // Convert offsets to radians for FOV check
        val azimuthOffsetRad = Math.toRadians(azimuthOffsetDeg.toDouble())
        val elevationOffsetRad = Math.toRadians(elevationOffsetDeg.toDouble())

        // Step 5: Check if within FOV and within visual range
        val slantDistance = calculateSlantDistance(groundDistanceMeters, altitudeDiff)
        val maxVisualRange = when (skyObject) {
            is Aircraft -> MAX_AIRCRAFT_VISUAL_RANGE_M
            is Drone -> MAX_DRONE_VISUAL_RANGE_M
        }
        val withinRange = slantDistance <= maxVisualRange
        val aboveHorizon = elevationDeg > -2f  // Must be at or above horizon (allow -2° for Earth curvature)
        val isInView = fovCalculator.isInFieldOfView(azimuthOffsetRad, elevationOffsetRad) && withinRange && aboveHorizon

        // Step 6: Map to normalized screen coordinates (0.0 to 1.0)
        // Screen X: center is 0.5, left is 0.0, right is 1.0
        // Screen Y: center is 0.5, top is 0.0, bottom is 1.0
        val halfHFovRad = fovCalculator.horizontalFovRadians / 2.0
        val halfVFovRad = fovCalculator.verticalFovRadians / 2.0

        // Normalized position within FOV: -1.0 to 1.0 maps to 0.0 to 1.0
        val screenX = (0.5 + azimuthOffsetRad / (2.0 * halfHFovRad)).toFloat()
        // Elevation is inverted: higher elevation = lower screen Y (toward top)
        val screenY = (0.5 - elevationOffsetRad / (2.0 * halfVFovRad)).toFloat()

        return ScreenPosition(
            skyObject = skyObject,
            screenX = screenX.coerceIn(0f, 1f),
            screenY = screenY.coerceIn(0f, 1f),
            isInView = isInView,
            bearingDegrees = normalizeAngle360(bearingDeg),
            elevationDegrees = elevationDeg,
            distanceMeters = slantDistance,
            groundDistanceMeters = groundDistanceMeters
        )
    }

    /**
     * Calculate the initial bearing (forward azimuth) from point 1 to point 2.
     *
     * Uses the spherical trigonometry formula:
     * bearing = atan2(sin(dlon)*cos(lat2), cos(lat1)*sin(lat2) - sin(lat1)*cos(lat2)*cos(dlon))
     *
     * @return Bearing in degrees (0-360, clockwise from north)
     */
    fun calculateBearing(
        lat1Deg: Double, lon1Deg: Double,
        lat2Deg: Double, lon2Deg: Double
    ): Float {
        val lat1 = Math.toRadians(lat1Deg)
        val lat2 = Math.toRadians(lat2Deg)
        val dLon = Math.toRadians(lon2Deg - lon1Deg)

        val y = sin(dLon) * cos(lat2)
        val x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon)

        val bearingRad = atan2(y, x)
        var bearingDeg = Math.toDegrees(bearingRad).toFloat()

        // Normalize to 0-360
        if (bearingDeg < 0) bearingDeg += 360f
        return bearingDeg
    }

    /**
     * Calculate the great-circle distance between two points using the haversine formula.
     *
     * @return Distance in meters
     */
    fun calculateHaversineDistance(
        lat1Deg: Double, lon1Deg: Double,
        lat2Deg: Double, lon2Deg: Double
    ): Double {
        val lat1 = Math.toRadians(lat1Deg)
        val lat2 = Math.toRadians(lat2Deg)
        val dLat = Math.toRadians(lat2Deg - lat1Deg)
        val dLon = Math.toRadians(lon2Deg - lon1Deg)

        val a = sin(dLat / 2) * sin(dLat / 2) +
            cos(lat1) * cos(lat2) *
            sin(dLon / 2) * sin(dLon / 2)

        val c = 2 * atan2(sqrt(a), sqrt(1 - a))

        return EARTH_RADIUS_METERS * c
    }

    /**
     * Calculate the elevation angle from the user to a sky object.
     *
     * @param altitudeDiffMeters Altitude difference (object altitude - user altitude) in meters
     * @param groundDistanceMeters Horizontal ground distance in meters
     * @return Elevation angle in degrees (positive = above horizon, negative = below)
     */
    fun calculateElevationAngle(altitudeDiffMeters: Double, groundDistanceMeters: Double): Float {
        if (groundDistanceMeters == 0.0 && altitudeDiffMeters == 0.0) return 0f
        return Math.toDegrees(atan2(altitudeDiffMeters, groundDistanceMeters)).toFloat()
    }

    /**
     * Calculate the slant (line-of-sight) distance from user to object,
     * accounting for both ground distance and altitude difference.
     *
     * @return Slant distance in meters
     */
    private fun calculateSlantDistance(groundDistanceMeters: Double, altitudeDiffMeters: Double): Double {
        return sqrt(
            groundDistanceMeters * groundDistanceMeters +
                altitudeDiffMeters * altitudeDiffMeters
        )
    }

    /**
     * Normalize an angle difference to the range -180 to 180 degrees.
     *
     * This correctly handles the 360/0 degree boundary. For example,
     * if the camera points at 350 degrees and the object is at 10 degrees,
     * the difference should be +20, not -340.
     */
    private fun normalizeAngleDifference(diff: Float): Float {
        var result = diff % 360f
        if (result > 180f) result -= 360f
        if (result < -180f) result += 360f
        return result
    }

    /**
     * Normalize an angle to the 0-360 degree range.
     */
    private fun normalizeAngle360(degrees: Float): Float {
        var result = degrees % 360f
        if (result < 0f) result += 360f
        return result
    }
}

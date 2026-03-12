package com.friendorfoe.sensor

/**
 * Represents the current orientation of the device in 3D space.
 *
 * All angles are in degrees for external consumption.
 * Internally the sensor fusion engine works in radians.
 *
 * @property azimuthDegrees Compass heading in degrees (0-360, 0 = north, 90 = east)
 * @property pitchDegrees Elevation angle in degrees (-90 to 90, positive = tilted up toward sky)
 * @property rollDegrees Roll angle in degrees (-180 to 180, 0 = portrait upright)
 */
data class DeviceOrientation(
    val azimuthDegrees: Float = 0f,
    val pitchDegrees: Float = 0f,
    val rollDegrees: Float = 0f
) {
    /** Azimuth in radians */
    val azimuthRadians: Double get() = Math.toRadians(azimuthDegrees.toDouble())

    /** Pitch in radians */
    val pitchRadians: Double get() = Math.toRadians(pitchDegrees.toDouble())

    /** Roll in radians */
    val rollRadians: Double get() = Math.toRadians(rollDegrees.toDouble())
}

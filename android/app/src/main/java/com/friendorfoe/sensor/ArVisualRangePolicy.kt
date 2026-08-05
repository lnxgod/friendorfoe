package com.friendorfoe.sensor

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject

internal object ArVisualRangePolicy {
    private const val METERS_PER_STATUTE_MILE = 1_609.344
    private const val AIRCRAFT_MAX_DISTANCE_METERS = 20.0 * METERS_PER_STATUTE_MILE
    private const val DRONE_MAX_DISTANCE_METERS = 2_000.0

    fun includes(skyObject: SkyObject, distanceMeters: Double): Boolean =
        distanceMeters <= when (skyObject) {
            is Aircraft -> AIRCRAFT_MAX_DISTANCE_METERS
            is Drone -> DRONE_MAX_DISTANCE_METERS
        }
}

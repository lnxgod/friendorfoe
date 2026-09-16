package com.friendorfoe.data.repository

import android.location.Location
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject

internal data class UserLocationFix(
    val latitude: Double,
    val longitude: Double,
    val accuracyMeters: Float,
)

internal fun validatedLocationAccuracyMeters(
    hasAccuracy: Boolean,
    accuracyMeters: Float,
): Float {
    return if (hasAccuracy && accuracyMeters.isFinite() && accuracyMeters >= 0f) {
        accuracyMeters
    } else {
        Float.POSITIVE_INFINITY
    }
}

internal fun Location.validatedLocationAccuracyMeters(): Float =
    validatedLocationAccuracyMeters(hasAccuracy(), accuracy)

internal fun userLocationFixForStart(
    latitude: Double,
    longitude: Double,
    accuracyMeters: Float,
): UserLocationFix? {
    val scannerOnly = latitude == 0.0 &&
        longitude == 0.0 &&
        accuracyMeters == Float.POSITIVE_INFINITY
    return if (scannerOnly) {
        null
    } else {
        UserLocationFix(latitude, longitude, accuracyMeters)
    }
}

internal fun mergeUserLocationFix(
    current: UserLocationFix,
    requested: UserLocationFix?,
): UserLocationFix = requested ?: current

/** Refresh geographic ranges even when a provider supplied a distance at an older origin. */
internal fun refreshObjectDistances(
    objects: List<SkyObject>,
    location: UserLocationFix,
    measure: (UserLocationFix, Position) -> Double = { origin, target ->
        val result = FloatArray(1)
        Location.distanceBetween(
            origin.latitude, origin.longitude, target.latitude, target.longitude, result,
        )
        result[0].toDouble()
    },
): List<SkyObject> {
    if (!validCoordinates(location.latitude, location.longitude) ||
        (location.latitude == 0.0 && location.longitude == 0.0 &&
            !location.accuracyMeters.isFinite())
    ) return objects
    return objects.map { obj ->
        val position = obj.position
        if (validCoordinates(position.latitude, position.longitude) &&
            (position.latitude != 0.0 || position.longitude != 0.0)
        ) {
            val distance = measure(location, position)
            if (distance.isFinite() && distance >= 0.0) obj.copyWithDistance(distance) else obj
        } else {
            obj
        }
    }
}

private fun validCoordinates(latitude: Double, longitude: Double): Boolean =
    latitude.isFinite() && latitude in -90.0..90.0 &&
        longitude.isFinite() && longitude in -180.0..180.0

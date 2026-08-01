package com.friendorfoe.data.repository

import android.location.Location

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

package com.friendorfoe.domain.model

/** Shared range for aircraft alerts and list priority, in statute miles. */
object AircraftRange {
    const val DEFAULT_MILES = 10
    const val MIN_MILES = 1
    const val MAX_MILES = 50
    const val METERS_PER_MILE = 1609.344

    fun normalizeMiles(miles: Int): Int = miles.coerceIn(MIN_MILES, MAX_MILES)

    fun contains(distanceMeters: Double?, miles: Int): Boolean =
        distanceMeters != null && distanceMeters.isFinite() &&
            distanceMeters >= 0.0 && distanceMeters <= normalizeMiles(miles) * METERS_PER_MILE
}

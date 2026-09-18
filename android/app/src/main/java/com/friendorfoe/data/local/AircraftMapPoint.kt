package com.friendorfoe.data.local

import androidx.room.Embedded

/** Recorded position with the most recent saved aircraft identity; never a live detection. */
data class AircraftMapPoint(
    @Embedded val point: TrackingEntity,
    val label: String,
    val category: String,
)

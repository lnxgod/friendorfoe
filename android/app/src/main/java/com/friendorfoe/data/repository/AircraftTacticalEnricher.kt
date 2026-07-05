package com.friendorfoe.data.repository

import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.detection.MilitaryClassifier
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

internal class AircraftTacticalEnricher(
    private val lookupDetail: suspend (Aircraft) -> AircraftDetailDto?
) {

    suspend fun enrichAircraft(
        userLatitude: Double,
        userLongitude: Double,
        aircraft: Aircraft
    ): Aircraft {
        val withDistance = aircraft.withDistanceFrom(userLatitude, userLongitude)
        if (!withDistance.shouldLookupDetail()) return withDistance

        val detail = lookupDetail(withDistance) ?: return withDistance
        val merged = withDistance.withProviderDetail(detail)
        val result = MilitaryClassifier.classify(
            icaoHex = merged.icaoHex,
            callsign = merged.callsign,
            typeCode = merged.aircraftType,
            registration = merged.registration,
            ownerName = detail.operator
        )

        return if (result.category != null) {
            merged.copy(
                category = result.category,
                classificationSignals = (merged.classificationSignals.orEmpty() +
                    result.signals +
                    "ENRICHMENT:HEXDB").distinct()
            )
        } else {
            merged
        }
    }

    suspend fun enrichAircraft(
        userLatitude: Double,
        userLongitude: Double,
        aircraft: List<Aircraft>
    ): List<Aircraft> = aircraft.map { enrichAircraft(userLatitude, userLongitude, it) }

    private fun Aircraft.withDistanceFrom(userLatitude: Double, userLongitude: Double): Aircraft {
        if (distanceMeters != null) return this
        if (position.latitude == 0.0 && position.longitude == 0.0) return this
        return copy(
            distanceMeters = distanceMeters(
                lat1 = userLatitude,
                lon1 = userLongitude,
                lat2 = position.latitude,
                lon2 = position.longitude
            )
        )
    }

    private fun Aircraft.shouldLookupDetail(): Boolean {
        val distance = distanceMeters ?: return false
        if (distance > TACTICAL_ENRICHMENT_RADIUS_METERS) return false
        if (category in alreadySpecificCategories) return false
        return classificationSignals.orEmpty().none { signal ->
            signal.startsWith("AIRLINE:") || signal.startsWith("CALLSIGN:CARGO_")
        }
    }

    private fun Aircraft.withProviderDetail(detail: AircraftDetailDto): Aircraft {
        val detailType = detail.aircraftType?.trim()?.ifBlank { null }
        val detailOperator = detail.operator?.trim()?.ifBlank { null }
        return copy(
            registration = detail.registration?.trim()?.ifBlank { null } ?: registration,
            aircraftType = detailType ?: aircraftType,
            aircraftModel = detail.aircraftDescription?.trim()?.ifBlank { null } ?: aircraftModel,
            operatorName = detailOperator ?: operatorName
        )
    }

    private fun distanceMeters(
        lat1: Double,
        lon1: Double,
        lat2: Double,
        lon2: Double
    ): Double {
        val dLat = Math.toRadians(lat2 - lat1)
        val dLon = Math.toRadians(lon2 - lon1)
        val startLat = Math.toRadians(lat1)
        val endLat = Math.toRadians(lat2)
        val a = sin(dLat / 2) * sin(dLat / 2) +
            cos(startLat) * cos(endLat) * sin(dLon / 2) * sin(dLon / 2)
        val c = 2 * atan2(sqrt(a), sqrt(1 - a))
        return EARTH_RADIUS_METERS * c
    }

    companion object {
        private const val EARTH_RADIUS_METERS = 6_371_000.0
        private const val METERS_PER_MILE = 1609.344
        const val TACTICAL_ENRICHMENT_RADIUS_METERS = 15.0 * METERS_PER_MILE

        private val alreadySpecificCategories = setOf(
            ObjectCategory.DRONE,
            ObjectCategory.EMERGENCY,
            ObjectCategory.GOVERNMENT,
            ObjectCategory.MILITARY
        )
    }
}

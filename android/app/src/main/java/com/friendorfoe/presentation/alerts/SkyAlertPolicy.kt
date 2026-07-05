package com.friendorfoe.presentation.alerts

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject
import kotlin.math.roundToInt

data class SkyAlertSettings(
    val droneAlertsEnabled: Boolean,
    val helicopterAlertsEnabled: Boolean,
    val militaryAlertsEnabled: Boolean,
    val policeAlertsEnabled: Boolean
)

data class SkyAlertCandidate(
    val key: String,
    val title: String,
    val body: String
)

class SkyAlertPolicy(
    private val cooldownMs: Long = 10 * 60 * 1000L
) {
    private val lastNotifiedAt = mutableMapOf<String, Long>()

    fun shouldNotify(
        candidate: SkyAlertCandidate,
        nowMs: Long = System.currentTimeMillis()
    ): Boolean {
        val last = lastNotifiedAt[candidate.key]
        if (last != null && nowMs - last < cooldownMs) return false
        lastNotifiedAt[candidate.key] = nowMs
        return true
    }

    fun reset() {
        lastNotifiedAt.clear()
    }

    companion object {
        private const val METERS_PER_MILE = 1609.344
        const val TACTICAL_ALERT_RADIUS_MILES = 15.0
        private const val TACTICAL_ALERT_RADIUS_METERS =
            TACTICAL_ALERT_RADIUS_MILES * METERS_PER_MILE

        fun candidateFor(
            skyObject: SkyObject,
            settings: SkyAlertSettings
        ): SkyAlertCandidate? = when (skyObject) {
            is Drone -> droneCandidate(skyObject, settings)
            is Aircraft -> aircraftCandidate(skyObject, settings)
        }

        private fun droneCandidate(
            drone: Drone,
            settings: SkyAlertSettings
        ): SkyAlertCandidate? {
            if (!settings.droneAlertsEnabled) return null
            val label = listOfNotNull(
                drone.manufacturer,
                drone.model,
                drone.droneId.takeIf { it.isNotBlank() }
            ).firstOrNull() ?: "Drone"
            val rangeText = drone.estimatedDistanceMeters
                ?: drone.distanceMeters
            return SkyAlertCandidate(
                key = "sky:drone:${drone.id}",
                title = "Drone nearby",
                body = "$label detected${rangeText?.let { " around ${formatDistance(it)}" } ?: ""}"
            )
        }

        private fun aircraftCandidate(
            aircraft: Aircraft,
            settings: SkyAlertSettings
        ): SkyAlertCandidate? {
            if (settings.helicopterAlertsEnabled && aircraft.category == ObjectCategory.HELICOPTER) {
                return aircraftCandidate(
                    keyPrefix = "helicopter",
                    title = "Helicopter nearby",
                    aircraft = aircraft,
                    rangeRequired = false
                )
            }
            if (settings.militaryAlertsEnabled &&
                aircraft.category == ObjectCategory.MILITARY &&
                aircraft.isWithinTacticalRange()
            ) {
                return aircraftCandidate(
                    keyPrefix = "military",
                    title = "Military aircraft nearby",
                    aircraft = aircraft,
                    rangeRequired = true
                )
            }
            if (settings.policeAlertsEnabled &&
                aircraft.category in policeAlertCategories &&
                aircraft.isWithinTacticalRange()
            ) {
                return aircraftCandidate(
                    keyPrefix = "police",
                    title = "Police / emergency vehicle nearby",
                    aircraft = aircraft,
                    rangeRequired = true
                )
            }
            return null
        }

        private fun aircraftCandidate(
            keyPrefix: String,
            title: String,
            aircraft: Aircraft,
            rangeRequired: Boolean
        ): SkyAlertCandidate? {
            if (rangeRequired && aircraft.distanceMeters == null) return null
            val label = aircraft.callsign
                ?: aircraft.registration
                ?: aircraft.aircraftModel
                ?: aircraft.aircraftType
                ?: aircraft.icaoHex
            val distanceText = aircraft.distanceMeters?.let(::formatDistance)
            return SkyAlertCandidate(
                key = "sky:$keyPrefix:${aircraft.id}",
                title = title,
                body = listOfNotNull(label, distanceText).joinToString(" - ")
            )
        }

        private fun Aircraft.isWithinTacticalRange(): Boolean =
            distanceMeters?.let { it <= TACTICAL_ALERT_RADIUS_METERS } == true

        private fun formatDistance(meters: Double): String =
            if (meters >= METERS_PER_MILE) {
                val miles = meters / METERS_PER_MILE
                "${"%.1f".format(miles)} mi"
            } else {
                "${meters.roundToInt()} m"
            }

        private val policeAlertCategories = setOf(
            ObjectCategory.GOVERNMENT,
            ObjectCategory.EMERGENCY,
            ObjectCategory.GROUND_VEHICLE
        )
    }
}

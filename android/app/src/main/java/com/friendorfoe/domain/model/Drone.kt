package com.friendorfoe.domain.model

import java.time.Instant

/**
 * A drone detected via BLE Remote ID or WiFi SSID pattern matching.
 *
 * @property id Unique identifier (serial number or generated ID)
 * @property position Current geographic position
 * @property source REMOTE_ID for BLE detections, WIFI for SSID pattern matches
 * @property category Always DRONE
 * @property confidence Higher for Remote ID (~0.9), lower for WiFi (~0.3)
 * @property firstSeen When first detected
 * @property lastUpdated When last signal received
 * @property distanceMeters Distance from user in meters
 * @property screenX Projected screen X, null if not in camera FOV
 * @property screenY Projected screen Y, null if not in camera FOV
 * @property droneId Serial number or session ID from Remote ID, or generated for WiFi
 * @property manufacturer Detected or inferred manufacturer (e.g., "DJI", "Skydio")
 * @property model Drone model if known, null otherwise
 * @property operatorLatitude Operator location latitude from Remote ID, null for WiFi
 * @property operatorLongitude Operator location longitude from Remote ID, null for WiFi
 * @property ssid WiFi SSID if detected via WiFi, null for Remote ID
 * @property signalStrengthDbm Signal strength in dBm, for distance estimation
 * @property estimatedDistanceMeters Rough distance estimate from signal strength
 * @property bssid WiFi BSSID (MAC address) of the transmitter, null for BLE Remote ID
 * @property frequencyMhz WiFi channel center frequency in MHz (e.g., 2437), null for BLE
 * @property channelWidthMhz WiFi channel bandwidth in MHz (20/40/80/160), null for BLE
 */
data class Drone(
    override val id: String,
    override val position: Position,
    override val source: DetectionSource,
    override val category: ObjectCategory = ObjectCategory.DRONE,
    override val confidence: Float,
    override val firstSeen: Instant,
    override val lastUpdated: Instant,
    override val distanceMeters: Double? = null,
    override val screenX: Float? = null,
    override val screenY: Float? = null,
    val droneId: String,
    val manufacturer: String? = null,
    val model: String? = null,
    val operatorLatitude: Double? = null,
    val operatorLongitude: Double? = null,
    val ssid: String? = null,
    val signalStrengthDbm: Int? = null,
    val estimatedDistanceMeters: Double? = null,
    val bssid: String? = null,
    val frequencyMhz: Int? = null,
    val channelWidthMhz: Int? = null,
    val operatorId: String? = null,
    val uaType: Int? = null
) : SkyObject() {

    fun uaTypeLabel(): String? = when (uaType) {
        0 -> "None/Unknown"
        1 -> "Aeroplane"
        2 -> "Helicopter/Multirotor"
        3 -> "Gyroplane"
        4 -> "Hybrid Lift (VTOL)"
        5 -> "Ornithopter"
        6 -> "Glider"
        7 -> "Kite"
        8 -> "Free Balloon"
        9 -> "Captive Balloon"
        10 -> "Airship"
        11 -> "Free Fall / Parachute"
        12 -> "Rocket"
        13 -> "Tethered Powered Aircraft"
        14 -> "Ground Obstacle"
        15 -> "Other"
        else -> null
    }

    override fun displayLabel(): String {
        val name = manufacturer ?: "Drone"
        val alt = "${(position.altitudeMeters * 3.281).toInt()}ft"
        val suffix = when {
            source == DetectionSource.WIFI && confidence < 0.5f -> " ?"
            else -> ""
        }
        return "$name $alt$suffix"
    }

    override fun displaySummary(): String {
        val name = if (manufacturer != null && model != null) {
            "$manufacturer $model"
        } else {
            manufacturer ?: "Unknown drone"
        }
        val sourceLabel = when (source) {
            DetectionSource.REMOTE_ID -> "Remote ID"
            DetectionSource.WIFI -> "WiFi (low confidence)"
            else -> source.name
        }
        return "$name | $sourceLabel | ID: ${droneId.take(12)}"
    }
}

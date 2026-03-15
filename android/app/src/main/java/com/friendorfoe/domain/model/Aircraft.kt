package com.friendorfoe.domain.model

import java.time.Instant

/**
 * An aircraft detected via ADS-B transponder data.
 *
 * @property id Unique identifier (typically ICAO hex code)
 * @property position Current geographic position
 * @property source Always ADS_B for aircraft
 * @property category Computed from aircraft type / operator
 * @property confidence Detection confidence (ADS-B is typically high ~0.95)
 * @property firstSeen When first detected in this session
 * @property lastUpdated When position was last refreshed
 * @property distanceMeters Distance from user in meters
 * @property screenX Projected screen X, null if not in camera FOV
 * @property screenY Projected screen Y, null if not in camera FOV
 * @property icaoHex ICAO 24-bit hex address (e.g., "A1B2C3")
 * @property callsign Flight callsign (e.g., "UAL123"), null if unknown
 * @property registration Aircraft registration (e.g., "N12345"), null if unknown
 * @property aircraftType ICAO type designator (e.g., "B738"), null if unknown
 * @property aircraftModel Human-readable model (e.g., "Boeing 737-800"), null if unknown
 * @property airline Operating airline name, null if unknown or private
 * @property origin Departure airport IATA code, null if unknown
 * @property destination Arrival airport IATA code, null if unknown
 * @property squawk Transponder squawk code, null if not available
 * @property isOnGround Whether the aircraft is on the ground
 * @property photoUrl URL to aircraft photo, null if not enriched yet
 */
data class Aircraft(
    override val id: String,
    override val position: Position,
    override val source: DetectionSource = DetectionSource.ADS_B,
    override val category: ObjectCategory,
    override val confidence: Float = 0.95f,
    override val firstSeen: Instant,
    override val lastUpdated: Instant,
    override val distanceMeters: Double? = null,
    override val screenX: Float? = null,
    override val screenY: Float? = null,
    val icaoHex: String,
    val callsign: String? = null,
    val registration: String? = null,
    val aircraftType: String? = null,
    val aircraftModel: String? = null,
    val airline: String? = null,
    val origin: String? = null,
    val destination: String? = null,
    val squawk: String? = null,
    val isOnGround: Boolean = false,
    val photoUrl: String? = null,
    val classificationSignals: List<String>? = null
) : SkyObject() {

    override fun displayLabel(): String {
        val name = callsign ?: icaoHex
        val type = aircraftType ?: ""
        val alt = "${(position.altitudeMeters * 3.281).toInt()}ft" // meters to feet
        return "$name $type $alt".trim()
    }

    override fun displaySummary(): String {
        val name = callsign ?: icaoHex
        val route = if (origin != null && destination != null) "$origin -> $destination" else ""
        val model = aircraftModel ?: aircraftType ?: "Unknown type"
        return "$name | $model $route".trim()
    }
}

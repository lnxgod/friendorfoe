package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName

/**
 * Data Transfer Object for aircraft data from the backend API.
 *
 * Maps to the backend's /aircraft/nearby response format.
 */
data class AircraftDto(
    @SerializedName("icao_hex") val icaoHex: String,
    @SerializedName("callsign") val callsign: String?,
    @SerializedName("latitude") val latitude: Double,
    @SerializedName("longitude") val longitude: Double,
    @SerializedName("altitude_m") val altitudeMeters: Double,
    @SerializedName("heading") val heading: Float?,
    @SerializedName("speed_mps") val speedMps: Float?,
    @SerializedName("aircraft_type") val aircraftType: String?,
    @SerializedName("registration") val registration: String?,
    @SerializedName("category") val category: String?,
    @SerializedName("squawk") val squawk: String?,
    @SerializedName("on_ground") val onGround: Boolean = false,
    @SerializedName("last_contact") val lastContact: Long
)

/**
 * Enriched aircraft detail from the backend, including photo and route info.
 */
data class AircraftDetailDto(
    @SerializedName("icao_hex") val icaoHex: String,
    @SerializedName("callsign") val callsign: String?,
    @SerializedName("registration") val registration: String?,
    @SerializedName("aircraft_type") val aircraftType: String?,
    @SerializedName("aircraft_model") val aircraftModel: String?,
    @SerializedName("airline") val airline: String?,
    @SerializedName("origin") val origin: String?,
    @SerializedName("destination") val destination: String?,
    @SerializedName("photo_url") val photoUrl: String?,
    @SerializedName("category") val category: String?
)

/**
 * Enriched drone detail from the backend.
 */
data class DroneDetailDto(
    @SerializedName("drone_id") val droneId: String,
    @SerializedName("manufacturer") val manufacturer: String?,
    @SerializedName("model") val model: String?,
    @SerializedName("serial_number") val serialNumber: String?
)

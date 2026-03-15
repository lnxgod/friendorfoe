package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName

/**
 * Photo sub-object from aircraft detail response.
 */
data class AircraftPhotoDto(
    @SerializedName("url") val url: String?,
    @SerializedName("photographer") val photographer: String?,
    @SerializedName("thumbnail_url") val thumbnailUrl: String?
)

/**
 * Route sub-object from aircraft detail response.
 */
data class RouteInfoDto(
    @SerializedName("airline") val airline: String?,
    @SerializedName("airline_iata") val airlineIata: String?,
    @SerializedName("flight_number") val flightNumber: String?,
    @SerializedName("origin") val origin: String?,
    @SerializedName("destination") val destination: String?
)

/**
 * Enriched aircraft detail DTO.
 */
data class AircraftDetailDto(
    @SerializedName("icao_hex") val icaoHex: String,
    @SerializedName("callsign") val callsign: String?,
    @SerializedName("registration") val registration: String?,
    @SerializedName("aircraft_type") val aircraftType: String?,
    @SerializedName("aircraft_description") val aircraftDescription: String?,
    @SerializedName("operator") val operator: String?,
    @SerializedName("photo") val photo: AircraftPhotoDto?,
    @SerializedName("route") val route: RouteInfoDto?,
    @SerializedName("country") val country: String?
)

package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName

/**
 * Shared response model for ADSBx v2 format APIs (adsb.fi and airplanes.live).
 *
 * Both APIs return the same JSON structure with an `ac` array of aircraft objects.
 */
data class AdsbxResponse(
    val ac: List<AdsbxAircraft>?,
    val total: Int?,
    val now: Long?
)

/**
 * Single aircraft from an ADSBx v2 format response.
 *
 * Fields are nullable since not all aircraft report all data.
 * [altBaro] is typed as [Any] because it can be an Int (feet) or the string "ground".
 */
data class AdsbxAircraft(
    val hex: String?,
    val flight: String?,
    val lat: Double?,
    val lon: Double?,
    @SerializedName("alt_baro") val altBaro: Any?,
    @SerializedName("alt_geom") val altGeom: Int?,
    val gs: Double?,
    val track: Double?,
    @SerializedName("baro_rate") val baroRate: Int?,
    val category: String?,
    val t: String?,
    val r: String?,
    @SerializedName("seen_pos") val seenPos: Double?,
    val squawk: String? = null
)

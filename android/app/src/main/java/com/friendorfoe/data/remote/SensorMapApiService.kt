package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName
import retrofit2.http.GET

/**
 * Retrofit interface for the Friend or Foe backend sensor map API.
 *
 * Retrieves triangulated drone positions from multiple ESP32 sensor nodes.
 * Base URL is configurable (default: http://10.0.2.2:8000 for emulator,
 * override via BuildConfig or settings).
 */
interface SensorMapApiService {

    @GET("detections/drones/map")
    suspend fun getDroneMap(): DroneMapDto

    @GET("detections/sensors")
    suspend fun getSensors(): SensorsDto
}

data class DroneMapDto(
    @SerializedName("drone_count") val droneCount: Int = 0,
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    val drones: List<LocatedDroneDto> = emptyList(),
    val sensors: List<SensorDto> = emptyList()
)

data class LocatedDroneDto(
    @SerializedName("drone_id") val droneId: String,
    val lat: Double,
    val lon: Double,
    val alt: Double? = null,
    @SerializedName("heading_deg") val headingDeg: Float? = null,
    @SerializedName("speed_mps") val speedMps: Float? = null,
    @SerializedName("position_source") val positionSource: String = "unknown",
    @SerializedName("accuracy_m") val accuracyM: Double? = null,
    @SerializedName("range_m") val rangeM: Double? = null,
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    val confidence: Float = 0f,
    val manufacturer: String? = null,
    val model: String? = null,
    @SerializedName("operator_lat") val operatorLat: Double? = null,
    @SerializedName("operator_lon") val operatorLon: Double? = null,
    @SerializedName("operator_id") val operatorId: String? = null,
    val observations: List<SensorObservationDto> = emptyList()
)

data class SensorObservationDto(
    @SerializedName("device_id") val deviceId: String,
    @SerializedName("sensor_lat") val sensorLat: Double,
    @SerializedName("sensor_lon") val sensorLon: Double,
    val rssi: Int? = null,
    @SerializedName("estimated_distance_m") val estimatedDistanceM: Double? = null,
    val confidence: Float = 0f,
    val source: String = ""
)

data class SensorDto(
    @SerializedName("device_id") val deviceId: String,
    val lat: Double,
    val lon: Double,
    val alt: Double? = null,
    @SerializedName("last_seen") val lastSeen: Double = 0.0
)

data class SensorsDto(
    val count: Int = 0,
    val sensors: List<SensorDto> = emptyList()
)

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

    @GET("detections/nodes/status")
    suspend fun getNodesStatus(): NodeStatusDto

    @GET("detections/drone-alerts")
    suspend fun getDroneAlerts(): DroneAlertsDto

    @GET("health")
    suspend fun getHealth(): HealthDto
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
    val observations: List<SensorObservationDto> = emptyList(),
    val classification: String? = null
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
    @SerializedName("last_seen") val lastSeen: Double = 0.0,
    val online: Boolean = true
)

data class SensorsDto(
    val count: Int = 0,
    val sensors: List<SensorDto> = emptyList()
)

data class NodeStatusDto(
    val count: Int = 0,
    val nodes: List<NodeDto> = emptyList()
)

data class NodeDto(
    @SerializedName("device_id") val deviceId: String,
    @SerializedName("last_seen") val lastSeen: Double = 0.0,
    @SerializedName("detection_count") val detectionCount: Int = 0,
    @SerializedName("total_batches") val totalBatches: Int = 0,
    @SerializedName("total_detections") val totalDetections: Int = 0,
    val lat: Double = 0.0,
    val lon: Double = 0.0,
    val ip: String? = null,
    val name: String? = null,
    @SerializedName("age_s") val ageS: Double = 0.0,
    val online: Boolean = false,
    @SerializedName("gps_registered") val gpsRegistered: Boolean = false
)

data class DroneAlertsDto(
    @SerializedName("active_drone_count") val activeDroneCount: Int = 0,
    @SerializedName("active_drones") val activeDrones: List<ActiveDroneDto> = emptyList(),
    @SerializedName("recent_alerts") val recentAlerts: List<DroneAlertDto> = emptyList(),
    @SerializedName("total_alerts") val totalAlerts: Int = 0
)

data class ActiveDroneDto(
    @SerializedName("drone_id") val droneId: String,
    @SerializedName("last_seen") val lastSeen: Double = 0.0,
    @SerializedName("age_s") val ageS: Double = 0.0,
    val active: Boolean = true,
    val lat: Double? = null,
    val lon: Double? = null,
    @SerializedName("position_source") val positionSource: String? = null,
    @SerializedName("sensor_count") val sensorCount: Int? = null,
    @SerializedName("accuracy_m") val accuracyM: Double? = null,
    val classification: String? = null
)

data class DroneAlertDto(
    @SerializedName("alert_type") val alertType: String,
    val severity: String,
    @SerializedName("drone_id") val droneId: String? = null,
    val classification: String? = null,
    val source: String? = null,
    val ssid: String? = null,
    val rssi: Int? = null,
    val manufacturer: String? = null,
    @SerializedName("device_id") val deviceId: String? = null,
    val message: String = "",
    val timestamp: Double = 0.0
)

data class HealthDto(
    val status: String = "unknown",
    val version: String = "",
    val redis: String = "unknown",
    val database: String = "unknown"
)

package com.friendorfoe.data.remote

import com.google.gson.annotations.SerializedName
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path
import retrofit2.http.Query

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

    @GET("detections/events")
    suspend fun getEvents(
        @Query("types") types: List<String>? = null,
        @Query("acknowledged") acknowledged: Boolean? = null,
        @Query("since_hours") sinceHours: Float = 24f,
        @Query("limit") limit: Int = 200,
    ): EventsDto

    @GET("detections/events/stats")
    suspend fun getEventStats(): EventStatsDto

    @POST("detections/events/{eventId}/ack")
    suspend fun ackEvent(@Path("eventId") eventId: Int): AckResponseDto

    @GET("detections/probes")
    suspend fun getProbeDevices(
        @Query("max_age_s") maxAgeS: Int = 86400,
        @Query("drone_only") droneOnly: Boolean = false,
    ): ProbeDevicesDto

    @GET("detections/devices/live")
    suspend fun getLivePrivacyDevices(
        @Query("min_confidence") minConfidence: Float = 0f,
        @Query("trackers_only") trackersOnly: Boolean = false,
    ): LivePrivacyDevicesDto

    @GET("detections/wifi/ap-inventory")
    suspend fun getWifiApInventory(
        @Query("max_age_s") maxAgeS: Int = 86400,
    ): WifiApInventoryDto

    @GET("detections/calibrate/model")
    suspend fun getCalibrationModel(): CalibrationModelDto

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
    val classification: String? = null,
    @SerializedName("probed_ssids") val probedSsids: List<String>? = null,
    @SerializedName("age_s") val ageS: Double? = null,
    @SerializedName("first_seen") val firstSeen: Double? = null,
    @SerializedName("last_seen") val lastSeen: Double? = null,
    val bssid: String? = null,
    val ssid: String? = null,
    @SerializedName("identity_source") val identitySource: String? = null,
    @SerializedName("range_authority") val rangeAuthority: String? = null,
    @SerializedName("geometry_trust") val geometryTrust: String? = null,
    @SerializedName("source_tier") val sourceTier: String? = null,
    @SerializedName("uncertainty_m") val uncertaintyM: Double? = null,
    @SerializedName("calibration_state") val calibrationState: String? = null,
    @SerializedName("mac_is_randomized") val macIsRandomized: Boolean? = null,
    @SerializedName("mac_identity_kind") val macIdentityKind: String? = null,
    @SerializedName("mac_reason") val macReason: String? = null,
    val brand: String? = null,
    @SerializedName("brand_source") val brandSource: String? = null,
    @SerializedName("brand_confidence") val brandConfidence: Float? = null,
    @SerializedName("device_class") val deviceClass: String? = null,
    @SerializedName("device_class_confidence") val deviceClassConfidence: Float? = null,
    val evidence: List<String>? = null,
    @SerializedName("related_entities") val relatedEntities: List<RelatedEntityDto>? = null,
)

data class SensorObservationDto(
    @SerializedName("device_id") val deviceId: String,
    @SerializedName("sensor_lat") val sensorLat: Double,
    @SerializedName("sensor_lon") val sensorLon: Double,
    val rssi: Int? = null,
    @SerializedName("estimated_distance_m") val estimatedDistanceM: Double? = null,
    @SerializedName("scanner_estimated_distance_m") val scannerEstimatedDistanceM: Double? = null,
    @SerializedName("backend_estimated_distance_m") val backendEstimatedDistanceM: Double? = null,
    @SerializedName("distance_source") val distanceSource: String? = null,
    @SerializedName("range_model") val rangeModel: String? = null,
    @SerializedName("range_authority") val rangeAuthority: String? = null,
    @SerializedName("geometry_trust") val geometryTrust: String? = null,
    @SerializedName("source_tier") val sourceTier: String? = null,
    @SerializedName("uncertainty_m") val uncertaintyM: Double? = null,
    val confidence: Float = 0f,
    val source: String = "",
    val ssid: String? = null,
    val bssid: String? = null,
    @SerializedName("ie_hash") val ieHash: String? = null,
    @SerializedName("mac_is_randomized") val macIsRandomized: Boolean? = null,
    @SerializedName("mac_identity_kind") val macIdentityKind: String? = null,
    @SerializedName("mac_reason") val macReason: String? = null,
    val brand: String? = null,
    @SerializedName("brand_source") val brandSource: String? = null,
    @SerializedName("brand_confidence") val brandConfidence: Float? = null,
    @SerializedName("device_class") val deviceClass: String? = null,
    @SerializedName("device_class_confidence") val deviceClassConfidence: Float? = null,
    @SerializedName("related_entities") val relatedEntities: List<RelatedEntityDto>? = null,
)

data class RelatedEntityDto(
    @SerializedName("entity_id") val entityId: String,
    @SerializedName("relation_type") val relationType: String,
    val confidence: Float = 0f,
    val reason: String? = null,
)

data class LivePrivacyDevicesDto(
    val devices: List<LivePrivacyDeviceDto> = emptyList(),
    val summary: LivePrivacySummaryDto = LivePrivacySummaryDto(),
)

data class LivePrivacySummaryDto(
    @SerializedName("total_tracked") val totalTracked: Int = 0,
    val active: Int = 0,
    @SerializedName("physical_devices") val physicalDevices: Int = 0,
    @SerializedName("trackers_active") val trackersActive: Int = 0,
    val classified: Int = 0,
    val unclassified: Int = 0,
    @SerializedName("privacy_kind_counts") val privacyKindCounts: Map<String, Int> = emptyMap(),
    @SerializedName("apple_continuity_subtypes") val appleContinuitySubtypes: Map<String, Int> = emptyMap(),
    @SerializedName("beacon_density") val beaconDensity: Int = 0,
)

data class LivePrivacyDeviceDto(
    val fingerprint: String? = null,
    @SerializedName("device_type") val deviceType: String? = null,
    val manufacturer: String? = null,
    @SerializedName("is_tracker") val isTracker: Boolean = false,
    @SerializedName("current_rssi") val currentRssi: Int? = null,
    @SerializedName("avg_rssi") val avgRssi: Float? = null,
    @SerializedName("first_seen") val firstSeen: Double? = null,
    @SerializedName("last_seen") val lastSeen: Double? = null,
    @SerializedName("idle_s") val idleS: Double? = null,
    @SerializedName("last_bssid") val lastBssid: String? = null,
    val source: String? = null,
    val confidence: Float = 0f,
    @SerializedName("seen_by") val seenBy: List<String> = emptyList(),
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    @SerializedName("mac_rotations") val macRotations: Int = 0,
    @SerializedName("ble_ja3") val bleJa3: String? = null,
    @SerializedName("ble_company_id") val bleCompanyId: Int? = null,
    @SerializedName("ble_apple_type") val bleAppleType: Int? = null,
    @SerializedName("ble_apple_flags") val bleAppleFlags: Int? = null,
    @SerializedName("ble_svc_uuids") val bleSvcUuids: String? = null,
    @SerializedName("privacy_kind") val privacyKind: String? = null,
    @SerializedName("risk_level") val riskLevel: String? = null,
    @SerializedName("display_label") val displayLabel: String? = null,
    @SerializedName("display_detail") val displayDetail: String? = null,
    val evidence: List<String>? = null,
    @SerializedName("privacy_evidence") val privacyEvidence: List<Map<String, Any?>> = emptyList(),
    @SerializedName("apple_continuity") val appleContinuity: Map<String, Any?>? = null,
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
    @SerializedName("gps_registered") val gpsRegistered: Boolean = false,
    @SerializedName("firmware_version") val firmwareVersion: String? = null,
    @SerializedName("board_type") val boardType: String? = null,
    @SerializedName("wifi_ssid") val wifiSsid: String? = null,
    @SerializedName("wifi_rssi") val wifiRssi: Int? = null,
    @SerializedName("scan_mode") val scanMode: String? = null,
    @SerializedName("scan_profile") val scanProfile: String? = null,
    @SerializedName("calibration_uuid") val calibrationUuid: String? = null,
    @SerializedName("dedup_seen") val dedupSeen: Int? = null,
    @SerializedName("dedup_sent") val dedupSent: Int? = null,
    @SerializedName("dedup_collapsed") val dedupCollapsed: Int? = null,
    @SerializedName("cal_seen") val calSeen: Int? = null,
    @SerializedName("cal_sent") val calSent: Int? = null,
    @SerializedName("source_fixups_since_boot") val sourceFixupsSinceBoot: Int = 0,
    @SerializedName("source_fixups_recent") val sourceFixupsRecent: Int = 0,
    @SerializedName("source_fixups_recent_by_rule") val sourceFixupsRecentByRule: Map<String, Int> = emptyMap(),
    @SerializedName("source_fixups_recent_window_s") val sourceFixupsRecentWindowS: Int = 0,
    val reporting: ReportingStatusDto? = null,
    val scanners: List<ScannerStatusDto> = emptyList(),
)

data class ReportingStatusDto(
    @SerializedName("network_mode") val networkMode: String? = null,
    @SerializedName("backend_enabled") val backendEnabled: Boolean? = null,
    @SerializedName("network_ttl_s") val networkTtlS: Int? = null,
    @SerializedName("wifi_sta") val wifiSta: Boolean? = null,
    val standalone: Boolean? = null,
    @SerializedName("uploads_ok") val uploadsOk: Int? = null,
    @SerializedName("uploads_fail") val uploadsFail: Int? = null,
    @SerializedName("last_upload_age_s") val lastUploadAgeS: Long? = null,
)

data class ScannerStatusDto(
    val slot: Int? = null,
    val uart: String? = null,
    val connected: Boolean? = null,
    val ver: String? = null,
    val board: String? = null,
    val chip: String? = null,
    val caps: String? = null,
    @SerializedName("scan_profile") val scanProfile: String? = null,
    @SerializedName("expected_scan_profile") val expectedScanProfile: String? = null,
    @SerializedName("slot_role") val slotRole: String? = null,
    @SerializedName("role_acked") val roleAcked: Boolean? = null,
    val health: String? = null,
    @SerializedName("uart_raw_seen") val uartRawSeen: Boolean? = null,
    @SerializedName("uart_raw_age_s") val uartRawAgeS: Long? = null,
    @SerializedName("uart_json_err") val uartJsonErr: Int? = null,
    @SerializedName("cmd_rx") val cmdRx: Int? = null,
    @SerializedName("cmd_last_age_s") val cmdLastAgeS: Long? = null,
    val toff: Long? = null,
    val tcnt: Int? = null,
    @SerializedName("uart_tx_dropped") val uartTxDropped: Int? = null,
    @SerializedName("uart_tx_high_water") val uartTxHighWater: Int? = null,
    @SerializedName("tx_queue_depth") val txQueueDepth: Int? = null,
    @SerializedName("tx_queue_capacity") val txQueueCapacity: Int? = null,
    @SerializedName("tx_queue_pressure_pct") val txQueuePressurePct: Int? = null,
    @SerializedName("noise_drop_ble") val noiseDropBle: Int? = null,
    @SerializedName("noise_drop_wifi") val noiseDropWifi: Int? = null,
    @SerializedName("probe_seen") val probeSeen: Int? = null,
    @SerializedName("probe_sent") val probeSent: Int? = null,
    @SerializedName("probe_drop_low_value") val probeDropLowValue: Int? = null,
    @SerializedName("probe_drop_rate_limit") val probeDropRateLimit: Int? = null,
    @SerializedName("probe_drop_pressure") val probeDropPressure: Int? = null,
    @SerializedName("ble_adv_seen") val bleAdvSeen: Int? = null,
    @SerializedName("ble_fp_emit") val bleFpEmit: Int? = null,
    @SerializedName("ble_meta_seen") val bleMetaSeen: Int? = null,
    @SerializedName("ble_tracker_seen") val bleTrackerSeen: Int? = null,
    @SerializedName("rid_emit") val ridEmit: Int? = null,
    @SerializedName("privacy_seen") val privacySeen: Int? = null,
    @SerializedName("wifi_total_frames") val wifiTotalFrames: Int? = null,
    @SerializedName("wifi_drone_ssid_emit") val wifiDroneSsidEmit: Int? = null,
    @SerializedName("wifi_notable_ssid_emit") val wifiNotableSsidEmit: Int? = null,
    @SerializedName("wifi_last_drone_ssid") val wifiLastDroneSsid: String? = null,
    @SerializedName("wifi_last_notable_ssid") val wifiLastNotableSsid: String? = null,
    @SerializedName("fw_state") val fwState: String? = null,
    @SerializedName("target_ver") val targetVer: String? = null,
    @SerializedName("ota_state") val otaState: String? = null,
    @SerializedName("last_fw_error") val lastFwError: String? = null,
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

data class EventsDto(
    val count: Int = 0,
    val events: List<EventDto> = emptyList(),
)

data class EventDto(
    val id: Int,
    @SerializedName("event_type") val eventType: String,
    val identifier: String,
    val severity: String,
    val title: String = "",
    val message: String = "",
    @SerializedName("first_seen_at") val firstSeenAt: String = "",
    @SerializedName("last_seen_at") val lastSeenAt: String = "",
    @SerializedName("sighting_count") val sightingCount: Int = 0,
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    @SerializedName("sensor_ids") val sensorIds: List<String> = emptyList(),
    @SerializedName("best_rssi") val bestRssi: Int? = null,
    val metadata: Map<String, Any?> = emptyMap(),
    val acknowledged: Boolean = false,
    @SerializedName("acknowledged_at") val acknowledgedAt: String? = null,
)

data class EventStatsDto(
    val total: Int = 0,
    val unacknowledged: Int = 0,
    @SerializedName("critical_unacked") val criticalUnacked: Int = 0,
    @SerializedName("by_type") val byType: Map<String, Int> = emptyMap(),
    @SerializedName("unack_by_type") val unackByType: Map<String, Int> = emptyMap(),
    @SerializedName("by_severity") val bySeverity: Map<String, Int> = emptyMap(),
)

data class AckResponseDto(
    val ok: Boolean = false,
    @SerializedName("event_id") val eventId: Int? = null,
    val acked: Int? = null,
)

data class ProbeDevicesDto(
    val count: Int = 0,
    val devices: List<ProbeDeviceDto> = emptyList(),
)

data class ProbeDeviceDto(
    val identity: String,
    @SerializedName("ie_hash") val ieHash: String? = null,
    val mac: String? = null,
    val macs: List<String> = emptyList(),
    @SerializedName("probed_ssids") val probedSsids: List<String> = emptyList(),
    @SerializedName("probe_count") val probeCount: Int = 0,
    @SerializedName("best_rssi") val bestRssi: Int? = null,
    val classification: String? = null,
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    val sensors: List<String> = emptyList(),
    @SerializedName("first_seen") val firstSeen: Double? = null,
    @SerializedName("first_seen_age_s") val firstSeenAgeS: Double? = null,
    @SerializedName("last_seen") val lastSeen: Double? = null,
    @SerializedName("age_s") val ageS: Double? = null,
    @SerializedName("seen_24h_count") val seen24hCount: Int = 0,
    @SerializedName("sensor_count_24h") val sensorCount24h: Int = 0,
    @SerializedName("activity_level") val activityLevel: String? = null,
    @SerializedName("latest_event_types") val latestEventTypes: List<String> = emptyList(),
    val lat: Double? = null,
    val lon: Double? = null,
    @SerializedName("mac_is_randomized") val macIsRandomized: Boolean? = null,
    @SerializedName("mac_identity_kind") val macIdentityKind: String? = null,
    @SerializedName("mac_reason") val macReason: String? = null,
    val brand: String? = null,
    @SerializedName("brand_source") val brandSource: String? = null,
    @SerializedName("brand_confidence") val brandConfidence: Float? = null,
    @SerializedName("device_class") val deviceClass: String? = null,
    @SerializedName("device_class_confidence") val deviceClassConfidence: Float? = null,
    @SerializedName("identity_source") val identitySource: String? = null,
    val evidence: List<String> = emptyList(),
    @SerializedName("related_entities") val relatedEntities: List<RelatedEntityDto> = emptyList(),
)

data class WifiApInventoryDto(
    val count: Int = 0,
    val aps: List<WifiApDto> = emptyList(),
)

data class WifiApDto(
    val bssid: String? = null,
    val ssid: String? = null,
    @SerializedName("auth_m") val authMode: Int? = null,
    val channel: Int? = null,
    val vendor: String? = null,
    val sensors: List<String> = emptyList(),
    @SerializedName("sensor_count") val sensorCount: Int = 0,
    val count: Int = 0,
    @SerializedName("best_rssi") val bestRssi: Int? = null,
    @SerializedName("first_seen") val firstSeen: Double? = null,
    @SerializedName("last_seen") val lastSeen: Double? = null,
    @SerializedName("age_s") val ageS: Double? = null,
    @SerializedName("mac_is_randomized") val macIsRandomized: Boolean? = null,
    @SerializedName("mac_identity_kind") val macIdentityKind: String? = null,
    @SerializedName("mac_reason") val macReason: String? = null,
    val brand: String? = null,
    @SerializedName("brand_source") val brandSource: String? = null,
    @SerializedName("brand_confidence") val brandConfidence: Float? = null,
    @SerializedName("device_class") val deviceClass: String? = null,
    @SerializedName("device_class_confidence") val deviceClassConfidence: Float? = null,
    @SerializedName("identity_source") val identitySource: String? = null,
    val evidence: List<String> = emptyList(),
)

data class CalibrationModelDto(
    @SerializedName("rssi_ref") val rssiRef: Double? = null,
    @SerializedName("path_loss_exponent") val pathLossExponent: Double? = null,
    @SerializedName("is_calibrated") val isCalibrated: Boolean = false,
    @SerializedName("is_active") val isActive: Boolean = false,
    @SerializedName("is_trusted") val isTrusted: Boolean = false,
    @SerializedName("active_model_source") val activeModelSource: String? = null,
    @SerializedName("applied_listener_count") val appliedListenerCount: Int = 0,
    @SerializedName("last_calibration") val lastCalibration: Double? = null,
    @SerializedName("r_squared") val rSquared: Double? = null,
)

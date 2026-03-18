package com.friendorfoe.domain.model

/**
 * Source of detection for a sky object.
 *
 * Each source has different reliability characteristics:
 * - ADS_B: High confidence, from aircraft transponder via backend API
 * - REMOTE_ID: High confidence, from FAA-mandated BLE broadcast
 * - WIFI: Low confidence, from WiFi SSID pattern matching
 */
enum class DetectionSource {
    /** ADS-B transponder data via OpenSky/ADS-B Exchange backend */
    ADS_B,

    /** FAA Remote ID via BLE (ASTM F3411) */
    REMOTE_ID,

    /** FAA Remote ID via WiFi Aware (NaN) */
    WIFI_NAN,

    /** FAA Remote ID via WiFi Beacon vendor IE */
    WIFI_BEACON,

    /** WiFi SSID pattern matching (e.g., DJI-*, TELLO-*) */
    WIFI
}

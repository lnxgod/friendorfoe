package com.friendorfoe.detection

import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * WiFi SSID pattern scanner for detecting older/non-compliant drones.
 *
 * TODO: Implement in detection task (Phase 1.4):
 * - Trigger WiFi scan (respecting 4-per-2-minute Android limit)
 * - Match SSIDs against known drone patterns: DJI-*, TELLO-*, SKYDIO-*, etc.
 * - Estimate distance from signal strength (RSSI)
 * - Emit detected drones with low confidence
 * - Handle NEARBY_WIFI_DEVICES permission
 *
 * Known patterns:
 * - DJI-MAVIC-*, DJI-MINI-*, DJI-PHANTOM-*
 * - TELLO-*
 * - SKYDIO-*
 * - PARROT-*
 * - AUTEL-*
 */
@Singleton
class WifiDroneScanner @Inject constructor() {

    /** Start periodic WiFi scanning for drone SSIDs. */
    fun startScanning(): Flow<Drone> {
        // Stub: will be implemented in detection task
        return emptyFlow()
    }

    /** Stop scanning. */
    fun stopScanning() {
        // Stub
    }
}

package com.friendorfoe.detection

import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * BLE Remote ID scanner for detecting compliant drones.
 *
 * TODO: Implement in detection task (Phase 1.4):
 * - Start/stop BLE scanning for OpenDroneID advertisements
 * - Parse ASTM F3411 Remote ID messages
 * - Extract: drone serial, position, altitude, speed, operator location
 * - Emit detected drones as a Flow
 * - Handle Android BLE permissions (BLUETOOTH_SCAN)
 *
 * Reference: OpenDroneID receiver-android library
 * FAA Remote ID mandate effective March 2024
 */
@Singleton
class RemoteIdScanner @Inject constructor() {

    /** Start scanning for Remote ID broadcasts. */
    fun startScanning(): Flow<Drone> {
        // Stub: will be implemented in detection task
        return emptyFlow()
    }

    /** Stop scanning. */
    fun stopScanning() {
        // Stub
    }
}

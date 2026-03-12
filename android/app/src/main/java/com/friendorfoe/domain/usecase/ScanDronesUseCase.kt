package com.friendorfoe.domain.usecase

import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import javax.inject.Inject

/**
 * Use case for scanning nearby drones via BLE Remote ID and WiFi.
 *
 * Combines results from both BLE and WiFi detection sources into
 * a unified stream of Drone objects.
 *
 * TODO: Implement in detection task - requires BLE scanner and WiFi scanner
 */
class ScanDronesUseCase @Inject constructor(
    // TODO: Inject BLE Remote ID scanner
    // TODO: Inject WiFi SSID scanner
) {
    /**
     * Start scanning for nearby drones.
     *
     * @return Flow emitting detected drones as they are found
     */
    operator fun invoke(): Flow<List<Drone>> {
        // Stub: will be implemented when detection modules are built
        return emptyFlow()
    }
}

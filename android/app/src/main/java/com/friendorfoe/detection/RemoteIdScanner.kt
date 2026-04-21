package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid
import android.util.Log
import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * BLE Remote ID scanner for detecting compliant drones.
 *
 * Scans for OpenDroneID BLE advertisements as defined by ASTM F3411.
 * Message parsing is delegated to [OpenDroneIdParser] (shared with WiFi NaN scanner).
 *
 * FAA Remote ID mandate effective March 2024 requires all drones >=250g
 * to broadcast identification and location via BLE or WiFi.
 */
@Singleton
class RemoteIdScanner @Inject constructor(
    private val bluetoothManager: BluetoothManager
) {

    companion object {
        private const val TAG = "RemoteIdScanner"
    }

    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var bleScanner: BluetoothLeScanner? = null
    private var activeScanCallback: ScanCallback? = null

    // Track parsed data per drone MAC address. We may receive Basic ID and Location
    // in separate advertisements, so we accumulate partial state.
    private val droneStates = java.util.concurrent.ConcurrentHashMap<String, OpenDroneIdParser.DronePartialState>()

    /**
     * Start scanning for Remote ID broadcasts.
     *
     * Returns a Flow that emits Drone objects as they are detected via BLE.
     * If BLE is unavailable or disabled, the flow completes without emitting.
     *
     * Requires BLUETOOTH_SCAN permission (Android 12+) or BLUETOOTH + ACCESS_FINE_LOCATION (older).
     */
    @SuppressLint("MissingPermission")
    fun startScanning(): Flow<Drone> = callbackFlow {
        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled) {
            Log.w(TAG, "Bluetooth not available or disabled, skipping Remote ID scan")
            close()
            return@callbackFlow
        }

        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            Log.w(TAG, "BLE scanner not available")
            close()
            return@callbackFlow
        }
        bleScanner = scanner

        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setReportDelay(0) // Immediate callback per result
            .build()

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                try {
                    val drone = processScanResult(result)
                    if (drone != null) {
                        trySend(drone)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing BLE scan result", e)
                }
            }

            override fun onBatchScanResults(results: List<ScanResult>) {
                for (result in results) {
                    try {
                        val drone = processScanResult(result)
                        if (drone != null) {
                            trySend(drone)
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error processing batch BLE scan result", e)
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "BLE scan failed with error code: $errorCode")
            }
        }

        activeScanCallback = callback
        // Some phones miss ODID packets when service-data filtering is pushed
        // into the controller. Scan unfiltered and parse the FFFA payload here.
        Log.i(TAG, "Starting BLE Remote ID scan (unfiltered compatibility mode)")
        scanner.startScan(null, scanSettings, callback)

        awaitClose {
            Log.i(TAG, "Stopping BLE Remote ID scan")
            try {
                scanner.stopScan(callback)
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping BLE scan", e)
            }
            droneStates.clear()
        }
    }

    /** Stop scanning (for imperative callers; flow-based callers cancel the coroutine). */
    @SuppressLint("MissingPermission")
    fun stopScanning() {
        try {
            activeScanCallback?.let { cb -> bleScanner?.stopScan(cb) }
        } catch (e: Exception) {
            Log.w(TAG, "Error stopping BLE scan", e)
        } finally {
            activeScanCallback = null
            bleScanner = null
            droneStates.clear()
        }
    }

    /**
     * Process a BLE scan result containing OpenDroneID service data.
     *
     * Returns a fully-formed Drone if we have both Basic ID and Location data,
     * or a partial Drone with just the serial if we only have Basic ID so far.
     */
    private fun processScanResult(result: ScanResult): Drone? {
        val deviceAddress = result.device.address
        val serviceData = result.scanRecord?.getServiceData(
            ParcelUuid(OpenDroneIdParser.OPEN_DRONE_ID_UUID)
        ) ?: return null

        if (serviceData.isEmpty()) return null

        // OpenDroneID BLE service data: the 25-byte ODID message.
        // Android's getServiceData() already strips the UUID prefix.
        // Some implementations prepend App Code (0x0D) + counter byte (27 bytes total),
        // others send the raw 25-byte message directly.
        val messageData = when {
            serviceData.size >= 27 -> serviceData.copyOfRange(2, 27) // Skip app code + counter
            serviceData.size >= 25 -> serviceData.copyOfRange(0, 25) // Raw 25-byte message
            else -> {
                Log.d(TAG, "Service data too short: ${serviceData.size} bytes")
                return null
            }
        }

        val now = Instant.now()

        // Parse into a temporary state first to extract the serial number
        val tempState = droneStates.getOrPut(deviceAddress) {
            OpenDroneIdParser.DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
        }

        synchronized(tempState) {
            tempState.lastUpdated = now
            tempState.signalStrengthDbm = result.rssi
            OpenDroneIdParser.parseMessage(messageData, tempState)
        }

        // If we now have a serial, use it as the canonical key (handles multi-drone on same MAC)
        val serial = tempState.droneId ?: return null
        val canonicalState = droneStates.getOrPut(serial) {
            OpenDroneIdParser.DronePartialState(deviceAddress = deviceAddress, firstSeen = now)
        }

        synchronized(canonicalState) {
            // Copy latest data from the MAC-keyed state to the serial-keyed state
            canonicalState.lastUpdated = now
            canonicalState.signalStrengthDbm = result.rssi
            canonicalState.droneId = serial
            canonicalState.latitude = tempState.latitude
            canonicalState.longitude = tempState.longitude
            canonicalState.altitudeMeters = tempState.altitudeMeters
            canonicalState.heading = tempState.heading
            canonicalState.speedMps = tempState.speedMps
            canonicalState.verticalSpeedMps = tempState.verticalSpeedMps
            canonicalState.heightAglMeters = tempState.heightAglMeters
            canonicalState.geodeticAltitudeMeters = tempState.geodeticAltitudeMeters
            canonicalState.operatorLatitude = tempState.operatorLatitude
            canonicalState.operatorLongitude = tempState.operatorLongitude
            canonicalState.operatorId = tempState.operatorId
            canonicalState.selfIdText = tempState.selfIdText
            canonicalState.uaType = tempState.uaType
            canonicalState.idType = tempState.idType
            canonicalState.horizontalAccuracyCode = tempState.horizontalAccuracyCode
            canonicalState.verticalAccuracyCode = tempState.verticalAccuracyCode

            return canonicalState.toDroneOrNull(idPrefix = "rid_")
        }
    }
}

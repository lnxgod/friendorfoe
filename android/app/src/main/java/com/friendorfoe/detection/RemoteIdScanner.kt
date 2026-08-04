package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

internal fun useLegacyRemoteIdScan(extendedAdvertisingSupported: Boolean): Boolean =
    !extendedAdvertisingSupported

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
    private val dropLogLock = Any()
    private var droppedSinceLastLog = 0
    private var lastDropLogAtMs = 0L

    private val payloadProcessor = BleRemoteIdPayloadProcessor()

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
        val bluetoothEnabled = try {
            adapter?.isEnabled == true
        } catch (e: SecurityException) {
            Log.w(TAG, "Bluetooth permission missing, skipping Remote ID scan")
            false
        }
        if (!bluetoothEnabled) {
            Log.w(TAG, "Bluetooth not available or disabled, skipping Remote ID scan")
            close()
            return@callbackFlow
        }

        val scanner = try {
            adapter?.bluetoothLeScanner
        } catch (e: SecurityException) {
            Log.w(TAG, "Bluetooth scan permission missing; Remote ID scan deferred")
            close()
            return@callbackFlow
        }
        if (scanner == null) {
            Log.w(TAG, "BLE scanner not available")
            close()
            return@callbackFlow
        }
        bleScanner = scanner

        val extendedAdvertisingSupported = try {
            adapter?.isLeExtendedAdvertisingSupported == true
        } catch (e: SecurityException) {
            false
        }
        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            // Receive extended advertisements on capable adapters while
            // retaining legacy-only compatibility on older controllers.
            .setLegacy(useLegacyRemoteIdScan(extendedAdvertisingSupported))
            .setReportDelay(0) // Immediate callback per result
            .build()

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                try {
                    val drone = processScanResult(result)
                    if (drone != null) {
                        if (trySend(drone).isFailure) {
                            recordDroppedUpdate()
                        }
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
                            if (trySend(drone).isFailure) {
                                recordDroppedUpdate()
                            }
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
        try {
            scanner.startScan(null, scanSettings, callback)
        } catch (e: SecurityException) {
            // The app lifecycle may reach this point while the startup runtime
            // permission dialog is still open. A missing optional radio
            // permission must disable this source, never terminate the app.
            Log.w(TAG, "Bluetooth scan permission missing; Remote ID scan deferred")
            activeScanCallback = null
            bleScanner = null
            close()
            return@callbackFlow
        } catch (e: IllegalStateException) {
            Log.w(TAG, "Bluetooth scanner is not ready; Remote ID scan skipped", e)
            activeScanCallback = null
            bleScanner = null
            close()
            return@callbackFlow
        }

        awaitClose {
            Log.i(TAG, "Stopping BLE Remote ID scan")
            try {
                scanner.stopScan(callback)
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping BLE scan", e)
            }
            payloadProcessor.clear()
        }
    }

    /** Aggregate overload warnings so a busy scanner cannot create a log storm. */
    private fun recordDroppedUpdate() {
        val nowMs = SystemClock.elapsedRealtime()
        synchronized(dropLogLock) {
            droppedSinceLastLog++
            if (lastDropLogAtMs == 0L || nowMs - lastDropLogAtMs >= 5_000L) {
                Log.w(
                    TAG,
                    "Dropped $droppedSinceLastLog Remote ID update(s) while the consumer was busy"
                )
                droppedSinceLastLog = 0
                lastDropLogAtMs = nowMs
            }
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
            payloadProcessor.clear()
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

        // Android has already stripped the service UUID. Preserve complete raw
        // or App Code-prefixed payloads, including 103-byte Message Packs.
        val selection = BleRemoteIdPayloadSelector.select(serviceData) ?: run {
            Log.d(TAG, "Invalid OpenDroneID service data: ${serviceData.size} bytes")
            return null
        }

        val now = Instant.now()
        val txPowerDbm = result.txPower.takeIf { it != ScanResult.TX_POWER_NOT_PRESENT }
        val estimatedDistanceMeters = RssiDistanceEstimator.estimateBleRemoteId(
            rssi = result.rssi,
            txPowerDbm = txPowerDbm
        )

        return payloadProcessor.process(
            deviceAddress = deviceAddress,
            payload = selection.payload,
            now = now,
            signalStrengthDbm = result.rssi,
            estimatedDistanceMeters = estimatedDistanceMeters,
            transportCounter = selection.transactionCounter,
            observationTimestampNanos = result.timestampNanos
        )
    }
}

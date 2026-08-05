package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import com.friendorfoe.domain.model.Drone
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.buffer
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch
import java.time.Instant
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton

internal fun useLegacyRemoteIdScan(extendedAdvertisingSupported: Boolean): Boolean =
    !extendedAdvertisingSupported

internal fun useAllSupportedRemoteIdPhy(
    extendedAdvertisingSupported: Boolean,
    codedPhySupported: Boolean,
): Boolean = extendedAdvertisingSupported && codedPhySupported

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
        private const val REMOTE_ID_INPUT_BUFFER_CAPACITY = 512
        private const val REMOTE_ID_OUTPUT_BUFFER_CAPACITY = 256
        private val OPEN_DRONE_ID_PARCEL_UUID =
            ParcelUuid(OpenDroneIdParser.OPEN_DRONE_ID_UUID)
    }

    private data class BleRemoteIdObservation(
        val deviceAddress: String,
        val serviceData: ByteArray,
        val rssi: Int,
        val txPowerDbm: Int?,
        val timestampNanos: Long,
    )

    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var activeScanCallback: ScanCallback? = null
    private val activeSessionLock = Any()
    private var activeSessionCloser: (() -> Unit)? = null
    private var stopEpoch = 0L
    private val dropLogLock = Any()
    private var droppedSinceLastLog = 0
    private var lastDropLogAtMs = 0L

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
        val startEpoch = synchronized(activeSessionLock) { stopEpoch }
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
        val extendedAdvertisingSupported = try {
            adapter?.isLeExtendedAdvertisingSupported == true
        } catch (e: SecurityException) {
            false
        }
        val codedPhySupported = try {
            adapter?.isLeCodedPhySupported == true
        } catch (e: SecurityException) {
            false
        }
        val scanSettingsBuilder = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            // Receive extended advertisements on capable adapters while
            // retaining legacy-only compatibility on older controllers.
            .setLegacy(useLegacyRemoteIdScan(extendedAdvertisingSupported))
            .setReportDelay(0) // Immediate callback per result
        if (useAllSupportedRemoteIdPhy(extendedAdvertisingSupported, codedPhySupported)) {
            scanSettingsBuilder.setPhy(ScanSettings.PHY_LE_ALL_SUPPORTED)
        }
        val scanSettings = scanSettingsBuilder.build()

        val frameGate = BleRemoteIdFrameGate()
        val payloadProcessor = BleRemoteIdPayloadProcessor()
        val observationQueue = Channel<BleRemoteIdObservation>(
            capacity = REMOTE_ID_INPUT_BUFFER_CAPACITY,
        )

        // Android dispatches ScanCallback on the main looper. Keep that path to
        // validation, exact-repeat suppression, and a bounded enqueue; parsing
        // and distance math stay ordered on one background worker.
        val processingJob = launch(Dispatchers.Default) {
            for (observation in observationQueue) {
                try {
                    val drone = processObservation(observation, payloadProcessor)
                    if (drone != null) send(drone)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing queued BLE Remote ID observation", e)
                }
            }
        }

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                try {
                    enqueueScanResult(result, observationQueue, frameGate)
                } catch (e: Exception) {
                    Log.e(TAG, "Error processing BLE scan result", e)
                }
            }

            override fun onBatchScanResults(results: List<ScanResult>) {
                for (result in results) {
                    try {
                        enqueueScanResult(result, observationQueue, frameGate)
                    } catch (e: Exception) {
                        Log.e(TAG, "Error processing batch BLE scan result", e)
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "BLE scan failed with error code: $errorCode")
            }
        }

        val sessionClosed = AtomicBoolean(false)
        val closeSession: () -> Unit = {
            if (sessionClosed.compareAndSet(false, true)) {
                Log.i(TAG, "Stopping BLE Remote ID scan")
                try {
                    scanner.stopScan(callback)
                } catch (e: Exception) {
                    Log.w(TAG, "Error stopping BLE scan", e)
                }
                observationQueue.close()
                processingJob.cancel()
                frameGate.clear()
                payloadProcessor.clear()
                close()
                synchronized(activeSessionLock) {
                    if (activeScanCallback === callback) {
                        activeScanCallback = null
                        activeSessionCloser = null
                    }
                }
            }
        }

        // Some phones miss ODID packets when service-data filtering is pushed
        // into the controller. Scan unfiltered and parse the FFFA payload here.
        Log.i(TAG, "Starting BLE Remote ID scan (unfiltered compatibility mode)")
        var stoppedBeforeStart = false
        try {
            synchronized(activeSessionLock) {
                if (stopEpoch != startEpoch) {
                    stoppedBeforeStart = true
                } else {
                    activeScanCallback = callback
                    activeSessionCloser = closeSession
                    scanner.startScan(null, scanSettings, callback)
                }
            }
            if (stoppedBeforeStart) {
                closeSession()
                return@callbackFlow
            }
        } catch (e: SecurityException) {
            // The app lifecycle may reach this point while the startup runtime
            // permission dialog is still open. A missing optional radio
            // permission must disable this source, never terminate the app.
            Log.w(TAG, "Bluetooth scan permission missing; Remote ID scan deferred")
            closeSession()
            return@callbackFlow
        } catch (e: IllegalStateException) {
            Log.w(TAG, "Bluetooth scanner is not ready; Remote ID scan skipped", e)
            closeSession()
            return@callbackFlow
        }

        awaitClose(closeSession)
    }.buffer(capacity = REMOTE_ID_OUTPUT_BUFFER_CAPACITY)

    /** Aggregate overload warnings so a busy scanner cannot create a log storm. */
    private fun recordDroppedUpdate() {
        val nowMs = SystemClock.elapsedRealtime()
        synchronized(dropLogLock) {
            droppedSinceLastLog++
            if (lastDropLogAtMs == 0L || nowMs - lastDropLogAtMs >= 5_000L) {
                Log.w(
                    TAG,
                    "Dropped $droppedSinceLastLog Remote ID frame(s) while the parser worker was busy"
                )
                droppedSinceLastLog = 0
                lastDropLogAtMs = nowMs
            }
        }
    }

    /** Stop scanning (for imperative callers; flow-based callers cancel the coroutine). */
    @SuppressLint("MissingPermission")
    fun stopScanning() {
        val closeSession = synchronized(activeSessionLock) {
            stopEpoch++
            activeSessionCloser
        }
        closeSession?.invoke()
    }

    private fun enqueueScanResult(
        result: ScanResult,
        observationQueue: Channel<BleRemoteIdObservation>,
        frameGate: BleRemoteIdFrameGate,
    ) {
        val serviceData = result.scanRecord?.getServiceData(
            OPEN_DRONE_ID_PARCEL_UUID
        ) ?: return
        if (serviceData.isEmpty()) return

        val descriptor = BleRemoteIdPayloadSelector.inspect(serviceData) ?: return
        val deviceAddress = result.device.address
        val observationTimestampNanos = result.timestampNanos.takeIf { it > 0L }
            ?: SystemClock.elapsedRealtimeNanos()
        val admission = frameGate.admit(
            deviceAddress = deviceAddress,
            serviceData = serviceData,
            descriptor = descriptor,
            observationTimestampNanos = observationTimestampNanos,
        ) ?: return
        val accepted = observationQueue.trySend(
            BleRemoteIdObservation(
                deviceAddress = deviceAddress,
                serviceData = admission.serviceData,
                rssi = result.rssi,
                txPowerDbm = result.txPower.takeIf {
                    it != ScanResult.TX_POWER_NOT_PRESENT
                },
                timestampNanos = observationTimestampNanos,
            )
        ).isSuccess
        if (!accepted) {
            // Let the advertiser's next redundant copy retry a temporarily full
            // worker queue instead of hiding the first sighting for this sweep.
            frameGate.rollback(admission)
            recordDroppedUpdate()
        }
    }

    private fun processObservation(
        observation: BleRemoteIdObservation,
        payloadProcessor: BleRemoteIdPayloadProcessor,
    ): Drone? {
        // Android has already stripped the service UUID. Preserve complete raw
        // or App Code-prefixed payloads, including standard Message Packs.
        val selection = BleRemoteIdPayloadSelector.select(observation.serviceData) ?: return null
        val drone = payloadProcessor.process(
            deviceAddress = observation.deviceAddress,
            payload = selection.payload,
            now = Instant.now(),
            signalStrengthDbm = observation.rssi,
            // RSSI distance uses pow(); calculate it only for observations that
            // actually produce a visible Drone below.
            estimatedDistanceMeters = null,
            transportCounter = selection.transactionCounter,
            observationTimestampNanos = observation.timestampNanos,
        ) ?: return null

        return drone.copy(
            estimatedDistanceMeters = RssiDistanceEstimator.estimateBleRemoteId(
                rssi = observation.rssi,
                txPowerDbm = observation.txPowerDbm,
            )
        )
    }
}

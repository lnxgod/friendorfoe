package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.util.Log
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Detected smart glasses or privacy-intrusion device.
 */
data class GlassesDetection(
    val mac: String,
    val deviceName: String?,
    val deviceType: String,
    val manufacturer: String,
    val hasCamera: Boolean,
    val rssi: Int,
    val confidence: Float,
    val matchReason: String,
    val firstSeen: Instant,
    val lastSeen: Instant
)

/**
 * BLE scanner that detects smart glasses, body cameras, and other
 * privacy-intrusion devices by matching BLE advertisements against
 * known manufacturer Company IDs, service UUIDs, and device name patterns.
 *
 * Matches the ESP32 glasses_detector.c database for consistency.
 */
@Singleton
class GlassesDetector @Inject constructor(
    private val bluetoothManager: BluetoothManager
) {
    companion object {
        private const val TAG = "GlassesDetector"
    }

    // ── Manufacturer Company ID database (from Bluetooth SIG) ──

    private data class MfrEntry(
        val companyId: Int,
        val manufacturer: String,
        val deviceType: String,
        val confidence: Float,
        val hasCamera: Boolean
    )

    private val mfrDatabase = listOf(
        MfrEntry(0x01AB, "Meta", "Smart Glasses", 0.90f, true),
        MfrEntry(0x058E, "Meta", "Smart Glasses", 0.90f, true),
        MfrEntry(0x03C2, "Snap", "Smart Glasses", 0.85f, true),
        MfrEntry(0x00E0, "Google", "Smart Glasses", 0.80f, true),
        MfrEntry(0x060C, "Vuzix", "Smart Glasses", 0.85f, true),
        MfrEntry(0x009E, "Bose", "Audio Glasses", 0.75f, false),
        MfrEntry(0x009F, "Bose", "Audio Glasses", 0.75f, false),
        MfrEntry(0x034D, "Axon", "Body Camera", 0.85f, true),
        MfrEntry(0x09B1, "Brilliant Labs", "Smart Glasses", 0.80f, true),
        MfrEntry(0x0BC6, "TCL", "Smart Glasses", 0.70f, true),
        MfrEntry(0x0962, "Rokid", "Smart Glasses", 0.75f, true),
        MfrEntry(0x0171, "Amazon", "Smart Glasses", 0.50f, false),
    )

    // ── 16-bit Service UUID database ──

    private data class UuidEntry(
        val uuid16: Int,
        val manufacturer: String,
        val deviceType: String,
        val confidence: Float,
        val hasCamera: Boolean
    )

    private val uuidDatabase = listOf(
        UuidEntry(0xFD5F, "Meta", "Smart Glasses", 0.95f, true),
        UuidEntry(0xFDD2, "Bose", "Audio Glasses", 0.85f, false),
        UuidEntry(0xFE45, "Snap", "Smart Glasses", 0.80f, true),
        UuidEntry(0xFE15, "Amazon", "Smart Glasses", 0.70f, false),
    )

    // ── Device name prefix database ──

    private data class NameEntry(
        val prefix: String,
        val manufacturer: String,
        val deviceType: String,
        val confidence: Float,
        val hasCamera: Boolean,
        val exact: Boolean = false
    )

    private val nameDatabase = listOf(
        NameEntry("RB Meta", "Meta", "Smart Glasses", 0.95f, true),
        NameEntry("Ray-Ban Stories", "Meta", "Smart Glasses", 0.95f, true),
        NameEntry("Spectacles", "Snap", "Smart Glasses", 0.90f, true),
        NameEntry("Echo Frames", "Amazon", "Smart Glasses", 0.90f, false, exact = true),
        NameEntry("Vuzix", "Vuzix", "Smart Glasses", 0.90f, true),
        NameEntry("XREAL", "Xreal", "Smart Glasses", 0.85f, true),
        NameEntry("Nreal", "Xreal", "Smart Glasses", 0.85f, true),
        NameEntry("Rokid", "Rokid", "Smart Glasses", 0.85f, true),
        NameEntry("RayNeo", "TCL", "Smart Glasses", 0.90f, true),
        NameEntry("Monocle", "Brilliant Labs", "Smart Glasses", 0.85f, true),
        NameEntry("Even Realities", "Even Realities", "Smart Glasses", 0.80f, true),
        NameEntry("INMO", "INMO", "Smart Glasses", 0.80f, true),
        NameEntry("IMA0", "INMO", "Smart Glasses", 0.80f, true),
        NameEntry("Solos AirGo", "Solos", "Smart Glasses", 0.80f, false),
        NameEntry("Glass", "Google", "Smart Glasses", 0.70f, true),
        NameEntry("Bose Frames", "Bose", "Audio Glasses", 0.90f, false),
        NameEntry("Axon Body", "Axon", "Body Camera", 0.90f, true),
        NameEntry("Axon Signal", "Axon", "Body Camera", 0.85f, true),
        NameEntry("V380_", "Generic", "Spy Camera", 0.75f, true),
        NameEntry("IPC_", "Generic", "Spy Camera", 0.70f, true),
        NameEntry("LookCam_", "Generic", "Spy Camera", 0.70f, true),
    )

    private val GAP_APPEARANCE_EYEGLASSES = 0x01C0

    private val detectedDevices = java.util.concurrent.ConcurrentHashMap<String, GlassesDetection>()
    private var bleScanner: BluetoothLeScanner? = null
    private var activeScanCallback: ScanCallback? = null

    /**
     * Start scanning for smart glasses / privacy devices.
     * Returns a Flow that emits GlassesDetection objects.
     */
    @SuppressLint("MissingPermission")
    fun startScanning(): Flow<GlassesDetection> = callbackFlow {
        val adapter = bluetoothManager.adapter
        if (adapter == null || !adapter.isEnabled) {
            Log.w(TAG, "Bluetooth not available, skipping glasses scan")
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

        // No scan filter — we need to see ALL BLE advertisements
        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setReportDelay(0)
            .build()

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val detection = checkScanResult(result)
                if (detection != null) {
                    trySend(detection)
                }
            }

            override fun onBatchScanResults(results: List<ScanResult>) {
                for (result in results) {
                    val detection = checkScanResult(result)
                    if (detection != null) {
                        trySend(detection)
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "BLE scan failed: $errorCode")
            }
        }

        activeScanCallback = callback
        Log.i(TAG, "Starting smart glasses BLE scan")
        scanner.startScan(null, scanSettings, callback)

        awaitClose {
            Log.i(TAG, "Stopping smart glasses BLE scan")
            try { scanner.stopScan(callback) } catch (e: Exception) {
                Log.w(TAG, "Error stopping glasses scan", e)
            }
            detectedDevices.clear()
        }
    }

    @SuppressLint("MissingPermission")
    fun stopScanning() {
        try {
            activeScanCallback?.let { cb -> bleScanner?.stopScan(cb) }
        } catch (e: Exception) {
            Log.w(TAG, "Error stopping glasses scan", e)
        } finally {
            activeScanCallback = null
            bleScanner = null
            detectedDevices.clear()
        }
    }

    private fun checkScanResult(result: ScanResult): GlassesDetection? {
        val mac = result.device.address
        val rssi = result.rssi
        val record = result.scanRecord ?: return null

        var bestConf = 0f
        var bestMfr = ""
        var bestType = ""
        var bestCamera = false
        var bestReason = ""

        // 1. Check manufacturer specific data
        val mfrData = record.manufacturerSpecificData
        if (mfrData != null) {
            for (i in 0 until mfrData.size()) {
                val companyId = mfrData.keyAt(i)
                val entry = mfrDatabase.find { it.companyId == companyId }
                if (entry != null && entry.confidence > bestConf) {
                    bestConf = entry.confidence
                    bestMfr = entry.manufacturer
                    bestType = entry.deviceType
                    bestCamera = entry.hasCamera
                    bestReason = "mfr_cid:0x${companyId.toString(16).uppercase().padStart(4, '0')}"
                }
            }
        }

        // 2. Check service UUIDs
        val serviceUuids = record.serviceUuids
        if (serviceUuids != null) {
            for (parcelUuid in serviceUuids) {
                val uuid = parcelUuid.uuid
                // Check if it's a 16-bit UUID (standard Bluetooth base UUID)
                val uuid16 = uuid.mostSignificantBits.ushr(32).toInt() and 0xFFFF
                val entry = uuidDatabase.find { it.uuid16 == uuid16 }
                if (entry != null && entry.confidence > bestConf) {
                    bestConf = entry.confidence
                    bestMfr = entry.manufacturer
                    bestType = entry.deviceType
                    bestCamera = entry.hasCamera
                    bestReason = "uuid16:0x${uuid16.toString(16).uppercase().padStart(4, '0')}"
                }
            }
        }

        // Also check service data UUIDs
        val serviceData = record.serviceData
        if (serviceData != null) {
            for ((parcelUuid, _) in serviceData) {
                val uuid = parcelUuid.uuid
                val uuid16 = uuid.mostSignificantBits.ushr(32).toInt() and 0xFFFF
                val entry = uuidDatabase.find { it.uuid16 == uuid16 }
                if (entry != null && entry.confidence > bestConf) {
                    bestConf = entry.confidence
                    bestMfr = entry.manufacturer
                    bestType = entry.deviceType
                    bestCamera = entry.hasCamera
                    bestReason = "svc_data:0x${uuid16.toString(16).uppercase().padStart(4, '0')}"
                }
            }
        }

        // 3. Check device name
        val deviceName = record.deviceName
        if (deviceName != null && deviceName.isNotEmpty()) {
            for (entry in nameDatabase) {
                val matches = if (entry.exact) {
                    deviceName.equals(entry.prefix, ignoreCase = true)
                } else {
                    deviceName.startsWith(entry.prefix, ignoreCase = true)
                }
                if (matches && entry.confidence > bestConf) {
                    bestConf = entry.confidence
                    bestMfr = entry.manufacturer
                    bestType = entry.deviceType
                    bestCamera = entry.hasCamera
                    bestReason = "name:${entry.prefix}"
                }
            }
        }

        if (bestConf < 0.50f) return null

        val now = Instant.now()
        val existing = detectedDevices[mac]
        val detection = GlassesDetection(
            mac = mac,
            deviceName = deviceName,
            deviceType = bestType,
            manufacturer = bestMfr,
            hasCamera = bestCamera,
            rssi = rssi,
            confidence = bestConf,
            matchReason = bestReason,
            firstSeen = existing?.firstSeen ?: now,
            lastSeen = now
        )
        detectedDevices[mac] = detection

        Log.i(TAG, "Detected $bestType ($bestMfr) RSSI=$rssi conf=$bestConf [$bestReason] cam=$bestCamera")
        return detection
    }
}

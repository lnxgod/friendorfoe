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
 * Category for privacy-threatening devices, used for tree view grouping.
 */
enum class PrivacyCategory(val label: String, val icon: String, val threatLevel: Int) {
    SMART_GLASSES("Smart Glasses", "\uD83D\uDC53", 3),
    BLE_TRACKER("BLE Trackers", "\uD83D\uDCCD", 3),
    HIDDEN_CAMERA("Hidden Cameras", "\uD83D\uDCF7", 3),
    BODY_CAMERA("Body Cameras", "\uD83D\uDCF9", 2),
    VEHICLE_CAMERA("Vehicle Cameras", "\uD83D\uDE97", 2),
    ATTACK_TOOL("Attack Tools", "\u26A0\uFE0F", 3),
    ACTION_CAMERA("Action Cameras", "\uD83C\uDFA5", 1),
    DASH_CAMERA("Dash Cameras", "\uD83D\uDE99", 1),
    IOT_DEVICE("IoT Devices", "\uD83C\uDFE0", 1),
    INFORMATIONAL("Informational", "\u2139\uFE0F", 0),
}

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
    val lastSeen: Instant,
    val details: Map<String, String> = emptyMap(),
    val category: PrivacyCategory = PrivacyCategory.INFORMATIONAL
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
    private val bluetoothManager: BluetoothManager,
    private val detectionPrefs: com.friendorfoe.data.DetectionPrefs
) {
    companion object {
        private const val TAG = "GlassesDetector"

        /** Suspicious WiFi SSID patterns that indicate hidden cameras, attack tools, or spy devices */
        private data class WifiPattern(
            val prefix: String,
            val manufacturer: String,
            val deviceType: String,
            val confidence: Float,
            val hasCamera: Boolean
        )

        private val wifiSsidPatterns = listOf(
            // Hidden cameras / spy cameras — app ecosystems
            WifiPattern("MV", "V380", "Hidden Camera", 0.85f, true),
            WifiPattern("V380-", "V380", "Hidden Camera", 0.85f, true),
            WifiPattern("YDXJ_", "YI", "IP Camera", 0.85f, true),
            WifiPattern("IPC-", "Generic", "IP Camera", 0.75f, true),
            WifiPattern("IP_CAM_", "Generic", "IP Camera", 0.80f, true),
            WifiPattern("IPCAM-", "Generic", "IP Camera", 0.75f, true),
            WifiPattern("HDWiFiCam", "Generic", "Hidden Camera", 0.85f, true),
            WifiPattern("CLOUDCAM", "Generic", "Hidden Camera", 0.80f, true),
            WifiPattern("HIDVCAM", "Generic", "Hidden Camera", 0.90f, true),
            WifiPattern("GW_AP", "Yoosee", "IP Camera", 0.75f, true),
            WifiPattern("JXLCAM", "Generic", "Spy Camera", 0.85f, true),
            WifiPattern("iCam-", "iCam365", "Hidden Camera", 0.80f, true),
            WifiPattern("iCam365-", "iCam365", "Hidden Camera", 0.80f, true),
            WifiPattern("XM-", "XMeye", "IP Camera", 0.80f, true),
            WifiPattern("CareCam-", "CareCam", "Hidden Camera", 0.80f, true),
            WifiPattern("BVCAM-", "BVCAM", "Hidden Camera", 0.80f, true),
            WifiPattern("P2PCam-", "P2PCam", "Hidden Camera", 0.80f, true),
            WifiPattern("TUTK-", "ThroughTek", "IP Camera", 0.75f, true),
            WifiPattern("SmartLife-", "Tuya", "IoT Camera", 0.70f, true),
            WifiPattern("TuyaSmart-", "Tuya", "IoT Camera", 0.70f, true),
            WifiPattern("AI_", "TinyCam", "Hidden Camera", 0.80f, true),
            WifiPattern("AIS", "TinyCam", "Hidden Camera", 0.75f, true),
            WifiPattern("DGK-", "SpyGear", "Hidden Camera", 0.80f, true),
            WifiPattern("WIFI-CAM", "Generic", "Hidden Camera", 0.75f, true),
            // "Cam-" removed — too generic
            // Smart home cameras (WiFi setup/AP mode)
            WifiPattern("Wyze_", "Wyze", "IP Camera", 0.80f, true),
            WifiPattern("Reolink_", "Reolink", "IP Camera", 0.80f, true),
            WifiPattern("Tapo_", "TP-Link", "IP Camera", 0.80f, true),
            WifiPattern("TAPO-", "TP-Link", "IP Camera", 0.80f, true),
            WifiPattern("Amcrest_", "Amcrest", "IP Camera", 0.80f, true),
            WifiPattern("Hik-", "Hikvision", "IP Camera", 0.85f, true),
            WifiPattern("HIKVISION_", "Hikvision", "IP Camera", 0.85f, true),
            WifiPattern("DH_", "Dahua", "IP Camera", 0.80f, true),
            WifiPattern("Eufy ", "Eufy", "IP Camera", 0.75f, true),
            WifiPattern("Blink-", "Amazon", "IP Camera", 0.80f, true),
            WifiPattern("Arlo-VMB", "Arlo", "IP Camera", 0.80f, true),
            WifiPattern("Furbo_", "Furbo", "Pet Camera", 0.80f, true),
            WifiPattern("Petcube", "Petcube", "Pet Camera", 0.80f, true),
            WifiPattern("Nanit_", "Nanit", "Baby Monitor", 0.75f, true),
            // Espressif-based DIY / cheap spy cameras
            WifiPattern("ESP-", "Espressif", "Possible Camera", 0.55f, true),
            WifiPattern("ESP32-", "Espressif", "Possible Camera", 0.55f, true),
            WifiPattern("AI-THINKER_", "AI-Thinker", "Possible Camera", 0.65f, true),
            // Endoscope / borescope cameras (peeping tools)
            WifiPattern("DEPSTECH_", "DEPSTECH", "Endoscope Camera", 0.85f, true),
            WifiPattern("Jetion_", "Jetion", "Endoscope Camera", 0.80f, true),
            WifiPattern("WiFi_Cam_", "Generic", "Endoscope Camera", 0.75f, true),
            // Trail / game cameras (outdoor surveillance)
            WifiPattern("REVEAL", "Tactacam", "Trail Camera", 0.80f, true),
            WifiPattern("4K WIFI CAM", "CamPark", "Trail Camera", 0.75f, true),
            // WiFi SD card adapters (data exfiltration)
            WifiPattern("ez Share", "ez Share", "WiFi SD Card", 0.70f, false),
            // Action cameras
            WifiPattern("GoPro", "GoPro", "Action Camera", 0.90f, true),
            WifiPattern("Insta360", "Insta360", "Action Camera", 0.90f, true),
            WifiPattern("OsmoAction", "DJI", "Action Camera", 0.90f, true),
            // Dash cameras
            WifiPattern("BlackVue", "BlackVue", "Dash Camera", 0.90f, true),
            WifiPattern("DR900", "BlackVue", "Dash Camera", 0.85f, true),
            WifiPattern("DR750", "BlackVue", "Dash Camera", 0.85f, true),
            WifiPattern("VIOFO_", "Viofo", "Dash Camera", 0.85f, true),
            WifiPattern("70mai_", "70mai", "Dash Camera", 0.85f, true),
            WifiPattern("Nextbase", "Nextbase", "Dash Camera", 0.85f, true),
            WifiPattern("Thinkware", "Thinkware", "Dash Camera", 0.85f, true),
            WifiPattern("DDPai_", "DDPai", "Dash Camera", 0.85f, true),
            WifiPattern("vYou_DDPai", "DDPai", "Dash Camera", 0.85f, true),
            WifiPattern("Garmin_VIRB", "Garmin", "Action Camera", 0.85f, true),
            WifiPattern("Akaso_", "Akaso", "Action Camera", 0.85f, true),
            WifiPattern("SJCAM_", "SJCAM", "Action Camera", 0.85f, true),
            WifiPattern("Rexing_", "Rexing", "Dash Camera", 0.85f, true),
            WifiPattern("APEMAN_", "Apeman", "Dash Camera", 0.80f, true),
            WifiPattern("YI-", "YI", "Dash Camera", 0.80f, true),
            // Body cameras
            WifiPattern("Axon Body", "Axon", "Body Camera", 0.90f, true),
            WifiPattern("WGVISTA", "Motorola", "Body Camera", 0.85f, true),
            // Attack tools
            WifiPattern("Pineapple", "Hak5", "Attack Tool", 0.90f, false),
            // Smart home cameras (setup mode)
            WifiPattern("Ring Setup", "Ring", "Doorbell Camera", 0.85f, true),
        )

        /**
         * Check a WiFi SSID for privacy-threatening device patterns.
         * @return GlassesDetection if match, null otherwise.
         */
        fun checkWifiSsid(ssid: String, bssid: String, rssi: Int): GlassesDetection? {
            for (pattern in wifiSsidPatterns) {
                if (ssid.startsWith(pattern.prefix, ignoreCase = true)) {
                    return GlassesDetection(
                        mac = bssid,
                        deviceName = ssid,
                        deviceType = pattern.deviceType,
                        manufacturer = pattern.manufacturer,
                        hasCamera = pattern.hasCamera,
                        rssi = rssi,
                        confidence = pattern.confidence,
                        matchReason = "wifi_ssid:${pattern.prefix}",
                        firstSeen = Instant.now(),
                        lastSeen = Instant.now()
                    )
                }
            }
            return null
        }
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
        // Smart Glasses
        MfrEntry(0x01AB, "Meta", "Smart Glasses", 0.90f, true),
        MfrEntry(0x058E, "Meta", "Smart Glasses", 0.90f, true),
        MfrEntry(0x03C2, "Snap", "Smart Glasses", 0.85f, true),
        // Google CID 0x00E0 removed — too broad, matches Nest/Chromecast/Pixel
        MfrEntry(0x060C, "Vuzix", "Smart Glasses", 0.85f, true),
        MfrEntry(0x009E, "Bose", "Audio Glasses", 0.75f, false),
        MfrEntry(0x009F, "Bose", "Audio Glasses", 0.75f, false),
        MfrEntry(0x034D, "Axon", "Body Camera", 0.85f, true),
        MfrEntry(0x09B1, "Brilliant Labs", "Smart Glasses", 0.80f, true),
        MfrEntry(0x0BC6, "TCL", "Smart Glasses", 0.70f, true),
        MfrEntry(0x0962, "Rokid", "Smart Glasses", 0.75f, true),
        // Amazon CID 0x0171 removed — too broad, matches Echo/Fire/Kindle/Ring
        // Trackers / Stalkerware
        MfrEntry(0x000D, "Tile", "BLE Tracker", 0.85f, false),
        MfrEntry(0x067C, "Tile", "BLE Tracker", 0.85f, false),
        // Samsung CID 0x0075 removed — matches ALL Samsung devices (phones, watches, buds)
        // SmartTag detected by UUID 0xFD5A/0xFD59 instead (much more specific)
        // IoT / Camera ecosystems
        MfrEntry(0x07D0, "Tuya", "IoT Camera", 0.65f, true),
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
        // Smart Glasses
        UuidEntry(0xFD5F, "Meta", "Smart Glasses", 0.95f, true),
        UuidEntry(0xFDD2, "Bose", "Audio Glasses", 0.85f, false),
        UuidEntry(0xFE45, "Snap", "Smart Glasses", 0.80f, true),
        UuidEntry(0xFE15, "Amazon", "Smart Glasses", 0.70f, false),
        // Trackers / Stalkerware
        UuidEntry(0xFD5A, "Samsung", "BLE Tracker", 0.90f, false),
        UuidEntry(0xFD59, "Samsung", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFEED, "Tile", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFEEC, "Tile", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFCB2, "DULT", "BLE Tracker", 0.90f, false),
        UuidEntry(0xFE2C, "Google", "Fast Pair", 0.65f, false),
        // Retail Tracking — informational only
        UuidEntry(0xFEAA, "Google", "Eddystone Beacon", 0.65f, false),
        // IoT ecosystems
        // Xiaomi UUID 0xFD2E removed — too broad, matches all Mi Home devices
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
        NameEntry("Glass EE", "Google", "Smart Glasses", 0.85f, true),
        NameEntry("Bose Frames", "Bose", "Audio Glasses", 0.90f, false),
        // Body cameras
        NameEntry("Axon Body", "Axon", "Body Camera", 0.90f, true),
        NameEntry("Axon Signal", "Axon", "Body Camera", 0.85f, true),
        NameEntry("VISTA_", "Motorola", "Body Camera", 0.85f, true),
        // Hidden cameras / spy cameras
        NameEntry("V380_", "Generic", "Spy Camera", 0.75f, true),
        NameEntry("IPC_", "Generic", "Spy Camera", 0.70f, true),
        NameEntry("LookCam_", "Generic", "Spy Camera", 0.70f, true),
        // "Camera-" removed — too generic, matches legitimate devices
        NameEntry("CLOUDCAM-", "Generic", "Spy Camera", 0.80f, true),
        NameEntry("HIDVCAM-", "Generic", "Hidden Camera", 0.90f, true),
        NameEntry("HDWiFiCam-", "Generic", "Hidden Camera", 0.85f, true),
        // Vehicles with cameras
        NameEntry("Tesla ", "Tesla", "Vehicle Camera", 0.90f, true),
        // Attack / hacking tools
        NameEntry("Flipper ", "Flipper Zero", "Attack Tool", 0.90f, false),
        // Trackers (BLE name-based)
        NameEntry("Tile ", "Tile", "BLE Tracker", 0.65f, false),
        NameEntry("SmartTag", "Samsung", "BLE Tracker", 0.80f, false),
        NameEntry("Chipolo", "Chipolo", "BLE Tracker", 0.85f, false),
        NameEntry("Pebblebee", "Pebblebee", "BLE Tracker", 0.85f, false),
        NameEntry("eufy SmartTrack", "Eufy", "BLE Tracker", 0.85f, false),
        NameEntry("Nutale", "Nutale", "BLE Tracker", 0.80f, false),
        // Camera accessories / remotes
        NameEntry("AB Shutter3", "Generic", "Camera Remote", 0.80f, false, exact = true),
        // Endoscope cameras (BLE pairing)
        NameEntry("DEPSTECH", "DEPSTECH", "Endoscope Camera", 0.85f, true),
        // Robot vacuums with cameras
        NameEntry("roborock-", "Roborock", "Robot Vacuum", 0.60f, true),
        NameEntry("iRobot-", "iRobot", "Robot Vacuum", 0.60f, true),
        NameEntry("Ecovacs-", "Ecovacs", "Robot Vacuum", 0.60f, true),
    )

    private val GAP_APPEARANCE_EYEGLASSES = 0x01C0

    private fun categorize(deviceType: String): PrivacyCategory = when {
        deviceType.contains("Glasses", ignoreCase = true) -> PrivacyCategory.SMART_GLASSES
        deviceType.contains("Audio Glasses", ignoreCase = true) -> PrivacyCategory.SMART_GLASSES
        deviceType.contains("Tracker", ignoreCase = true) -> PrivacyCategory.BLE_TRACKER
        deviceType.contains("AirTag", ignoreCase = true) -> PrivacyCategory.BLE_TRACKER
        deviceType.contains("Hidden Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("Spy Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("IP Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("Body Camera", ignoreCase = true) -> PrivacyCategory.BODY_CAMERA
        deviceType.contains("Vehicle", ignoreCase = true) -> PrivacyCategory.VEHICLE_CAMERA
        deviceType.contains("Attack", ignoreCase = true) -> PrivacyCategory.ATTACK_TOOL
        deviceType.contains("Action Camera", ignoreCase = true) -> PrivacyCategory.ACTION_CAMERA
        deviceType.contains("Dash Camera", ignoreCase = true) -> PrivacyCategory.DASH_CAMERA
        deviceType.contains("Endoscope", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("Robot Vacuum", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
        deviceType.contains("IoT", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
        deviceType.contains("Camera Remote", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
        deviceType.contains("Doorbell", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("Trail Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
        deviceType.contains("Beacon", ignoreCase = true) -> PrivacyCategory.INFORMATIONAL
        deviceType.contains("Fast Pair", ignoreCase = true) -> PrivacyCategory.INFORMATIONAL
        else -> PrivacyCategory.INFORMATIONAL
    }

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

    /** Ignore a MAC address — won't be reported again */
    fun ignoreDevice(mac: String) = detectionPrefs.ignoreMac(mac)

    /** Un-ignore a MAC address */
    fun unignoreDevice(mac: String) = detectionPrefs.unignoreMac(mac)

    private fun checkScanResult(result: ScanResult): GlassesDetection? {
        val mac = result.device.address

        // Skip ignored devices
        if (mac in detectionPrefs.getIgnoredMacs()) return null

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

        // 1b. Apple-specific: AirTag (FindMy type 0x12) and iBeacon (type 0x02)
        if (mfrData != null) {
            val appleData = mfrData.get(0x004C) // Apple Inc. Company ID
            if (appleData != null && appleData.size >= 3) {
                val appleType = appleData[0].toInt() and 0xFF
                if (appleType == 0x12 && appleData.size >= 3) {
                    // FindMy type 0x12 — but only flag as tracker if it looks like
                    // an actual AirTag/accessory. Check length byte = 0x19 (25 bytes)
                    // which indicates a FindMy accessory vs a phone relay.
                    val lengthByte = appleData[1].toInt() and 0xFF
                    if (lengthByte == 0x19 && appleData.size >= 27) {
                        // This is a FindMy accessory (AirTag, Chipolo, etc.)
                        val statusByte = appleData[2].toInt() and 0xFF
                        val separated = (statusByte and 0x20) == 0 // bit 5 = 0 means separated
                        if (separated) {
                            // Separated AirTag — potential stalker tracker
                            val c = 0.95f
                            if (c > bestConf) {
                                bestConf = c; bestMfr = "Apple"; bestType = "AirTag (Separated)"
                                bestCamera = false; bestReason = "airtag_separated"
                            }
                        } else {
                            // AirTag near its owner — low threat, informational only
                            val c = 0.65f
                            if (c > bestConf) {
                                bestConf = c; bestMfr = "Apple"; bestType = "AirTag (Near Owner)"
                                bestCamera = false; bestReason = "airtag_near_owner"
                            }
                        }
                    }
                    // Else: iPhone/iPad/Mac FindMy relay — skip entirely (not a tracker)
                } else if (appleType == 0x02 && appleData.size >= 21) {
                    // iBeacon (retail tracking) — informational
                    val c = 0.65f
                    if (c > bestConf) {
                        bestConf = c; bestMfr = "Apple"; bestType = "iBeacon"
                        bestCamera = false; bestReason = "ibeacon:0x02"
                    }
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

        if (bestConf < 0.60f) return null

        // Parse rich details from the raw packet
        val parsedDetails = BlePacketParser.parseAllDetails(result)

        // Assign category based on device type
        val category = categorize(bestType)

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
            lastSeen = now,
            details = parsedDetails,
            category = category
        )
        detectedDevices[mac] = detection

        Log.i(TAG, "Detected $bestType ($bestMfr) RSSI=$rssi conf=$bestConf [$bestReason] cam=$bestCamera details=$parsedDetails")
        return detection
    }
}

package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.util.Log
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Category for privacy-threatening devices, used for tree view grouping.
 */
enum class PrivacyCategory(val label: String, val icon: String, val threatLevel: Int) {
    // Threat level 3 — high privacy threat
    SMART_GLASSES("Smart Glasses", "\uD83D\uDC53", 3),
    HIDDEN_CAMERA("Hidden Cameras", "\uD83D\uDCF7", 3),
    ATTACK_TOOL("Attack Tools", "\u26A0\uFE0F", 3),
    ULTRASONIC_BEACON("Ultrasonic Beacons", "\uD83D\uDD0A", 3),
    RETAIL_TRACKER("Retail Trackers", "\uD83D\uDED2", 3),
    // Threat level 2 — awareness
    SURVEILLANCE_CAMERA("Surveillance Cameras", "\uD83D\uDCF9", 2),
    ALPR_CAMERA("ALPR / Plate Readers", "\uD83D\uDE94", 2),
    BODY_CAMERA("Body Cameras", "\uD83D\uDCF9", 2),
    VEHICLE_CAMERA("Vehicle Cameras", "\uD83D\uDE97", 2),
    BABY_MONITOR("Baby Monitors", "\uD83D\uDC76", 2),
    THERMAL_CAMERA("Thermal Cameras", "\uD83C\uDF21\uFE0F", 2),
    CONFERENCE_CAMERA("Conference Cameras", "\uD83C\uDFA5", 2),
    VIDEO_INTERCOM("Video Intercoms", "\uD83D\uDEAA", 2),
    // Threat level 1 — nearby devices
    DOORBELL_CAMERA("Doorbell Cameras", "\uD83D\uDEAA", 1),
    SMART_SPEAKER("Smart Speakers", "\uD83D\uDD0A", 1),
    SMART_HOME_HUB("Smart Home Hubs", "\uD83C\uDFE0", 1),
    SMART_LOCK("Smart Locks", "\uD83D\uDD12", 1),
    TRAIL_CAMERA("Trail Cameras", "\uD83C\uDF32", 1),
    OBD_TRACKER("OBD / Car Trackers", "\uD83D\uDD0C", 1),
    GPS_TRACKER("GPS Trackers", "\uD83D\uDCE1", 1),
    FLEET_DASHCAM("Fleet Dashcams", "\uD83D\uDE9A", 1),
    ACTION_CAMERA("Action Cameras", "\uD83C\uDFA5", 1),
    DASH_CAMERA("Dash Cameras", "\uD83D\uDE99", 1),
    BLE_TRACKER("BLE Trackers", "\uD83D\uDCCD", 1),
    IOT_DEVICE("IoT Devices", "\uD83D\uDCE1", 1),
    // Threat level 0 — informational
    SMART_TV("Smart TVs", "\uD83D\uDCFA", 0),
    DRONE_CONTROLLER("Drone Controllers", "\uD83C\uDFAE", 0),
    E_SCOOTER("E-Scooters", "\uD83D\uDEF4", 0),
    FINDMY("FindMy / AirTags", "\uD83D\uDCF1", 0),
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
    val category: PrivacyCategory = PrivacyCategory.INFORMATIONAL,
    /** True if detected from phone's bonded/paired device list (not BLE scan) */
    val isBonded: Boolean = false,
    // ── v0.59 BLE enrichment — populated when BlePacketParser.parseAdvertisement runs ──
    val bleCompanyId: Int? = null,
    val bleAppleType: Int? = null,
    val bleAppleFlags: Int? = null,
    val bleAppleAction: Int? = null,
    val bleAppleIosVersion: Int? = null,
    val bleAdvFlags: Int? = null,
    val bleDualModeHost: Boolean = false,
    val bleJa3Hash: UInt? = null,
    val bleServiceUuids: List<Int> = emptyList(),
    val bleAppearance: Int? = null,
    val bleLocalName: String? = null,
    /**
     * Stable identity across BT Private Resolvable Address (RPA) rotation.
     * For MAC-rotating high-risk classes (Meta Glasses, Ray-Ban, Oakley, Quest)
     * this is `fp:<mfr>|<type>|<ja3>|<uuids>|<name>` so one physical device stays
     * one detection as its RPA cycles. For stable-MAC devices (AirTags, most
     * generic BLE peripherals) it falls back to `mac:<addr>` so we never
     * accidentally collapse two separate physical trackers into one.
     */
    val fingerprintKey: String = "mac:$mac",
    /** All MACs seen for this fingerprint since firstSeen — diagnostic + UI badge. */
    val seenMacs: Set<String> = setOf(mac)
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

        /**
         * Derive a stable identity key for a BLE detection.
         *
         * MAC-rotating high-risk classes (Meta Glasses, Ray-Ban, Oakley,
         * Luxottica frames, Meta Quest) are keyed on
         * `fp:<mfr>|<type>|<ja3>|<uuids>|<name-prefix>` — bytes that don't
         * change when the device's Private Resolvable Address cycles. This
         * collapses many MACs from one physical pair into a single detection.
         *
         * Everything else falls back to `mac:<addr>` so we never collide two
         * distinct physical peripherals (e.g. two AirTags) into one row.
         *
         * Exposed in the companion so unit tests can exercise it without
         * standing up a full ScanResult.
         */
        fun computeFingerprintKey(
            mac: String,
            bestMfr: String,
            bestType: String,
            ja3: UInt?,
            serviceUuids16: List<Int>?,
            deviceName: String?
        ): String {
            val m = bestMfr
            val t = bestType
            val isRotating = m.contains("Meta", ignoreCase = true) ||
                m.contains("Ray-Ban", ignoreCase = true) ||
                m.contains("Oakley", ignoreCase = true) ||
                m.contains("Luxottica", ignoreCase = true) ||
                t.contains("Quest", ignoreCase = true) ||
                t.contains("Smart Glasses", ignoreCase = true)
            if (!isRotating) return "mac:$mac"
            val ja3Part = ja3?.toString(16) ?: "noja3"
            val uuidPart = serviceUuids16
                ?.sorted()
                ?.joinToString(",") { it.toString(16) }
                .orEmpty()
            val namePart = deviceName?.take(16).orEmpty()
            return "fp:$m|$t|$ja3Part|$uuidPart|$namePart"
        }

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
            WifiPattern("Ring-", "Ring", "Doorbell Camera", 0.80f, true),
            WifiPattern("SimpliSafe-", "SimpliSafe", "Surveillance Camera", 0.80f, true),
            WifiPattern("BLINK-", "Blink", "Surveillance Camera", 0.80f, true),
            WifiPattern("Nest-", "Google", "Surveillance Camera", 0.75f, true),
            // Surveillance / ALPR cameras
            WifiPattern("Verkada-", "Verkada", "Surveillance Camera", 0.90f, true),
            WifiPattern("Rhombus-", "Rhombus", "Surveillance Camera", 0.85f, true),
            WifiPattern("Flock-", "Flock Safety", "ALPR Camera", 0.85f, true),
            WifiPattern("FLK-", "Flock Safety", "ALPR Camera", 0.85f, true),
            WifiPattern("ELSAG-", "Leonardo", "ALPR Camera", 0.85f, true),
            // Smart speakers / hubs (setup AP mode)
            WifiPattern("Sonos_", "Sonos", "Smart Speaker", 0.80f, false),
            WifiPattern("Google-Home-", "Google", "Smart Home Hub", 0.75f, false),
            // Baby monitors (setup AP)
            WifiPattern("OwletCam-", "Owlet", "Baby Monitor", 0.85f, true),
            WifiPattern("Miku-", "Miku", "Baby Monitor", 0.85f, true),
            WifiPattern("VTech_", "VTech", "Baby Monitor", 0.80f, true),
            WifiPattern("Hubble-", "Hubble", "Baby Monitor", 0.80f, true),
            WifiPattern("CuboAi-", "CuboAi", "Baby Monitor", 0.85f, true),
            WifiPattern("Lollipop-", "Lollipop", "Baby Monitor", 0.80f, true),
            WifiPattern("iBaby-", "iBaby", "Baby Monitor", 0.80f, true),
            // More camera setup APs
            WifiPattern("EZVIZ_", "EZVIZ", "Surveillance Camera", 0.80f, true),
            WifiPattern("Lorex_", "Lorex", "Surveillance Camera", 0.80f, true),
            WifiPattern("ZOSI_", "ZOSI", "Surveillance Camera", 0.75f, true),
            WifiPattern("Swann-SWIFI-", "Swann", "Surveillance Camera", 0.85f, true),
            WifiPattern("Annke_", "Annke", "Surveillance Camera", 0.75f, true),
            WifiPattern("RemoBellS_", "Remo+", "Doorbell Camera", 0.80f, true),
            WifiPattern("RemoBellW_", "Remo+", "Doorbell Camera", 0.80f, true),
            // Chinese IP cameras
            WifiPattern("IPCAM-", "Generic", "Hidden Camera", 0.80f, true),
            WifiPattern("Care-AP", "CareCam", "Hidden Camera", 0.80f, true),
            WifiPattern("Danale-", "Danale", "Hidden Camera", 0.80f, true),
            // Trail cameras
            WifiPattern("Spypoint-", "Spypoint", "Trail Camera", 0.85f, true),
            // Deauther / attack tools
            WifiPattern("pwned", "Spacehuhn", "Attack Tool", 0.95f, false),
            WifiPattern("Advanced-Deauther", "Generic", "Attack Tool", 0.95f, false),
        )

        /** Assign a privacy category based on device type string. Used by both BLE and WiFi paths. */
        fun categorizeDeviceType(deviceType: String): PrivacyCategory = when {
            deviceType.contains("Glasses", ignoreCase = true) -> PrivacyCategory.SMART_GLASSES
            deviceType.contains("Audio Glasses", ignoreCase = true) -> PrivacyCategory.SMART_GLASSES
            // AirTag / FindMy separated into their own low-priority section
            deviceType.contains("AirTag", ignoreCase = true) -> PrivacyCategory.FINDMY
            deviceType.contains("FindMy", ignoreCase = true) -> PrivacyCategory.FINDMY
            // ALPR / license plate readers
            deviceType.contains("ALPR", ignoreCase = true) -> PrivacyCategory.ALPR_CAMERA
            deviceType.contains("Plate Reader", ignoreCase = true) -> PrivacyCategory.ALPR_CAMERA
            // Surveillance / security cameras
            deviceType.contains("Surveillance", ignoreCase = true) -> PrivacyCategory.SURVEILLANCE_CAMERA
            deviceType.contains("Security Camera", ignoreCase = true) -> PrivacyCategory.SURVEILLANCE_CAMERA
            deviceType.contains("Doorbell Camera", ignoreCase = true) -> PrivacyCategory.DOORBELL_CAMERA
            deviceType.contains("Doorbell", ignoreCase = true) -> PrivacyCategory.DOORBELL_CAMERA
            deviceType.contains("Police Camera", ignoreCase = true) -> PrivacyCategory.BODY_CAMERA
            deviceType.contains("Tracker", ignoreCase = true) -> PrivacyCategory.BLE_TRACKER
            deviceType.contains("Hidden Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
            deviceType.contains("Spy Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
            deviceType.contains("IP Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
            deviceType.contains("Body Camera", ignoreCase = true) -> PrivacyCategory.BODY_CAMERA
            deviceType.contains("Vehicle", ignoreCase = true) -> PrivacyCategory.VEHICLE_CAMERA
            deviceType.contains("Attack", ignoreCase = true) -> PrivacyCategory.ATTACK_TOOL
            deviceType.contains("Action Camera", ignoreCase = true) -> PrivacyCategory.ACTION_CAMERA
            deviceType.contains("Dash Camera", ignoreCase = true) -> PrivacyCategory.DASH_CAMERA
            deviceType.contains("Endoscope", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
            // Ultrasonic beacons
            deviceType.contains("Ultrasonic", ignoreCase = true) -> PrivacyCategory.ULTRASONIC_BEACON
            // Retail tracking
            deviceType.contains("Retail Tracker", ignoreCase = true) -> PrivacyCategory.RETAIL_TRACKER
            deviceType.contains("Retail Beacon", ignoreCase = true) -> PrivacyCategory.RETAIL_TRACKER
            deviceType.contains("Location Beacon", ignoreCase = true) -> PrivacyCategory.RETAIL_TRACKER
            // Conference cameras
            deviceType.contains("Conference", ignoreCase = true) -> PrivacyCategory.CONFERENCE_CAMERA
            deviceType.contains("Meeting Camera", ignoreCase = true) -> PrivacyCategory.CONFERENCE_CAMERA
            // Video intercoms
            deviceType.contains("Video Intercom", ignoreCase = true) -> PrivacyCategory.VIDEO_INTERCOM
            deviceType.contains("Intercom", ignoreCase = true) -> PrivacyCategory.VIDEO_INTERCOM
            // Fleet dashcams
            deviceType.contains("Fleet Dashcam", ignoreCase = true) -> PrivacyCategory.FLEET_DASHCAM
            deviceType.contains("Fleet Camera", ignoreCase = true) -> PrivacyCategory.FLEET_DASHCAM
            // Baby monitors
            deviceType.contains("Baby Monitor", ignoreCase = true) -> PrivacyCategory.BABY_MONITOR
            // Thermal cameras
            deviceType.contains("Thermal", ignoreCase = true) -> PrivacyCategory.THERMAL_CAMERA
            // Trail / game cameras
            deviceType.contains("Trail Camera", ignoreCase = true) -> PrivacyCategory.TRAIL_CAMERA
            deviceType.contains("Game Camera", ignoreCase = true) -> PrivacyCategory.TRAIL_CAMERA
            // OBD / car trackers
            deviceType.contains("OBD", ignoreCase = true) -> PrivacyCategory.OBD_TRACKER
            deviceType.contains("Car Tracker", ignoreCase = true) -> PrivacyCategory.OBD_TRACKER
            // GPS trackers
            deviceType.contains("GPS Tracker", ignoreCase = true) -> PrivacyCategory.GPS_TRACKER
            deviceType.contains("Pet Tracker", ignoreCase = true) -> PrivacyCategory.GPS_TRACKER
            // Smart speakers & home hubs (always-listening)
            deviceType.contains("Smart Speaker", ignoreCase = true) -> PrivacyCategory.SMART_SPEAKER
            deviceType.contains("Smart Home Hub", ignoreCase = true) -> PrivacyCategory.SMART_HOME_HUB
            // Smart locks (physical security)
            deviceType.contains("Smart Lock", ignoreCase = true) -> PrivacyCategory.SMART_LOCK
            // Smart TVs (ACR tracking)
            deviceType.contains("Smart TV", ignoreCase = true) -> PrivacyCategory.SMART_TV
            // Drone controllers
            deviceType.contains("Drone Controller", ignoreCase = true) -> PrivacyCategory.DRONE_CONTROLLER
            // E-Scooters
            deviceType.contains("E-Scooter", ignoreCase = true) -> PrivacyCategory.E_SCOOTER
            deviceType.contains("Robot Vacuum", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
            deviceType.contains("IoT", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
            deviceType.contains("Camera Remote", ignoreCase = true) -> PrivacyCategory.IOT_DEVICE
            deviceType.contains("Trail Camera", ignoreCase = true) -> PrivacyCategory.HIDDEN_CAMERA
            deviceType.contains("Beacon", ignoreCase = true) -> PrivacyCategory.INFORMATIONAL
            deviceType.contains("Fast Pair", ignoreCase = true) -> PrivacyCategory.INFORMATIONAL
            else -> PrivacyCategory.INFORMATIONAL
        }

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
                        lastSeen = Instant.now(),
                        category = categorizeDeviceType(pattern.deviceType)
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
        // Meta — 0x01AB is general Meta, 0x058E is Meta Technologies (Quest headsets)
        MfrEntry(BleSignatures.CID_META, "Meta", "Smart Glasses", 0.90f, true),
        MfrEntry(BleSignatures.CID_META_TECH, "Meta", "VR Headset", 0.90f, true),  // Quest 2/3/Pro
        // Luxottica — frame-manufacturer CID on every Ray-Ban Meta / Oakley Meta unit.
        // This is the headline Meta-detection fix: Marauder matches on this CID,
        // firmware v0.58 matches on this CID, and the Android app did NOT until v0.59.
        MfrEntry(BleSignatures.CID_LUXOTTICA, "Meta", "Smart Glasses", 0.95f, true),
        // Flipper Zero — attack / hacking tool
        MfrEntry(BleSignatures.CID_FLIPPER, "Flipper Zero", "Attack Tool", 0.95f, false),
        MfrEntry(BleSignatures.CID_SNAP, "Snap", "Smart Glasses", 0.85f, true),
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
        // Wearables with cameras/sensors
        MfrEntry(0x0B1A, "Humane", "AI Pin", 0.85f, true),
        MfrEntry(0x0E3C, "Rabbit", "AI Device", 0.70f, false),
        MfrEntry(0x0B14, "XREAL", "Smart Glasses", 0.85f, true),
        // Action cameras
        MfrEntry(0x0413, "DJI", "Action Camera", 0.80f, true),
        MfrEntry(0x08A9, "Insta360", "Action Camera", 0.85f, true),
        // IoT / Camera ecosystems
        MfrEntry(0x07D0, "Tuya", "IoT Camera", 0.65f, true),
        MfrEntry(0x02D3, "Ring", "Doorbell Camera", 0.75f, true),
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
        // Meta devices
        UuidEntry(0xFD5F, "Meta", "Smart Glasses", 0.95f, true),  // Ray-Ban Meta
        UuidEntry(0xFEB8, "Meta", "VR Headset", 0.90f, true),     // Quest companion
        UuidEntry(0xFEB7, "Meta", "Meta Device", 0.85f, true),    // General Meta
        UuidEntry(0xFDD2, "Bose", "Audio Glasses", 0.85f, false),
        UuidEntry(0xFE45, "Snap", "Smart Glasses", 0.80f, true),
        UuidEntry(0xFE15, "Amazon", "Smart Glasses", 0.70f, false),
        // Trackers / Stalkerware
        UuidEntry(0xFD5A, "Samsung", "BLE Tracker", 0.90f, false),
        UuidEntry(0xFD59, "Samsung", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFEED, "Tile", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFEEC, "Tile", "BLE Tracker", 0.85f, false),
        UuidEntry(0xFCB2, "DULT", "BLE Tracker", 0.90f, false),
        UuidEntry(0xFE2C, "Google", "Fast Pair", 0.50f, false), // below threshold
        // Action cameras
        UuidEntry(0xFEA6, "GoPro", "Action Camera", 0.90f, true),
        // Samsung wearables
        UuidEntry(0xFD6A, "Samsung", "Galaxy Ring", 0.85f, false),
        UuidEntry(0xFD69, "Samsung", "Galaxy Ring", 0.85f, false),
        // Retail Tracking — below threshold, informational only
        UuidEntry(0xFEAA, "Google", "Eddystone Beacon", 0.50f, false),
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
        NameEntry("Ray-Ban Meta", "Meta", "Smart Glasses", 0.95f, true),
        NameEntry("Ray-Ban Stories", "Meta", "Smart Glasses", 0.95f, true),
        NameEntry("Oakley Meta", "Meta", "Smart Glasses", 0.95f, true),
        NameEntry("Meta Neural", "Meta", "Smart Glasses", 0.90f, false),
        NameEntry("Meta Quest", "Meta", "VR Headset", 0.95f, true),
        NameEntry("Quest 2", "Meta", "VR Headset", 0.95f, true),
        NameEntry("Quest 3", "Meta", "VR Headset", 0.95f, true),
        NameEntry("Quest Pro", "Meta", "VR Headset", 0.95f, true),
        NameEntry("Oculus Quest", "Meta", "VR Headset", 0.95f, true),
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
        NameEntry("Humane", "Humane", "AI Pin", 0.90f, true),
        NameEntry("Vision Pro", "Apple", "VR Headset", 0.95f, true),
        NameEntry("Galaxy Ring", "Samsung", "Smart Ring", 0.85f, false),
        NameEntry("Solos AirGo", "Solos", "Smart Glasses", 0.80f, false),
        NameEntry("Glass EE", "Google", "Smart Glasses", 0.85f, true),
        NameEntry("Bose Frames", "Bose", "Audio Glasses", 0.90f, false),
        // Body cameras
        NameEntry("Axon Body", "Axon", "Body Camera", 0.90f, true),
        NameEntry("Axon Signal", "Axon", "Body Camera", 0.85f, true),
        NameEntry("VISTA_", "Motorola", "Body Camera", 0.85f, true),
        // Surveillance / security cameras (BLE setup mode)
        NameEntry("Nest Cam", "Google", "Surveillance Camera", 0.85f, true),
        NameEntry("Nest Hello", "Google", "Doorbell Camera", 0.85f, true),
        NameEntry("Nest Doorbell", "Google", "Doorbell Camera", 0.85f, true),
        NameEntry("Arlo ", "Arlo", "Surveillance Camera", 0.80f, true),
        NameEntry("Wyze Cam", "Wyze", "Surveillance Camera", 0.80f, true),
        NameEntry("eufy Indoor", "Eufy", "Surveillance Camera", 0.80f, true),
        NameEntry("eufy Doorbell", "Eufy", "Doorbell Camera", 0.80f, true),
        NameEntry("eufy Floodlight", "Eufy", "Surveillance Camera", 0.80f, true),
        NameEntry("SimpliSafe", "SimpliSafe", "Surveillance Camera", 0.75f, true),
        NameEntry("Verkada", "Verkada", "Surveillance Camera", 0.90f, true),
        NameEntry("Rhombus", "Rhombus", "Surveillance Camera", 0.85f, true),
        NameEntry("Reolink", "Reolink", "Surveillance Camera", 0.75f, true),
        // ALPR / license plate readers
        NameEntry("Flock", "Flock Safety", "ALPR Camera", 0.90f, true),
        NameEntry("FLK-", "Flock Safety", "ALPR Camera", 0.85f, true),
        NameEntry("ELSAG", "Leonardo", "ALPR Camera", 0.90f, true),
        NameEntry("AutoVu", "Genetec", "ALPR Camera", 0.85f, true),
        NameEntry("Vigilant", "Motorola", "ALPR Camera", 0.80f, true),
        // Police / fleet cameras
        NameEntry("Axon Fleet", "Axon", "Police Camera", 0.90f, true),
        NameEntry("WatchGuard", "Motorola", "Police Camera", 0.85f, true),
        // Action cameras (bonded for remote control)
        NameEntry("GoPro ", "GoPro", "Action Camera", 0.90f, true),
        NameEntry("GoPro Hero", "GoPro", "Action Camera", 0.90f, true),
        NameEntry("GoPro MAX", "GoPro", "Action Camera", 0.90f, true),
        NameEntry("Insta360", "Insta360", "Action Camera", 0.90f, true),
        NameEntry("ONE X", "Insta360", "Action Camera", 0.85f, true),
        NameEntry("Osmo Action", "DJI", "Action Camera", 0.90f, true),
        NameEntry("DJI Action", "DJI", "Action Camera", 0.90f, true),
        NameEntry("Osmo Pocket", "DJI", "Action Camera", 0.90f, true),
        // Audio recorders / smart pens
        NameEntry("ICD-", "Sony", "Voice Recorder", 0.85f, false),
        NameEntry("PCM-", "Sony", "Voice Recorder", 0.85f, false),
        NameEntry("Livescribe", "Livescribe", "Smart Pen", 0.85f, false),
        // OBD2 / vehicle trackers (bonded for data)
        NameEntry("OBDII", "Generic", "OBD Tracker", 0.75f, false),
        NameEntry("OBD2", "Generic", "OBD Tracker", 0.75f, false),
        NameEntry("ELM327", "Generic", "OBD Tracker", 0.75f, false),
        NameEntry("OBDLink", "OBDLink", "OBD Tracker", 0.80f, false),
        NameEntry("Vgate", "Vgate", "OBD Tracker", 0.75f, false),
        // GPS trackers (bonded for config)
        NameEntry("Tracki", "Tracki", "GPS Tracker", 0.90f, false),
        NameEntry("Spytec", "Spytec", "GPS Tracker", 0.90f, false),
        // Drones (bonded for QuickTransfer/app pairing/RID)
        NameEntry("DJI Mini", "DJI", "Drone", 0.90f, true),
        NameEntry("DJI-Mini", "DJI", "Drone", 0.90f, true),
        NameEntry("DJI-Mavic", "DJI", "Drone", 0.90f, true),
        NameEntry("DJI-Avata", "DJI", "Drone", 0.90f, true),
        NameEntry("DJI Neo", "DJI", "Drone", 0.90f, true),
        NameEntry("DJI RC", "DJI", "Drone Controller", 0.90f, true),
        NameEntry("DJI Goggles", "DJI", "FPV Goggles", 0.90f, true),
        NameEntry("RC-N1", "DJI", "Drone Controller", 0.85f, true),
        NameEntry("RC Pro", "DJI", "Drone Controller", 0.85f, true),
        NameEntry("Autel-", "Autel", "Drone", 0.85f, true),
        NameEntry("EVO-", "Autel", "Drone", 0.85f, true),
        NameEntry("Anafi", "Parrot", "Drone", 0.85f, true),
        NameEntry("Skycontroller", "Parrot", "Drone Controller", 0.85f, true),
        NameEntry("Skydio", "Skydio", "Drone", 0.90f, true),
        NameEntry("FIMI-", "FIMI", "Drone", 0.80f, true),
        NameEntry("Tello", "Ryze", "Drone", 0.80f, true),
        // Smart locks (bonded = access to physical location)
        NameEntry("August", "August", "Smart Lock", 0.85f, false),
        NameEntry("Yale-", "Yale", "Smart Lock", 0.85f, false),
        NameEntry("Yale Assure", "Yale", "Smart Lock", 0.85f, false),
        NameEntry("Schlage", "Schlage", "Smart Lock", 0.85f, false),
        // Vehicle digital keys
        NameEntry("Tesla ", "Tesla", "Vehicle Key", 0.90f, true),
        // Personal / child / pet trackers
        NameEntry("Jiobit", "Jiobit", "Child Tracker", 0.85f, false),
        NameEntry("Tractive", "Tractive", "Pet Tracker", 0.75f, false),
        NameEntry("Whistle", "Whistle", "Pet Tracker", 0.75f, false),
        // FPV radio controllers
        NameEntry("ExpressLRS", "ELRS", "FPV Controller", 0.80f, false),
        NameEntry("TBS Crossfire", "TBS", "FPV Controller", 0.80f, false),
        // Dash cameras — many advertise BLE continuously for app wake-up
        NameEntry("Garmin Dash", "Garmin", "Dash Camera", 0.90f, true),
        NameEntry("DC 67W", "Garmin", "Dash Camera", 0.90f, true),
        NameEntry("DC Mini", "Garmin", "Dash Camera", 0.90f, true),
        NameEntry("DC Live", "Garmin", "Dash Camera", 0.85f, true),
        NameEntry("DC Tandem", "Garmin", "Dash Camera", 0.90f, true),  // interior cabin cam
        NameEntry("Nexar", "Nexar", "Dash Camera", 0.90f, true),      // continuous BLE advertiser
        NameEntry("nexarOne", "Nexar", "Dash Camera", 0.90f, true),
        NameEntry("BlackVue", "BlackVue", "Dash Camera", 0.90f, true), // continuous BLE advertiser
        NameEntry("Thinkware", "Thinkware", "Dash Camera", 0.85f, true),
        NameEntry("U3000", "Thinkware", "Dash Camera", 0.85f, true),
        NameEntry("Vantrue", "Vantrue", "Dash Camera", 0.85f, true),
        NameEntry("VIOFO", "VIOFO", "Dash Camera", 0.80f, true),
        NameEntry("70mai", "70mai", "Dash Camera", 0.80f, true),
        NameEntry("Rexing", "Rexing", "Dash Camera", 0.75f, true),
        NameEntry("Owlcam", "Owl", "Dash Camera", 0.85f, true),       // continuous BLE advertiser
        NameEntry("DDPai", "DDPai", "Dash Camera", 0.75f, true),
        NameEntry("DDP", "DDPai", "Dash Camera", 0.75f, true),
        // Fleet / commercial vehicle cameras
        NameEntry("Samsara", "Samsara", "Fleet Camera", 0.85f, true),  // continuous BLE
        NameEntry("Dash-", "Lytx", "Fleet Camera", 0.75f, true),      // Surfsight/DriveCam
        NameEntry("Motive", "Motive", "Fleet Camera", 0.80f, true),
        NameEntry("Ring Car", "Ring", "Car Camera", 0.85f, true),
        // Rideshare tablet cameras
        NameEntry("Vugo", "Vugo", "Rideshare Display", 0.80f, true),
        // Hearing aids (bonded, have microphones — medical privacy)
        NameEntry("Phonak", "Phonak", "Hearing Aid", 0.90f, false),
        NameEntry("R-Phonak", "Phonak", "Hearing Aid", 0.90f, false),
        NameEntry("L-Phonak", "Phonak", "Hearing Aid", 0.90f, false),
        NameEntry("ReSound", "ReSound", "Hearing Aid", 0.85f, false),
        NameEntry("Oticon", "Oticon", "Hearing Aid", 0.85f, false),
        NameEntry("Starkey", "Starkey", "Hearing Aid", 0.85f, false),
        NameEntry("Signia", "Signia", "Hearing Aid", 0.85f, false),
        NameEntry("Widex", "Widex", "Hearing Aid", 0.85f, false),
        NameEntry("Cochlear", "Cochlear", "Cochlear Implant", 0.90f, false),
        // Medical devices (bonded — extreme privacy)
        NameEntry("DXCM", "Dexcom", "CGM Sensor", 0.90f, false),
        NameEntry("Dexcom", "Dexcom", "CGM Sensor", 0.90f, false),
        NameEntry("NovoPen", "Novo Nordisk", "Insulin Pen", 0.85f, false),
        NameEntry("InPen", "Medtronic", "Insulin Pen", 0.85f, false),
        // Smart rings
        NameEntry("Oura", "Oura", "Smart Ring", 0.85f, false),
        NameEntry("Ultrahuman", "Ultrahuman", "Smart Ring", 0.80f, false),
        // Satellite communicators (bonded, GPS tracking)
        NameEntry("inReach", "Garmin", "Satellite Tracker", 0.90f, false),
        NameEntry("ZOLEO", "Zoleo", "Satellite Tracker", 0.85f, false),
        // Adult / intimate devices (EXTREME privacy)
        NameEntry("Lovense", "Lovense", "Personal Device", 0.90f, false),
        NameEntry("Lush", "Lovense", "Personal Device", 0.85f, false),
        NameEntry("We-Vibe", "We-Vibe", "Personal Device", 0.90f, false),
        // Smart gun safes (reveals firearm presence)
        NameEntry("VAULTEK", "Vaultek", "Gun Safe", 0.90f, false),
        // E-bikes / scooters
        NameEntry("VanMoof", "VanMoof", "E-Bike", 0.80f, false),
        NameEntry("Cowboy", "Cowboy", "E-Bike", 0.80f, false),
        NameEntry("Ninebot", "Segway", "E-Scooter", 0.80f, false),
        NameEntry("Segway", "Segway", "E-Scooter", 0.80f, false),
        // Smart pens (capture handwriting)
        NameEntry("Neo Smartpen", "NeoLAB", "Smart Pen", 0.85f, false),
        NameEntry("Pen+", "Moleskine", "Smart Pen", 0.80f, false),
        // Smart displays with cameras
        NameEntry("Echo Show", "Amazon", "Smart Display", 0.85f, true),
        NameEntry("Portal", "Meta", "Smart Display", 0.80f, true),
        // Smart home (bonded during setup)
        NameEntry("Ring Setup", "Ring", "Doorbell Camera", 0.80f, true),
        NameEntry("Echo-", "Amazon", "Smart Speaker", 0.70f, false),
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
        // Smart speakers (always-listening)
        NameEntry("Sonos Move", "Sonos", "Smart Speaker", 0.85f, false),
        NameEntry("Sonos Roam", "Sonos", "Smart Speaker", 0.85f, false),
        NameEntry("Sonos ", "Sonos", "Smart Speaker", 0.75f, false),
        // Smart home hubs (always-listening microphones)
        NameEntry("Google Home", "Google", "Smart Home Hub", 0.80f, false),
        NameEntry("Google Nest", "Google", "Smart Home Hub", 0.80f, false),
        NameEntry("Nest Mini", "Google", "Smart Home Hub", 0.85f, false),
        NameEntry("Nest Audio", "Google", "Smart Home Hub", 0.85f, false),
        NameEntry("Nest Hub", "Google", "Smart Home Hub", 0.85f, false),
        NameEntry("HomePod", "Apple", "Smart Home Hub", 0.85f, false),
        // Smart locks (physical security)
        NameEntry("August ", "August", "Smart Lock", 0.70f, false),
        NameEntry("Yale ", "Yale", "Smart Lock", 0.70f, false),
        NameEntry("Schlage", "Schlage", "Smart Lock", 0.75f, false),
        NameEntry("Kwikset", "Kwikset", "Smart Lock", 0.70f, false),
        NameEntry("Level Lock", "Level", "Smart Lock", 0.75f, false),
        // Baby monitors (camera + microphone, always-on)
        NameEntry("Owlet", "Owlet", "Baby Monitor", 0.85f, true),
        NameEntry("Miku-", "Miku", "Baby Monitor", 0.85f, true),
        NameEntry("CuboAi-", "CuboAi", "Baby Monitor", 0.85f, true),
        NameEntry("Lollipop-", "Lollipop", "Baby Monitor", 0.80f, true),
        NameEntry("iBaby-", "iBaby", "Baby Monitor", 0.80f, true),
        NameEntry("Hubble-", "Hubble", "Baby Monitor", 0.75f, true),
        // More security cameras (BLE setup)
        NameEntry("EZVIZ_", "EZVIZ", "Surveillance Camera", 0.80f, true),
        NameEntry("Lorex_", "Lorex", "Surveillance Camera", 0.80f, true),
        NameEntry("ZOSI_", "ZOSI", "Surveillance Camera", 0.75f, true),
        NameEntry("Swann", "Swann", "Surveillance Camera", 0.80f, true),
        NameEntry("Annke_", "Annke", "Surveillance Camera", 0.75f, true),
        // More doorbells
        NameEntry("RemoBell", "Remo+", "Doorbell Camera", 0.80f, true),
        NameEntry("EZVIZ DB", "EZVIZ", "Doorbell Camera", 0.80f, true),
        NameEntry("Circle-", "Logitech", "Doorbell Camera", 0.80f, true),
        // OBD2 / car diagnostic dongles (vehicle tracking potential)
        NameEntry("ELM327", "Generic", "OBD Tracker", 0.80f, false),
        NameEntry("VEEPEAK", "Veepeak", "OBD Tracker", 0.85f, false),
        NameEntry("BlueDriver", "BlueDriver", "OBD Tracker", 0.85f, false),
        NameEntry("FIXD", "Fixd", "OBD Tracker", 0.85f, false),
        NameEntry("OBDLink", "OBDLink", "OBD Tracker", 0.85f, false),
        NameEntry("Carly", "Carly", "OBD Tracker", 0.70f, false),
        NameEntry("OBDII", "Generic", "OBD Tracker", 0.75f, false),
        // GPS trackers (location surveillance)
        NameEntry("Tracki_", "Tracki", "GPS Tracker", 0.85f, false),
        NameEntry("Bouncie_", "Bouncie", "GPS Tracker", 0.85f, false),
        NameEntry("Invoxia_", "Invoxia", "GPS Tracker", 0.85f, false),
        NameEntry("LandAirSea", "LandAirSea", "GPS Tracker", 0.85f, false),
        NameEntry("Spytec_", "Spytec", "GPS Tracker", 0.85f, false),
        // Pet trackers (location tracking)
        NameEntry("Whistle_", "Whistle", "Pet Tracker", 0.80f, false),
        NameEntry("Fi_", "Fi", "Pet Tracker", 0.75f, false),
        NameEntry("Tractive_", "Tractive", "Pet Tracker", 0.80f, false),
        NameEntry("Jiobit_", "Jiobit", "Pet Tracker", 0.80f, false),
        NameEntry("PitPat_", "PitPat", "Pet Tracker", 0.75f, false),
        // Thermal cameras (can see through walls / in dark)
        NameEntry("FLIR-ONE-", "FLIR", "Thermal Camera", 0.90f, true),
        NameEntry("FLIR-Edge-", "FLIR", "Thermal Camera", 0.90f, true),
        NameEntry("Seek-", "Seek Thermal", "Thermal Camera", 0.85f, true),
        NameEntry("SeekThermal", "Seek Thermal", "Thermal Camera", 0.85f, true),
        NameEntry("InfiRay-", "InfiRay", "Thermal Camera", 0.85f, true),
        // Trail / game cameras (outdoor surveillance)
        NameEntry("Spypoint-", "Spypoint", "Trail Camera", 0.85f, true),
        NameEntry("Bushnell-", "Bushnell", "Trail Camera", 0.80f, true),
        NameEntry("Moultrie-", "Moultrie", "Trail Camera", 0.80f, true),
        NameEntry("Reconyx-", "Reconyx", "Trail Camera", 0.85f, true),
        NameEntry("Stealth-", "Stealth Cam", "Trail Camera", 0.80f, true),
        // Smart TVs (ACR tracking, microphones)
        NameEntry("[TV] Samsung", "Samsung", "Smart TV", 0.85f, false),
        NameEntry("[LG] webOS TV", "LG", "Smart TV", 0.85f, false),
        NameEntry("BRAVIA", "Sony", "Smart TV", 0.80f, false),
        NameEntry("Vizio SmartCast", "Vizio", "Smart TV", 0.80f, false),
        NameEntry("Roku TV", "Roku", "Smart TV", 0.75f, false),
        // DJI drone controllers (drone operator nearby)
        NameEntry("DJI-RC-", "DJI", "Drone Controller", 0.90f, false),
        NameEntry("DJI-RC Pro", "DJI", "Drone Controller", 0.90f, false),
        // Retail / location tracking beacons
        NameEntry("Estimote", "Estimote", "Retail Beacon", 0.85f, false),
        NameEntry("Kontakt", "Kontakt.io", "Retail Beacon", 0.85f, false),
        NameEntry("Gimbal", "Gimbal", "Retail Beacon", 0.80f, false),
        NameEntry("Radius", "Radius Networks", "Retail Beacon", 0.80f, false),
        NameEntry("RetailNext", "RetailNext", "Retail Tracker", 0.90f, false),
        NameEntry("Footfall", "FootfallCam", "Retail Tracker", 0.85f, false),
        NameEntry("V-Count", "V-Count", "Retail Tracker", 0.85f, false),
        NameEntry("Density ", "Density", "Retail Tracker", 0.80f, false),
        NameEntry("VergeSense", "VergeSense", "Retail Tracker", 0.85f, false),
        NameEntry("XY Find", "XY Sense", "Retail Tracker", 0.80f, false),
        // Conference / meeting room cameras (always-on mics + cameras)
        NameEntry("Meeting Owl", "Owl Labs", "Conference Camera", 0.90f, true),
        NameEntry("Owl Pro", "Owl Labs", "Conference Camera", 0.90f, true),
        NameEntry("Owl 3", "Owl Labs", "Conference Camera", 0.90f, true),
        NameEntry("Poly Studio", "Poly", "Conference Camera", 0.85f, true),
        NameEntry("Rally", "Logitech", "Conference Camera", 0.80f, true),
        NameEntry("MeetUp", "Logitech", "Conference Camera", 0.85f, true),
        NameEntry("RoomMate", "Logitech", "Conference Camera", 0.85f, true),
        NameEntry("Neat Bar", "Neat", "Conference Camera", 0.85f, true),
        NameEntry("Neat Board", "Neat", "Conference Camera", 0.85f, true),
        NameEntry("Cisco Webex", "Cisco", "Conference Camera", 0.85f, true),
        // Video intercoms (building access with cameras)
        NameEntry("DoorBird", "DoorBird", "Video Intercom", 0.85f, true),
        NameEntry("ButterflyMX", "ButterflyMX", "Video Intercom", 0.85f, true),
        NameEntry("Akuvox", "Akuvox", "Video Intercom", 0.80f, true),
        NameEntry("2N ", "2N", "Video Intercom", 0.75f, true),
        // Fleet / AI dashcams
        NameEntry("Samsara", "Samsara", "Fleet Dashcam", 0.85f, true),
        NameEntry("Motive", "Motive", "Fleet Dashcam", 0.85f, true),
        NameEntry("KeepTruckin", "Motive", "Fleet Dashcam", 0.85f, true),
        NameEntry("Geotab", "Geotab", "Fleet Dashcam", 0.80f, false),
        NameEntry("Verizon Hum", "Verizon", "Fleet Dashcam", 0.80f, true),
        NameEntry("Zubie", "Zubie", "Fleet Dashcam", 0.75f, false),
        NameEntry("Lytx", "Lytx", "Fleet Dashcam", 0.85f, true),
        // Insurance telematics (car tracking)
        NameEntry("Snapshot", "Progressive", "OBD Tracker", 0.85f, false),
        NameEntry("Drivewise", "Allstate", "OBD Tracker", 0.85f, false),
        NameEntry("Drive Safe", "State Farm", "OBD Tracker", 0.80f, false),
        // E-Scooters (location tracking, informational)
        NameEntry("Lime-", "Lime", "E-Scooter", 0.80f, false),
        NameEntry("Bird ", "Bird", "E-Scooter", 0.75f, false),
        NameEntry("Lyft Scooter", "Lyft", "E-Scooter", 0.80f, false),
        NameEntry("Spin ", "Spin", "E-Scooter", 0.70f, false),
    )

    private val GAP_APPEARANCE_EYEGLASSES = 0x01C0

    private fun categorize(deviceType: String): PrivacyCategory = categorizeDeviceType(deviceType)

    private val detectedDevices = java.util.concurrent.ConcurrentHashMap<String, GlassesDetection>()
    /** MAC addresses of devices bonded to THIS phone — used to tag "your device" vs "nearby threat" */
    private val myBondedAddresses = java.util.concurrent.CopyOnWriteArraySet<String>()
    private var bleScanner: BluetoothLeScanner? = null
    private var activeScanCallback: ScanCallback? = null
    @Volatile var isScanRunning = false
        private set

    /** Clear all cached detections — used by refresh button in UI */
    fun clearAllDetections() {
        detectedDevices.clear()
        Log.i(TAG, "All cached detections cleared (manual refresh)")
    }

    /**
     * Start scanning for smart glasses / privacy devices.
     * Returns a Flow that emits GlassesDetection objects.
     *
     * Includes automatic retry on scan failure and periodic scan restart
     * to catch devices with slow advertising intervals (e.g. Meta glasses
     * that are already paired and connected reduce their BLE advertising
     * to ~10s+ intervals — easy to miss on initial scan start).
     */
    @SuppressLint("MissingPermission")
    fun startScanning(): Flow<GlassesDetection> = callbackFlow {
        val SCAN_RESTART_INTERVAL_MS = 15_000L // Restart scan every 15s — resets Android dedup
        val SCAN_FAILURE_RETRY_MS = 5_000L     // Wait 5s before retrying after failure
        val MAX_RETRIES = 3

        var retryCount = 0

        val scanSettings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .setReportDelay(0)
            .build()

        fun buildScanCallback(): ScanCallback = object : ScanCallback() {
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
                val errorName = when (errorCode) {
                    SCAN_FAILED_ALREADY_STARTED -> "ALREADY_STARTED"
                    SCAN_FAILED_APPLICATION_REGISTRATION_FAILED -> "REGISTRATION_FAILED"
                    SCAN_FAILED_FEATURE_UNSUPPORTED -> "UNSUPPORTED"
                    SCAN_FAILED_INTERNAL_ERROR -> "INTERNAL_ERROR"
                    else -> "UNKNOWN($errorCode)"
                }
                Log.e(TAG, "BLE scan failed: $errorName")
                isScanRunning = false

                // ALREADY_STARTED means a scan is running — that's actually fine
                if (errorCode == SCAN_FAILED_ALREADY_STARTED) {
                    isScanRunning = true
                }
            }
        }

        suspend fun doStartScan(): Boolean {
            val adapter = bluetoothManager.adapter
            if (adapter == null || !adapter.isEnabled) {
                Log.w(TAG, "Bluetooth not available, skipping glasses scan")
                return false
            }

            val scanner = adapter.bluetoothLeScanner
            if (scanner == null) {
                Log.w(TAG, "BLE scanner not available")
                return false
            }
            bleScanner = scanner

            val callback = buildScanCallback()

            // Stop any previous scan before starting a new one
            activeScanCallback?.let { old ->
                try { scanner.stopScan(old) } catch (_: Exception) {}
            }

            activeScanCallback = callback
            try {
                scanner.startScan(null, scanSettings, callback)
                isScanRunning = true
                retryCount = 0
                Log.i(TAG, "BLE privacy scan started successfully")
                return true
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start BLE scan: ${e.message}")
                isScanRunning = false
                return false
            }
        }

        /**
         * Overlap-rotate the scanner to defeat Android's opaque per-ad-data
         * dedup cache without creating a blind spot.
         *
         * The old "stop then start" approach left a ~100 ms window in which
         * advertisements were dropped — a Meta pair with 2–10 s ad cadence
         * could be missed for a full cycle. Instead: start a second scan,
         * wait 2 s for it to deliver, then stop the old one. Android allows
         * up to 4 concurrent app-level scans, so this is safe.
         *
         * Fingerprint-keyed merging in checkScanResult() absorbs the brief
         * double-delivery window into a single detection per physical device.
         */
        suspend fun rotateScannerOverlap() {
            val scanner = bleScanner ?: run { doStartScan(); return }
            val oldCallback = activeScanCallback
            val newCallback = buildScanCallback()
            try {
                scanner.startScan(null, scanSettings, newCallback)
            } catch (e: Exception) {
                Log.w(TAG, "Overlap scan start failed, falling back to sequential", e)
                doStartScan()
                return
            }
            activeScanCallback = newCallback
            isScanRunning = true
            delay(2000)
            oldCallback?.let {
                try { scanner.stopScan(it) } catch (_: Exception) {}
            }
        }

        // Initial start with retry
        var started = false
        while (!started && retryCount < MAX_RETRIES) {
            started = doStartScan()
            if (!started) {
                retryCount++
                Log.w(TAG, "BLE scan start failed, retry $retryCount/$MAX_RETRIES in ${SCAN_FAILURE_RETRY_MS}ms")
                delay(SCAN_FAILURE_RETRY_MS)
            }
        }

        if (!started) {
            Log.e(TAG, "BLE scan failed after $MAX_RETRIES retries")
            close()
            return@callbackFlow
        }

        // Check bonded/paired BLE devices — catches glasses that are already
        // Build set of YOUR bonded device addresses — used to tag BLE scan
        // results as "your device" vs "someone else's nearby device"
        fun refreshBondedList() {
            try {
                val adapter = bluetoothManager.adapter ?: return
                myBondedAddresses.clear()
                adapter.bondedDevices?.forEach { myBondedAddresses.add(it.address) }
            } catch (_: SecurityException) {}
        }
        refreshBondedList()

        // Check ACTIVELY CONNECTED devices — Android suppresses BLE ads from
        // devices connected to THIS phone, so the scanner never sees them.
        // This catches YOUR Meta glasses, Quest, etc. that are connected right now.
        suspend fun checkConnectedDevices() {
            try {
                val profiles = listOf(
                    android.bluetooth.BluetoothProfile.GATT,
                    android.bluetooth.BluetoothProfile.HEADSET,
                    7  // A2DP = BluetoothProfile.A2DP
                )
                val connectedDevices = mutableMapOf<String, android.bluetooth.BluetoothDevice>()
                for (profile in profiles) {
                    try {
                        bluetoothManager.getConnectedDevices(profile).forEach { device ->
                            connectedDevices[device.address] = device
                        }
                    } catch (_: Exception) {}
                }

                for ((address, device) in connectedDevices) {
                    val name = device.name ?: continue
                    val nameMatch = nameDatabase.firstOrNull { entry ->
                        if (entry.exact) name.equals(entry.prefix, ignoreCase = true)
                        else name.contains(entry.prefix, ignoreCase = true)
                    }
                    if (nameMatch != null && nameMatch.confidence >= 0.70f) {
                        val key = "connected_${address}"
                        val now = java.time.Instant.now()
                        val existing = detectedDevices[key]
                        val detection = GlassesDetection(
                            mac = address,
                            deviceName = name,
                            manufacturer = nameMatch.manufacturer,
                            deviceType = nameMatch.deviceType,
                            confidence = nameMatch.confidence,
                            rssi = -30, // Connected = very close
                            hasCamera = nameMatch.hasCamera,
                            matchReason = "connected:$name",
                            category = categorize(nameMatch.deviceType),
                            firstSeen = existing?.firstSeen ?: now,
                            lastSeen = now,
                            isBonded = true // It's YOUR connected device
                        )
                        detectedDevices[key] = detection
                        trySend(detection)
                        if (existing == null) {
                            Log.i(TAG, "Connected device detected: $name (${nameMatch.deviceType})")
                        }
                    }
                }

                // Remove stale connected entries (device disconnected)
                val connectedAddrs = connectedDevices.keys.map { "connected_$it" }.toSet()
                detectedDevices.keys.filter { it.startsWith("connected_") && it !in connectedAddrs }
                    .forEach { key ->
                        val det = detectedDevices[key]
                        val age = det?.let { java.time.Duration.between(it.lastSeen, java.time.Instant.now()).seconds } ?: 999
                        if (age > 30) {
                            detectedDevices.remove(key)
                            Log.d(TAG, "Connected device gone: ${det?.deviceName}")
                        }
                    }
            } catch (e: SecurityException) {
                Log.w(TAG, "No permission to check connected devices")
            }
        }

        // Check connected devices immediately
        checkConnectedDevices()

        // Periodic: restart BLE scan + check connected devices + Classic BT discovery
        val restartJob = CoroutineScope(Dispatchers.Default).launch {
            while (true) {
                delay(SCAN_RESTART_INTERVAL_MS)
                refreshBondedList()
                checkConnectedDevices()
                try {
                    val adapter = bluetoothManager.adapter
                    if (adapter?.isDiscovering == false) {
                        adapter.startDiscovery()
                    }
                } catch (_: SecurityException) {}
                Log.d(TAG, "Scan cycle: ${myBondedAddresses.size} bonded, ${detectedDevices.size} detected")
                rotateScannerOverlap()
            }
        }

        awaitClose {
            Log.i(TAG, "Stopping smart glasses BLE scan")
            isScanRunning = false
            restartJob.cancel()
            try {
                activeScanCallback?.let { cb -> bleScanner?.stopScan(cb) }
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping glasses scan", e)
            }
            activeScanCallback = null
            bleScanner = null
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
                    // iBeacon (retail tracking) — below threshold, filtered
                    val c = 0.50f
                    if (c > bestConf) {
                        bestConf = c; bestMfr = "Apple"; bestType = "iBeacon"
                        bestCamera = false; bestReason = "ibeacon:0x02"
                    }
                }
            }
        }

        // 2. Check service UUIDs
        // Helper: extract 16-bit UUID only if it's a standard Bluetooth SIG base UUID
        fun extractUuid16(uuid: java.util.UUID): Int? {
            // Bluetooth SIG base: 0000XXXX-0000-1000-8000-00805F9B34FB
            if (uuid.leastSignificantBits != -0x7FFFFF7FA64CB4FDL) return null
            if ((uuid.mostSignificantBits and 0xFFFFFFFFL) != 0x00001000L) return null
            return uuid.mostSignificantBits.ushr(32).toInt() and 0xFFFF
        }

        val serviceUuids = record.serviceUuids
        if (serviceUuids != null) {
            for (parcelUuid in serviceUuids) {
                val uuid16 = extractUuid16(parcelUuid.uuid) ?: continue
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
                val uuid16 = extractUuid16(parcelUuid.uuid) ?: continue
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

        // 3. Check device name (try scan record first, fall back to cached system name)
        @android.annotation.SuppressLint("MissingPermission")
        val deviceName = record.deviceName?.takeIf { it.isNotEmpty() } ?: result.device.name
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
        val parsedDetails = BlePacketParser.parseAllDetails(result).toMutableMap()

        // Full v0.59 advertisement walk — Apple / Microsoft deep decode,
        // service UUIDs, advertising flags, appearance, local name.
        val adv = BlePacketParser.parseAdvertisement(result)

        // Prefer the Apple enriched label when available — folds in
        // "AirPods in, Watch paired" / "Wi-Fi Password Share" / iOS version.
        adv.apple?.let { ac ->
            ac.flagLabel()?.let { parsedDetails["Apple State"] = it }
            ac.nearbyActionSubType?.let { sub ->
                BleSignatures.nearbyActionName(sub)?.let { parsedDetails["Apple Action"] = it }
            }
            ac.iosVersionNibble?.let { if (it >= 10) parsedDetails["iOS"] = it.toString() }
            ac.authTag?.let {
                parsedDetails["Auth Tag"] = "%02x%02x%02x".format(
                    it[0].toInt() and 0xFF, it[1].toInt() and 0xFF, it[2].toInt() and 0xFF)
            }
        }
        adv.microsoft?.label?.let { parsedDetails["Swift Pair"] = it }
        if (adv.advFlags != null) {
            parsedDetails["Dual-mode BT"] = if (adv.dualModeHost) "yes" else "no"
        }
        adv.appearance?.takeIf { it != 0 }?.let {
            parsedDetails["Appearance"] = "0x%04x".format(it)
        }

        // Assign category based on device type
        val category = categorize(bestType)

        val now = Instant.now()
        // Tag whether this is YOUR device or someone else's
        val isMyDevice = myBondedAddresses.contains(mac)

        // Compute BLE-JA3 structural hash — same output as the firmware for
        // the same advertisement, so future backend-forwarding can correlate.
        val ja3 = BleFeatureExtractor.computeJa3Hash(
            result,
            addrType = if (result.device.type == android.bluetooth.BluetoothDevice.DEVICE_TYPE_LE) 1 else 0,
            props = 0
        )

        // Stable identity key across RPA rotation for high-risk classes.
        // Unknown/low-signal devices keep MAC keying so we never collapse
        // two distinct physical peripherals into one row.
        val fingerprintKey = computeFingerprintKey(
            mac = mac,
            bestMfr = bestMfr,
            bestType = bestType,
            ja3 = ja3,
            serviceUuids16 = adv.serviceUuids16,
            deviceName = deviceName
        )

        val existing = detectedDevices[fingerprintKey]
        val mergedMacs = (existing?.seenMacs ?: emptySet()) + mac
        if (existing != null && mac !in existing.seenMacs) {
            Log.i(TAG, "RPA rotation: $bestMfr $bestType added MAC $mac " +
                "(now ${mergedMacs.size} MACs, fp=$fingerprintKey)")
        }

        val detection = GlassesDetection(
            mac = mac,
            deviceName = deviceName,
            deviceType = bestType,
            manufacturer = bestMfr,
            hasCamera = bestCamera,
            rssi = rssi,
            confidence = bestConf,
            matchReason = if (isMyDevice) "own_device:$bestReason" else bestReason,
            firstSeen = existing?.firstSeen ?: now,
            lastSeen = now,
            details = parsedDetails.toMap(),
            category = category,
            isBonded = isMyDevice,
            bleCompanyId = adv.companyId,
            bleAppleType = adv.apple?.subType,
            bleAppleFlags = adv.apple?.flagsByte,
            bleAppleAction = adv.apple?.nearbyActionSubType,
            bleAppleIosVersion = adv.apple?.iosVersionNibble,
            bleAdvFlags = adv.advFlags,
            bleDualModeHost = adv.dualModeHost,
            bleJa3Hash = ja3,
            bleServiceUuids = adv.serviceUuids16,
            bleAppearance = adv.appearance,
            bleLocalName = adv.localName,
            fingerprintKey = fingerprintKey,
            seenMacs = mergedMacs
        )
        detectedDevices[fingerprintKey] = detection

        Log.i(TAG, "Detected $bestType ($bestMfr) RSSI=$rssi conf=$bestConf [$bestReason] cam=$bestCamera details=$parsedDetails")
        return detection
    }
}

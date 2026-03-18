package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.util.Log
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import dagger.hilt.android.qualifiers.ApplicationContext
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.pow

/**
 * WiFi SSID pattern scanner for detecting older or non-compliant drones.
 *
 * Scans nearby WiFi networks and matches SSIDs against known drone manufacturer
 * naming patterns. This is a low-confidence detection method since any device
 * could broadcast a matching SSID, but it catches older drones that lack
 * FAA Remote ID compliance.
 *
 * Respects Android WiFi scan throttling (4 scans per 2 minutes in foreground).
 * Estimates rough distance from signal strength using log-distance path loss model.
 *
 * Known SSID patterns:
 * - DJI-* (Mavic, Mini, Phantom, Inspire, etc.)
 * - TELLO-* (Ryze/DJI Tello)
 * - SKYDIO-* (Skydio 2, X2, etc.)
 * - MAVIC-* (some DJI models broadcast as MAVIC-*)
 * - PARROT-* (Parrot Anafi, etc.)
 * - AUTEL-* (Autel EVO, etc.)
 */
@Singleton
class WifiDroneScanner @Inject constructor(
    @ApplicationContext private val context: Context,
    private val wifiManager: WifiManager
) {

    companion object {
        private const val TAG = "WifiDroneScanner"

        /**
         * RSSI reference at 1 meter distance (dBm).
         * Typical for WiFi direct/hotspot from a drone at close range.
         */
        private const val RSSI_REF = -40

        /**
         * Path loss exponent for outdoor line-of-sight propagation.
         * 2.0 = free space, 2.5 = light obstruction typical of outdoor drone use.
         */
        private const val PATH_LOSS_EXPONENT = 2.5

        /** Maximum scans allowed within the throttle window (Android limit). */
        private const val MAX_SCANS_IN_WINDOW = 4

        /** Throttle window duration in milliseconds (2 minutes). */
        private const val THROTTLE_WINDOW_MS = 2 * 60 * 1000L

        /** Interval between scan attempts in milliseconds (30 seconds). */
        private const val SCAN_INTERVAL_MS = 30_000L

        /**
         * Known drone SSID patterns mapped to manufacturer names.
         * Patterns are matched as prefixes (case-insensitive).
         */
        private val DRONE_SSID_PATTERNS = listOf(
            // Major consumer brands - DJI
            DronePattern("DJI-", "DJI"),
            DronePattern("TELLO-", "Ryze/DJI"),
            DronePattern("MAVIC-", "DJI"),
            DronePattern("PHANTOM-", "DJI"),
            DronePattern("INSPIRE-", "DJI"),
            DronePattern("MINI SE-", "DJI"),
            DronePattern("MINI2-", "DJI"),
            DronePattern("MINI3-", "DJI"),
            DronePattern("MINI4-", "DJI"),
            DronePattern("SPARK-", "DJI"),
            DronePattern("FPV-", "DJI"),
            DronePattern("AVATA-", "DJI"),
            DronePattern("AGRAS-", "DJI"),         // DJI Agras agricultural drones
            DronePattern("MATRICE-", "DJI"),       // DJI Matrice enterprise series
            DronePattern("AIR 2S-", "DJI"),        // DJI Air 2S
            DronePattern("AIR2-", "DJI"),          // DJI Air 2
            DronePattern("FLIP-", "DJI"),          // DJI Flip
            DronePattern("DJI NEO-", "DJI"),       // DJI Neo
            // Skydio / Parrot / Autel
            DronePattern("SKYDIO-", "Skydio"),
            DronePattern("PARROT-", "Parrot"),
            DronePattern("ANAFI-", "Parrot"),
            DronePattern("BEBOP-", "Parrot"),
            DronePattern("DISCO-", "Parrot"),
            DronePattern("ARDRONE-", "Parrot"),
            DronePattern("AUTEL-", "Autel"),
            DronePattern("EVO-", "Autel"),
            // HOVERAir (Zero Zero Robotics) - X1, X1 Pro, etc.
            DronePattern("HOVERAIR", "HOVERAir"),
            DronePattern("HOVER AIR", "HOVERAir"),
            DronePattern("HOVER_AIR", "HOVERAir"),
            DronePattern("HOVER-AIR", "HOVERAir"),
            DronePattern("HOVERAir", "HOVERAir"),
            DronePattern("HOVER X1", "HOVERAir"),
            DronePattern("HOVER-X1", "HOVERAir"),
            DronePattern("HOVER_X1", "HOVERAir"),
            DronePattern("X1PRO", "HOVERAir"),
            DronePattern("X1-PRO", "HOVERAir"),
            DronePattern("X1 PRO", "HOVERAir"),
            // HOVER- removed: too generic, redundant with 8 more specific HOVER patterns above
            // Holy Stone
            DronePattern("HOLY", "Holy Stone"),
            DronePattern("HS-", "Holy Stone"),
            // Other known brands
            DronePattern("SIMREX-", "SIMREX"),
            DronePattern("NEHEME-", "Neheme"),
            DronePattern("AOVO-", "AOVO"),
            DronePattern("TENSSENX-", "TENSSENX"),
            DronePattern("SNAPTAIN-", "Snaptain"),
            DronePattern("POTENSIC-", "Potensic"),
            DronePattern("RUKO-", "Ruko"),
            DronePattern("SYMA-", "Syma"),
            DronePattern("HUBSAN-", "Hubsan"),
            DronePattern("EACHINE-", "Eachine"),
            DronePattern("FIMI-", "Fimi"),
            DronePattern("XIAOMI-", "Xiaomi"),
            DronePattern("YUNEEC-", "Yuneec"),
            DronePattern("TYPHOON-", "Yuneec"),
            DronePattern("MANTIS-", "Yuneec"),
            DronePattern("WINGSLAND-", "Wingsland"),
            DronePattern("BETAFPV-", "BetaFPV"),
            DronePattern("GEPRC-", "GEPRC"),
            DronePattern("EMAX-", "EMAX"),
            // Other brands
            DronePattern("POWEREGG-", "PowerVision"),
            DronePattern("DOBBY-", "ZEROTECH"),
            DronePattern("SPLASHDRONE-", "Swellpro"),
            DronePattern("CONTIXO-", "Contixo"),
            DronePattern("SKYVIPER-", "Sky Viper"),
            DronePattern("DROCON-", "Drocon"),
            // Enterprise / commercial
            DronePattern("FREEFLY-", "Freefly"),
            DronePattern("SENSEFLY-", "senseFly"),
            DronePattern("WINGCOPTER-", "Wingcopter"),
            DronePattern("FLYABILITY-", "Flyability"),
            // FPV and hobby brands
            DronePattern("IFLIGHT-", "iFlight"),
            DronePattern("FLYWOO-", "Flywoo"),
            // DIATONE- removed: frame manufacturer, no WiFi broadcasts
            DronePattern("WALKERA-", "Walkera"),
            DronePattern("BLADE-", "Blade"),
            DronePattern("CADDX-", "Caddx"),
            DronePattern("WALKSNAIL-", "Walksnail"),
            DronePattern("AVATAR-", "Walksnail"),
            // TBS- removed: radio equipment, not WiFi hotspots
            DronePattern("RUNCAM-", "RunCam"),
            // Budget Chinese drones using "WiFi UAV" / generic FPV apps
            DronePattern("WIFI-UAV", "Generic"),
            DronePattern("WIFI_UAV", "Generic"),
            DronePattern("WIFIUAV", "Generic"),
            DronePattern("WiFi-720P", "Generic"),
            DronePattern("WiFi-1080P", "Generic"),
            DronePattern("WiFi-4K", "Generic"),
            DronePattern("WIFI_CAMERA", "Generic"),
            DronePattern("WiFi_FPV", "Generic"),
            DronePattern("WiFi-FPV", "Generic"),
            DronePattern("RCDrone", "Generic"),
            DronePattern("RC-DRONE", "Generic"),
            DronePattern("RCTOY", "Generic"),
            DronePattern("UFO-", "Generic"),
            // Chinese brands commonly using WiFi UAV-type apps
            DronePattern("JJRC-", "JJRC"),
            DronePattern("MJX-", "MJX"),
            DronePattern("VISUO-", "Visuo"),
            DronePattern("SJRC-", "SJRC"),
            DronePattern("4DRC-", "4DRC"),
            DronePattern("FLYHAL-", "Flyhal"),
            DronePattern("LYZRC-", "LYZRC"),
            DronePattern("XINLIN-", "Xinlin"),
            DronePattern("E58-", "Eachine"),
            DronePattern("E88-", "Eachine"),
            DronePattern("E99-", "Eachine"),
            DronePattern("V2PRO", "Generic"),
            // Generic drone SSIDs
            DronePattern("DRONE-", "Unknown"),
            DronePattern("UAV-", "Unknown"),
            DronePattern("QUADCOPTER-", "Unknown"),
        )
    }

    /** Timestamps of recent scan requests for throttle enforcement. */
    private val scanTimestamps = java.util.concurrent.ConcurrentLinkedDeque<Long>()

    /** Partial state accumulators for ASTM F3411 WiFi Beacon Remote ID, keyed by BSSID. */
    private val wifiBeaconRidStates = mutableMapOf<String, OpenDroneIdParser.DronePartialState>()

    /**
     * Start periodic WiFi scanning for drone SSIDs.
     *
     * Returns a Flow that emits Drone objects when SSIDs matching known drone
     * patterns are found. Automatically respects Android scan throttling limits.
     *
     * Requires NEARBY_WIFI_DEVICES (Android 13+) or ACCESS_FINE_LOCATION + CHANGE_WIFI_STATE.
     */
    @SuppressLint("MissingPermission")
    fun startScanning(): Flow<Drone> = callbackFlow {
        Log.i(TAG, "Starting WiFi drone scanner")

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action == WifiManager.SCAN_RESULTS_AVAILABLE_ACTION) {
                    val success = intent.getBooleanExtra(
                        WifiManager.EXTRA_RESULTS_UPDATED, false
                    )
                    Log.d(TAG, "WiFi scan results available, success=$success")

                    processScanResults().forEach { drone ->
                        trySend(drone)
                    }
                }
            }
        }

        val filter = IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION)
        context.registerReceiver(receiver, filter)

        // Launch a coroutine to periodically trigger scans
        val scanJob = launch {
            while (isActive) {
                triggerScanIfAllowed()
                delay(SCAN_INTERVAL_MS)
            }
        }

        awaitClose {
            Log.i(TAG, "Stopping WiFi drone scanner")
            scanJob.cancel()
            try {
                context.unregisterReceiver(receiver)
            } catch (e: Exception) {
                Log.w(TAG, "Error unregistering WiFi receiver", e)
            }
        }
    }

    /** Stop scanning (for imperative callers; flow-based callers cancel the coroutine). */
    fun stopScanning() {
        scanTimestamps.clear()
        wifiBeaconRidStates.clear()
    }

    /**
     * Trigger a WiFi scan if the throttle limit has not been reached.
     *
     * Android limits foreground apps to 4 scans per 2 minutes.
     * We track recent scan timestamps and skip if we've hit the limit.
     */
    @Suppress("DEPRECATION")
    private fun triggerScanIfAllowed() {
        val now = System.currentTimeMillis()

        // Remove scan timestamps outside the throttle window
        var oldest = scanTimestamps.peekFirst()
        while (oldest != null && now - oldest > THROTTLE_WINDOW_MS) {
            scanTimestamps.pollFirst()
            oldest = scanTimestamps.peekFirst()
        }

        if (scanTimestamps.size >= MAX_SCANS_IN_WINDOW) {
            Log.d(TAG, "WiFi scan throttled (${scanTimestamps.size} scans in last 2 min)")
            return
        }

        @SuppressLint("MissingPermission")
        val started = wifiManager.startScan()
        if (started) {
            scanTimestamps.add(now)
            Log.d(TAG, "WiFi scan initiated (${scanTimestamps.size}/$MAX_SCANS_IN_WINDOW in window)")
        } else {
            Log.d(TAG, "WiFi startScan() returned false (throttled by OS)")
            // Even if startScan returns false, cached results may still be available
        }
    }

    /**
     * Process WiFi scan results, filtering for drone-matching SSIDs,
     * OUI prefixes, and DJI DroneID Information Elements.
     *
     * Detection priority:
     * 1. DJI DroneID IE → conf 0.85, full position from parsed GPS
     * 2. SSID pattern match → conf 0.3, no position
     * 3. OUI match (known drone vendor) → conf 0.4, no position
     *
     * Uses cached scan results from WifiManager (which persist even when
     * startScan() is throttled).
     */
    @SuppressLint("MissingPermission")
    private fun processScanResults(): List<Drone> {
        @Suppress("DEPRECATION")
        val scanResults = wifiManager.scanResults ?: return emptyList()
        val now = Instant.now()
        val drones = mutableListOf<Drone>()
        val seenBssids = mutableSetOf<String>()

        for (result in scanResults) {
            val ssid = result.SSID ?: ""
            val bssid = result.BSSID ?: continue
            val rssi = result.level
            val estimatedDistance = estimateDistance(rssi)
            val freqMhz = result.frequency
            val chanWidth = mapChannelWidth(result.channelWidth)

            // --- Priority 1: Try DJI DroneID IE parsing ---
            val droneIdData = DjiDroneIdParser.parse(result)
            if (droneIdData != null) {
                seenBssids.add(bssid)
                val serial = droneIdData.serialPrefix ?: ssid.ifBlank { bssid }

                Log.i(TAG, "DJI DroneID IE: BSSID=$bssid, lat=${droneIdData.latitude}, " +
                        "lon=${droneIdData.longitude}, alt=${droneIdData.altitudeMeters}m, " +
                        "speed=${droneIdData.speedMps}m/s, serial=$serial")

                drones.add(Drone(
                    id = "wifi_dji_${bssid.replace(":", "").lowercase()}",
                    position = Position(
                        latitude = droneIdData.latitude,
                        longitude = droneIdData.longitude,
                        altitudeMeters = droneIdData.altitudeMeters,
                        heading = droneIdData.headingDegrees,
                        speedMps = droneIdData.speedMps
                    ),
                    source = DetectionSource.WIFI,
                    category = ObjectCategory.DRONE,
                    confidence = 0.85f,
                    firstSeen = now,
                    lastUpdated = now,
                    droneId = serial,
                    manufacturer = "DJI",
                    model = if (ssid.isNotBlank()) inferModel(ssid, DronePattern("DJI-", "DJI")) else null,
                    ssid = ssid.ifBlank { null },
                    signalStrengthDbm = rssi,
                    estimatedDistanceMeters = estimatedDistance,
                    bssid = bssid,
                    frequencyMhz = freqMhz,
                    channelWidthMhz = chanWidth,
                    operatorLatitude = droneIdData.homeLatitude,
                    operatorLongitude = droneIdData.homeLongitude
                ))
                continue
            }

            // --- Priority 1.5: ASTM F3411 WiFi Beacon Remote ID ---
            val beaconState = wifiBeaconRidStates.getOrPut(bssid) {
                OpenDroneIdParser.DronePartialState(bssid, now)
            }
            beaconState.lastUpdated = now
            beaconState.signalStrengthDbm = rssi
            if (WifiBeaconRemoteIdParser.parse(result, beaconState)) {
                seenBssids.add(bssid)
                beaconState.toDroneOrNull(idPrefix = "wfb_", detectionSource = DetectionSource.WIFI_BEACON)?.let { drone ->
                    drones.add(drone.copy(
                        ssid = ssid.ifBlank { null },
                        bssid = bssid,
                        frequencyMhz = freqMhz,
                        channelWidthMhz = chanWidth,
                        signalStrengthDbm = rssi,
                        estimatedDistanceMeters = estimatedDistance
                    ))
                }
                continue
            }

            // --- Priority 2: SSID pattern matching ---
            if (ssid.isNotBlank()) {
                val matchedPattern = matchDronePattern(ssid)
                if (matchedPattern != null) {
                    seenBssids.add(bssid)
                    Log.d(TAG, "Drone WiFi SSID: $ssid, RSSI=$rssi dBm, ~${estimatedDistance.toInt()}m")

                    drones.add(Drone(
                        id = "wifi_${ssid.lowercase().replace(Regex("[^a-z0-9]"), "_")}",
                        position = Position(0.0, 0.0, 0.0),
                        source = DetectionSource.WIFI,
                        category = ObjectCategory.DRONE,
                        confidence = 0.3f,
                        firstSeen = now,
                        lastUpdated = now,
                        droneId = ssid,
                        manufacturer = matchedPattern.manufacturer,
                        model = inferModel(ssid, matchedPattern),
                        ssid = ssid,
                        signalStrengthDbm = rssi,
                        estimatedDistanceMeters = estimatedDistance,
                        bssid = bssid,
                        frequencyMhz = freqMhz,
                        channelWidthMhz = chanWidth
                    ))
                    continue
                }
            }

            // --- Priority 3: OUI prefix matching (catches hidden/generic SSIDs) ---
            if (bssid !in seenBssids) {
                val ouiEntry = WifiOuiDatabase.lookup(bssid)
                if (ouiEntry != null && !ouiEntry.highFalsePositiveRisk) {
                    seenBssids.add(bssid)
                    val displaySsid = ssid.ifBlank { "(hidden)" }
                    Log.d(TAG, "Drone WiFi OUI: BSSID=$bssid (${ouiEntry.manufacturer}), " +
                            "SSID=$displaySsid, RSSI=$rssi dBm, ~${estimatedDistance.toInt()}m")

                    drones.add(Drone(
                        id = "wifi_oui_${bssid.replace(":", "").lowercase()}",
                        position = Position(0.0, 0.0, 0.0),
                        source = DetectionSource.WIFI,
                        category = ObjectCategory.DRONE,
                        confidence = 0.4f,
                        firstSeen = now,
                        lastUpdated = now,
                        droneId = bssid,
                        manufacturer = ouiEntry.manufacturer,
                        ssid = ssid.ifBlank { null },
                        signalStrengthDbm = rssi,
                        estimatedDistanceMeters = estimatedDistance,
                        bssid = bssid,
                        frequencyMhz = freqMhz,
                        channelWidthMhz = chanWidth
                    ))
                }
            }
        }

        if (drones.isNotEmpty()) {
            Log.i(TAG, "Found ${drones.size} potential drone(s) via WiFi " +
                    "(${drones.count { it.confidence >= 0.85f }} DroneID, " +
                    "${drones.count { it.source == DetectionSource.WIFI_BEACON }} BeaconRID, " +
                    "${drones.count { it.confidence == 0.4f }} OUI, " +
                    "${drones.count { it.confidence == 0.3f }} SSID)")
        } else if (scanResults.isNotEmpty()) {
            val ssids = scanResults.mapNotNull { it.SSID }.filter { it.isNotBlank() }
            Log.d(TAG, "No drone SSIDs/OUIs matched. Visible networks (${ssids.size}): ${ssids.take(10)}")
        }

        return drones
    }

    /**
     * Match an SSID against known drone patterns.
     *
     * @return The matching DronePattern or null if no match.
     */
    private fun matchDronePattern(ssid: String): DronePattern? {
        val upper = ssid.uppercase()
        return DRONE_SSID_PATTERNS.firstOrNull { pattern ->
            upper.startsWith(pattern.prefix.uppercase())
        }
    }

    /**
     * Infer the drone model from the SSID suffix after the manufacturer prefix.
     *
     * For example, "DJI-MAVIC3-ABCDEF" might yield model "MAVIC3".
     */
    private fun inferModel(ssid: String, pattern: DronePattern): String? {
        val suffix = ssid.substringAfter(pattern.prefix, "")
        if (suffix.isBlank()) return null

        // Some SSIDs have format: PREFIX-MODEL-SERIAL or PREFIX-SERIAL
        // Try to extract the model part (before the last dash-separated segment)
        val parts = suffix.split("-")
        return if (parts.size >= 2) {
            // If there are multiple parts, the first is likely the model
            parts.first().takeIf { it.isNotBlank() }
        } else {
            null
        }
    }

    /**
     * Estimate distance from RSSI using the log-distance path loss model.
     *
     * Formula: d = 10^((RSSI_ref - RSSI) / (10 * n))
     *
     * Where:
     *   RSSI_ref = reference RSSI at 1 meter (-40 dBm typical for drone hotspot)
     *   n = path loss exponent (2.5 for outdoor with light obstruction)
     *   RSSI = measured signal strength in dBm
     *
     * This is a rough approximation. Actual distance varies significantly with
     * environment, antenna orientation, obstacles, etc.
     *
     * @param rssi Measured signal strength in dBm
     * @return Estimated distance in meters
     */
    private fun estimateDistance(rssi: Int): Double {
        val exponent = (RSSI_REF - rssi) / (10.0 * PATH_LOSS_EXPONENT)
        return 10.0.pow(exponent).coerceIn(0.5, 5000.0)
    }

    /** Map Android's ScanResult.channelWidth constants to integer MHz values. */
    @Suppress("DEPRECATION")
    private fun mapChannelWidth(channelWidth: Int): Int = when (channelWidth) {
        ScanResult.CHANNEL_WIDTH_20MHZ -> 20
        ScanResult.CHANNEL_WIDTH_40MHZ -> 40
        ScanResult.CHANNEL_WIDTH_80MHZ -> 80
        ScanResult.CHANNEL_WIDTH_160MHZ -> 160
        ScanResult.CHANNEL_WIDTH_80MHZ_PLUS_MHZ -> 160
        else -> 20
    }

    /**
     * Data class holding a known drone SSID pattern and its manufacturer.
     */
    private data class DronePattern(
        val prefix: String,
        val manufacturer: String
    )
}

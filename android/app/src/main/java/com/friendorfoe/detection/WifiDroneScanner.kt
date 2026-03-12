package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
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
import java.util.LinkedList
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
            DronePattern("DJI-", "DJI"),
            DronePattern("TELLO-", "Ryze/DJI"),
            DronePattern("SKYDIO-", "Skydio"),
            DronePattern("MAVIC-", "DJI"),
            DronePattern("PARROT-", "Parrot"),
            DronePattern("AUTEL-", "Autel")
        )
    }

    /** Timestamps of recent scan requests for throttle enforcement. */
    private val scanTimestamps = LinkedList<Long>()

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
        while (scanTimestamps.isNotEmpty() && now - scanTimestamps.peek() > THROTTLE_WINDOW_MS) {
            scanTimestamps.poll()
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
     * Process WiFi scan results, filtering for drone-matching SSIDs.
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

        for (result in scanResults) {
            val ssid = result.SSID ?: continue
            if (ssid.isBlank()) continue

            val matchedPattern = matchDronePattern(ssid) ?: continue

            val rssi = result.level
            val estimatedDistance = estimateDistance(rssi)

            Log.d(TAG, "Drone WiFi detected: SSID=$ssid, RSSI=$rssi dBm, ~${estimatedDistance.toInt()}m")

            // WiFi detection doesn't give us the drone's actual position,
            // so we set lat/lon/alt to 0 - the presentation layer should use
            // estimatedDistanceMeters for display instead.
            val drone = Drone(
                id = "wifi_${ssid.lowercase().replace(Regex("[^a-z0-9]"), "_")}",
                position = Position(
                    latitude = 0.0,
                    longitude = 0.0,
                    altitudeMeters = 0.0
                ),
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
                estimatedDistanceMeters = estimatedDistance
            )

            drones.add(drone)
        }

        if (drones.isNotEmpty()) {
            Log.i(TAG, "Found ${drones.size} potential drone(s) via WiFi")
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

    /**
     * Data class holding a known drone SSID pattern and its manufacturer.
     */
    private data class DronePattern(
        val prefix: String,
        val manufacturer: String
    )
}

package com.friendorfoe.detection

import android.annotation.SuppressLint
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.util.Log
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Detects WiFi anomalies that indicate evil twin attacks, rogue APs,
 * or karma/SSID spoofing attacks.
 *
 * Detection methods (all work on stock Android, no root):
 * 1. Same SSID on multiple BSSIDs with mixed security (open + WPA)
 * 2. Same SSID with different OUI vendors (unusual for legitimate mesh)
 * 3. Single BSSID broadcasting many different SSIDs (karma attack)
 */
@Singleton
class WifiAnomalyDetector @Inject constructor(
    private val wifiManager: WifiManager
) {
    companion object {
        private const val TAG = "WifiAnomalyDetector"
    }

    data class WifiAnomaly(
        val type: String,       // "evil_twin", "rogue_ap", "karma_attack"
        val ssid: String,
        val details: String,
        val threatLevel: Int,   // 1=low, 2=medium, 3=high
        val bssids: List<String>,
        val timestamp: Instant
    )

    // Track BSSID → SSID history for karma detection
    private val bssidSsidHistory = mutableMapOf<String, MutableSet<String>>()

    /**
     * Analyze current WiFi scan results for anomalies.
     * Call periodically (every 10-15 seconds).
     */
    @SuppressLint("MissingPermission")
    fun analyze(): List<WifiAnomaly> {
        @Suppress("DEPRECATION")
        val scanResults = wifiManager.scanResults ?: return emptyList()
        val anomalies = mutableListOf<WifiAnomaly>()
        val now = Instant.now()

        // Group by SSID
        val bySSID = scanResults
            .filter { it.SSID?.isNotBlank() == true }
            .groupBy { it.SSID ?: "" }

        for ((ssid, results) in bySSID) {
            if (results.size < 2) continue

            // Check 1: Mixed security (HIGH threat — classic evil twin)
            val securities = results.map { getSecurityType(it) }.toSet()
            if (securities.size > 1 && securities.contains("OPEN")) {
                anomalies.add(WifiAnomaly(
                    type = "evil_twin",
                    ssid = ssid,
                    details = "Same SSID with mixed security: ${securities.joinToString(" + ")}. " +
                        "An open AP alongside a secured one is a classic evil twin attack.",
                    threatLevel = 3,
                    bssids = results.map { it.BSSID },
                    timestamp = now
                ))
                Log.w(TAG, "EVIL TWIN: '$ssid' has mixed security: $securities")
                continue
            }

            // Check 2: Different OUI vendors (MEDIUM threat)
            val ouis = results.map { it.BSSID.take(8).uppercase() }.toSet()
            if (ouis.size > 1) {
                // Multiple vendors for same SSID — suspicious unless mesh
                anomalies.add(WifiAnomaly(
                    type = "rogue_ap",
                    ssid = ssid,
                    details = "Same SSID '$ssid' broadcast by ${ouis.size} different vendors: " +
                        "${ouis.joinToString(", ")}. Could indicate a rogue access point.",
                    threatLevel = 2,
                    bssids = results.map { it.BSSID },
                    timestamp = now
                ))
            }
        }

        // Check 3: Karma attack — single BSSID broadcasting many SSIDs
        for (result in scanResults) {
            val bssid = result.BSSID ?: continue
            val ssid = result.SSID ?: continue
            if (ssid.isBlank()) continue

            val history = bssidSsidHistory.getOrPut(bssid) { mutableSetOf() }
            history.add(ssid)

            if (history.size >= 5) {
                anomalies.add(WifiAnomaly(
                    type = "karma_attack",
                    ssid = bssid,
                    details = "Single AP ($bssid) broadcasting ${history.size} different SSIDs: " +
                        "${history.take(5).joinToString(", ")}${if (history.size > 5) "..." else ""}. " +
                        "This is characteristic of a WiFi Pineapple karma attack.",
                    threatLevel = 3,
                    bssids = listOf(bssid),
                    timestamp = now
                ))
                Log.w(TAG, "KARMA ATTACK: $bssid broadcasting ${history.size} SSIDs")
            }
        }

        // Prune old BSSID history (keep last 50 entries)
        if (bssidSsidHistory.size > 50) {
            val toRemove = bssidSsidHistory.keys.take(bssidSsidHistory.size - 50)
            toRemove.forEach { bssidSsidHistory.remove(it) }
        }

        return anomalies
    }

    private fun getSecurityType(result: ScanResult): String {
        val caps = result.capabilities ?: return "UNKNOWN"
        return when {
            caps.contains("WPA3") -> "WPA3"
            caps.contains("WPA2") -> "WPA2"
            caps.contains("WPA") -> "WPA"
            caps.contains("WEP") -> "WEP"
            else -> "OPEN"
        }
    }
}

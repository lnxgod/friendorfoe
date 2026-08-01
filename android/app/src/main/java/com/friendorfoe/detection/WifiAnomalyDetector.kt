package com.friendorfoe.detection

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
 * 2. Single BSSID broadcasting many different SSIDs (karma attack)
 *
 * Note: a rule that flagged "same SSID across multiple OUI vendors" was
 * removed — it false-positived on legitimate multi-vendor mesh deployments
 * (Eero + Google Home extender, Apple TV mesh, etc.). The mixed-security
 * check below is the actually-diagnostic evil twin signal.
 */
@Singleton
class WifiAnomalyDetector @Inject constructor(
    private val wifiScanCoordinator: WifiScanCoordinator
) {
    companion object {
        private const val TAG = "WifiAnomalyDetector"

        internal fun analyzeNetworksForTest(networks: List<WifiNetwork>): List<WifiAnomaly> {
            return analyzeNetworks(networks, mutableMapOf(), Instant.now())
        }

        private fun analyzeNetworks(
            networks: List<WifiNetwork>,
            bssidSsidHistory: MutableMap<String, MutableSet<String>>,
            now: Instant
        ): List<WifiAnomaly> {
            if (networks.isEmpty()) return emptyList()
            val anomalies = mutableListOf<WifiAnomaly>()

            // Pwnagotchi default BSSID — deliberately attention-getting. Marauder
            // matches on this; any hit on our LAN is a deliberate pen-test beacon.
            for (network in networks) {
                if (network.bssid.equals(BleSignatures.PWNAGOTCHI_BSSID, ignoreCase = true)) {
                    anomalies.add(WifiAnomaly(
                        type = "pwnagotchi",
                        ssid = network.ssid.ifBlank { "(hidden)" },
                        details = "Pwnagotchi device detected — source MAC ${network.bssid} is the default " +
                            "Pwnagotchi pen-test beacon. It attempts to capture WPA handshakes passively.",
                        threatLevel = 3,
                        bssids = listOf(network.bssid),
                        evidence = listOf(network.toEvidence()),
                        timestamp = now
                    ))
                    safeLogWarning("PWNAGOTCHI: ${network.bssid} (ssid=${network.ssid})")
                }
            }

            // Group by SSID
            val bySSID = networks
                .filter { it.ssid.isNotBlank() }
                .groupBy { it.ssid }

            for ((ssid, results) in bySSID) {
                if (results.size < 2) continue

                // Check 1: Mixed security (HIGH threat — classic evil twin)
                val securities = results.map { getSecurityType(it.capabilities) }.toSet()
                if (securities.size > 1 && securities.contains("OPEN")) {
                    val evidence = results.map { it.toEvidence() }
                    anomalies.add(WifiAnomaly(
                        type = "evil_twin",
                        ssid = ssid,
                        details = "Same SSID with mixed security: ${securities.joinToString(" + ")}. " +
                            "An open AP alongside a secured one is a classic evil twin attack. " +
                            "Observed APs: ${evidence.joinToString("; ") { it.summary() }}",
                        threatLevel = 3,
                        bssids = evidence.map { it.bssid },
                        evidence = evidence,
                        timestamp = now
                    ))
                    safeLogWarning("EVIL TWIN: '$ssid' has mixed security: $securities")
                    continue
                }
            }

            // Check 3: Karma attack — single BSSID broadcasting many SSIDs
            for (network in networks) {
                if (network.ssid.isBlank()) continue

                val history = bssidSsidHistory.getOrPut(network.bssid) { mutableSetOf() }
                history.add(network.ssid)

                if (history.size >= 5) {
                    anomalies.add(WifiAnomaly(
                        type = "karma_attack",
                        ssid = network.bssid,
                        details = "Single AP (${network.bssid}) broadcasting ${history.size} different SSIDs: " +
                            "${history.take(5).joinToString(", ")}${if (history.size > 5) "..." else ""}. " +
                            "This is characteristic of a WiFi Pineapple karma attack.",
                        threatLevel = 3,
                        bssids = listOf(network.bssid),
                        evidence = listOf(network.toEvidence()),
                        timestamp = now
                    ))
                    safeLogWarning("KARMA ATTACK: ${network.bssid} broadcasting ${history.size} SSIDs")
                }
            }

            // Prune old BSSID history (keep last 50 entries)
            if (bssidSsidHistory.size > 50) {
                val toRemove = bssidSsidHistory.keys.take(bssidSsidHistory.size - 50)
                toRemove.forEach { bssidSsidHistory.remove(it) }
            }

            return anomalies
        }

        private fun WifiNetwork.toEvidence(): WifiAnomalyEvidence {
            return WifiAnomalyEvidence(
                bssid = bssid,
                security = getSecurityType(capabilities),
                rssi = rssi,
                frequencyMhz = frequencyMhz
            )
        }

        private fun getSecurityType(capabilities: String?): String {
            val caps = capabilities ?: return "UNKNOWN"
            return when {
                caps.contains("WPA3") -> "WPA3"
                caps.contains("WPA2") -> "WPA2"
                caps.contains("WPA") -> "WPA"
                caps.contains("WEP") -> "WEP"
                else -> "OPEN"
            }
        }

        private fun safeLogWarning(message: String) {
            try {
                Log.w(TAG, message)
            } catch (_: RuntimeException) {
                // Android Log is not available in plain JVM unit tests.
            }
        }
    }

    data class WifiNetwork(
        val ssid: String,
        val bssid: String,
        val capabilities: String?,
        val rssi: Int,
        val frequencyMhz: Int
    )

    data class WifiAnomalyEvidence(
        val bssid: String,
        val security: String,
        val rssi: Int,
        val frequencyMhz: Int
    ) {
        fun summary(): String {
            val frequency = if (frequencyMhz > 0) ", ${frequencyMhz}MHz" else ""
            return "$bssid $security, ${rssi}dBm$frequency"
        }
    }

    data class WifiAnomaly(
        val type: String,       // "evil_twin", "rogue_ap", "karma_attack"
        val ssid: String,
        val details: String,
        val threatLevel: Int,   // 1=low, 2=medium, 3=high
        val bssids: List<String>,
        val evidence: List<WifiAnomalyEvidence> = emptyList(),
        val timestamp: Instant
    )

    // Track BSSID → SSID history for karma detection
    private val bssidSsidHistory = mutableMapOf<String, MutableSet<String>>()

    fun analyzeBatch(batch: WifiScanBatch): List<WifiAnomaly> {
        val networks = batch.networks.map { network ->
            WifiNetwork(
                ssid = network.ssid,
                bssid = network.bssid,
                capabilities = network.capabilities,
                rssi = network.rssi,
                frequencyMhz = network.frequencyMhz,
            )
        }
        return synchronized(bssidSsidHistory) {
            analyzeNetworks(
                networks = networks,
                bssidSsidHistory = bssidSsidHistory,
                now = Instant.ofEpochMilli(batch.observedWallMs),
            )
        }
    }

    /**
     * Analyze current WiFi scan results for anomalies.
     * Call periodically (every 10-15 seconds).
     */
    fun analyze(): List<WifiAnomaly> {
        val batch = wifiScanCoordinator.currentBatch.value ?: return emptyList()
        return analyzeBatch(batch)
    }
}

package com.friendorfoe.detection

import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Reports advertised Wi-Fi security weaknesses and possible access-point anomalies.
 *
 * Detection methods (all work on stock Android, no root):
 * 1. Same SSID on multiple BSSIDs with mixed security (open + WPA)
 * 2. Single BSSID broadcasting many different SSIDs (karma attack)
 *
 * Note: a rule that flagged "same SSID across multiple OUI vendors" was
 * removed — it false-positived on legitimate multi-vendor mesh deployments
 * (Eero + Google Home extender, Apple TV mesh, etc.). Mixed-security names
 * also occur legitimately; they are awareness findings, not proof of an attack.
 */
@Singleton
class WifiAnomalyDetector @Inject constructor(
    private val wifiScanCoordinator: WifiScanCoordinator
) {
    companion object {
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

            // Match the advertised default address. A broadcast cannot authenticate its sender.
            for (network in networks) {
                if (network.bssid.equals(BleSignatures.PWNAGOTCHI_BSSID, ignoreCase = true)) {
                    anomalies.add(WifiAnomaly(
                        type = "pwnagotchi",
                        ssid = network.ssid,
                        reason = "The access point uses the default Pwnagotchi Wi-Fi address.",
                        details = "Observed access point address: ${network.bssid}. This address can be imitated.",
                        threatLevel = 3,
                        bssids = listOf(network.bssid),
                        evidence = listOf(network.toEvidence()),
                        timestamp = now
                    ))
                }
            }

            // Advertised legacy support is observable on stock Android. It does not
            // establish the cipher used by any client or prove an active attack.
            for (network in networks.distinctBy { it.bssid.lowercase() }) {
                val security = WifiSecurityProfile.parse(network.capabilities)
                if (security.obsoleteModes.isEmpty()) continue
                anomalies.add(WifiAnomaly(
                    type = "weak_security",
                    ssid = network.ssid,
                    reason = "This access point advertises obsolete ${security.obsoleteModes.joinToString(" + ")} security.",
                    details = "The access point advertises obsolete " +
                        "${security.obsoleteModes.joinToString(" + ")} security. " +
                        "Prefer WPA2 with AES/CCMP or WPA3. AP: ${network.toEvidence().summary()}",
                    threatLevel = 2,
                    bssids = listOf(network.bssid),
                    evidence = listOf(network.toEvidence()),
                    timestamp = now,
                ))
            }

            // Group by SSID
            val bySSID = networks
                .filter { it.ssid.isAdvertisedWifiName() }
                .groupBy { it.ssid }

            for ((ssid, results) in bySSID) {
                if (results.size < 2) continue

                // OWE/SAE/EAP/DPP and unknown AKMs are never assumed open.
                val distinctAps = results.distinctBy { it.bssid.lowercase() }
                val profiles = distinctAps.map { WifiSecurityProfile.parse(it.capabilities) }
                val securities = profiles.map { it.label }.toSet()
                if (distinctAps.size >= 2 && profiles.any { it.open } && profiles.any { it.authenticated }) {
                    val evidence = distinctAps.map { it.toEvidence() }
                    anomalies.add(WifiAnomaly(
                        type = "evil_twin",
                        ssid = ssid,
                        reason = "Access points using this name advertise both open and secured Wi-Fi (${securities.sorted().joinToString(", ")}).",
                        details = "Same SSID with mixed security: ${securities.joinToString(" + ")}. " +
                            "Check which AP you join; this can be a legitimate configuration or a spoofed network. " +
                            "Observed APs: ${evidence.joinToString("; ") { it.summary() }}",
                        threatLevel = 2,
                        bssids = evidence.map { it.bssid },
                        evidence = evidence,
                        timestamp = now
                    ))
                    continue
                }
            }

            // Check 3: Karma attack — single BSSID broadcasting many SSIDs
            for (network in networks) {
                if (!network.ssid.isAdvertisedWifiName()) continue

                val history = bssidSsidHistory.getOrPut(network.bssid) { mutableSetOf() }
                history.add(network.ssid)

                if (history.size >= 5) {
                    anomalies.add(WifiAnomaly(
                        type = "karma_attack",
                        ssid = network.ssid,
                        reason = "The same access point has advertised ${history.size} different network names.",
                        details = "Single AP (${network.bssid}) broadcasting ${history.size} different SSIDs: " +
                            "${history.take(5).joinToString(", ")}${if (history.size > 5) "..." else ""}. " +
                            "Access points can advertise more than one name for legitimate networks.",
                        threatLevel = 3,
                        bssids = listOf(network.bssid),
                        evidence = listOf(network.toEvidence()),
                        timestamp = now
                    ))
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

        private fun getSecurityType(capabilities: String?): String =
            WifiSecurityProfile.parse(capabilities).label
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
        val type: String,       // "weak_security", "evil_twin", "pwnagotchi", "karma_attack"
        val ssid: String,
        val details: String,
        val threatLevel: Int,   // 1=low, 2=medium, 3=high
        val bssids: List<String>,
        val evidence: List<WifiAnomalyEvidence> = emptyList(),
        val timestamp: Instant,
        val reason: String = details,
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

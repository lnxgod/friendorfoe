package com.friendorfoe.detection

import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.transform

/**
 * Maps all visible WiFi scan results through the privacy signature table.
 *
 * This intentionally runs beside drone WiFi matching: hidden cameras and attack
 * APs should not have to look drone-like before they can appear in Privacy.
 */
@Singleton
class WifiPrivacyScanner @Inject constructor(
    private val wifiScanCoordinator: WifiScanCoordinator
) {
    data class WifiPrivacyNetwork(
        val ssid: String,
        val bssid: String,
        val rssi: Int
    )

    companion object {
        internal fun detectPrivacyNetworksForTest(
            networks: List<WifiPrivacyNetwork>
        ): List<GlassesDetection> = detectPrivacyNetworks(networks)

        private fun detectPrivacyNetworks(
            networks: List<WifiPrivacyNetwork>
        ): List<GlassesDetection> {
            val seen = mutableSetOf<String>()
            return networks.mapNotNull { network ->
                val detection = GlassesDetector.checkWifiSsid(
                    ssid = network.ssid,
                    bssid = network.bssid,
                    rssi = network.rssi
                ) ?: return@mapNotNull null
                val key = detection.fingerprintKey.ifBlank { "mac:${detection.mac}" }
                detection.takeIf { seen.add(key.lowercase()) }
            }
        }
    }

    fun startScanning(): Flow<GlassesDetection> =
        wifiScanCoordinator.scanResults().transform { scanResults ->
            val networks = scanResults.mapNotNull { result ->
                val bssid = result.BSSID ?: return@mapNotNull null
                WifiPrivacyNetwork(
                    ssid = result.SSID.orEmpty(),
                    bssid = bssid,
                    rssi = result.level
                )
            }
            detectPrivacyNetworks(networks).forEach { emit(it) }
        }
}

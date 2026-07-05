package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiAnomalyDetectorTest {

    @Test
    fun evil_twin_details_include_each_ap_and_security() {
        val anomalies = WifiAnomalyDetector.analyzeNetworksForTest(
            listOf(
                WifiAnomalyDetector.WifiNetwork(
                    ssid = "CafeWiFi",
                    bssid = "00:11:22:33:44:55",
                    capabilities = "[WPA2-PSK-CCMP][ESS]",
                    rssi = -49,
                    frequencyMhz = 2437
                ),
                WifiAnomalyDetector.WifiNetwork(
                    ssid = "CafeWiFi",
                    bssid = "66:77:88:99:AA:BB",
                    capabilities = "[ESS]",
                    rssi = -38,
                    frequencyMhz = 2437
                )
            )
        )

        assertEquals(1, anomalies.size)
        val anomaly = anomalies.single()
        assertEquals("evil_twin", anomaly.type)
        assertEquals(2, anomaly.evidence.size)
        assertTrue(anomaly.details.contains("00:11:22:33:44:55 WPA2"))
        assertTrue(anomaly.details.contains("66:77:88:99:AA:BB OPEN"))
        assertTrue(anomaly.details.contains("-38dBm"))
        assertTrue(anomaly.details.contains("2437MHz"))
    }
}

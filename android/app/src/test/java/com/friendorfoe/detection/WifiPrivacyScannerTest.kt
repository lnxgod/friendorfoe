package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiPrivacyScannerTest {

    @Test
    fun maps_all_visible_wifi_networks_to_privacy_detections() {
        val detections = WifiPrivacyScanner.detectPrivacyNetworksForTest(
            listOf(
                WifiPrivacyScanner.WifiPrivacyNetwork(
                    ssid = "LookCam_AB12",
                    bssid = "AA:BB:CC:00:00:01",
                    rssi = -48
                ),
                WifiPrivacyScanner.WifiPrivacyNetwork(
                    ssid = "Home WiFi",
                    bssid = "AA:BB:CC:00:00:02",
                    rssi = -62
                ),
                WifiPrivacyScanner.WifiPrivacyNetwork(
                    ssid = "Advanced-Deauther",
                    bssid = "AA:BB:CC:00:00:03",
                    rssi = -55
                )
            )
        )

        assertEquals(2, detections.size)
        assertTrue(detections.any { it.category == PrivacyCategory.HIDDEN_CAMERA && it.manufacturer == "LookCam" })
        assertTrue(detections.any { it.category == PrivacyCategory.ATTACK_TOOL && it.manufacturer == "Generic" })
    }

    @Test
    fun de_dupes_repeated_network_identity() {
        val detections = WifiPrivacyScanner.detectPrivacyNetworksForTest(
            listOf(
                WifiPrivacyScanner.WifiPrivacyNetwork("CamHi_1234", "AA:BB:CC:00:00:04", -44),
                WifiPrivacyScanner.WifiPrivacyNetwork("CamHi_1234", "AA:BB:CC:00:00:04", -45)
            )
        )

        assertEquals(1, detections.size)
        assertEquals(PrivacyCategory.HIDDEN_CAMERA, detections.single().category)
    }
}

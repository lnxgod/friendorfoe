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
    fun maps_researched_wifi_privacy_signatures() {
        val detections = WifiPrivacyScanner.detectPrivacyNetworksForTest(
            listOf(
                WifiPrivacyScanner.WifiPrivacyNetwork("Pineapple_ABCD", "AA:BB:CC:00:00:05", -51),
                WifiPrivacyScanner.WifiPrivacyNetwork("pwnd", "AA:BB:CC:00:00:06", -53),
                WifiPrivacyScanner.WifiPrivacyNetwork("Marauder", "AA:BB:CC:00:00:07", -57),
                WifiPrivacyScanner.WifiPrivacyNetwork("VIOFO-A229-Pro", "AA:BB:CC:00:00:08", -49),
                WifiPrivacyScanner.WifiPrivacyNetwork("Arlo-VMB-1234567", "AA:BB:CC:00:00:09", -61)
            )
        )

        assertEquals(5, detections.size)
        assertTrue(detections.any { it.category == PrivacyCategory.ATTACK_TOOL && it.manufacturer == "Hak5" })
        assertTrue(detections.any { it.category == PrivacyCategory.ATTACK_TOOL && it.manufacturer == "Spacehuhn" })
        assertTrue(detections.any { it.category == PrivacyCategory.ATTACK_TOOL && it.manufacturer == "ESP32Marauder" })
        assertTrue(detections.any { it.category == PrivacyCategory.DASH_CAMERA && it.manufacturer == "Viofo" })
        assertTrue(detections.any { it.category == PrivacyCategory.HIDDEN_CAMERA && it.manufacturer == "Arlo" })
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

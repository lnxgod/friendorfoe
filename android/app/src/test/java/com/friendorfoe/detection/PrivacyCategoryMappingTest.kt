package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class PrivacyCategoryMappingTest {

    @Test
    fun maps_new_backend_privacy_categories() {
        assertEquals(
            PrivacyCategory.VENUE_BEACON,
            GlassesDetector.categorizeDeviceType("Venue Beacon")
        )
        assertEquals(
            PrivacyCategory.EVENT_BADGE,
            GlassesDetector.categorizeDeviceType("Event Badge")
        )
        assertEquals(
            PrivacyCategory.MOBILE_KEY_LOCK,
            GlassesDetector.categorizeDeviceType("Mobile Key Lock")
        )
        assertEquals(
            PrivacyCategory.BLE_HID,
            GlassesDetector.categorizeDeviceType("BLE HID")
        )
        assertEquals(
            PrivacyCategory.AURACAST,
            GlassesDetector.categorizeDeviceType("Auracast")
        )
        assertEquals(
            PrivacyCategory.APPLE_CONTINUITY,
            GlassesDetector.categorizeDeviceType("Apple Continuity Nearby Info")
        )
    }

    @Test
    fun maps_registered_flock_wifi_oui_to_alpr_camera() {
        val registeredOui = GlassesDetector.checkWifiBssid(
            ssid = "",
            bssid = "B4:1E:52:AA:BB:CC",
            rssi = -58
        )
        assertNotNull(registeredOui)
        assertEquals(PrivacyCategory.ALPR_CAMERA, registeredOui!!.category)
        assertEquals("Flock Safety", registeredOui.manufacturer)
        assertEquals("wifi_oui:flock:B4:1E:52", registeredOui.matchReason)

        assertNull(
            GlassesDetector.checkWifiBssid(
                ssid = "",
                bssid = "14:5A:FC:A9:10:EF",
                rssi = -58
            )
        )
        assertNull(
            GlassesDetector.checkWifiBssid(
                ssid = "",
                bssid = "82:6B:F2:00:00:01",
                rssi = -58
            )
        )
    }

    @Test
    fun maps_registered_flock_wifi_oui_across_common_mac_formats() {
        val bssids = listOf(
            " b4:1e:52:aa:bb:cc ",
            "B4-1E-52-AA-BB-CC",
            "B41E52AABBCC",
            "B41E.52AA.BBCC"
        )

        bssids.forEach { bssid ->
            val detection = GlassesDetector.checkWifiBssid(
                ssid = "",
                bssid = bssid,
                rssi = -58
            )
            assertNotNull("Expected Flock OUI for $bssid", detection)
            assertEquals("Flock Safety", detection!!.manufacturer)
            assertEquals("wifi_oui:flock:B4:1E:52", detection.matchReason)
        }

        listOf(
            "XB4:1E:52:AA:BB:CC",
            "B4:1E:5Z:AA:BB:CC"
        ).forEach { bssid ->
            assertNull(
                "Malformed near-match should not map to Flock: $bssid",
                GlassesDetector.checkWifiBssid(ssid = "", bssid = bssid, rssi = -58)
            )
        }
    }

    @Test
    fun rejects_broad_wifi_privacy_false_positives() {
        assertNull(
            GlassesDetector.checkWifiSsid(
                ssid = "MVP Guest",
                bssid = "AA:BB:CC:00:00:06",
                rssi = -52
            )
        )
        assertNull(
            GlassesDetector.checkWifiSsid(
                ssid = "FlockGuest",
                bssid = "AA:BB:CC:00:00:07",
                rssi = -52
            )
        )
        assertNull(
            GlassesDetector.checkWifiSsid(
                ssid = "1234567890",
                bssid = "AA:BB:CC:00:00:08",
                rssi = -52
            )
        )
        listOf(
            "Flock-Field-Bridge",
            "FlockOS-Field-Bridge",
            "FLK-Field",
            "Penguin-1234567890",
            "ALPR-maint"
        ).forEach { ssid ->
            assertNull(
                GlassesDetector.checkWifiSsid(
                    ssid = ssid,
                    bssid = "AA:BB:CC:00:00:09",
                    rssi = -52
                )
            )
        }
    }

    @Test
    fun maps_wifi_privacy_ssids_to_camera_and_attack_categories() {
        val tapo = GlassesDetector.checkWifiSsid(
            ssid = "Tapo_Cam_ABCD",
            bssid = "AA:BB:CC:00:00:02",
            rssi = -52
        )
        assertNotNull(tapo)
        assertEquals(PrivacyCategory.HIDDEN_CAMERA, tapo!!.category)
        assertEquals("TP-Link", tapo.manufacturer)

        val deauther = GlassesDetector.checkWifiSsid(
            ssid = "pwnd",
            bssid = "AA:BB:CC:00:00:03",
            rssi = -48
        )
        assertNotNull(deauther)
        assertEquals(PrivacyCategory.ATTACK_TOOL, deauther!!.category)
        assertEquals("Spacehuhn", deauther.manufacturer)

        val viofo = GlassesDetector.checkWifiSsid(
            ssid = "VIOFO-A229-Pro",
            bssid = "AA:BB:CC:00:00:04",
            rssi = -50
        )
        assertNotNull(viofo)
        assertEquals(PrivacyCategory.DASH_CAMERA, viofo!!.category)
        assertEquals("Viofo", viofo.manufacturer)

        val marauder = GlassesDetector.checkWifiSsid(
            ssid = "ESP32Marauder",
            bssid = "AA:BB:CC:00:00:05",
            rssi = -55
        )
        assertNotNull(marauder)
        assertEquals(PrivacyCategory.ATTACK_TOOL, marauder!!.category)
        assertEquals("ESP32Marauder", marauder.manufacturer)
    }

    @Test
    fun maps_recorder_pen_and_payment_reader_categories() {
        assertEquals(
            PrivacyCategory.VOICE_RECORDER,
            GlassesDetector.categorizeDeviceType("Voice Recorder")
        )
        assertEquals(
            PrivacyCategory.SMART_PEN,
            GlassesDetector.categorizeDeviceType("Smart Pen")
        )
        assertEquals(
            PrivacyCategory.PAYMENT_READER,
            GlassesDetector.categorizeDeviceType("Payment Reader")
        )
        assertEquals(
            PrivacyCategory.PAYMENT_READER,
            GlassesDetector.categorizeDeviceType("Card Reader")
        )
    }

    @Test
    fun keeps_medical_hearing_and_personal_devices_informational() {
        assertEquals(
            PrivacyCategory.INFORMATIONAL,
            GlassesDetector.categorizeDeviceType("Hearing Aid")
        )
        assertEquals(
            PrivacyCategory.INFORMATIONAL,
            GlassesDetector.categorizeDeviceType("CGM Sensor")
        )
        assertEquals(
            PrivacyCategory.INFORMATIONAL,
            GlassesDetector.categorizeDeviceType("Personal Device")
        )
    }
}

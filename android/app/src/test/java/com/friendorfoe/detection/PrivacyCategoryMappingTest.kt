package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
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
    fun maps_flock_wifi_signatures_to_alpr_camera() {
        val fieldOui = GlassesDetector.checkWifiBssid(
            ssid = "",
            bssid = "14:5A:FC:A9:10:EF",
            rssi = -58
        )
        assertNotNull(fieldOui)
        assertEquals(PrivacyCategory.ALPR_CAMERA, fieldOui!!.category)
        assertEquals("Flock Safety", fieldOui.manufacturer)
        assertEquals("wifi_oui:flock:14:5A:FC", fieldOui.matchReason)

        val penguin = GlassesDetector.checkWifiSsid(
            ssid = "Penguin-1234567890",
            bssid = "AA:BB:CC:00:00:01",
            rssi = -64
        )
        assertNotNull(penguin)
        assertEquals(PrivacyCategory.ALPR_CAMERA, penguin!!.category)
        assertEquals("Flock Safety", penguin.manufacturer)
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
            ssid = "Advanced-Deauther",
            bssid = "AA:BB:CC:00:00:03",
            rssi = -48
        )
        assertNotNull(deauther)
        assertEquals(PrivacyCategory.ATTACK_TOOL, deauther!!.category)
        assertEquals("Generic", deauther.manufacturer)
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

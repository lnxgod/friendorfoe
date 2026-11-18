package com.friendorfoe.detection

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class CameraBroadcastSignaturesTest {
    @Test fun thetaMatchesItsAdvertisedServiceButNotConnectedGattServices() {
        val match = CameraBroadcastSignatures.advertisedService(UUID.fromString("F74D1BA5-BC52-4F10-99C0-85E785EBE43F"))
        assertEquals("Ricoh", match?.brand)
        assertEquals(PrivacyCategory.ACTION_CAMERA, match?.category)
        for (uuid in listOf("f74d1ba5-bc52-4f10-99c0-85e785ebe43e",
            "b6ac7a7e-8c01-4a52-b188-68d53df53ea2", "0000180a-0000-1000-8000-00805f9b34fb")) {
            assertNull(CameraBroadcastSignatures.advertisedService(UUID.fromString(uuid)))
        }
    }

    @Test fun cameraSetupAndDirectNamesRequireCompleteFormats() {
        for ((ssid, brand) in mapOf("THETAYR12345678.OSC" to "Ricoh", "THETAYN12345678.OSC" to "Ricoh",
            "THETAAA01234567.OSC" to "Ricoh", "Kasa_Cam_12af" to "TP-Link", "Tapo_Cam_8Aa0" to "TP-Link")) {
            val detection = GlassesDetector.checkWifiSsid(ssid, "02:00:00:00:00:01", -55)
            assertEquals(ssid, brand, detection?.manufacturer)
            assertNotNull(ssid, detection?.details?.get("limitation"))
        }
        for (ssid in listOf("THETAYR1234567.OSC", "THETAYR12345678.OSC Guest", "12345678", "Guest.OSC",
            "Kasa_Cam_12GG", "Tapo_Cam_", "Tapo_Cam_12345", "Tapo_Plug_12AB", "TAPO-Hub",
            "TuyaSmart-Plug", "SmartLife-AB12", "ESP32-thermostat", "AI-THINKER_12", "AI_School", "AIS WiFi")) {
            assertNull(ssid, GlassesDetector.checkWifiSsid(ssid, "02:00:00:00:00:01", -55))
        }
    }

    @Test fun specificCameraNetworkIsMoreInformativeThanVendorOnlyBssid() {
        val hint = GlassesDetector.checkWifiSsid("Kasa_Cam_ABCD", "00:25:DF:11:22:33", -55)
        assertEquals("wifi_ssid:Kasa_Cam_", hint?.matchReason)
    }
}

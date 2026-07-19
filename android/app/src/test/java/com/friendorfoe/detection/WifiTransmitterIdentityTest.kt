package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class WifiTransmitterIdentityTest {

    @Test
    fun canonicalizes_valid_unicast_bssid() {
        assertEquals(
            "60:60:1F:AA:BB:CC",
            WifiTransmitterIdentity.normalize("60-60-1f-aa-bb-cc"),
        )
    }

    @Test
    fun rejects_non_identity_addresses() {
        listOf(
            null,
            "",
            "02:00:00:00:00:00",
            "00:00:00:00:00:00",
            "FF:FF:FF:FF:FF:FF",
            "01:00:5E:00:00:01",
            "not-a-mac",
        ).forEach { raw ->
            assertNull(raw, WifiTransmitterIdentity.normalize(raw))
        }
    }

    @Test
    fun same_ssid_uses_distinct_valid_bssids_for_detection_ids() {
        val first = WifiTransmitterIdentity.detectionId(
            prefix = "wifi",
            ssid = "DJI-MAVIC3",
            normalizedBssid = "60:60:1F:00:00:01",
        )
        val second = WifiTransmitterIdentity.detectionId(
            prefix = "wifi",
            ssid = "DJI-MAVIC3",
            normalizedBssid = "60:60:1F:00:00:02",
        )

        assertNotEquals(first, second)
    }

    @Test
    fun invalid_bssid_falls_back_to_normalized_ssid() {
        assertEquals(
            "wifi_dji_mavic3",
            WifiTransmitterIdentity.detectionId(
                prefix = "wifi",
                ssid = "DJI-MAVIC3",
                normalizedBssid = null,
            ),
        )
        assertEquals(
            "ssid:dji_mavic3",
            WifiTransmitterIdentity.identityKey(
                ssid = "DJI-MAVIC3",
                normalizedBssid = null,
            ),
        )
    }
}

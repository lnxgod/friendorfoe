package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlTransportPolicyTest {

    @Test
    fun `badge commands select USB even when legacy fallbacks are connected`() {
        assertEquals(
            BadgeControlTransport.USB,
            BadgeControlTransportPolicy.select(hasUsb = true, hasBle = true, hasHttp = true),
        )
        assertNull(BadgeControlTransportPolicy.select(hasUsb = false, hasBle = true, hasHttp = false))
        assertNull(BadgeControlTransportPolicy.select(hasUsb = false, hasBle = false, hasHttp = true))
        assertNull(BadgeControlTransportPolicy.select(hasUsb = false, hasBle = true, hasHttp = true))
    }

    @Test
    fun `badge BLE tether stays off while read only HTTP status remains available`() {
        assertFalse(BadgeControlTransportPolicy.allowsBleTether())
        assertTrue(BadgeControlTransportPolicy.allowsReadOnlyHttpStatus())
        assertEquals(
            "Attach a FoF badge over USB-C to send controls",
            BadgeControlTransportPolicy.controlConnectionGuidance(),
        )
    }

    @Test
    fun `badge command surfaces require USB while AP and debug status stay refreshable`() {
        assertTrue(BadgeControlTransportPolicy.allowsCommandSurface(BadgeUsbStatus.CONNECTED))
        for (status in BadgeUsbStatus.entries - BadgeUsbStatus.CONNECTED) {
            assertFalse(status.name, BadgeControlTransportPolicy.allowsCommandSurface(status))
        }

        assertTrue(BadgeControlTransportPolicy.allowsStatusRefresh(BadgeUsbStatus.CONNECTED))
        assertTrue(BadgeControlTransportPolicy.allowsStatusRefresh(BadgeUsbStatus.AP_CONNECTED))
        assertTrue(BadgeControlTransportPolicy.allowsStatusRefresh(BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED))
        assertFalse(BadgeControlTransportPolicy.allowsStatusRefresh(BadgeUsbStatus.BLE_CONNECTED))
        assertFalse(BadgeControlTransportPolicy.allowsStatusRefresh(BadgeUsbStatus.DISCONNECTED))
    }

    @Test
    fun `Android control commands use an exact fail closed allowlist`() {
        val allowed = setOf(
            "set_mode",
            "reboot",
            "badge_display_policy",
            "badge_display_policy_reset",
            "badge_theme",
            "badge_theme_reset",
            "display_nav",
        )
        allowed.forEach { command ->
            assertTrue(command, BadgeControlTransportPolicy.allowsAndroidControlCommand(command))
        }

        setOf(
            "",
            "bootloader",
            "FOF_BOOTLOADER",
            "rollback",
            "fw_relay",
            "fw_upload_begin",
            "uplink_ota_begin",
            "ota",
            "REBOOT",
            " reboot",
            "reboot ",
            "ble_investigate",
            "ble_investigation_chunk",
        ).forEach { command ->
            assertFalse(command, BadgeControlTransportPolicy.allowsAndroidControlCommand(command))
        }
    }
}

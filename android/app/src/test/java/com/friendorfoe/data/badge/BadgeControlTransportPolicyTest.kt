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
    fun `Android scanner firmware upload is disabled in favor of laptop USB staging`() {
        assertFalse(BadgeControlTransportPolicy.allowsAndroidFirmwareUpload())
        assertEquals(
            "Stage the shared scanner firmware from a laptop over USB. " +
                "The uplink automatically converges both scanner slots one at a time " +
                "when the staged version is newer.",
            BadgeControlTransportPolicy.scannerFirmwareStagingGuidance(),
        )
        assertFalse(
            BadgeControlTransportPolicy.scannerFirmwareStagingGuidance()
                .contains("then use the relay control", ignoreCase = true),
        )
        assertEquals(
            "Manual Per-Slot Relay (Recovery Only)",
            BadgeControlTransportPolicy.scannerFirmwareRecoveryHeading(),
        )
        assertEquals(
            "Recover Slot 0",
            BadgeControlTransportPolicy.scannerFirmwareRecoveryActionLabel("ble"),
        )
        assertEquals(
            "Recover Slot 1",
            BadgeControlTransportPolicy.scannerFirmwareRecoveryActionLabel("wifi"),
        )
    }
}

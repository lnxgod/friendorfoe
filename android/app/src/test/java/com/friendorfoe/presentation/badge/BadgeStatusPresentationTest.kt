package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeStatusPresentationTest {

    @Test
    fun `device identity never hides an actionable connection error`() {
        val state = BadgeUsbState(
            status = BadgeUsbStatus.ERROR,
            deviceName = "USB JTAG serial debug unit",
            message = "Could not claim USB badge interface",
        )

        assertEquals(
            listOf(
                "USB JTAG serial debug unit",
                "Could not claim USB badge interface",
            ),
            badgeStatusIdentityLines(state),
        )
    }

    @Test
    fun `health rows expose uplink scanners usb runtime heap stack and psram`() {
        val status = BadgeControlStatus(
            firmwareTarget = "uplink-s3-fof_badge",
            modeLabel = "USB Only",
            scanners = listOf(
                BadgeScannerStatus(
                    slot = 0,
                    connected = true,
                    slotRole = "ble",
                    scanProfile = "ble",
                    roleAcked = true,
                    health = "ok",
                    targetVersion = "0.64.76",
                ),
                BadgeScannerStatus(
                    slot = 1,
                    connected = false,
                    slotRole = "wifi",
                    scanProfile = "wifi",
                    roleAcked = false,
                    health = "offline",
                ),
            ),
            usbControlAgeSeconds = 4,
            resetReason = "software",
            crashCount = 1,
            stackMainFree = 4096,
            stackDisplayFree = 3072,
            stackUsbFree = 2048,
            stackUartBleFree = 1536,
            stackUartWifiFree = 1024,
            heapInternalFree = 131072,
            heapInternalMinFree = 65536,
            heapInternalLargest = 32768,
            psramTotal = 8L * 1024 * 1024,
            psramFree = 7L * 1024 * 1024,
            psramLargest = 6L * 1024 * 1024,
        )

        val rows = badgeStatusHealthRows(status).toMap()

        listOf(
            "Uplink",
            "Scanner 0",
            "Scanner 1",
            "USB age",
            "Runtime",
            "Heap",
            "Stack",
            "PSRAM",
        ).forEach { label -> assertTrue("missing $label", rows.containsKey(label)) }
        assertTrue(rows.getValue("Scanner 0").contains("Connected"))
        assertTrue(rows.getValue("Scanner 1").contains("Offline"))
        assertTrue(rows.getValue("USB age").contains("4s"))
        assertTrue(rows.getValue("Uplink").contains("Attention"))
        assertTrue(rows.getValue("PSRAM").contains("7.0 MiB free / 8.0 MiB"))
    }

    @Test
    fun `cached status is visibly stale unless a live transport owns it`() {
        val cached = BadgeUsbState(
            status = BadgeUsbStatus.ERROR,
            controlStatus = BadgeControlStatus(version = "0.64.76"),
        )
        assertTrue(badgeStatusIsStale(cached))

        listOf(
            BadgeUsbStatus.CONNECTED,
            BadgeUsbStatus.AP_CONNECTED,
            BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            BadgeUsbStatus.BLE_CONNECTED,
        ).forEach { liveStatus ->
            assertFalse(badgeStatusIsStale(cached.copy(status = liveStatus)))
        }
    }
}

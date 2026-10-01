package com.friendorfoe.data.probes

import org.junit.Assert.*
import org.junit.Test

class UsbProbeProtocolTest {
    private val hello = """{"type":"hello","protocol":1,"boot":"0123456789abcdef","firmware":"fof-wifi-probe","version":"1.0.0","sensor_id":"usb-aabbccddeeff","uptime_ms":1000,"channel":1,"dropped":0}"""
    private fun probe(seq: Int = 1, name: String = "Home, Wi-Fi", wildcard: Boolean = false, binary: Boolean = false) =
        """{"type":"probe","protocol":1,"boot":"0123456789abcdef","seq":$seq,"uptime_ms":1100,"mac":"02:11:22:33:44:55","ssid":"$name","wildcard":$wildcard,"binary":$binary,"rssi":-48,"channel":6}"""

    @Test fun usbIdentityDoesNotClaimBadgesBootloadersOrOtherCdcDevices() {
        assertTrue(isUsbProbeProduct(0x303a, 0x4001, "FoF WiFi Probe"))
        assertFalse(isUsbProbeProduct(0x303a, 0x1001, "USB JTAG/serial debug unit"))
        assertFalse(isUsbProbeProduct(0x303a, 0x4001, "FoF Badge"))
        assertFalse(isUsbProbeProduct(0x303a, 0x4001, null))
        assertFalse(isUsbProbeProduct(0x1234, 0x4001, "FoF WiFi Probe"))
    }

    @Test fun fragmentsUnicodeAndRecoversFromOverflowAndInvalidUtf8() {
        val framer = UsbProbeLineFramer()
        val bytes = "Café☕\nnext\r\n".toByteArray()
        val lines = bytes.flatMap { framer.accept(byteArrayOf(it), 1) }
        assertEquals(listOf("Café☕", "next"), lines)
        val oversized = ("x".repeat(513) + "\ngood\n").toByteArray()
        assertEquals(listOf("good"), framer.accept(oversized, oversized.size))
        assertEquals(emptyList<String>(), framer.accept(byteArrayOf(0xc3.toByte(), 10), 2))
        assertEquals(listOf("ok"), framer.accept("ok\n".toByteArray(), 3))
    }

    @Test fun requiresCompatibleHelloAndRejectsMalformedFields() {
        val session = UsbProbeSession()
        assertFalse(session.accept(probe(), 0, 0.0))
        assertFalse(session.accept(hello.replace("\"protocol\":1", "\"protocol\":2"), 0, 0.0))
        assertFalse(session.accept(hello.replace("fof-wifi-probe", "badge"), 0, 0.0))
        assertTrue(session.accept(hello, 0, 0.0))
        listOf("{}", "broken", probe().replace("-48", "null"), probe().replace("-48", "1"),
            probe().replace("\"channel\":6", "\"channel\":36"), probe().replace("\"seq\":1", "\"seq\":1.1"),
            probe().replace("02:11", "ff:11"), probe(name = "x".repeat(33)), probe(name = "", wildcard = false),
            probe(wildcard = true), probe(name = "", wildcard = true, binary = true)
        ).forEach { assertFalse(it, session.accept(it, 100, 0.1)) }
        assertTrue(session.accept(probe(), 100, 0.1))
    }

    @Test fun preservesLiteralNamesAndSeparatesWildcardAndBinary() {
        val session = UsbProbeSession()
        session.accept(hello, 0, 0.0)
        assertTrue(session.accept(probe(1), 100, 0.1))
        assertTrue(session.accept(probe(2, "(broadcast)"), 200, 0.2))
        assertTrue(session.accept(probe(3, "", wildcard = true), 300, 0.3))
        assertTrue(session.accept(probe(4, "", binary = true), 400, 0.4))
        val row = session.snapshot(1400).transmitters.single()
        assertEquals(listOf("(broadcast)", "Home, Wi-Fi"), row.targets.map { it.ssid })
        assertEquals(1, row.wildcardReports); assertEquals(1, row.unknownReports)
        assertEquals(4, row.reports); assertEquals(1.0, row.ageSeconds, 0.0)
        assertTrue(row.locallyAdministered)
    }

    @Test fun rejectsDuplicatesOldSessionsAndExpiredHeartbeatAndClearsOnReboot() {
        val session = UsbProbeSession()
        session.accept(hello, 0, 0.0)
        assertTrue(session.accept(probe(2), 100, 0.1))
        assertFalse(session.accept(probe(2), 200, 0.2))
        assertFalse(session.accept(probe(1), 200, 0.2))
        assertFalse(session.accept(probe(3).replace("0123456789abcdef", "1123456789abcdef"), 200, 0.2))
        assertFalse(session.accept(probe(3), 8001, 8.0))
        session.accept(hello.replace("0123456789abcdef", "1123456789abcdef"), 8100, 8.1)
        assertTrue(session.snapshot(8100).transmitters.isEmpty())
        assertFalse(session.accept(probe(4), 8100, 8.1))
    }

    @Test fun expiresEachTargetAndKeepsLatestSignal() {
        val session = UsbProbeSession()
        session.accept(hello, 0, 0.0)
        session.accept(probe(1, "Old"), 100, 0.1)
        session.accept(probe(2, "New").replace("-48", "-75"), 1000, 1.0)
        val row = session.snapshot(300500).transmitters.single()
        assertEquals(listOf("New"), row.targets.map { it.ssid })
        assertEquals(-75, row.rssi)
        assertTrue(session.snapshot(301001).transmitters.isEmpty())
    }

    @Test fun boundsHeavyTraffic() {
        val session = UsbProbeSession()
        session.accept(hello, 0, 0.0)
        repeat(6100) { session.accept(probe(it + 1), 100, 0.1) }
        assertEquals(6000, session.snapshot(100).transmitters.single().reports)
    }
}

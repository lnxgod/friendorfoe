package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.UUID

class BlePacketParserTest {

    @Test
    fun parses_iBeacon_manufacturer_data_with_full_metadata() {
        val data = bytes(
            0x02, 0x15,
            0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
            0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
            0x12, 0x34,
            0xAB, 0xCD,
            0xC5
        )

        val parsed = BlePacketParser.parseIBeaconManufacturerData(data)

        assertNotNull(parsed)
        assertEquals(UUID.fromString("e2c56db5-dffb-48d2-b060-d0f5a71096e0"), parsed!!.uuid)
        assertEquals(0x1234, parsed.major)
        assertEquals(0xABCD, parsed.minor)
        assertEquals(-59, parsed.txPower)
    }

    @Test
    fun rejects_short_or_non_iBeacon_manufacturer_data() {
        assertNull(BlePacketParser.parseIBeaconManufacturerData(bytes(0x02, 0x15, 0xE2)))
        assertNull(BlePacketParser.parseIBeaconManufacturerData(bytes(0x10, 0x02, 0x00)))
    }

    @Test
    fun parses_eddystone_url_service_data() {
        val data = bytes(
            0x10,
            0xEC,
            0x01,
            'e'.code, 'x'.code, 'a'.code, 'm'.code, 'p'.code, 'l'.code, 'e'.code,
            0x00
        )

        val parsed = BlePacketParser.parseEddystoneUrlServiceData(data)

        assertNotNull(parsed)
        assertEquals("https://www.example.com/", parsed!!.url)
        assertEquals(-20, parsed.txPower)
    }

    @Test
    fun parses_eddystone_tlm_service_data() {
        val data = bytes(
            0x20, 0x00,
            0x0B, 0xB8,
            0x17, 0x80,
            0x00, 0x00, 0x01, 0x23,
            0x00, 0x00, 0x04, 0xD2
        )

        val parsed = BlePacketParser.parseEddystoneTlmServiceData(data)

        assertNotNull(parsed)
        assertEquals(3000, parsed!!.batteryMillivolts)
        assertEquals(23.5f, parsed.temperatureC, 0.01f)
        assertEquals(291L, parsed.advertCount)
        assertEquals(123L, parsed.uptimeSeconds)
    }

    @Test
    fun parses_eddystone_uid_service_data() {
        val data = bytes(
            0x00,
            0xEE,
            0x01, 0x02, 0x03, 0x04, 0x05,
            0x06, 0x07, 0x08, 0x09, 0x0A,
            0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
            0x00, 0x00
        )

        val parsed = BlePacketParser.parseEddystoneUidServiceData(data)

        assertNotNull(parsed)
        assertEquals("0102030405060708090A", parsed!!.namespaceHex)
        assertEquals("0B0C0D0E0F10", parsed.instanceHex)
        assertEquals(-18, parsed.txPower)
    }

    @Test
    fun parses_eddystone_eid_service_data() {
        val data = bytes(
            0x30,
            0xED,
            0xA0, 0xA1, 0xA2, 0xA3,
            0xA4, 0xA5, 0xA6, 0xA7
        )

        val parsed = BlePacketParser.parseEddystoneEidServiceData(data)

        assertNotNull(parsed)
        assertEquals("A0A1A2A3A4A5A6A7", parsed!!.eidHex)
        assertEquals(-19, parsed.txPower)
    }

    private fun bytes(vararg values: Int): ByteArray =
        values.map { it.toByte() }.toByteArray()
}

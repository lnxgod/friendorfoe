package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Regression tests for BleSignatures — guards against accidental constant drift
 * versus the ESP32 scanner firmware. Numeric values mirror
 * esp32/scanner/main/detection/ble_fingerprint.c and uart_protocol.h.
 */
class BleSignaturesTest {

    @Test
    fun `core CIDs match firmware constants`() {
        assertEquals(0x004C, BleSignatures.CID_APPLE)
        assertEquals(0x0006, BleSignatures.CID_MICROSOFT)
        assertEquals(0x0075, BleSignatures.CID_SAMSUNG)
        assertEquals(0x2CA5, BleSignatures.CID_DJI)
        assertEquals(0x01AB, BleSignatures.CID_META)
        assertEquals(0x058E, BleSignatures.CID_META_TECH)
        // Luxottica — the Meta-detection parity fix
        assertEquals(0x0D53, BleSignatures.CID_LUXOTTICA)
        assertEquals(0x0E29, BleSignatures.CID_FLIPPER)
    }

    @Test
    fun `Meta service UUIDs match firmware`() {
        assertEquals(0xFD5F, BleSignatures.SVC_META_RAYBAN_G2)
        assertEquals(0xFEB7, BleSignatures.SVC_META_1)
        assertEquals(0xFEB8, BleSignatures.SVC_META_2)
    }

    @Test
    fun `Apple Continuity sub-types match firmware defines`() {
        assertEquals(0x05, BleSignatures.APPLE_AIRDROP)
        assertEquals(0x07, BleSignatures.APPLE_AIRPODS)
        assertEquals(0x09, BleSignatures.APPLE_AIRPLAY)
        assertEquals(0x0C, BleSignatures.APPLE_HANDOFF)
        assertEquals(0x0D, BleSignatures.APPLE_TETHER_SOURCE)
        assertEquals(0x0E, BleSignatures.APPLE_TETHER_TARGET)
        assertEquals(0x0F, BleSignatures.APPLE_NEARBY_ACTION)
        assertEquals(0x10, BleSignatures.APPLE_NEARBY_INFO)
        assertEquals(0x12, BleSignatures.APPLE_FINDMY)
    }

    @Test
    fun `Apple data-flags bit layout matches uart_protocol_h`() {
        assertEquals(0x01, BleSignatures.APPLE_FLAG_AIRPODS_IN)
        assertEquals(0x02, BleSignatures.APPLE_FLAG_WIFI_ON)
        assertEquals(0x04, BleSignatures.APPLE_FLAG_WATCH_PAIRED)
        assertEquals(0x08, BleSignatures.APPLE_FLAG_ICLOUD)
        assertEquals(0x10, BleSignatures.APPLE_FLAG_AUTH_TAG)
        assertEquals(0x20, BleSignatures.APPLE_FLAG_SCREEN_ON)
    }

    @Test
    fun `nearbyFlagLabels decodes multi-bit flag byte`() {
        // 0x05 = AirPods in + Watch paired
        val labels = BleSignatures.nearbyFlagLabels(0x05)
        assertTrue("AirPods in" in labels)
        assertTrue("Watch paired" in labels)
        assertEquals(2, labels.size)
    }

    @Test
    fun `nearbyFlagLabels returns empty for zero`() {
        assertTrue(BleSignatures.nearbyFlagLabels(0).isEmpty())
    }

    @Test
    fun `nearbyActionName maps Wi-Fi Password Share`() {
        assertEquals("Wi-Fi Password Share", BleSignatures.nearbyActionName(0x0B))
        assertEquals("Vision Pro Setup", BleSignatures.nearbyActionName(0x20))
        assertEquals("Transfer Number", BleSignatures.nearbyActionName(0x14))
        assertNull(BleSignatures.nearbyActionName(0xAA))   // unknown
    }

    @Test
    fun `microsoftSwiftPairLabel maps Xbox Controller scenario`() {
        assertEquals("Xbox Controller", BleSignatures.microsoftSwiftPairLabel(0x02))
        assertEquals("Surface Pen", BleSignatures.microsoftSwiftPairLabel(0x01))
        assertNull(BleSignatures.microsoftSwiftPairLabel(0x99))
    }

    @Test
    fun `advertising flags dual-mode host bit is 0x08`() {
        assertEquals(0x08, BleSignatures.ADV_FLAG_SIMUL_LE_BR_EDR_HOST)
    }

    @Test
    fun `Pwnagotchi default BSSID is correct pattern`() {
        assertEquals("DE:AD:BE:EF:DE:AD", BleSignatures.PWNAGOTCHI_BSSID)
    }

    @Test
    fun `FNV1a constants match RFC values`() {
        // 0x811c9dc5 offset basis and 0x01000193 prime — FNV-1a 32-bit.
        assertEquals(0x811c9dc5.toInt(), BleSignatures.FNV1A_OFFSET_32)
        assertEquals(0x01000193, BleSignatures.FNV1A_PRIME_32)
    }
}

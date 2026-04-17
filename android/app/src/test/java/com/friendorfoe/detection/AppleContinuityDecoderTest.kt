package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Fixture-driven tests for AppleContinuityDecoder. Byte sequences modelled after
 * furiousMAC Continuity captures. All input arrays represent the Apple mfr-data
 * bytes AFTER the 2-byte CID prefix — so the first byte is the Continuity type.
 */
class AppleContinuityDecoderTest {

    @Test
    fun `empty input returns null`() {
        assertNull(AppleContinuityDecoder.decode(ByteArray(0)))
    }

    @Test
    fun `AirTag with length byte 0x19 classifies as AirTag`() {
        // Type 0x12 + sub-length 0x19 + 25 bytes of opaque payload (padded)
        val bytes = byteArrayOf(
            0x12, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        )
        val result = AppleContinuityDecoder.decode(bytes)
        assertNotNull(result)
        assertEquals(0x12, result!!.subType)
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_AIRTAG, result.deviceType)
    }

    @Test
    fun `short FindMy payload classifies as AirTag legacy fallback`() {
        val bytes = byteArrayOf(0x12, 0x02, 0x00, 0x00)   // len <=8 heuristic
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_AIRTAG, result.deviceType)
    }

    @Test
    fun `longer FindMy payload without 0x19 classifies as generic FindMy`() {
        // Length byte != 0x19, payload > 8 bytes → generic FindMy accessory
        val bytes = byteArrayOf(
            0x12, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88.toByte(), 0x99.toByte()
        )
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_FINDMY, result.deviceType)
    }

    @Test
    fun `AirPods type 0x07 classifies as AirPods`() {
        val bytes = byteArrayOf(0x07, 0x19, 0x01, 0x02, 0x20, 0x75)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_AIRPODS, result.deviceType)
    }

    @Test
    fun `Handoff type 0x0C classifies as MacBook`() {
        val bytes = byteArrayOf(0x0C, 0x0E, 0x00, 0x00, 0x00)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_MACBOOK, result.deviceType)
    }

    @Test
    fun `Tether Source type 0x0D classifies as iPhone`() {
        // The only Apple Continuity type that unambiguously identifies an iPhone.
        val bytes = byteArrayOf(0x0D, 0x07, 0x05, 0x34)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_IPHONE, result.deviceType)
    }

    @Test
    fun `Nearby Info type 0x10 classifies as Apple Generic (not iPhone)`() {
        // v0.58 honest Apple: 0x10 does NOT reveal device model. Never claim iPhone.
        // Layout (CID-stripped):
        //   [0] = type 0x10
        //   [1..3] = auth tag (3 bytes)
        //   [4] = activity / iOS-version-high-nibble byte
        //   [5] = data-flags byte
        val bytes = byteArrayOf(
            0x10,
            0xA1.toByte(), 0xB2.toByte(), 0xC3.toByte(),   // auth
            0x15,                                          // high nibble 1 → "iOS 1" (fixture)
            0x05                                           // flags = AirPods in + Watch paired
        )
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_GENERIC, result.deviceType)
        assertNotNull(result.authTag)
        assertEquals(0xA1, result.authTag!![0].toInt() and 0xFF)
        assertEquals(0xB2, result.authTag!![1].toInt() and 0xFF)
        assertEquals(0xC3, result.authTag!![2].toInt() and 0xFF)
    }

    @Test
    fun `Nearby Info decodes iOS version nibble`() {
        val bytes = byteArrayOf(
            0x10,
            0x11, 0x22, 0x33,   // auth
            0xF0.toByte(),       // high nibble = 0xF — iOS 15 in real captures uses 0xFx
            0x00
        )
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(0xF, result.iosVersionNibble)
    }

    @Test
    fun `Nearby Info decodes data-flags byte with bit combinations`() {
        // flagsByte = 0x05 = AirPods + Watch paired
        val bytes = byteArrayOf(
            0x10,
            0x01, 0x02, 0x03,   // auth
            0x15,               // activity / iOS nibble
            0x05                // flags
        )
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(0x05, result.flagsByte)
        assertTrue(result.hasFlagLabels())
        val labels = result.flagLabel()!!
        assertTrue("label contains AirPods: $labels", labels.contains("AirPods"))
        assertTrue("label contains Watch: $labels", labels.contains("Watch"))
    }

    @Test
    fun `Nearby Action type 0x0F decodes sub-type byte`() {
        // Byte +1 in CID-stripped buffer holds the action sub-type.
        // 0x0B = Wi-Fi Password Share.
        val bytes = byteArrayOf(0x0F, 0x0B, 0x11, 0x22, 0x33, 0x00)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(0x0B, result.nearbyActionSubType)
        assertEquals("Apple (Wi-Fi Password Share)", result.enrichedLabel())
    }

    @Test
    fun `AirDrop type 0x05 classifies as Apple Generic`() {
        val bytes = byteArrayOf(0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals(AppleContinuityDecoder.AppleDeviceType.APPLE_GENERIC, result.deviceType)
    }

    @Test
    fun `enrichedLabel for Nearby Info with AirPods bit produces bullet list`() {
        val bytes = byteArrayOf(
            0x10,
            0x01, 0x02, 0x03,  // auth
            0x15,              // activity / iOS nibble
            0x01               // flagsByte = AirPods in
        )
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals("Apple (AirPods in)", result.enrichedLabel())
    }

    @Test
    fun `enrichedLabel falls back to device type for flag-less types`() {
        val bytes = byteArrayOf(0x12, 0x19, 0x00)
        val result = AppleContinuityDecoder.decode(bytes)!!
        assertEquals("AirTag", result.enrichedLabel())
    }
}

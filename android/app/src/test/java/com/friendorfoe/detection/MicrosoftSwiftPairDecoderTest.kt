package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class MicrosoftSwiftPairDecoderTest {

    @Test
    fun `empty input returns null`() {
        assertNull(MicrosoftSwiftPairDecoder.decode(ByteArray(0)))
    }

    @Test
    fun `non-advertising-beacon type returns null`() {
        // first byte must be 0x03 (Advertising Beacon sub-type)
        val bytes = byteArrayOf(0x01, 0x02)
        assertNull(MicrosoftSwiftPairDecoder.decode(bytes))
    }

    @Test
    fun `Xbox controller scenario byte 0x02 decodes`() {
        val bytes = byteArrayOf(0x03, 0x02)
        val result = MicrosoftSwiftPairDecoder.decode(bytes)!!
        assertEquals(0x03, result.beaconType)
        assertEquals(0x02, result.scenario)
        assertEquals("Xbox Controller", result.label)
    }

    @Test
    fun `Surface Pen scenario byte 0x01 decodes`() {
        val bytes = byteArrayOf(0x03, 0x01)
        val result = MicrosoftSwiftPairDecoder.decode(bytes)!!
        assertEquals("Surface Pen", result.label)
    }

    @Test
    fun `unknown scenario returns SwiftPair with null label`() {
        val bytes = byteArrayOf(0x03, 0x77)
        val result = MicrosoftSwiftPairDecoder.decode(bytes)!!
        assertEquals(0x77, result.scenario)
        assertNull(result.label)
    }
}

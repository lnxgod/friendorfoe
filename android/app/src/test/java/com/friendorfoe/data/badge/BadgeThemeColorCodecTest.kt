package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class BadgeThemeColorCodecTest {

    @Test
    fun `hex parser accepts optional hash and either hex case`() {
        assertEquals(Rgb888(255, 76, 169), BadgeThemeColorCodec.parseHex("#FF4CA9"))
        assertEquals(Rgb888(255, 76, 169), BadgeThemeColorCodec.parseHex("ff4ca9"))
    }

    @Test
    fun `hex parser accepts channel boundary values`() {
        assertEquals(Rgb888(0, 0, 0), BadgeThemeColorCodec.parseHex("000000"))
        assertEquals(Rgb888(255, 255, 255), BadgeThemeColorCodec.parseHex("#FFFFFF"))
    }

    @Test
    fun `hex parser requires exactly six digits`() {
        listOf("", "#", "12345", "#12345", "1234567", "#1234567", "##123456").forEach {
            assertNull(BadgeThemeColorCodec.parseHex(it))
        }
    }

    @Test
    fun `hex parser rejects invalid characters and whitespace`() {
        listOf(
            "GG0000",
            "12345Z",
            "+12345",
            "-12345",
            " 123456",
            "123456 ",
            "#12-456",
        ).forEach {
            assertNull(BadgeThemeColorCodec.parseHex(it))
        }
    }

    @Test
    fun `rgb888 channels enforce their bounds`() {
        assertThrows(IllegalArgumentException::class.java) { Rgb888(-1, 0, 0) }
        assertThrows(IllegalArgumentException::class.java) { Rgb888(0, 256, 0) }
        assertThrows(IllegalArgumentException::class.java) { Rgb888(0, 0, 300) }
    }

    @Test
    fun `rgb565 inputs enforce their bounds`() {
        assertThrows(IllegalArgumentException::class.java) {
            BadgeThemeColorCodec.rgb565ToRgb888(-1)
        }
        assertThrows(IllegalArgumentException::class.java) {
            BadgeThemeColorCodec.rgb565ToRgb888(0x1_0000)
        }
        assertThrows(IllegalArgumentException::class.java) {
            BadgeThemeColorCodec.effectiveHex(-1)
        }
    }

    @Test
    fun `rgb888 to rgb565 truncates low channel bits`() {
        assertEquals(0x0000, BadgeThemeColorCodec.rgb888ToRgb565(Rgb888(7, 3, 7)))
        assertEquals(0x0821, BadgeThemeColorCodec.rgb888ToRgb565(Rgb888(8, 4, 8)))
        assertEquals(0xFA75, BadgeThemeColorCodec.rgb888ToRgb565(Rgb888(255, 76, 169)))
        assertEquals(0xFFFF, BadgeThemeColorCodec.rgb888ToRgb565(Rgb888(255, 255, 255)))
    }

    @Test
    fun `rgb565 preview uses floor expansion and uppercase hex`() {
        assertEquals(Rgb888(255, 76, 172), BadgeThemeColorCodec.rgb565ToRgb888(0xFA75))
        assertEquals("#FF4CAC", BadgeThemeColorCodec.effectiveHex(0xFA75))
        assertEquals("#000000", BadgeThemeColorCodec.effectiveHex(0x0000))
    }

    @Test
    fun `every rgb565 value survives the quantized round trip`() {
        for (rgb565 in 0x0000..0xFFFF) {
            assertEquals(
                rgb565,
                BadgeThemeColorCodec.rgb888ToRgb565(
                    BadgeThemeColorCodec.rgb565ToRgb888(rgb565),
                ),
            )
        }
    }
}

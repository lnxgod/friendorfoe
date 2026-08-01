package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Test

class BadgeUsbLineFramerTest {

    @Test
    fun `64 KiB protocol maximum accepts a chunked 24 KiB status frame`() {
        assertEquals(64 * 1024, BADGE_USB_MAX_FRAME_BYTES)
        val status = "FOF_STATUS:{\"padding\":\"${"x".repeat(24 * 1024)}\"}"
        val lines = mutableListOf<String>()
        var dropped = 0
        val framer = BadgeUsbLineFramer(
            onLine = { bytes, length ->
                lines += String(bytes, 0, length, Charsets.UTF_8)
            },
            onOverlongLine = { dropped++ },
        )

        val wire = (status + "\n").toByteArray()
        var offset = 0
        while (offset < wire.size) {
            val end = minOf(offset + 257, wire.size)
            framer.accept(wire.copyOfRange(offset, end))
            offset = end
        }

        assertEquals(listOf(status), lines)
        assertEquals(0, dropped)
    }

    @Test
    fun `overlong frame is dropped once and the next line is delivered`() {
        val lines = mutableListOf<String>()
        var dropped = 0
        val framer = BadgeUsbLineFramer(
            onLine = { bytes, length ->
                lines += String(bytes, 0, length, Charsets.UTF_8)
            },
            onOverlongLine = { dropped++ },
        )

        framer.accept(ByteArray(BADGE_USB_MAX_FRAME_BYTES) { 'x'.code.toByte() })
        framer.accept("\nFOF_PONG:ok\n".toByteArray())

        assertEquals(listOf("FOF_PONG:ok"), lines)
        assertEquals(1, dropped)
    }
}

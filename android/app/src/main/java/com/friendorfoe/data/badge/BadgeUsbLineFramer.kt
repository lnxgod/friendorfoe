package com.friendorfoe.data.badge

internal const val BADGE_USB_MAX_FRAME_BYTES = 64 * 1024

/**
 * Frames newline-delimited badge protocol messages without converting partial
 * chunks to text. The protocol maximum includes the terminating newline.
 */
internal class BadgeUsbLineFramer(
    private val onLine: (ByteArray, Int) -> Unit,
    private val onOverlongLine: () -> Unit,
    maxFrameBytes: Int = BADGE_USB_MAX_FRAME_BYTES,
) {
    private val lineBuffer = ByteArray(maxFrameBytes - 1)
    private var lineLength = 0
    private var droppingLine = false

    init {
        require(maxFrameBytes >= 2) { "Badge frame maximum must include payload and delimiter" }
    }

    fun accept(bytes: ByteArray, length: Int = bytes.size) {
        require(length in 0..bytes.size) { "Invalid badge USB chunk length" }
        for (index in 0 until length) {
            val byte = bytes[index]
            if (byte == '\n'.code.toByte() || byte == '\r'.code.toByte()) {
                if (!droppingLine && lineLength > 0) {
                    onLine(lineBuffer, lineLength)
                }
                lineLength = 0
                droppingLine = false
            } else if (!droppingLine) {
                if (lineLength < lineBuffer.size) {
                    lineBuffer[lineLength++] = byte
                } else {
                    lineLength = 0
                    droppingLine = true
                    onOverlongLine()
                }
            }
        }
    }
}

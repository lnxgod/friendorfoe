package com.friendorfoe.data.badge

data class Rgb888(
    val red: Int,
    val green: Int,
    val blue: Int,
) {
    init {
        require(red in 0..255) { "red must be between 0 and 255" }
        require(green in 0..255) { "green must be between 0 and 255" }
        require(blue in 0..255) { "blue must be between 0 and 255" }
    }
}

object BadgeThemeColorCodec {
    fun parseHex(value: String): Rgb888? {
        val digits = value.removePrefix("#")
        if (digits.length != 6) return null
        if (digits.any { it !in '0'..'9' && it !in 'a'..'f' && it !in 'A'..'F' }) return null
        val packed = digits.toIntOrNull(radix = 16) ?: return null
        return Rgb888(
            red = (packed shr 16) and 0xFF,
            green = (packed shr 8) and 0xFF,
            blue = packed and 0xFF,
        )
    }

    fun rgb888ToRgb565(rgb: Rgb888): Int =
        ((rgb.red shr 3) shl 11) or
            ((rgb.green shr 2) shl 5) or
            (rgb.blue shr 3)

    fun rgb565ToRgb888(rgb565: Int): Rgb888 {
        require(rgb565 in 0..0xFFFF) { "rgb565 must be between 0 and 65535" }
        return Rgb888(
            red = ((rgb565 shr 11) and 0x1F) * 255 / 31,
            green = ((rgb565 shr 5) and 0x3F) * 255 / 63,
            blue = (rgb565 and 0x1F) * 255 / 31,
        )
    }

    fun effectiveHex(rgb565: Int): String {
        val rgb = rgb565ToRgb888(rgb565)
        return buildString(capacity = 7) {
            append('#')
            appendHexByte(rgb.red)
            appendHexByte(rgb.green)
            appendHexByte(rgb.blue)
        }
    }

    private fun StringBuilder.appendHexByte(value: Int) {
        append(HEX_DIGITS[value shr 4])
        append(HEX_DIGITS[value and 0x0F])
    }

    private const val HEX_DIGITS = "0123456789ABCDEF"
}

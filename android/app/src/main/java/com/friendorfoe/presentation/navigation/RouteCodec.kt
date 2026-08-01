package com.friendorfoe.presentation.navigation

import java.nio.charset.StandardCharsets
import java.net.URLDecoder

fun encodeRouteSegment(value: String): String = buildString {
    value.toByteArray(StandardCharsets.UTF_8).forEach { byte ->
        val unsigned = byte.toInt() and 0xFF
        val unreserved = unsigned in 'a'.code..'z'.code ||
            unsigned in 'A'.code..'Z'.code ||
            unsigned in '0'.code..'9'.code ||
            unsigned == '-'.code || unsigned == '.'.code ||
            unsigned == '_'.code || unsigned == '*'.code
        if (unreserved) {
            append(unsigned.toChar())
        } else {
            append('%')
            append(HEX_DIGITS[unsigned ushr 4])
            append(HEX_DIGITS[unsigned and 0x0F])
        }
    }
}

private const val HEX_DIGITS = "0123456789ABCDEF"

fun decodeRouteSegment(value: String): String? = runCatching {
    URLDecoder.decode(
        value.replace("+", "%2B"),
        StandardCharsets.UTF_8.name(),
    )
}.getOrNull()

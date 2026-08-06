package com.friendorfoe.data.badge

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonPrimitive

internal const val BADGE_LITE_PRODUCT_ID = "badge_lite"
internal const val BADGE_LITE_TARGET = "uplink-s3-backend"
internal const val BADGE_LITE_PROJECT = "fof_backend_uplink"
internal const val BADGE_LITE_HARDWARE = "seeed_xiao_esp32s3"
internal const val BADGE_LITE_MODE = "headless"
internal const val BADGE_LITE_OWNER_ID = "badge_lite"

internal val BADGE_LITE_REQUIRED_CAPABILITIES = setOf(
    "display_none",
    "usb_live",
    "usb_live_ack",
)

internal const val BADGE_LITE_HEARTBEAT_MS = 5_000
internal const val BADGE_LITE_LEASE_MS = 15_000L

internal sealed interface BadgeLiteLiveFrame {
    data class Ready(
        val sessionId: String,
        val heartbeatMs: Int,
        val leaseMs: Long,
    ) : BadgeLiteLiveFrame

    data class Heartbeat(
        val sessionId: String,
        val sequence: ULong,
    ) : BadgeLiteLiveFrame

    data class Stopped(
        val sessionId: String,
    ) : BadgeLiteLiveFrame
}

/**
 * Parse only the acknowledged-live records implemented by Backend Badge Lite.
 * A malformed recognized record is ignored; it must never advance a live lease.
 */
internal fun parseBadgeLiteLiveFrame(line: String): BadgeLiteLiveFrame? {
    val prefix = when {
        line.startsWith("FOF_LIVE_READY:") -> "FOF_LIVE_READY:"
        line.startsWith("FOF_LIVE_HEARTBEAT:") -> "FOF_LIVE_HEARTBEAT:"
        line.startsWith("FOF_LIVE_STOPPED:") -> "FOF_LIVE_STOPPED:"
        else -> return null
    }
    val payload = runCatching {
        JsonParser.parseString(line.removePrefix(prefix)).asJsonObject
    }.getOrNull() ?: return null

    return when (prefix) {
        "FOF_LIVE_READY:" -> payload.parseLiteReady()
        "FOF_LIVE_HEARTBEAT:" -> payload.parseLiteHeartbeat()
        else -> payload.parseLiteStopped()
    }
}

internal fun badgeLiteLiveStartWire(): String =
    "FOF_LIVE_START:{\"client\":\"android\",\"protocol\":1}"

internal fun badgeLiteLiveAckWire(sessionId: String, sequence: ULong): String =
    "FOF_LIVE_ACK:{\"session_id\":${JsonPrimitive(sessionId)},\"sequence\":$sequence}"

private fun JsonObject.parseLiteReady(): BadgeLiteLiveFrame.Ready? {
    if (keySet() != setOf("session_id", "heartbeat_ms", "lease_ms")) return null
    val sessionId = strictLiteSessionId("session_id") ?: return null
    val heartbeatMs = strictLiteUnsigned("heartbeat_ms") ?: return null
    val leaseMs = strictLiteUnsigned("lease_ms") ?: return null
    if (heartbeatMs != BADGE_LITE_HEARTBEAT_MS.toULong() ||
        leaseMs != BADGE_LITE_LEASE_MS.toULong()
    ) {
        return null
    }
    return BadgeLiteLiveFrame.Ready(sessionId, heartbeatMs.toInt(), leaseMs.toLong())
}

private fun JsonObject.parseLiteHeartbeat(): BadgeLiteLiveFrame.Heartbeat? {
    if (keySet() != setOf("session_id", "sequence")) return null
    val sessionId = strictLiteSessionId("session_id") ?: return null
    val sequence = strictLiteUnsigned("sequence") ?: return null
    return BadgeLiteLiveFrame.Heartbeat(sessionId, sequence)
}

private fun JsonObject.parseLiteStopped(): BadgeLiteLiveFrame.Stopped? {
    if (keySet() != setOf("session_id")) return null
    return strictLiteSessionId("session_id")?.let(BadgeLiteLiveFrame::Stopped)
}

private fun JsonObject.strictLiteSessionId(key: String): String? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    if (!primitive.isString) return null
    return primitive.asString.takeIf { value ->
        value.isNotEmpty() && value.length <= 128 && value.all { it.code in 0x20..0x7E }
    }
}

private fun JsonObject.strictLiteUnsigned(key: String): ULong? {
    val primitive = get(key)?.takeIf { it.isJsonPrimitive }?.asJsonPrimitive ?: return null
    if (!primitive.isNumber) return null
    return primitive.asString.toULongOrNull()
}

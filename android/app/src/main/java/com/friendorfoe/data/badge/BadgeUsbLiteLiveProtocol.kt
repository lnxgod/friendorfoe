package com.friendorfoe.data.badge

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonPrimitive
import java.math.BigInteger

internal const val BADGE_LITE_LIVE_START_LINE =
    "FOF_LIVE_START:{\"client\":\"new_dash\",\"protocol\":1}"

internal fun badgeUsbLiteLiveStartLine(owner: BadgeUsbOwnerKey): String? =
    BADGE_LITE_LIVE_START_LINE.takeIf {
        owner.productKind == BadgeUsbProductKind.BADGE_LITE
    }

private val BadgeLiteLiveSequenceMax = BigInteger("18446744073709551615")
private val BadgeLiteLiveUnsignedPattern = Regex("^(0|[1-9][0-9]*)$")

internal data class BadgeUsbLiteLiveReady(
    val sessionId: String,
    val heartbeatMs: Long,
    val leaseMs: Long,
)

internal data class BadgeUsbLiteLiveHeartbeat(
    val sessionId: String,
    val sequence: String,
)

internal data class BadgeUsbLiteLiveAckTicket(
    val owner: BadgeUsbOwnerKey,
    val sessionId: String,
    val sequence: String,
)

private fun badgeLiteLiveSessionIdValid(sessionId: String): Boolean =
    sessionId.isNotEmpty() && sessionId.length <= 32 &&
        sessionId.none { it.code < 0x20 || it.code == 0x7f }

private fun JsonObject.badgeLiteStrictString(key: String): String? = get(key)
    ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
    ?.asString

private fun JsonObject.badgeLiteStrictPositiveLong(key: String): Long? {
    val primitive = get(key)
        ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isNumber }
        ?.asJsonPrimitive
        ?: return null
    val raw = primitive.toString()
    if (!BadgeLiteLiveUnsignedPattern.matches(raw)) return null
    return raw.toLongOrNull()?.takeIf { it > 0L }
}

private fun parseBadgeLiteLiveObject(
    line: String,
    prefix: String,
    exactKeys: Set<String>,
): JsonObject? = runCatching {
    if (!line.startsWith(prefix)) return@runCatching null
    val objectValue = JsonParser.parseString(line.removePrefix(prefix))
        .takeIf { it.isJsonObject }
        ?.asJsonObject
        ?: return@runCatching null
    objectValue.takeIf { it.keySet() == exactKeys }
}.getOrNull()

internal fun parseBadgeUsbLiteLiveReady(line: String): BadgeUsbLiteLiveReady? {
    val value = parseBadgeLiteLiveObject(
        line = line.trim(),
        prefix = "FOF_LIVE_READY:",
        exactKeys = setOf("session_id", "heartbeat_ms", "lease_ms"),
    ) ?: return null
    val sessionId = value.badgeLiteStrictString("session_id")
        ?.takeIf(::badgeLiteLiveSessionIdValid)
        ?: return null
    val heartbeatMs = value.badgeLiteStrictPositiveLong("heartbeat_ms") ?: return null
    val leaseMs = value.badgeLiteStrictPositiveLong("lease_ms") ?: return null
    if (leaseMs < heartbeatMs) return null
    return BadgeUsbLiteLiveReady(sessionId, heartbeatMs, leaseMs)
}

internal fun parseBadgeUsbLiteLiveHeartbeat(line: String): BadgeUsbLiteLiveHeartbeat? {
    val value = parseBadgeLiteLiveObject(
        line = line.trim(),
        prefix = "FOF_LIVE_HEARTBEAT:",
        exactKeys = setOf("session_id", "sequence"),
    ) ?: return null
    val sessionId = value.badgeLiteStrictString("session_id")
        ?.takeIf(::badgeLiteLiveSessionIdValid)
        ?: return null
    val sequencePrimitive = value.get("sequence")
        ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isNumber }
        ?.asJsonPrimitive
        ?: return null
    val sequence = sequencePrimitive.toString()
    val numericSequence = sequence.takeIf(BadgeLiteLiveUnsignedPattern::matches)
        ?.let(::BigInteger)
        ?.takeIf { it > BigInteger.ZERO && it <= BadgeLiteLiveSequenceMax }
        ?: return null
    return BadgeUsbLiteLiveHeartbeat(sessionId, numericSequence.toString())
}

internal fun badgeUsbLiteLiveAckLine(sessionId: String, sequence: String): String? {
    if (!badgeLiteLiveSessionIdValid(sessionId) ||
        !BadgeLiteLiveUnsignedPattern.matches(sequence)
    ) return null
    val numericSequence = runCatching { BigInteger(sequence) }.getOrNull()
        ?.takeIf { it > BigInteger.ZERO && it <= BadgeLiteLiveSequenceMax }
        ?: return null
    return "FOF_LIVE_ACK:" + JsonObject().apply {
        addProperty("session_id", sessionId)
        add("sequence", JsonPrimitive(numericSequence))
    }
}

/**
 * Binds Lite live frames to the exact verified USB owner. Native badge owners can never bind,
 * and a heartbeat is acknowledged only after a READY frame establishes its current session.
 */
internal class BadgeUsbLiteLiveGate {
    private var boundOwner: BadgeUsbOwnerKey? = null
    private var sessionId: String? = null
    private var lastAckSequence = BigInteger.ZERO
    private var pendingTicket: BadgeUsbLiteLiveAckTicket? = null

    @Synchronized
    fun bind(owner: BadgeUsbOwnerKey): Boolean {
        if (owner.productKind != BadgeUsbProductKind.BADGE_LITE) return false
        boundOwner = owner
        sessionId = null
        lastAckSequence = BigInteger.ZERO
        pendingTicket = null
        return true
    }

    @Synchronized
    fun acceptReady(owner: BadgeUsbOwnerKey, ready: BadgeUsbLiteLiveReady): Boolean {
        if (!owns(owner)) return false
        if (sessionId != ready.sessionId) {
            sessionId = ready.sessionId
            lastAckSequence = BigInteger.ZERO
            pendingTicket = null
        }
        return true
    }

    @Synchronized
    fun prepareAck(
        owner: BadgeUsbOwnerKey,
        heartbeat: BadgeUsbLiteLiveHeartbeat,
    ): BadgeUsbLiteLiveAckTicket? {
        if (!owns(owner) || heartbeat.sessionId != sessionId || pendingTicket != null) return null
        val sequence = runCatching { BigInteger(heartbeat.sequence) }.getOrNull() ?: return null
        if (sequence <= lastAckSequence || sequence > BadgeLiteLiveSequenceMax) return null
        return BadgeUsbLiteLiveAckTicket(owner, heartbeat.sessionId, sequence.toString()).also {
            pendingTicket = it
        }
    }

    @Synchronized
    fun completeAck(ticket: BadgeUsbLiteLiveAckTicket, sent: Boolean): Boolean {
        val pending = pendingTicket ?: return false
        if (!ticketsMatch(pending, ticket) || !owns(ticket.owner)) return false
        pendingTicket = null
        if (sent) {
            lastAckSequence = BigInteger(ticket.sequence)
        }
        return true
    }

    @Synchronized
    fun clear(owner: BadgeUsbOwnerKey): Boolean {
        if (!owns(owner)) return false
        boundOwner = null
        sessionId = null
        lastAckSequence = BigInteger.ZERO
        pendingTicket = null
        return true
    }

    @Synchronized
    fun activeSession(owner: BadgeUsbOwnerKey): String? =
        sessionId?.takeIf { owns(owner) }

    private fun owns(owner: BadgeUsbOwnerKey): Boolean =
        owner.productKind == BadgeUsbProductKind.BADGE_LITE &&
            badgeUsbOwnerKeysMatch(boundOwner, owner)

    private fun ticketsMatch(
        expected: BadgeUsbLiteLiveAckTicket,
        actual: BadgeUsbLiteLiveAckTicket,
    ): Boolean = badgeUsbOwnerKeysMatch(expected.owner, actual.owner) &&
        expected.sessionId == actual.sessionId &&
        expected.sequence == actual.sequence
}

package com.friendorfoe.data.badge

internal data class BadgeCommandStartAuthority(
    val token: BadgeActiveTransportToken,
    val targetId: String,
    val command: BadgeCommand,
)

internal fun canStartBadgeCommand(
    evidence: BadgeConnectionEvidence,
    expected: BadgeCommandStartAuthority,
    nowElapsedMs: Long,
): Boolean {
    if (expected.targetId.isBlank() || evidence.lastValidStatusAtElapsedMs == null) return false
    val currentEvidence = evidence.aged(nowElapsedMs)
    return currentEvidence.matchesActiveToken(expected.token) &&
        currentEvidence.targetId == expected.targetId &&
        badgeCapability(
            evidence = currentEvidence,
            capability = expected.command.requiredCapability(),
            payloadBytes = expected.command.payloadSizeOrNull(),
        ) == BadgeCapabilitySupport.SUPPORTED
}

internal class BadgeCommandStartGate(
    private val transportGate: BadgeTransportGenerationGate,
) {
    fun startIfAuthorized(
        expected: BadgeCommandStartAuthority,
        currentEvidence: () -> BadgeConnectionEvidence,
        nowElapsedMs: () -> Long,
        resourceIsCurrent: () -> Boolean,
        start: () -> Unit,
    ): Boolean {
        var started = false
        transportGate.runIfCurrent(expected.token) {
            if (!canStartBadgeCommand(
                    evidence = currentEvidence(),
                    expected = expected,
                    nowElapsedMs = nowElapsedMs(),
                ) || !resourceIsCurrent()
            ) {
                return@runIfCurrent
            }
            start()
            started = true
        }
        return started
    }
}

internal data class BadgeHttpStatusRequestToken(
    val sessionGeneration: Long,
    val transport: BadgeTransport,
    val requestGeneration: Long,
)

internal class BadgeHttpStatusRequestGate {
    private val lock = Any()
    private var activeSessionGeneration: Long? = null
    private var requestGeneration = 0L
    private val latestByTransport = mutableMapOf<BadgeTransport, BadgeHttpStatusRequestToken>()
    private val activeByTransport =
        mutableMapOf<BadgeTransport, MutableSet<BadgeHttpStatusRequestToken>>()

    fun startSession(sessionGeneration: Long) = synchronized(lock) {
        activeSessionGeneration = sessionGeneration
        latestByTransport.clear()
        activeByTransport.clear()
    }

    fun stopSession() = synchronized(lock) {
        activeSessionGeneration = null
        latestByTransport.clear()
        activeByTransport.clear()
    }

    fun begin(
        sessionGeneration: Long,
        transport: BadgeTransport,
    ): BadgeHttpStatusRequestToken? = synchronized(lock) {
        if (activeSessionGeneration != sessionGeneration) return@synchronized null
        val token = BadgeHttpStatusRequestToken(
            sessionGeneration = sessionGeneration,
            transport = transport,
            requestGeneration = ++requestGeneration,
        )
        latestByTransport[transport] = token
        activeByTransport.getOrPut(transport) { mutableSetOf() } += token
        token
    }

    fun finish(token: BadgeHttpStatusRequestToken) = synchronized(lock) {
        activeByTransport[token.transport]?.let { active ->
            active -= token
            if (active.isEmpty()) activeByTransport -= token.transport
        }
    }

    fun runIfNoActiveRequest(transport: BadgeTransport, action: () -> Unit): Boolean =
        synchronized(lock) {
            if (activeSessionGeneration == null) return@synchronized false
            if (activeByTransport[transport]?.isNotEmpty() == true) return@synchronized false
            action()
            true
        }

    fun runIfLatest(token: BadgeHttpStatusRequestToken, action: () -> Unit): Boolean =
        synchronized(lock) {
            if (activeSessionGeneration != token.sessionGeneration ||
                latestByTransport[token.transport] != token ||
                token !in activeByTransport[token.transport].orEmpty()
            ) {
                return@synchronized false
            }
            action()
            true
        }
}

internal class BadgeBleScanLeaseCoordinator<T : Any> {
    private val lock = Any()
    private var active: T? = null

    fun startIfIdle(operation: T, start: () -> Unit): Boolean = synchronized(lock) {
        if (active != null) return@synchronized false
        active = operation
        try {
            start()
            true
        } catch (failure: Throwable) {
            if (active === operation) active = null
            throw failure
        }
    }

    fun isCurrent(operation: T): Boolean = synchronized(lock) { active === operation }

    fun completeIfCurrent(operation: T): Boolean = synchronized(lock) {
        if (active !== operation) return@synchronized false
        active = null
        true
    }

    fun stopIfCurrent(operation: T, stop: (T) -> Unit): Boolean = synchronized(lock) {
        if (active !== operation) return@synchronized false
        try {
            stop(operation)
        } finally {
            if (active === operation) active = null
        }
        true
    }

    fun stopCurrent(stop: (T) -> Unit): Boolean = synchronized(lock) {
        val operation = active ?: return@synchronized false
        try {
            stop(operation)
        } finally {
            if (active === operation) active = null
        }
        true
    }
}

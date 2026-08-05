package com.friendorfoe.data.badge

import java.util.concurrent.atomic.AtomicReference

internal const val USB_STATUS_MAX_CONSECUTIVE_MISSES = 3
internal const val USB_READER_SILENCE_TIMEOUT_MS = 15_250L
internal const val USB_RECONNECT_MAX_ATTEMPTS = 30
internal const val USB_RECONNECT_INTERVAL_MS = 500L

internal fun badgeUsbGenericPermissionCleanupMatches(
    permissionLifecycleSession: Long,
    permissionAttachmentToken: Any,
    permissionReconnectOperation: Any?,
    expectedLifecycleSession: Long,
    expectedAttachmentToken: Any?,
): Boolean = expectedAttachmentToken != null &&
    permissionLifecycleSession == expectedLifecycleSession &&
    permissionAttachmentToken == expectedAttachmentToken &&
    permissionReconnectOperation == null

internal fun badgeUsbReconnectPermissionCleanupMatches(
    permissionLifecycleSession: Long,
    permissionReconnectOperation: Any?,
    expectedLifecycleSession: Long,
    expectedReconnectOperation: Any,
): Boolean = permissionLifecycleSession == expectedLifecycleSession &&
    permissionReconnectOperation === expectedReconnectOperation

internal fun badgeUsbPermissionMayDispatch(
    activeOperation: Any?,
    expectedOperation: Any,
    selectionStampCurrent: Boolean,
    lifecycleActive: Boolean,
    attachmentAccepted: Boolean,
    reconnectOwned: Boolean,
): Boolean = activeOperation === expectedOperation &&
    selectionStampCurrent &&
    lifecycleActive &&
    attachmentAccepted &&
    reconnectOwned

internal class BadgeUsbPermissionDispatchGate {
    private var cancelled = false
    private var dispatched = false

    @Synchronized
    fun cancel() {
        cancelled = true
    }

    @Synchronized
    fun dispatchIfActive(sideEffect: () -> Unit): Boolean {
        if (cancelled || dispatched) return false
        dispatched = true
        sideEffect()
        return true
    }
}

internal class BadgeUsbAtomicSlot<T : Any> {
    private val value = AtomicReference<T?>(null)

    fun replace(replacement: T): T? = value.getAndSet(replacement)

    fun clear(expected: T): Boolean = value.compareAndSet(expected, null)

    fun take(): T? = value.getAndSet(null)

    fun current(): T? = value.get()
}

internal class BadgeUsbReconnectSelectionGate {
    private var stamp = 1L

    @Synchronized
    fun <T> withBarrier(block: () -> T): T = block()

    @Synchronized
    fun currentStamp(): Long = stamp

    @Synchronized
    fun advanceStamp(): Long {
        check(stamp != Long.MAX_VALUE) { "USB selection stamp exhausted" }
        stamp += 1L
        return stamp
    }

    @Synchronized
    fun isStampCurrent(expectedStamp: Long): Boolean = stamp == expectedStamp
}

internal class BadgeUsbEnumerationEpochGate {
    private var epoch = 1L
    private var exhausted = false

    @Synchronized
    fun currentEpoch(): Long = epoch

    @Synchronized
    fun advanceEpoch(): Long {
        if (exhausted || epoch == Long.MAX_VALUE) {
            exhausted = true
            throw IllegalStateException("USB enumeration epoch exhausted")
        }
        epoch += 1L
        return epoch
    }

    @Synchronized
    fun isEpochCurrent(expectedEpoch: Long): Boolean = !exhausted && epoch == expectedEpoch
}

/**
 * Linearizes logical USB-session revocation against platform I/O without holding
 * the repository selection lock across a blocking bulkTransfer call.
 */
internal class BadgeUsbIoArbiter {
    internal class SessionState(
        val identity: Any,
        var accepting: Boolean = true,
        var inFlight: Int = 0,
    )

    internal class Drain internal constructor(internal val state: SessionState)

    internal class Lease internal constructor(
        private val arbiter: BadgeUsbIoArbiter,
        private val state: SessionState,
    ) : AutoCloseable {
        private var released = false

        override fun close() {
            synchronized(this) {
                if (released) return
                released = true
            }
            arbiter.release(state)
        }
    }

    private val monitor = Object()
    private var active: SessionState? = null

    fun activate(identity: Any): Boolean = synchronized(monitor) {
        if (active != null) return@synchronized false
        active = SessionState(identity)
        true
    }

    fun tryAcquire(identity: Any): Lease? = synchronized(monitor) {
        val state = active?.takeIf { it.identity === identity && it.accepting }
            ?: return@synchronized null
        state.inFlight += 1
        Lease(this, state)
    }

    fun revoke(identity: Any): Drain? = synchronized(monitor) {
        val state = active?.takeIf { it.identity === identity } ?: return@synchronized null
        state.accepting = false
        Drain(state)
    }

    fun awaitDrained(drain: Drain, timeoutMs: Long): Boolean = synchronized(monitor) {
        if (active !== drain.state || drain.state.accepting) return@synchronized false
        val timeoutNanos = timeoutMs.coerceAtLeast(0L) * 1_000_000L
        val deadline = System.nanoTime() + timeoutNanos
        while (drain.state.inFlight != 0) {
            val remaining = deadline - System.nanoTime()
            if (remaining <= 0L) return@synchronized false
            val waitMillis = remaining / 1_000_000L
            val waitNanos = (remaining % 1_000_000L).toInt()
            try {
                monitor.wait(waitMillis, waitNanos)
            } catch (interrupted: InterruptedException) {
                Thread.currentThread().interrupt()
                return@synchronized false
            }
        }
        true
    }

    fun completeDrain(drain: Drain): Boolean = synchronized(monitor) {
        if (active !== drain.state || drain.state.accepting || drain.state.inFlight != 0) {
            return@synchronized false
        }
        active = null
        monitor.notifyAll()
        true
    }

    private fun release(state: SessionState) = synchronized(monitor) {
        check(state.inFlight > 0) { "USB I/O lease released more than once" }
        state.inFlight -= 1
        if (state.inFlight == 0) monitor.notifyAll()
    }
}

internal data class BadgeUsbIoPublicationResult(
    val published: Boolean,
    val drain: BadgeUsbIoArbiter.Drain?,
)

internal fun publishBadgeUsbIoSession(
    arbiter: BadgeUsbIoArbiter,
    identity: Any,
    publication: () -> Boolean,
): BadgeUsbIoPublicationResult {
    if (!arbiter.activate(identity)) {
        return BadgeUsbIoPublicationResult(published = false, drain = null)
    }
    if (publication()) {
        return BadgeUsbIoPublicationResult(published = true, drain = null)
    }
    return BadgeUsbIoPublicationResult(
        published = false,
        drain = checkNotNull(arbiter.revoke(identity)) {
            "Activated badge USB publication lost its exact drain"
        },
    )
}

internal class BadgeUsbIoCleanupPhaseGate {
    private enum class Phase { DRAINING, DRAINED, CLOSED, COMPLETED }

    private var phase = Phase.DRAINING

    @Synchronized
    fun phaseName(): String = phase.name

    @Synchronized
    fun shouldAttemptClose(): Boolean = phase == Phase.DRAINED

    @Synchronized
    fun shouldAttemptDrainCompletion(): Boolean = phase == Phase.CLOSED

    @Synchronized
    fun isCompleted(): Boolean = phase == Phase.COMPLETED

    @Synchronized
    fun markDrained(): Boolean {
        if (phase != Phase.DRAINING) return false
        phase = Phase.DRAINED
        return true
    }

    @Synchronized
    fun markClosed(): Boolean {
        if (phase != Phase.DRAINED) return false
        phase = Phase.CLOSED
        return true
    }

    @Synchronized
    fun markCompleted(): Boolean {
        if (phase != Phase.CLOSED) return false
        phase = Phase.COMPLETED
        return true
    }
}

internal class BadgeUsbRetainedCleanupSlot {
    private var cleanupIdentity: Any? = null
    private var workerIdentity: Any? = null

    @Synchronized
    fun tryInstall(cleanup: Any, worker: Any): Boolean {
        if (cleanupIdentity != null && cleanupIdentity !== cleanup) return false
        if (workerIdentity != null) return false
        cleanupIdentity = cleanup
        workerIdentity = worker
        return true
    }

    @Synchronized
    fun finishWorker(
        cleanup: Any,
        worker: Any,
        completed: Boolean,
    ): Boolean {
        if (cleanupIdentity !== cleanup || workerIdentity !== worker) return false
        workerIdentity = null
        if (completed) cleanupIdentity = null
        return true
    }

    @Synchronized
    fun ownsCleanup(cleanup: Any): Boolean = cleanupIdentity === cleanup
}

internal class BadgeUsbReceiverLifetimeGate {
    @Volatile
    private var registered = false

    @Synchronized
    fun registerOnce(registration: () -> Unit): Boolean {
        if (registered) return false
        registration()
        registered = true
        return true
    }

    fun isRegistered(): Boolean = registered
}

internal class BadgeUsbReconnectOperationGate {
    private enum class State { ACTIVE, TERMINAL, COMPLETED }

    private var state = State.ACTIVE

    @Synchronized
    fun isActive(): Boolean = state == State.ACTIVE

    @Synchronized
    fun tryTerminalize(): Boolean {
        if (state != State.ACTIVE) return false
        state = State.TERMINAL
        return true
    }

    @Synchronized
    fun publishConnectingIfActive(publication: () -> Unit): Boolean {
        if (state != State.ACTIVE) return false
        publication()
        return true
    }

    @Synchronized
    fun <T : Any> prepareIfActive(preparation: () -> T?): T? {
        if (state != State.ACTIVE) return null
        return preparation()
    }

    fun completeAndClearIfActive(
        operationIsCurrent: () -> Boolean,
        publication: () -> Boolean,
        completion: () -> Unit,
    ): Boolean = completeHandshakeAndClearIfActive(
        operationIsCurrent = operationIsCurrent,
        fullCommit = publication,
        completion = completion,
    )

    @Synchronized
    fun completeHandshakeAndClearIfActive(
        operationIsCurrent: () -> Boolean,
        fullCommit: () -> Boolean,
        completion: () -> Unit,
    ): Boolean {
        if (state != State.ACTIVE || !operationIsCurrent()) return false
        if (!fullCommit()) return false
        state = State.COMPLETED
        completion()
        return true
    }
}

internal enum class BadgeUsbStatusPollDecision {
    FRESH,
    MISS,
    TERMINATE,
    STALE_OWNER,
}

internal data class BadgeUsbStatusPollTicket(
    val owner: BadgeUsbOwnerKey,
    val ownerGeneration: Long,
    val acceptedStatusGeneration: Long,
    val baselineResponsesCompleted: Long?,
    val pollSequence: Long,
)

internal class BadgeUsbStatusPollGate {
    private var boundOwner: BadgeUsbOwnerKey? = null
    private var ownerGeneration = 0L
    private var acceptedStatusGeneration = 0L
    private var responseCounterRequired = false
    private var highestResponsesCompleted: Long? = null
    private var recordedResponsesCompleted: Long? = null
    private var nextPollSequence = 1L
    private var outstandingTicket: BadgeUsbStatusPollTicket? = null
    private var consecutiveMisses = 0

    @Synchronized
    fun bind(owner: BadgeUsbOwnerKey, initialResponsesCompleted: Long?) {
        require(initialResponsesCompleted == null || initialResponsesCompleted >= 0L) {
            "Initial USB response counter must be non-negative"
        }
        boundOwner = owner
        ownerGeneration++
        acceptedStatusGeneration = 0L
        responseCounterRequired = initialResponsesCompleted != null
        highestResponsesCompleted = initialResponsesCompleted
        recordedResponsesCompleted = initialResponsesCompleted
        outstandingTicket = null
        consecutiveMisses = 0
    }

    @Synchronized
    fun beginPoll(owner: BadgeUsbOwnerKey): BadgeUsbStatusPollTicket? {
        val currentOwner = boundOwner
        if (!badgeUsbOwnerKeysMatch(currentOwner, owner)) return null
        val sequence = nextPollSequence++
        return BadgeUsbStatusPollTicket(
            owner = currentOwner!!,
            ownerGeneration = ownerGeneration,
            acceptedStatusGeneration = acceptedStatusGeneration,
            baselineResponsesCompleted = if (responseCounterRequired) {
                highestResponsesCompleted
            } else {
                null
            },
            pollSequence = sequence,
        ).also {
            outstandingTicket = it
        }
    }

    @Synchronized
    fun recordStatus(owner: BadgeUsbOwnerKey, responsesCompleted: Long?): Boolean {
        if (!badgeUsbOwnerKeysMatch(boundOwner, owner)) return false
        acceptedStatusGeneration++
        recordedResponsesCompleted = responsesCompleted?.takeIf { it >= 0L }
        recordedResponsesCompleted?.let { recorded ->
            val previousHighest = highestResponsesCompleted
            if (responseCounterRequired &&
                (previousHighest == null || recorded > previousHighest)
            ) {
                highestResponsesCompleted = recorded
            }
        }
        return true
    }

    @Synchronized
    fun finishPoll(
        ticket: BadgeUsbStatusPollTicket,
        owner: BadgeUsbOwnerKey,
    ): BadgeUsbStatusPollDecision {
        if (!badgeUsbOwnerKeysMatch(boundOwner, owner) ||
            !badgeUsbOwnerKeysMatch(boundOwner, ticket.owner) ||
            ticket.ownerGeneration != ownerGeneration ||
            !ticketsMatch(outstandingTicket, ticket)
        ) {
            return BadgeUsbStatusPollDecision.STALE_OWNER
        }
        outstandingTicket = null

        val acceptedAfterPoll = acceptedStatusGeneration > ticket.acceptedStatusGeneration
        val fresh = if (ticket.baselineResponsesCompleted == null) {
            acceptedAfterPoll
        } else {
            acceptedAfterPoll &&
                recordedResponsesCompleted?.let { it > ticket.baselineResponsesCompleted } == true
        }
        if (fresh) {
            consecutiveMisses = 0
            return BadgeUsbStatusPollDecision.FRESH
        }

        consecutiveMisses = (consecutiveMisses + 1)
            .coerceAtMost(USB_STATUS_MAX_CONSECUTIVE_MISSES)
        return if (consecutiveMisses >= USB_STATUS_MAX_CONSECUTIVE_MISSES) {
            BadgeUsbStatusPollDecision.TERMINATE
        } else {
            BadgeUsbStatusPollDecision.MISS
        }
    }

    @Synchronized
    fun clear(owner: BadgeUsbOwnerKey): Boolean {
        if (!badgeUsbOwnerKeysMatch(boundOwner, owner)) return false
        boundOwner = null
        outstandingTicket = null
        acceptedStatusGeneration = 0L
        responseCounterRequired = false
        highestResponsesCompleted = null
        recordedResponsesCompleted = null
        consecutiveMisses = 0
        return true
    }

    private fun ticketsMatch(
        expected: BadgeUsbStatusPollTicket?,
        actual: BadgeUsbStatusPollTicket?,
    ): Boolean = expected != null && actual != null &&
        badgeUsbOwnerKeysMatch(expected.owner, actual.owner) &&
        expected.ownerGeneration == actual.ownerGeneration &&
        expected.acceptedStatusGeneration == actual.acceptedStatusGeneration &&
        expected.baselineResponsesCompleted == actual.baselineResponsesCompleted &&
        expected.pollSequence == actual.pollSequence
}

internal class BadgeUsbReaderSilenceGate(
    private val timeoutMs: Long = USB_READER_SILENCE_TIMEOUT_MS,
) {
    private var started = false
    private var monotonicFailure = false
    private var terminalExpired = false
    private var lastObservedElapsedMs = 0L
    private var deadlineElapsedMs = 0L

    init {
        require(timeoutMs > 0L) { "Reader silence timeout must be positive" }
    }

    @Synchronized
    fun start(nowElapsedMs: Long) {
        require(nowElapsedMs >= 0L) { "Monotonic time must be non-negative" }
        started = true
        monotonicFailure = false
        terminalExpired = false
        lastObservedElapsedMs = nowElapsedMs
        deadlineElapsedMs = deadlineFrom(nowElapsedMs)
    }

    @Synchronized
    fun recordRead(byteCount: Int, nowElapsedMs: Long): Boolean {
        if (!started || terminalExpired || monotonicFailure || !observe(nowElapsedMs)) {
            return false
        }
        if (nowElapsedMs >= deadlineElapsedMs) {
            terminalExpired = true
            return false
        }
        if (byteCount <= 0) return false
        deadlineElapsedMs = deadlineFrom(nowElapsedMs)
        return true
    }

    @Synchronized
    fun isExpired(nowElapsedMs: Long): Boolean {
        if (!started || terminalExpired || monotonicFailure || !observe(nowElapsedMs)) {
            return true
        }
        if (nowElapsedMs >= deadlineElapsedMs) {
            terminalExpired = true
            return true
        }
        return false
    }

    private fun observe(nowElapsedMs: Long): Boolean {
        if (nowElapsedMs < lastObservedElapsedMs) {
            monotonicFailure = true
            return false
        }
        lastObservedElapsedMs = nowElapsedMs
        return true
    }

    private fun deadlineFrom(nowElapsedMs: Long): Long =
        if (nowElapsedMs > Long.MAX_VALUE - timeoutMs) Long.MAX_VALUE
        else nowElapsedMs + timeoutMs
}

internal enum class BadgeUsbReconnectDecision {
    RETRY,
    EXPIRED,
    STALE,
}

internal data class BadgeUsbReconnectTicket(
    val oldOwner: BadgeUsbOwnerKey,
    val lifecycleSession: Long,
    val hardwareId: String,
    val generation: Long,
)

internal class BadgeUsbReconnectGate {
    private var nextGeneration = 1L
    private var currentTicket: BadgeUsbReconnectTicket? = null
    private var attemptsConsumed = 0

    @Synchronized
    fun bind(oldOwner: BadgeUsbOwnerKey): BadgeUsbReconnectTicket? {
        val hardwareId = canonicalBadgeHardwareId(oldOwner.hardwareId)
            ?.takeIf { it == oldOwner.hardwareId }
            ?: return null
        return BadgeUsbReconnectTicket(
            oldOwner = oldOwner,
            lifecycleSession = oldOwner.lifecycleSession,
            hardwareId = hardwareId,
            generation = nextGeneration++,
        ).also {
            currentTicket = it
            attemptsConsumed = 0
        }
    }

    @Synchronized
    fun nextAttempt(
        ticket: BadgeUsbReconnectTicket,
        lifecycleSession: Long,
    ): BadgeUsbReconnectDecision {
        val current = currentTicket
        if (!ticketsMatch(current, ticket) || lifecycleSession != current?.lifecycleSession) {
            return BadgeUsbReconnectDecision.STALE
        }
        if (attemptsConsumed >= USB_RECONNECT_MAX_ATTEMPTS) {
            return BadgeUsbReconnectDecision.EXPIRED
        }
        attemptsConsumed++
        return BadgeUsbReconnectDecision.RETRY
    }

    @Synchronized
    fun isCurrent(
        ticket: BadgeUsbReconnectTicket,
        lifecycleSession: Long,
    ): Boolean = ticketsMatch(currentTicket, ticket) &&
        lifecycleSession == currentTicket?.lifecycleSession

    @Synchronized
    fun expectedHardwareId(
        ticket: BadgeUsbReconnectTicket,
        lifecycleSession: Long,
    ): String? = currentTicket
        ?.takeIf { ticketsMatch(it, ticket) && lifecycleSession == it.lifecycleSession }
        ?.hardwareId

    @Synchronized
    fun expectedProductKind(
        ticket: BadgeUsbReconnectTicket,
        lifecycleSession: Long,
    ): BadgeUsbProductKind? = currentTicket
        ?.takeIf { ticketsMatch(it, ticket) && lifecycleSession == it.lifecycleSession }
        ?.oldOwner
        ?.productKind

    @Synchronized
    fun clear(ticket: BadgeUsbReconnectTicket): Boolean {
        if (!ticketsMatch(currentTicket, ticket)) return false
        currentTicket = null
        attemptsConsumed = 0
        return true
    }

    private fun ticketsMatch(
        expected: BadgeUsbReconnectTicket?,
        actual: BadgeUsbReconnectTicket?,
    ): Boolean = expected != null && actual != null &&
        expected.generation == actual.generation &&
        expected.lifecycleSession == actual.lifecycleSession &&
        expected.hardwareId == actual.hardwareId &&
        badgeUsbOwnerKeysMatch(expected.oldOwner, actual.oldOwner)
}

internal enum class BadgeUsbReconnectCandidateAction {
    PRESERVE_RECOVERY,
    NORMAL_REFRESH,
    CONNECT_ONE,
    FAIL_AMBIGUOUS,
}

internal enum class BadgeUsbReconnectCandidatePreparation {
    REJECT_BEFORE_SELECTION,
    REUSE_ATTEMPT,
    SELECT_AND_BIND,
}

internal enum class BadgeUsbReconnectAmbiguityDecision {
    TERMINALIZE_EXPECTED,
    REPORT_GENERIC,
    STALE_NO_OP,
}

internal fun badgeUsbReconnectAmbiguityDecision(
    expectedOperationIdentity: Any?,
    expectedGeneration: Long?,
    expectedLifecycleSession: Long,
    currentOperationIdentity: Any?,
    currentGeneration: Long?,
    currentLifecycleSession: Long?,
    lifecycleActive: Boolean,
): BadgeUsbReconnectAmbiguityDecision = when {
    !lifecycleActive -> BadgeUsbReconnectAmbiguityDecision.STALE_NO_OP
    expectedOperationIdentity == null && currentOperationIdentity == null ->
        BadgeUsbReconnectAmbiguityDecision.REPORT_GENERIC
    expectedOperationIdentity == null || currentOperationIdentity !== expectedOperationIdentity ->
        BadgeUsbReconnectAmbiguityDecision.STALE_NO_OP
    expectedGeneration == null || currentGeneration != expectedGeneration ||
        currentLifecycleSession != expectedLifecycleSession ->
        BadgeUsbReconnectAmbiguityDecision.STALE_NO_OP
    else -> BadgeUsbReconnectAmbiguityDecision.TERMINALIZE_EXPECTED
}

internal fun badgeUsbReconnectCandidatePreparation(
    operationActive: Boolean,
    attemptIdentity: BadgeUsbDeviceIdentity?,
    attemptConnectionBound: Boolean,
    candidateIdentity: BadgeUsbDeviceIdentity,
): BadgeUsbReconnectCandidatePreparation = when {
    !operationActive -> BadgeUsbReconnectCandidatePreparation.REJECT_BEFORE_SELECTION
    attemptIdentity == candidateIdentity -> BadgeUsbReconnectCandidatePreparation.REUSE_ATTEMPT
    attemptConnectionBound -> BadgeUsbReconnectCandidatePreparation.REJECT_BEFORE_SELECTION
    else -> BadgeUsbReconnectCandidatePreparation.SELECT_AND_BIND
}

internal fun badgeUsbReconnectCandidateAction(
    candidateCount: Int,
    preserveRecovery: Boolean,
): BadgeUsbReconnectCandidateAction {
    require(candidateCount >= 0) { "USB candidate count must be non-negative" }
    return when {
        candidateCount == 0 && preserveRecovery ->
            BadgeUsbReconnectCandidateAction.PRESERVE_RECOVERY
        candidateCount == 0 -> BadgeUsbReconnectCandidateAction.NORMAL_REFRESH
        candidateCount == 1 -> BadgeUsbReconnectCandidateAction.CONNECT_ONE
        else -> BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS
    }
}

internal fun badgeUsbReconnectDetachMatches(
    ticket: BadgeUsbReconnectTicket,
    detachedIdentity: BadgeUsbDeviceIdentity,
): Boolean = ticket.oldOwner.attachmentToken.identity == detachedIdentity

internal fun badgeUsbReconnectExpiryOwnsConnecting(
    ticket: BadgeUsbReconnectTicket,
    reconnectAttachmentToken: BadgeUsbAttachmentToken,
    reconnectConnectionIdentity: Any?,
    lifecycleActive: Boolean,
    status: BadgeUsbStatus,
    transportLabel: String,
    activeLifecycleSession: Long?,
    activeAttachmentToken: BadgeUsbAttachmentToken?,
    activeConnection: Any?,
    activeVerifiedOwner: BadgeUsbOwnerKey?,
): Boolean = lifecycleActive &&
    status == BadgeUsbStatus.CONNECTING &&
    transportLabel == "USB-C" &&
    activeLifecycleSession == ticket.lifecycleSession &&
    reconnectConnectionIdentity != null &&
    activeAttachmentToken == reconnectAttachmentToken &&
    activeConnection === reconnectConnectionIdentity &&
    activeVerifiedOwner == null

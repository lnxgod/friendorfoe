package com.friendorfoe.data.badge

import com.friendorfoe.data.time.MonotonicClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicLong

enum class BadgeTransport {
    USB_SERIAL,
    LOCAL_AP_HTTP,
    BLE_GATT,
    DEBUG_BRIDGE,
}

enum class BadgeConnectionPhase {
    DISCONNECTED,
    PERMISSION_NEEDED,
    CONNECTING,
    TRANSPORT_OPEN,
    LIVE,
    STALE,
    EXPIRED,
    ERROR,
}

enum class BadgeCapability {
    READ_STATUS,
    DISPLAY_NAV,
    NETWORK_MODE,
    THEME_V1,
    DISPLAY_POLICY_V1,
    REBOOT,
    BOOTLOADER,
}

enum class BadgeCapabilitySupport {
    SUPPORTED,
    UNSUPPORTED,
    UNKNOWN,
}

data class BadgeConnectionEvidence(
    val transport: BadgeTransport? = null,
    val transportGeneration: Long? = null,
    val phase: BadgeConnectionPhase = BadgeConnectionPhase.DISCONNECTED,
    val lastValidStatusAtElapsedMs: Long? = null,
    val protocolVersion: String? = null,
    val targetId: String? = null,
    val usbCandidateCount: Int? = null,
    val exactEspressifVendorMatch: Boolean = false,
    val serialInterfaceReadable: Boolean = false,
    val badgeApEndpoint: String? = null,
    val negotiatedBleMtu: Int? = null,
    val fofBleServicePresent: Boolean = false,
    val bleStatusCharacteristicPresent: Boolean = false,
    val bleControlCharacteristicPresent: Boolean = false,
    val bleBonded: Boolean = false,
    val bleEncrypted: Boolean = false,
    val debugBridgeSerialPort: String? = null,
    val debugPhysicalStatusAtElapsedMs: Long? = null,
    val debugBridgeLastError: String? = null,
    val releaseCertifiedMutations: Set<BadgeCapability> = emptySet(),
)

fun badgeFreshness(
    transport: BadgeTransport,
    receivedAtElapsedMs: Long,
    nowElapsedMs: Long,
): BadgeConnectionPhase {
    val ageMs = when {
        nowElapsedMs <= receivedAtElapsedMs -> 0L
        else -> runCatching {
            Math.subtractExact(nowElapsedMs, receivedAtElapsedMs)
        }.getOrDefault(Long.MAX_VALUE)
    }
    val staleAfterMs = when (transport) {
        BadgeTransport.BLE_GATT -> 20_000L
        BadgeTransport.USB_SERIAL,
        BadgeTransport.LOCAL_AP_HTTP,
        BadgeTransport.DEBUG_BRIDGE,
        -> 10_000L
    }
    return when {
        ageMs >= 60_000L -> BadgeConnectionPhase.EXPIRED
        ageMs >= staleAfterMs -> BadgeConnectionPhase.STALE
        else -> BadgeConnectionPhase.LIVE
    }
}

fun verifiedBadgeConnectionPhase(
    transport: BadgeTransport,
    effectiveReceivedAtElapsedMs: Long,
    nowElapsedMs: Long,
): BadgeConnectionPhase = badgeFreshness(
    transport = transport,
    receivedAtElapsedMs = effectiveReceivedAtElapsedMs,
    nowElapsedMs = nowElapsedMs,
)

fun BadgeConnectionEvidence.aged(nowElapsedMs: Long): BadgeConnectionEvidence {
    val activeTransport = transport ?: return copy(phase = BadgeConnectionPhase.DISCONNECTED)
    if (phase != BadgeConnectionPhase.LIVE && phase != BadgeConnectionPhase.STALE) return this
    val receivedAt = lastValidStatusAtElapsedMs ?: return this
    return copy(phase = badgeFreshness(activeTransport, receivedAt, nowElapsedMs))
}

fun badgeCapability(
    evidence: BadgeConnectionEvidence,
    capability: BadgeCapability,
    payloadBytes: Int? = null,
): BadgeCapabilitySupport {
    val transport = evidence.transport ?: return BadgeCapabilitySupport.UNKNOWN

    if (capability == BadgeCapability.REBOOT || capability == BadgeCapability.BOOTLOADER) {
        if (transport != BadgeTransport.USB_SERIAL) return BadgeCapabilitySupport.UNSUPPORTED
    }
    if (transport == BadgeTransport.BLE_GATT && capability !in setOf(
            BadgeCapability.READ_STATUS,
            BadgeCapability.DISPLAY_NAV,
        )
    ) {
        return BadgeCapabilitySupport.UNSUPPORTED
    }
    if (transport == BadgeTransport.BLE_GATT && capability == BadgeCapability.DISPLAY_NAV) {
        val mtu = evidence.negotiatedBleMtu
        if (mtu != null && payloadBytes != null && payloadBytes > mtu - 3) {
            return BadgeCapabilitySupport.UNSUPPORTED
        }
    }

    if (evidence.phase != BadgeConnectionPhase.LIVE || evidence.protocolVersion.isNullOrBlank()) {
        return BadgeCapabilitySupport.UNKNOWN
    }

    val transportEvidenceComplete = when (transport) {
        BadgeTransport.USB_SERIAL -> evidence.usbCandidateCount == 1 &&
            evidence.exactEspressifVendorMatch &&
            evidence.serialInterfaceReadable
        BadgeTransport.LOCAL_AP_HTTP -> evidence.badgeApEndpoint == BADGE_AP_ENDPOINT
        BadgeTransport.BLE_GATT -> evidence.fofBleServicePresent &&
            evidence.bleStatusCharacteristicPresent &&
            evidence.bleControlCharacteristicPresent
        BadgeTransport.DEBUG_BRIDGE -> !evidence.debugBridgeSerialPort.isNullOrBlank() &&
            evidence.debugPhysicalStatusAtElapsedMs != null
    }
    if (!transportEvidenceComplete) return BadgeCapabilitySupport.UNKNOWN

    if (capability == BadgeCapability.READ_STATUS) return BadgeCapabilitySupport.SUPPORTED

    if (transport == BadgeTransport.BLE_GATT) {
        if (evidence.negotiatedBleMtu == null || payloadBytes == null) {
            return BadgeCapabilitySupport.UNKNOWN
        }
        if (!evidence.bleBonded || !evidence.bleEncrypted) {
            return BadgeCapabilitySupport.UNKNOWN
        }
    }
    if (transport == BadgeTransport.DEBUG_BRIDGE &&
        (evidence.debugBridgeLastError == null || evidence.debugBridgeLastError.isNotEmpty())
    ) {
        return BadgeCapabilitySupport.UNKNOWN
    }
    if ((capability == BadgeCapability.REBOOT || capability == BadgeCapability.BOOTLOADER) &&
        evidence.targetId.isNullOrBlank()
    ) {
        return BadgeCapabilitySupport.UNKNOWN
    }
    if (capability !in evidence.releaseCertifiedMutations) {
        return BadgeCapabilitySupport.UNKNOWN
    }
    return BadgeCapabilitySupport.SUPPORTED
}

internal const val BADGE_AP_ENDPOINT = "http://192.168.4.1"

internal data class BadgeTransportSessionToken(val sessionGeneration: Long)

internal data class BadgeActiveTransportToken(
    val sessionGeneration: Long,
    val transportGeneration: Long,
    val transport: BadgeTransport,
)

internal class BadgeTransportGenerationGate {
    private val generationCounter = AtomicLong(0)
    private val lock = Any()
    private var started = false
    private var sessionGeneration = 0L
    private var activeTransport: BadgeActiveTransportToken? = null

    fun startSession(): Long = synchronized(lock) {
        if (!started) {
            started = true
            sessionGeneration = generationCounter.incrementAndGet()
            activeTransport = null
        }
        sessionGeneration
    }

    fun stopSession() = synchronized(lock) {
        started = false
        sessionGeneration = generationCounter.incrementAndGet()
        activeTransport = null
    }

    fun captureSession(session: Long): BadgeTransportSessionToken =
        BadgeTransportSessionToken(session)

    fun isSessionCurrent(session: Long): Boolean = synchronized(lock) {
        started && sessionGeneration == session
    }

    fun runIfSessionCurrent(session: Long, action: () -> Unit): Boolean =
        synchronized(lock) {
            if (!started || sessionGeneration != session) return@synchronized false
            action()
            true
        }

    fun runIfSessionHasNoActiveTransport(
        session: Long,
        action: () -> Unit,
    ): Boolean = synchronized(lock) {
        if (!started || sessionGeneration != session || activeTransport != null) {
            return@synchronized false
        }
        action()
        true
    }

    fun claim(
        session: BadgeTransportSessionToken,
        transport: BadgeTransport,
    ): BadgeActiveTransportToken? = synchronized(lock) {
        if (!started || session.sessionGeneration != sessionGeneration) return@synchronized null
        val current = activeTransport
        when {
            current == null -> newActiveToken(transport)
            current.transport == transport -> current
            transport == BadgeTransport.USB_SERIAL -> newActiveToken(transport)
            current.transport == BadgeTransport.USB_SERIAL -> null
            else -> null
        }
    }

    fun claimNewConnection(
        session: BadgeTransportSessionToken,
        transport: BadgeTransport,
    ): BadgeActiveTransportToken? = synchronized(lock) {
        if (!started || session.sessionGeneration != sessionGeneration) return@synchronized null
        val current = activeTransport
        when {
            current == null -> newActiveToken(transport)
            current.transport == transport -> newActiveToken(transport)
            transport == BadgeTransport.USB_SERIAL -> newActiveToken(transport)
            current.transport == BadgeTransport.USB_SERIAL -> null
            else -> null
        }
    }

    fun isCurrent(token: BadgeActiveTransportToken): Boolean = synchronized(lock) {
        started && token == activeTransport && token.sessionGeneration == sessionGeneration
    }

    fun release(token: BadgeActiveTransportToken): Boolean = synchronized(lock) {
        if (activeTransport != token) return@synchronized false
        activeTransport = null
        true
    }

    fun releaseAndRunIfCurrent(
        token: BadgeActiveTransportToken,
        action: () -> Unit,
    ): Boolean = synchronized(lock) {
        if (!started || token != activeTransport ||
            token.sessionGeneration != sessionGeneration
        ) {
            return@synchronized false
        }
        action()
        activeTransport = null
        true
    }

    fun runIfCurrent(token: BadgeActiveTransportToken, action: () -> Unit): Boolean =
        synchronized(lock) {
            if (!started || token != activeTransport ||
                token.sessionGeneration != sessionGeneration
            ) {
                return@synchronized false
            }
            action()
            true
        }

    private fun newActiveToken(transport: BadgeTransport): BadgeActiveTransportToken {
        val token = BadgeActiveTransportToken(
            sessionGeneration = sessionGeneration,
            transportGeneration = generationCounter.incrementAndGet(),
            transport = transport,
        )
        activeTransport = token
        return token
    }
}

internal fun matchesActiveUsbTarget(
    detachedTargetId: String,
    activeTargetId: String?,
): Boolean = activeTargetId != null && detachedTargetId == activeTargetId

internal fun shouldRetainHttpLease(
    evidenceComplete: Boolean,
    phase: BadgeConnectionPhase,
): Boolean = evidenceComplete && phase != BadgeConnectionPhase.EXPIRED

internal fun shouldReleaseExpiredHttpLease(
    connection: BadgeConnectionEvidence,
    token: BadgeActiveTransportToken,
): Boolean = connection.phase == BadgeConnectionPhase.EXPIRED &&
    connection.transport == token.transport &&
    token.transport in setOf(BadgeTransport.LOCAL_AP_HTTP, BadgeTransport.DEBUG_BRIDGE)

internal fun BadgeConnectionEvidence.matchesActiveToken(
    token: BadgeActiveTransportToken,
): Boolean = transport == token.transport && transportGeneration == token.transportGeneration

internal fun canPublishExpectedRecoveryDisconnect(
    command: BadgeCommand,
    outcome: BadgeCommandOutcome,
    attemptedConnection: BadgeConnectionEvidence,
    currentConnection: BadgeConnectionEvidence,
): Boolean = command in setOf(BadgeCommand.Reboot, BadgeCommand.EnterBootloader) &&
    outcome is BadgeCommandOutcome.Acknowledged &&
    attemptedConnection.transport == BadgeTransport.USB_SERIAL &&
    attemptedConnection.transportGeneration != null &&
    !attemptedConnection.targetId.isNullOrBlank() &&
    currentConnection.transport == BadgeTransport.USB_SERIAL &&
    currentConnection.transportGeneration == attemptedConnection.transportGeneration &&
    currentConnection.targetId == attemptedConnection.targetId &&
    currentConnection.phase == BadgeConnectionPhase.DISCONNECTED

internal data class BadgeUsbPermissionRequest(
    val sessionGeneration: Long,
    val targetId: String,
    val nonce: Long,
)

internal class BadgeUsbPermissionRequestGate {
    private val lock = Any()
    private var nonce = 0L
    private var pending: BadgeUsbPermissionRequest? = null

    fun issue(sessionGeneration: Long, targetId: String): BadgeUsbPermissionRequest =
        synchronized(lock) {
            BadgeUsbPermissionRequest(
                sessionGeneration = sessionGeneration,
                targetId = targetId,
                nonce = ++nonce,
            ).also { pending = it }
        }

    fun consume(
        request: BadgeUsbPermissionRequest,
        currentSessionGeneration: Long,
        resultTargetId: String,
    ): Boolean = synchronized(lock) {
        if (pending != request ||
            request.sessionGeneration != currentSessionGeneration ||
            request.targetId != resultTargetId
        ) {
            return@synchronized false
        }
        pending = null
        true
    }

    fun clear() = synchronized(lock) {
        pending = null
    }
}

internal class BadgeRepositoryStateStore(
    initialState: BadgeRepositoryState,
    clock: MonotonicClock,
    scope: CoroutineScope,
) {
    private val _state = MutableStateFlow(initialState)
    val state: StateFlow<BadgeRepositoryState> = _state.asStateFlow()

    init {
        scope.launch {
            clock.ticks().collect { nowElapsedMs ->
                _state.update { current -> current.aged(nowElapsedMs) }
            }
        }
    }

    fun update(transform: (BadgeRepositoryState) -> BadgeRepositoryState) {
        _state.update(transform)
    }

    fun publishConnection(connection: BadgeConnectionEvidence) {
        _state.update { current ->
            val changedTarget = current.connection.transport != connection.transport ||
                current.connection.targetId != connection.targetId
            val concreteInstanceNeedsFreshEvidence =
                connection.phase in setOf(
                    BadgeConnectionPhase.PERMISSION_NEEDED,
                    BadgeConnectionPhase.CONNECTING,
                    BadgeConnectionPhase.TRANSPORT_OPEN,
                ) && connection.lastValidStatusAtElapsedMs == null
            val clearCachedState = changedTarget || concreteInstanceNeedsFreshEvidence
            current.copy(
                connection = connection,
                controlStatus = current.controlStatus.takeUnless { clearCachedState },
                detections = current.detections.takeUnless { clearCachedState }.orEmpty(),
            )
        }
    }

    fun publishDetection(detection: BadgeUsbDetection, maxRows: Int) {
        _state.update { current ->
            current.copy(
                detections = (listOf(detection) + current.detections).take(maxRows),
            )
        }
    }

    private fun BadgeRepositoryState.aged(nowElapsedMs: Long): BadgeRepositoryState {
        val activeTransport = connection.transport
        val receivedAt = connection.lastValidStatusAtElapsedMs
        val expired = activeTransport != null && receivedAt != null &&
            badgeFreshness(activeTransport, receivedAt, nowElapsedMs) ==
            BadgeConnectionPhase.EXPIRED
        return copy(
            connection = connection.aged(nowElapsedMs),
            controlStatus = controlStatus.takeUnless { expired },
            detections = detections.takeUnless { expired }.orEmpty(),
        )
    }
}

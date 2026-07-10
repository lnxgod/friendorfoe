package com.friendorfoe.detection

import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withTimeout

interface BleInvestigator {
    suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult

    suspend fun cancel()
}

@Singleton
class BleInvestigationCoordinator @Inject constructor(
    private val phoneInspector: BleInvestigator,
) {
    private val lifecycleLock = Any()
    private val requestGuard = Mutex()
    private val cancelRequested = AtomicBoolean(false)
    private val cleanupStarted = AtomicBoolean(false)
    private val mutableState = MutableStateFlow<BleInvestigationResult?>(null)

    @Volatile
    private var activeRequest: BleInvestigationRequest? = null

    @Volatile
    private var activeToken: Any? = null

    @Volatile
    private var activeOperation: Job? = null

    private var acceptingCancel = false

    val state: StateFlow<BleInvestigationResult?> = mutableState.asStateFlow()

    suspend fun investigatePhone(request: BleInvestigationRequest): BleInvestigationResult {
        if (request.route != BleInvestigationRoute.PHONE) {
            return terminalResult(
                request = request,
                state = BleInvestigationState.FAILED,
                summary = "Phone route required",
                error = "invalid_route",
            )
        }
        if (!requestGuard.tryLock()) {
            return terminalResult(
                request = request,
                state = BleInvestigationState.FAILED,
                summary = "Another BLE investigation is active",
                error = "busy",
            )
        }

        val requestToken = Any()
        synchronized(lifecycleLock) {
            activeRequest = request
            activeToken = requestToken
            activeOperation = null
            cancelRequested.set(false)
            cleanupStarted.set(false)
            acceptingCancel = true
        }
        return try {
            val result = withTimeout(bleInvestigationTimeoutMs(request.timeoutMs)) {
                coroutineScope {
                    val operation = async(start = CoroutineStart.LAZY) {
                        phoneInspector.investigate(request) { progress ->
                            publishProgress(requestToken, request, progress)
                        }
                    }
                    val cancelBeforeStart = synchronized(lifecycleLock) {
                        activeOperation = operation
                        cancelRequested.get()
                    }
                    if (cancelBeforeStart) operation.cancel() else operation.start()
                    try {
                        operation.await()
                    } finally {
                        synchronized(lifecycleLock) {
                            if (activeOperation === operation) activeOperation = null
                        }
                    }
                }
            }
            publishTerminal(requestToken, request, result)
        } catch (_: TimeoutCancellationException) {
            publishTerminal(
                requestToken,
                request,
                terminalResult(
                    request = request,
                    state = BleInvestigationState.FAILED,
                    summary = "BLE investigation timed out",
                    error = "timeout",
                    retainEvidence = true,
                ),
            )
        } catch (cancelled: CancellationException) {
            val result = terminalResult(
                request = request,
                state = BleInvestigationState.CANCELLED,
                summary = "BLE investigation cancelled",
                error = null,
                retainEvidence = true,
            )
            val published = publishTerminal(requestToken, request, result)
            if (cancelRequested.get()) published else throw cancelled
        } catch (_: Exception) {
            publishTerminal(
                requestToken,
                request,
                terminalResult(
                    request = request,
                    state = BleInvestigationState.FAILED,
                    summary = "BLE investigation failed",
                    error = "inspection_failed",
                    retainEvidence = true,
                ),
            )
        } finally {
            try {
                cleanupInspectorOnce()
            } finally {
                synchronized(lifecycleLock) {
                    if (activeToken === requestToken) {
                        acceptingCancel = false
                        activeOperation = null
                        activeRequest = null
                        activeToken = null
                        cancelRequested.set(false)
                    }
                }
                requestGuard.unlock()
            }
        }
    }

    suspend fun cancel() {
        val operation = synchronized(lifecycleLock) {
            val request = activeRequest ?: return
            if (!acceptingCancel || !cancelRequested.compareAndSet(false, true)) return
            acceptingCancel = false
            mutableState.value = terminalResult(
                request = request,
                state = BleInvestigationState.CANCELLED,
                summary = "BLE investigation cancelled",
                error = null,
                retainEvidence = true,
            )
            activeOperation
        }

        try {
            cleanupInspectorOnce()
        } finally {
            operation?.cancel(CancellationException("BLE investigation cancelled"))
        }
    }

    private fun publishProgress(
        requestToken: Any,
        request: BleInvestigationRequest,
        progress: BleInvestigationResult,
    ) = synchronized(lifecycleLock) {
        if (
            activeToken === requestToken &&
            acceptingCancel &&
            !cancelRequested.get() &&
            progress.requestId == request.requestId
        ) {
            mutableState.value = progress
        }
    }

    private fun publishTerminal(
        requestToken: Any,
        request: BleInvestigationRequest,
        proposed: BleInvestigationResult,
    ): BleInvestigationResult = synchronized(lifecycleLock) {
        if (activeToken !== requestToken) return@synchronized proposed
        val result = if (cancelRequested.get()) {
            terminalResult(
                request = request,
                state = BleInvestigationState.CANCELLED,
                summary = "BLE investigation cancelled",
                error = null,
                retainEvidence = true,
            )
        } else {
            proposed
        }
        acceptingCancel = false
        mutableState.value = result
        result
    }

    private suspend fun cleanupInspectorOnce() {
        if (cleanupStarted.compareAndSet(false, true)) {
            phoneInspector.cancel()
        }
    }

    private fun terminalResult(
        request: BleInvestigationRequest,
        state: BleInvestigationState,
        summary: String,
        error: String?,
        retainEvidence: Boolean = false,
    ): BleInvestigationResult {
        val current = mutableState.value?.takeIf {
            retainEvidence && it.requestId == request.requestId
        }
        return current?.copy(
            state = state,
            summary = summary,
            error = error,
        ) ?: BleInvestigationResult(
            requestId = request.requestId,
            transport = "phone",
            mode = request.target.mode,
            targetMac = request.target.mac,
            state = state,
            connectable = null,
            services = emptyList(),
            characteristics = emptyList(),
            reads = emptyMap(),
            bonded = false,
            encrypted = false,
            authenticationRequired = false,
            summary = summary,
            error = error,
            truncated = false,
        )
    }
}

internal const val BLE_INVESTIGATION_MAX_TIMEOUT_MS = 12_000L

internal fun bleInvestigationTimeoutMs(requestedTimeoutMs: Long): Long =
    requestedTimeoutMs.coerceIn(1L, BLE_INVESTIGATION_MAX_TIMEOUT_MS)

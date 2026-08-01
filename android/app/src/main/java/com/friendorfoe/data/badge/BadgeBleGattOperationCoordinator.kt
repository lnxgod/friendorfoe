package com.friendorfoe.data.badge

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull

internal enum class BadgeBleGattOperation {
    MTU_REQUEST,
    SERVICE_DISCOVERY,
    DESCRIPTOR_WRITE,
    STATUS_READ,
    CONTROL_WRITE,
}

internal class BadgeBleGattOperationCoordinator {
    private val lock = Any()
    private var active: BadgeBleGattOperation? = null
    private var epoch = 1L
    private val idleWaiters = mutableSetOf<CompletableDeferred<Unit>>()

    fun current(): BadgeBleGattOperation? = synchronized(lock) { active }

    fun currentEpoch(): Long = synchronized(lock) { epoch }

    fun tryBegin(operation: BadgeBleGattOperation): Boolean = synchronized(lock) {
        if (active != null) return@synchronized false
        active = operation
        true
    }

    suspend fun awaitAndBegin(
        operation: BadgeBleGattOperation,
        expectedEpoch: Long,
        timeoutMs: Long,
    ): Boolean = withTimeoutOrNull(timeoutMs) {
        while (true) {
            val waiter = synchronized(lock) {
                if (epoch != expectedEpoch) return@withTimeoutOrNull false
                if (active == null) {
                    active = operation
                    return@withTimeoutOrNull true
                }
                CompletableDeferred<Unit>().also { idleWaiters += it }
            }
            try {
                waiter.await()
            } finally {
                synchronized(lock) { idleWaiters -= waiter }
            }
        }
        @Suppress("UNREACHABLE_CODE")
        false
    } ?: false

    fun complete(operation: BadgeBleGattOperation): Boolean {
        val waiters = synchronized(lock) {
            if (active != operation) return false
            active = null
            idleWaiters.toList().also { idleWaiters.clear() }
        }
        waiters.forEach { it.complete(Unit) }
        return true
    }

    fun reset() {
        val waiters = synchronized(lock) {
            epoch += 1
            active = null
            idleWaiters.toList().also { idleWaiters.clear() }
        }
        waiters.forEach { it.complete(Unit) }
    }
}

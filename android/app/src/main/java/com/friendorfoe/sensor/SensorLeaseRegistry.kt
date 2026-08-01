package com.friendorfoe.sensor

import java.util.concurrent.atomic.AtomicBoolean

interface SensorFusionLease : AutoCloseable

internal class SensorLeaseRegistry(
    private val onFirstLease: () -> Unit,
    private val onLastLease: () -> Unit,
) {
    private val lock = Any()
    private val activeTokens = mutableSetOf<Any>()

    fun acquire(): SensorFusionLease {
        val token = Any()
        synchronized(lock) {
            val startsHardware = activeTokens.isEmpty()
            activeTokens += token
            if (startsHardware) {
                try {
                    onFirstLease()
                } catch (failure: Throwable) {
                    activeTokens -= token
                    throw failure
                }
            }
        }
        return object : SensorFusionLease {
            private val closed = AtomicBoolean(false)

            override fun close() {
                if (!closed.compareAndSet(false, true)) return
                synchronized(lock) {
                    if (!activeTokens.remove(token)) return
                    if (activeTokens.isEmpty()) onLastLease()
                }
            }
        }
    }
}

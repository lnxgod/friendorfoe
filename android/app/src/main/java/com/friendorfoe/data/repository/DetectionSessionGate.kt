package com.friendorfoe.data.repository

internal class DetectionSessionGate {
    private val lock = Any()
    private var nextGeneration = 0L
    private var activeGeneration: Long? = null

    fun ensureSession(
        onStarted: (Long) -> Unit,
        onActive: (Long) -> Unit,
    ): Long = synchronized(lock) {
        activeGeneration?.let { generation ->
            onActive(generation)
            return@synchronized generation
        }

        val generation = ++nextGeneration
        activeGeneration = generation
        try {
            onStarted(generation)
        } catch (error: Throwable) {
            activeGeneration = null
            throw error
        }
        generation
    }

    fun runIfActive(generation: Long, action: () -> Unit): Boolean = synchronized(lock) {
        if (activeGeneration != generation) return@synchronized false
        action()
        true
    }

    fun endSession(onEnded: () -> Unit): Boolean = synchronized(lock) {
        val wasActive = activeGeneration != null
        activeGeneration = null
        onEnded()
        wasActive
    }

    fun restartSession(
        onEnded: () -> Unit,
        onStarted: (Long) -> Unit,
    ): Long? = synchronized(lock) {
        if (activeGeneration == null) return@synchronized null
        activeGeneration = null
        onEnded()

        val generation = ++nextGeneration
        activeGeneration = generation
        try {
            onStarted(generation)
        } catch (error: Throwable) {
            activeGeneration = null
            throw error
        }
        generation
    }

    fun <T> withGate(action: () -> T): T = synchronized(lock) { action() }

    fun isActive(): Boolean = synchronized(lock) { activeGeneration != null }
}

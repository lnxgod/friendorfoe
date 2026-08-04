package com.friendorfoe.data.repository

/**
 * Publishes only new or repositioned formation pixels at a bounded cadence.
 * Identical carousel refreshes still update internal TTL state but stay quiet
 * at the UI boundary.
 */
internal class FormationPublicationGate(
    private val minimumIntervalMs: Long,
) {
    private var dirty = false
    private var lastPublishedAtMs: Long? = null

    init {
        require(minimumIntervalMs > 0)
    }

    fun shouldPublish(mapChanged: Boolean, nowMs: Long): Boolean {
        if (mapChanged) dirty = true
        if (!dirty) return false

        val lastPublished = lastPublishedAtMs
        if (lastPublished != null && nowMs - lastPublished < minimumIntervalMs) {
            return false
        }

        dirty = false
        lastPublishedAtMs = nowMs
        return true
    }

    fun pendingDelayMs(nowMs: Long): Long? {
        if (!dirty) return null
        val lastPublished = lastPublishedAtMs ?: return 0L
        return (minimumIntervalMs - (nowMs - lastPublished)).coerceAtLeast(0L)
    }
}

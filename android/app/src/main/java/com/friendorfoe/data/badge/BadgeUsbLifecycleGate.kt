package com.friendorfoe.data.badge

internal class BadgeUsbLifecycleGate {
    private var nextSession = 0L
    private var currentSession: Long? = null

    @Synchronized
    fun begin(): Boolean = if (currentSession != null) {
        false
    } else {
        currentSession = ++nextSession
        true
    }

    @Synchronized
    fun end(): Boolean = currentSession?.let(::end) ?: false

    @Synchronized
    fun end(session: Long): Boolean = if (currentSession == session) {
        currentSession = null
        true
    } else {
        false
    }

    @Synchronized
    fun activeSession(): Long? = currentSession

    @Synchronized
    fun isActive(session: Long): Boolean = currentSession == session

    @Synchronized
    fun canClean(session: Long): Boolean = currentSession == null && nextSession == session
}

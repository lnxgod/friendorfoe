package com.friendorfoe.data.badge

internal class BadgeUsbLifecycleGate {
    private var active = false

    @Synchronized
    fun begin(): Boolean = if (active) {
        false
    } else {
        active = true
        true
    }

    @Synchronized
    fun end(): Boolean = if (!active) {
        false
    } else {
        active = false
        true
    }
}

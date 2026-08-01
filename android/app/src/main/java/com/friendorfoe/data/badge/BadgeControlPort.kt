package com.friendorfoe.data.badge

import kotlinx.coroutines.flow.StateFlow

interface BadgeControlPort {
    val state: StateFlow<BadgeRepositoryState>
    fun start()
    fun stop()
    fun requestConnection()
    fun refreshStatus()
    suspend fun execute(command: BadgeCommand): BadgeCommandOutcome
}

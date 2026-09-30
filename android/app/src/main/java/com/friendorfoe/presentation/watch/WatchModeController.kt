package com.friendorfoe.presentation.watch

import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

data class WatchModeState(val active: Boolean = false, val starting: Boolean = false, val message: String? = null, val canAlert: Boolean = true)

@Singleton
class WatchModeController @Inject constructor(@ApplicationContext private val context: Context) {
    private val mutableState = MutableStateFlow(WatchModeState())
    val state = mutableState.asStateFlow()
    fun start() {
        if (state.value.active || state.value.starting) return
        mutableState.value = WatchModeState(starting = true)
        try { ContextCompat.startForegroundService(context, Intent(context, NearbyWatchService::class.java))
        } catch (_: Exception) { stopped("Couldn't start watch mode. Open the app and try again.") }
    }
    fun stop() { context.stopService(Intent(context, NearbyWatchService::class.java)) }
    fun started() { mutableState.value = WatchModeState(active = true, canAlert = false) }
    fun update(message: String, canAlert: Boolean) { mutableState.value = state.value.copy(message = message, canAlert = canAlert) }
    fun stopped(message: String? = null) { mutableState.value = WatchModeState(message = message) }
}

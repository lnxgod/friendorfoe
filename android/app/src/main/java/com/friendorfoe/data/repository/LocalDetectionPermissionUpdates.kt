package com.friendorfoe.data.repository

import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

@Singleton
class LocalDetectionPermissionUpdates @Inject constructor(
    private val provider: LocalDetectionPermissionProvider,
) {
    // A granted OS permission alone must never start a background privacy scanner.
    private var foreground = false
    private var watchActive = false
    private val _current = MutableStateFlow(LocalDetectionPermissions.None)
    val current: StateFlow<LocalDetectionPermissions> = _current.asStateFlow()

    @Synchronized
    fun setForeground(active: Boolean) {
        foreground = active
        publishCurrent()
    }

    @Synchronized
    fun setWatchActive(active: Boolean) {
        watchActive = active
        publishCurrent()
    }

    @Synchronized
    fun publishCurrent() = publish(provider.current())

    @Synchronized
    internal fun publish(permissions: LocalDetectionPermissions) {
        _current.value = when {
            foreground -> permissions
            watchActive -> permissions.copy(audioCapture = false)
            else -> LocalDetectionPermissions.None
        }
    }
}

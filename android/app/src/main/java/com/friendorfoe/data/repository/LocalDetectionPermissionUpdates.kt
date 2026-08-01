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
    private val _current = MutableStateFlow(provider.current())
    val current: StateFlow<LocalDetectionPermissions> = _current.asStateFlow()

    fun publishCurrent() {
        _current.value = provider.current()
    }

    internal fun publish(permissions: LocalDetectionPermissions) {
        _current.value = permissions
    }
}

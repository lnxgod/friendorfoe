package com.friendorfoe.presentation.permissions

import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.preferences.AppPreferences

fun interface PermissionLauncher {
    fun launch(permissions: Array<String>)
}

suspend fun requestFeaturePermissions(
    missing: Set<String>,
    preferences: AppPreferences,
    launcher: PermissionLauncher,
) {
    if (missing.isEmpty()) return
    preferences.markPermissionsRequested(missing)
    launcher.launch(missing.sorted().toTypedArray())
}

sealed interface PermissionToggleAction {
    data object NoChange : PermissionToggleAction
    data object ShowExplanation : PermissionToggleAction
    data class Commit(val checked: Boolean) : PermissionToggleAction
}

fun permissionToggleAction(
    configuredChecked: Boolean,
    effectiveChecked: Boolean,
    requestedChecked: Boolean,
    permissionState: PermissionUiState,
): PermissionToggleAction = when {
    configuredChecked && !permissionState.isUsable() -> PermissionToggleAction.Commit(false)
    requestedChecked == effectiveChecked -> PermissionToggleAction.NoChange
    !requestedChecked -> PermissionToggleAction.Commit(false)
    permissionState.isUsable() -> PermissionToggleAction.Commit(true)
    else -> PermissionToggleAction.ShowExplanation
}

internal class PendingFeatureRequestStore(
    private val savedStateHandle: SavedStateHandle,
) {
    fun begin(feature: AppFeature) {
        savedStateHandle[PENDING_FEATURE_KEY] = feature.name
    }

    fun pendingFeature(): AppFeature? = savedStateHandle.get<String>(PENDING_FEATURE_KEY)
        ?.let { name -> AppFeature.entries.firstOrNull { it.name == name } }

    fun clear() {
        savedStateHandle[PENDING_FEATURE_KEY] = null
    }

    private companion object {
        const val PENDING_FEATURE_KEY = "pending_permission_feature"
    }
}

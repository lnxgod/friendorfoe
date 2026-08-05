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
    requestedChecked: Boolean,
    permissionState: PermissionUiState,
): PermissionToggleAction = when {
    requestedChecked == configuredChecked -> PermissionToggleAction.NoChange
    !requestedChecked -> PermissionToggleAction.Commit(false)
    permissionState.isUsable() -> PermissionToggleAction.Commit(true)
    else -> PermissionToggleAction.ShowExplanation
}

@Suppress("UNUSED_PARAMETER")
fun permissionToggleAction(
    configuredChecked: Boolean,
    effectiveChecked: Boolean,
    requestedChecked: Boolean,
    permissionState: PermissionUiState,
): PermissionToggleAction = permissionToggleAction(
    configuredChecked = configuredChecked,
    requestedChecked = requestedChecked,
    permissionState = permissionState,
)

data class PendingFeatureRequest(
    val feature: AppFeature,
    val grantResultByPermission: Map<String, Boolean>? = null,
)

internal class PendingFeatureRequestStore(
    private val savedStateHandle: SavedStateHandle,
) {
    fun begin(feature: AppFeature) {
        savedStateHandle[PENDING_FEATURE_KEY] = feature.name
        savedStateHandle[PENDING_RESULT_KEYS] = null
        savedStateHandle[PENDING_RESULT_VALUES] = null
    }

    fun recordResult(result: Map<String, Boolean>): PendingFeatureRequest? {
        val feature = pendingFeature() ?: return null
        val ordered = result.toSortedMap()
        savedStateHandle[PENDING_RESULT_KEYS] = ArrayList(ordered.keys)
        savedStateHandle[PENDING_RESULT_VALUES] = ordered.values.toBooleanArray()
        return PendingFeatureRequest(feature, ordered)
    }

    fun pending(): PendingFeatureRequest? {
        val feature = pendingFeature() ?: return null
        val keys = savedStateHandle.get<ArrayList<String>>(PENDING_RESULT_KEYS)
        val values = savedStateHandle.get<BooleanArray>(PENDING_RESULT_VALUES)
        val result = if (keys != null && values != null && keys.size == values.size) {
            keys.indices.associate { index -> keys[index] to values[index] }
        } else {
            null
        }
        return PendingFeatureRequest(feature, result)
    }

    fun pendingFeature(): AppFeature? = savedStateHandle.get<String>(PENDING_FEATURE_KEY)
        ?.let { name -> AppFeature.entries.firstOrNull { it.name == name } }

    fun clear() {
        savedStateHandle[PENDING_FEATURE_KEY] = null
        savedStateHandle[PENDING_RESULT_KEYS] = null
        savedStateHandle[PENDING_RESULT_VALUES] = null
    }

    fun clearIfFeature(feature: AppFeature): Boolean {
        if (pendingFeature() != feature) return false
        clear()
        return true
    }

    private companion object {
        const val PENDING_FEATURE_KEY = "pending_permission_feature"
        const val PENDING_RESULT_KEYS = "pending_permission_result_keys"
        const val PENDING_RESULT_VALUES = "pending_permission_result_values"
    }
}

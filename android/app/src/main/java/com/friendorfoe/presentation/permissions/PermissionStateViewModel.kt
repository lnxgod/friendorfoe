package com.friendorfoe.presentation.permissions

import androidx.lifecycle.ViewModel
import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.repository.RuntimePermissionChangeNotifier
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

@HiltViewModel
class PermissionStateViewModel @Inject constructor(
    private val repository: PermissionStateRepository,
    private val preferences: AppPreferences,
    private val runtimePermissionChangeNotifier: RuntimePermissionChangeNotifier,
    savedStateHandle: SavedStateHandle,
) : ViewModel() {
    private val pendingRequest = PendingFeatureRequestStore(savedStateHandle)
    val states = repository.states

    suspend fun refresh(rationaleByPermission: Map<String, Boolean>): Map<AppFeature, PermissionUiState> =
        repository.refresh(rationaleByPermission)

    fun stateFor(feature: AppFeature): PermissionUiState = repository.stateFor(feature)

    fun missingPermissionsFor(feature: AppFeature): Set<String> =
        repository.missingPermissionsFor(feature)

    suspend fun requestPermissions(
        feature: AppFeature,
        missing: Set<String>,
        launcher: PermissionLauncher,
    ) {
        pendingRequest.begin(feature)
        requestFeaturePermissions(missing, preferences, launcher)
    }

    fun pendingFeature(): AppFeature? = pendingRequest.pendingFeature()

    fun clearPendingFeature() = pendingRequest.clear()

    fun onRuntimePermissionsChanged() {
        runtimePermissionChangeNotifier.onRuntimePermissionsChanged()
    }
}

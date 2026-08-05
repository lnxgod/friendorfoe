package com.friendorfoe.presentation.permissions

import androidx.lifecycle.ViewModel
import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.repository.RuntimePermissionChangeNotifier
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.launch

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

    fun requestPlanFor(feature: AppFeature): Set<String> = repository.requestPlanFor(feature)

    fun missingPermissionsFor(feature: AppFeature): Set<String> = requestPlanFor(feature)

    suspend fun requestPermissions(
        feature: AppFeature,
        launcher: PermissionLauncher,
    ) {
        val requestPlan = repository.requestPlanFor(feature)
        if (requestPlan.isEmpty()) {
            repository.refresh(emptyMap())
            return
        }
        pendingRequest.begin(feature)
        requestFeaturePermissions(requestPlan, preferences, launcher)
    }

    suspend fun requestPermissions(
        feature: AppFeature,
        missing: Set<String>,
        launcher: PermissionLauncher,
    ) {
        requestPermissions(feature, launcher)
    }

    fun pendingFeature(): AppFeature? = pendingRequest.pendingFeature()

    fun clearPendingFeature() = pendingRequest.clear()

    fun onRuntimePermissionsChanged() {
        runtimePermissionChangeNotifier.onRuntimePermissionsChanged()
    }

    fun onPermissionResult(
        grantResultByPermission: Map<String, Boolean>,
        rationaleByPermission: Map<String, Boolean>,
    ) {
        val pending = pendingRequest.recordResult(grantResultByPermission)
        viewModelScope.launch {
            runtimePermissionChangeNotifier.onRuntimePermissionsChanged()
            val evaluated = repository.refresh(
                rationaleByPermission = rationaleByPermission,
                grantResultByPermission = grantResultByPermission,
            )
            if (
                pending != null &&
                evaluated[pending.feature] != null &&
                evaluated[pending.feature] != PermissionUiState.Loading
            ) {
                pendingRequest.clearIfFeature(pending.feature)
            }
        }
    }
}

package com.friendorfoe.presentation.calibrate

import com.friendorfoe.calibration.CalibrationViewModel
import com.friendorfoe.calibration.preflightFailureReason
import com.friendorfoe.calibration.preflightReady
import com.friendorfoe.presentation.permissions.PermissionUiState

sealed interface CalibrateStartAction {
    data object StartWalk : CalibrateStartAction
    data object RequestPermissions : CalibrateStartAction
    data object OpenAppSettings : CalibrateStartAction
    data object Disabled : CalibrateStartAction
}

data class CalibrateScreenContract(
    val showPreflightChecklist: Boolean,
    val showDiagnosticsToggle: Boolean,
    val showDiagnosticsTabs: Boolean,
    val startAction: CalibrateStartAction,
    val startDisabledReason: String?,
)

fun buildCalibrateScreenContract(
    state: CalibrationViewModel.State,
    permissionState: PermissionUiState,
): CalibrateScreenContract {
    val (startAction, startDisabledReason) = when (permissionState) {
        PermissionUiState.Denied,
        PermissionUiState.Approximate,
        -> CalibrateStartAction.RequestPermissions to null

        PermissionUiState.PermanentlyDenied -> CalibrateStartAction.OpenAppSettings to null
        PermissionUiState.Loading -> CalibrateStartAction.Disabled to "Checking calibration permissions…"
        PermissionUiState.Granted -> when {
            !state.bluetoothEnabled -> {
                CalibrateStartAction.Disabled to "Bluetooth is off — enable it before starting a walk."
            }
            else -> state.preflightFailureReason()?.let {
                CalibrateStartAction.Disabled to it
            } ?: (CalibrateStartAction.StartWalk to null)
        }
        PermissionUiState.NotificationsBlocked,
        PermissionUiState.NotificationChannelBlocked,
        -> CalibrateStartAction.Disabled to "Calibration permissions are unavailable."
    }
    val preflightReady = state.preflightReady
    return CalibrateScreenContract(
        showPreflightChecklist = !preflightReady,
        showDiagnosticsToggle = false,
        showDiagnosticsTabs = false,
        startAction = startAction,
        startDisabledReason = startDisabledReason,
    )
}

@Deprecated("Use the canonical calibration permission state")
fun buildCalibrateScreenContract(
    state: CalibrationViewModel.State,
    hasRequiredPermissions: Boolean,
): CalibrateScreenContract = buildCalibrateScreenContract(
    state = state,
    permissionState = if (hasRequiredPermissions) {
        PermissionUiState.Granted
    } else {
        PermissionUiState.Denied
    },
)

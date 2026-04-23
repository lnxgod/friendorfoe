package com.friendorfoe.presentation.calibrate

import com.friendorfoe.calibration.CalibrationViewModel
import com.friendorfoe.calibration.preflightFailureReason
import com.friendorfoe.calibration.preflightReady

data class CalibrateScreenContract(
    val showPreflightChecklist: Boolean,
    val showDiagnosticsToggle: Boolean,
    val showDiagnosticsTabs: Boolean,
    val startDisabledReason: String?,
)

fun buildCalibrateScreenContract(
    state: CalibrationViewModel.State,
    hasRequiredPermissions: Boolean,
    diagnosticsExpanded: Boolean,
): CalibrateScreenContract {
    val startDisabledReason = when {
        !hasRequiredPermissions -> "Grant Bluetooth + location permissions to start a walk."
        !state.bluetoothEnabled -> "Bluetooth is off — enable it before starting a walk."
        else -> state.preflightFailureReason()
    }
    val diagnosticsUnlocked = state.preflightReady
    return CalibrateScreenContract(
        showPreflightChecklist = !diagnosticsUnlocked,
        showDiagnosticsToggle = diagnosticsUnlocked,
        showDiagnosticsTabs = diagnosticsUnlocked && diagnosticsExpanded,
        startDisabledReason = startDisabledReason,
    )
}

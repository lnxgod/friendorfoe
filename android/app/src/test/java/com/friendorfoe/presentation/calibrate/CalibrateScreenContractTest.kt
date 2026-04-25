package com.friendorfoe.presentation.calibrate

import com.friendorfoe.calibration.BackendReachabilityState
import com.friendorfoe.calibration.CalibrationAuthState
import com.friendorfoe.calibration.CalibrationViewModel
import com.friendorfoe.calibration.SensorLoadState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CalibrateScreenContractTest {

    @Test
    fun diagnosticsStayHiddenUntilPreflightReady() {
        val state = CalibrationViewModel.State(
            backendReachability = BackendReachabilityState.Unreachable,
            calibrationAuthState = CalibrationAuthState.Unknown,
            sensorLoadState = SensorLoadState.Unknown,
        )

        val contract = buildCalibrateScreenContract(
            state = state,
            hasRequiredPermissions = true,
        )

        assertTrue(contract.showPreflightChecklist)
        assertFalse(contract.showDiagnosticsToggle)
        assertFalse(contract.showDiagnosticsTabs)
    }

    @Test
    fun startWalkDisabledReasonExplainsWhyPreflightIsBlocked() {
        val state = CalibrationViewModel.State(
            backendReachability = BackendReachabilityState.Unreachable,
            calibrationAuthState = CalibrationAuthState.Unknown,
            sensorLoadState = SensorLoadState.Unknown,
        )

        val contract = buildCalibrateScreenContract(
            state = state,
            hasRequiredPermissions = true,
        )

        assertEquals("Backend unreachable — test connectivity first.", contract.startDisabledReason)
    }

    @Test
    fun diagnosticsStayHiddenAfterSuccessfulPreflight() {
        val state = CalibrationViewModel.State(
            backendReachability = BackendReachabilityState.Reachable,
            calibrationAuthState = CalibrationAuthState.Valid,
            sensorLoadState = SensorLoadState.Ready,
            availableSensors = listOf(
                CalibrationViewModel.SensorInfo(
                    deviceId = "gate",
                    name = "Gate",
                    lat = 1.0,
                    lon = 2.0,
                    online = true,
                    ageS = 1.0,
                )
            ),
        )

        val contract = buildCalibrateScreenContract(
            state = state,
            hasRequiredPermissions = true,
        )

        assertFalse(contract.showPreflightChecklist)
        assertFalse(contract.showDiagnosticsToggle)
        assertFalse(contract.showDiagnosticsTabs)
        assertEquals(null, contract.startDisabledReason)
    }
}

package com.friendorfoe.presentation.about

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.repository.SessionHealth
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CalibrationEntryGateTest {
    @Test
    fun requiresEnabledBackendAndHealthyCurrentEndpoint() {
        val endpointA = BackendEndpoint.parse("http://badge-lab:8000").getOrThrow()
        val endpointB = BackendEndpoint.parse("http://field-kit:8000").getOrThrow()

        assertFalse(
            calibrationEntryAvailable(
                backendEnabled = false,
                endpoint = endpointA,
                health = SessionHealth.Healthy(endpointA),
            ),
        )
        assertTrue(
            calibrationEntryAvailable(
                backendEnabled = true,
                endpoint = endpointA,
                health = SessionHealth.Healthy(endpointA),
            ),
        )
        assertFalse(
            calibrationEntryAvailable(
                backendEnabled = true,
                endpoint = endpointB,
                health = SessionHealth.Healthy(endpointA),
            ),
        )
        assertFalse(
            calibrationEntryAvailable(
                backendEnabled = true,
                endpoint = endpointA,
                health = SessionHealth.Untested,
            ),
        )
    }
}

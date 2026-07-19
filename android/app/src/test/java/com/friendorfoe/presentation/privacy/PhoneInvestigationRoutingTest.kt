package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyDetectionOrigin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PhoneInvestigationRoutingTest {
    private val now = 40_000L

    @Test
    fun `fresh Android GATT target uses phone path`() {
        assertNull(
            phoneInvestigationError(
                origin = PrivacyDetectionOrigin.ANDROID,
                target = gattTarget(),
                phoneAvailable = true,
                nowElapsedMs = now,
            ),
        )
    }

    @Test
    fun `phone path rejects unavailable passive invalid stale and mismatched targets`() {
        assertEquals(
            "phone_unavailable",
            phoneInvestigationError(
                PrivacyDetectionOrigin.ANDROID,
                gattTarget(),
                phoneAvailable = false,
                nowElapsedMs = now,
            ),
        )
        assertEquals(
            "phone_requires_gatt",
            phoneInvestigationError(
                PrivacyDetectionOrigin.ANDROID,
                gattTarget().copy(mode = BleInvestigationMode.PASSIVE_CAPTURE, mac = null),
                phoneAvailable = true,
                nowElapsedMs = now,
            ),
        )
        assertEquals(
            "invalid_target",
            phoneInvestigationError(
                PrivacyDetectionOrigin.ANDROID,
                gattTarget().copy(mac = "not-a-mac"),
                phoneAvailable = true,
                nowElapsedMs = now,
            ),
        )
        assertEquals(
            "stale_target",
            phoneInvestigationError(
                PrivacyDetectionOrigin.ANDROID,
                gattTarget().copy(observedAtElapsedMs = now - 30_001L),
                phoneAvailable = true,
                nowElapsedMs = now,
            ),
        )
        assertEquals(
            "origin_mismatch",
            phoneInvestigationError(
                PrivacyDetectionOrigin.BACKEND,
                gattTarget(),
                phoneAvailable = true,
                nowElapsedMs = now,
            ),
        )
    }

    @Test
    fun `rapid phone replacement remains blocked until terminal`() {
        assertTrue(shouldRejectConcurrentInvestigationStart(true, BleInvestigationState.QUEUED))
        assertTrue(shouldRejectConcurrentInvestigationStart(true, BleInvestigationState.READING))
        assertFalse(shouldRejectConcurrentInvestigationStart(true, BleInvestigationState.COMPLETE))
        assertFalse(shouldRejectConcurrentInvestigationStart(true, BleInvestigationState.FAILED))
        assertFalse(shouldRejectConcurrentInvestigationStart(false, null))
    }

    private fun gattTarget() = BleInvestigationTarget(
        mode = BleInvestigationMode.GATT,
        mac = "AA:BB:CC:DD:EE:FF",
        entityKey = "android:entity",
        observedAtElapsedMs = now - 1L,
        origin = PrivacyDetectionOrigin.ANDROID,
    )
}

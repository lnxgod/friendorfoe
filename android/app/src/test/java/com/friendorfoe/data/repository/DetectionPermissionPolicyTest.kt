package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionPermissionPolicyTest {
    @Test
    fun freshInstallStartsNoPermissionProtectedCollectors() {
        assertEquals(
            emptySet<ProtectedDetectionSource>(),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiScan = false,
                    audioCapture = false,
                )
            )
        )
    }

    @Test
    fun newlyGrantedCapabilitiesEnableOnlyTheirOwnedCollectors() {
        assertEquals(
            setOf(
                ProtectedDetectionSource.BLE_REMOTE_ID,
                ProtectedDetectionSource.BLE_PRIVACY,
            ),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = true,
                    wifiScan = false,
                    audioCapture = false,
                )
            )
        )
        assertEquals(
            setOf(
                ProtectedDetectionSource.WIFI_REMOTE_ID,
                ProtectedDetectionSource.WIFI_DRONE,
                ProtectedDetectionSource.WIFI_PRIVACY,
            ),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiScan = true,
                    audioCapture = false,
                )
            )
        )
        assertEquals(
            setOf(ProtectedDetectionSource.ULTRASONIC),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiScan = false,
                    audioCapture = true,
                )
            )
        )
    }

    @Test
    fun runningRepositoryRestartsWhenOwningFlowAddsPermission() {
        val none = LocalDetectionPermissions.None
        val bluetoothGranted = none.copy(bluetoothScan = true)

        assertTrue(shouldRestartForPermissionChange(true, none, bluetoothGranted))
        assertFalse(shouldRestartForPermissionChange(true, bluetoothGranted, bluetoothGranted))
        assertFalse(shouldRestartForPermissionChange(false, none, bluetoothGranted))
    }
}

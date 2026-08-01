package com.friendorfoe.data.repository

import android.Manifest
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
                    wifiAwareScan = false,
                    wifiManagerScanResults = false,
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
                    wifiAwareScan = false,
                    wifiManagerScanResults = false,
                    audioCapture = false,
                )
            )
        )
        assertEquals(
            setOf(
                ProtectedDetectionSource.WIFI_REMOTE_ID,
            ),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiAwareScan = true,
                    wifiManagerScanResults = false,
                    audioCapture = false,
                )
            )
        )
        assertEquals(
            setOf(
                ProtectedDetectionSource.WIFI_DRONE,
                ProtectedDetectionSource.WIFI_PRIVACY,
            ),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiAwareScan = false,
                    wifiManagerScanResults = true,
                    audioCapture = false,
                )
            )
        )
        assertEquals(
            setOf(ProtectedDetectionSource.ULTRASONIC),
            allowedProtectedDetectionSources(
                LocalDetectionPermissions(
                    bluetoothScan = false,
                    wifiAwareScan = false,
                    wifiManagerScanResults = false,
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

    @Test
    fun api33SeparatesNearbyWifiAwareFromLocationProtectedScanResults() {
        val nearbyOnly = localDetectionPermissionsFor(
            apiLevel = 33,
            granted = setOf(Manifest.permission.NEARBY_WIFI_DEVICES),
        )
        assertTrue(nearbyOnly.wifiAwareScan)
        assertFalse(nearbyOnly.wifiManagerScanResults)

        val fineLocationOnly = localDetectionPermissionsFor(
            apiLevel = 33,
            granted = setOf(Manifest.permission.ACCESS_FINE_LOCATION),
        )
        assertFalse(fineLocationOnly.wifiAwareScan)
        assertTrue(fineLocationOnly.wifiManagerScanResults)
    }

    @Test
    fun pre33FineLocationEnablesBothWifiTransports() {
        val permissions = localDetectionPermissionsFor(
            apiLevel = 32,
            granted = setOf(Manifest.permission.ACCESS_FINE_LOCATION),
        )

        assertTrue(permissions.wifiAwareScan)
        assertTrue(permissions.wifiManagerScanResults)
    }
}

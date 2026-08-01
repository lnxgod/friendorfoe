package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Test

class LocalDetectionPermissionUpdatesTest {

    @Test
    fun seedsFromProviderAndPublishesTheNextRuntimeSnapshot() {
        var permissions = LocalDetectionPermissions.None.copy(bluetoothScan = true)
        val updates = LocalDetectionPermissionUpdates(
            provider = object : LocalDetectionPermissionProvider {
                override fun current(): LocalDetectionPermissions = permissions
            },
        )

        assertEquals(permissions, updates.current.value)

        permissions = permissions.copy(wifiManagerScanResults = true, audioCapture = true)
        updates.publishCurrent()

        assertEquals(permissions, updates.current.value)
    }
}

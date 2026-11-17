package com.friendorfoe.data.repository

import org.junit.Assert.assertEquals
import org.junit.Test

class LocalDetectionPermissionUpdatesTest {

    @Test
    fun backgroundCannotRestartTheMicrophoneAndOnlyAnActiveWatchKeepsRadios() {
        var permissions = LocalDetectionPermissions.None.copy(bluetoothScan = true)
        val updates = LocalDetectionPermissionUpdates(
            provider = object : LocalDetectionPermissionProvider {
                override fun current(): LocalDetectionPermissions = permissions
            },
        )

        assertEquals(LocalDetectionPermissions.None, updates.current.value)
        updates.setForeground(true)
        assertEquals(permissions, updates.current.value)

        permissions = permissions.copy(wifiManagerScanResults = true, audioCapture = true)
        updates.publishCurrent()
        assertEquals(permissions, updates.current.value)

        updates.setWatchActive(true)
        updates.setForeground(false)
        assertEquals(permissions.copy(audioCapture = false), updates.current.value)
        updates.publishCurrent()
        assertEquals(permissions.copy(audioCapture = false), updates.current.value)
        updates.setWatchActive(false)
        updates.publish(permissions)
        assertEquals(LocalDetectionPermissions.None, updates.current.value)

        permissions = permissions.copy(bluetoothScan = false)
        updates.setForeground(true)
        assertEquals(permissions, updates.current.value)
    }
}

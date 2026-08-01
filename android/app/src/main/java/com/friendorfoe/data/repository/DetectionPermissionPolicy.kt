package com.friendorfoe.data.repository

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

internal data class LocalDetectionPermissions(
    val bluetoothScan: Boolean,
    val wifiScan: Boolean,
    val audioCapture: Boolean,
) {
    companion object {
        val None = LocalDetectionPermissions(
            bluetoothScan = false,
            wifiScan = false,
            audioCapture = false,
        )
    }
}

internal enum class ProtectedDetectionSource {
    BLE_REMOTE_ID,
    WIFI_REMOTE_ID,
    WIFI_DRONE,
    BLE_PRIVACY,
    WIFI_PRIVACY,
    ULTRASONIC,
}

internal fun allowedProtectedDetectionSources(
    permissions: LocalDetectionPermissions,
): Set<ProtectedDetectionSource> = buildSet {
    if (permissions.bluetoothScan) {
        add(ProtectedDetectionSource.BLE_REMOTE_ID)
        add(ProtectedDetectionSource.BLE_PRIVACY)
    }
    if (permissions.wifiScan) {
        add(ProtectedDetectionSource.WIFI_REMOTE_ID)
        add(ProtectedDetectionSource.WIFI_DRONE)
        add(ProtectedDetectionSource.WIFI_PRIVACY)
    }
    if (permissions.audioCapture) {
        add(ProtectedDetectionSource.ULTRASONIC)
    }
}

internal fun shouldRestartForPermissionChange(
    isRunning: Boolean,
    active: LocalDetectionPermissions,
    current: LocalDetectionPermissions,
): Boolean = isRunning && active != current

internal fun currentLocalDetectionPermissions(context: Context): LocalDetectionPermissions {
    fun isGranted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    val bluetoothScan = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        isGranted(Manifest.permission.BLUETOOTH_SCAN) &&
            isGranted(Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        isGranted(Manifest.permission.ACCESS_FINE_LOCATION)
    }
    val wifiScan = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        isGranted(Manifest.permission.NEARBY_WIFI_DEVICES)
    } else {
        isGranted(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    return LocalDetectionPermissions(
        bluetoothScan = bluetoothScan,
        wifiScan = wifiScan,
        audioCapture = isGranted(Manifest.permission.RECORD_AUDIO),
    )
}

package com.friendorfoe.data.repository

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

data class LocalDetectionPermissions(
    val bluetoothScan: Boolean,
    val wifiAwareScan: Boolean,
    val wifiManagerScanResults: Boolean,
    val audioCapture: Boolean,
) {
    companion object {
        val None = LocalDetectionPermissions(
            bluetoothScan = false,
            wifiAwareScan = false,
            wifiManagerScanResults = false,
            audioCapture = false,
        )
    }
}

interface LocalDetectionPermissionProvider {
    fun current(): LocalDetectionPermissions
}

@Singleton
class AndroidLocalDetectionPermissionProvider @Inject constructor(
    @ApplicationContext private val context: Context,
) : LocalDetectionPermissionProvider {
    override fun current(): LocalDetectionPermissions = currentLocalDetectionPermissions(context)
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
    if (permissions.wifiAwareScan) {
        add(ProtectedDetectionSource.WIFI_REMOTE_ID)
    }
    if (permissions.wifiManagerScanResults) {
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

internal fun localDetectionPermissionsFor(
    apiLevel: Int,
    granted: Set<String>,
): LocalDetectionPermissions {
    val fineLocation = Manifest.permission.ACCESS_FINE_LOCATION in granted
    val bluetoothScan = if (apiLevel >= Build.VERSION_CODES.S) {
        Manifest.permission.BLUETOOTH_SCAN in granted &&
            Manifest.permission.BLUETOOTH_CONNECT in granted
    } else {
        fineLocation
    }
    val wifiAwareScan = if (apiLevel >= Build.VERSION_CODES.TIRAMISU) {
        Manifest.permission.NEARBY_WIFI_DEVICES in granted
    } else {
        fineLocation
    }

    return LocalDetectionPermissions(
        bluetoothScan = bluetoothScan,
        wifiAwareScan = wifiAwareScan,
        wifiManagerScanResults = fineLocation,
        audioCapture = Manifest.permission.RECORD_AUDIO in granted,
    )
}

internal fun currentLocalDetectionPermissions(context: Context): LocalDetectionPermissions {
    fun isGranted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    val relevantPermissions = setOf(
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT,
        Manifest.permission.NEARBY_WIFI_DEVICES,
        Manifest.permission.RECORD_AUDIO,
    )
    return localDetectionPermissionsFor(
        apiLevel = Build.VERSION.SDK_INT,
        granted = relevantPermissions.filterTo(mutableSetOf(), ::isGranted),
    )
}

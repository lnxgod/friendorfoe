package com.friendorfoe.detection

import android.os.Build

/** Current reason Android Wi-Fi scanning can or cannot run. */
enum class WifiScanReadiness {
    READY,
    MISSING_FINE_LOCATION,
    MISSING_NEARBY_WIFI_DEVICES,
    LOCATION_SERVICES_DISABLED,
    WIFI_DISABLED,
    TRANSIENT_FAILURE,
}

/** Framework state reduced to a deterministic, unit-testable scan policy. */
data class WifiScanAccessSnapshot(
    val sdkInt: Int,
    val fineLocationGranted: Boolean,
    val nearbyWifiGranted: Boolean,
    val locationServicesEnabled: Boolean,
    val wifiEnabled: Boolean,
) {
    fun evaluate(): WifiScanReadiness = when {
        !fineLocationGranted -> WifiScanReadiness.MISSING_FINE_LOCATION
        sdkInt >= Build.VERSION_CODES.TIRAMISU && !nearbyWifiGranted ->
            WifiScanReadiness.MISSING_NEARBY_WIFI_DEVICES
        !locationServicesEnabled -> WifiScanReadiness.LOCATION_SERVICES_DISABLED
        !wifiEnabled -> WifiScanReadiness.WIFI_DISABLED
        else -> WifiScanReadiness.READY
    }
}

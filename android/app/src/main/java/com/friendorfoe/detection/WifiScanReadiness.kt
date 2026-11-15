package com.friendorfoe.detection

/** Current reason Android Wi-Fi scanning can or cannot run. */
enum class WifiScanReadiness {
    READY,
    MISSING_FINE_LOCATION,
    LOCATION_SERVICES_DISABLED,
    WIFI_DISABLED,
    TRANSIENT_FAILURE,
}

/** Framework state reduced to a deterministic, unit-testable scan policy. */
data class WifiScanAccessSnapshot(
    val fineLocationGranted: Boolean,
    val locationServicesEnabled: Boolean,
    val wifiEnabled: Boolean,
) {
    fun evaluate(): WifiScanReadiness = when {
        !fineLocationGranted -> WifiScanReadiness.MISSING_FINE_LOCATION
        !locationServicesEnabled -> WifiScanReadiness.LOCATION_SERVICES_DISABLED
        !wifiEnabled -> WifiScanReadiness.WIFI_DISABLED
        else -> WifiScanReadiness.READY
    }
}

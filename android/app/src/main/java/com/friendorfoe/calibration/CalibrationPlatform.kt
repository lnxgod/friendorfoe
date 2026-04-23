package com.friendorfoe.calibration

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.location.LocationListener
import android.location.LocationManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import javax.inject.Inject
import javax.inject.Singleton

enum class NetworkTransportState {
    Wifi,
    Other,
    Offline,
}

data class CalibrationNetworkSnapshot(
    val transport: NetworkTransportState = NetworkTransportState.Offline,
    val ssid: String? = null,
)

interface CalibrationPlatform {
    fun isBluetoothEnabled(): Boolean
    fun currentNetworkSnapshot(): CalibrationNetworkSnapshot
    fun requestLocationUpdates(listener: LocationListener)
    fun removeLocationUpdates(listener: LocationListener)
}

@Singleton
class AndroidCalibrationPlatform @Inject constructor(
    private val locationManager: LocationManager,
    private val bluetoothManager: BluetoothManager,
    private val wifiManager: WifiManager,
    private val connectivityManager: ConnectivityManager,
) : CalibrationPlatform {

    override fun isBluetoothEnabled(): Boolean = bluetoothManager.adapter?.isEnabled == true

    @SuppressLint("MissingPermission")
    override fun currentNetworkSnapshot(): CalibrationNetworkSnapshot {
        val activeNetwork = connectivityManager.activeNetwork
            ?: return CalibrationNetworkSnapshot()
        val capabilities = connectivityManager.getNetworkCapabilities(activeNetwork)
            ?: return CalibrationNetworkSnapshot()
        val transport = when {
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> NetworkTransportState.Wifi
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) ||
                capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) ||
                capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN) -> NetworkTransportState.Other
            else -> NetworkTransportState.Other
        }
        return CalibrationNetworkSnapshot(
            transport = transport,
            ssid = if (transport == NetworkTransportState.Wifi) readCurrentSsid() else null,
        )
    }

    override fun requestLocationUpdates(listener: LocationListener) {
        locationManager.requestLocationUpdates(
            LocationManager.GPS_PROVIDER, 1000L, 0.5f, listener
        )
        if (locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
            locationManager.requestLocationUpdates(
                LocationManager.NETWORK_PROVIDER, 2000L, 1.0f, listener
            )
        }
    }

    override fun removeLocationUpdates(listener: LocationListener) {
        locationManager.removeUpdates(listener)
    }

    @SuppressLint("MissingPermission")
    private fun readCurrentSsid(): String? {
        return try {
            val raw = wifiManager.connectionInfo?.ssid.orEmpty()
            raw.removePrefix("\"").removeSuffix("\"")
                .takeIf { it.isNotBlank() && it != "<unknown ssid>" }
        } catch (_: Exception) {
            null
        }
    }
}

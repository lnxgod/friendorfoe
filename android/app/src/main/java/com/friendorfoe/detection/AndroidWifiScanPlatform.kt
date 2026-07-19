package com.friendorfoe.detection

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.os.Build
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/** Narrow Android boundary used by the recoverable Wi-Fi scan coordinator. */
internal interface WifiScanPlatform {
    fun readiness(): WifiScanReadiness
    fun registerResultsReceiver(onResultsAvailable: () -> Unit)
    fun unregisterResultsReceiver()
    fun startScan(): Boolean
    fun cachedResults(): List<ScanResult>
}

@Singleton
class AndroidWifiScanPlatform @Inject constructor(
    @ApplicationContext private val context: Context,
    private val wifiManager: WifiManager,
    private val locationManager: LocationManager,
) : WifiScanPlatform {
    private var resultsReceiver: BroadcastReceiver? = null

    override fun readiness(): WifiScanReadiness = WifiScanAccessSnapshot(
        sdkInt = Build.VERSION.SDK_INT,
        fineLocationGranted = hasPermission(Manifest.permission.ACCESS_FINE_LOCATION),
        nearbyWifiGranted = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            hasPermission(Manifest.permission.NEARBY_WIFI_DEVICES),
        locationServicesEnabled = locationServicesEnabled(),
        wifiEnabled = wifiManager.isWifiEnabled,
    ).evaluate()

    override fun registerResultsReceiver(onResultsAvailable: () -> Unit) {
        if (resultsReceiver != null) return
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action == WifiManager.SCAN_RESULTS_AVAILABLE_ACTION) {
                    onResultsAvailable()
                }
            }
        }
        ContextCompat.registerReceiver(
            context,
            receiver,
            IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        resultsReceiver = receiver
    }

    override fun unregisterResultsReceiver() {
        val receiver = resultsReceiver ?: return
        context.unregisterReceiver(receiver)
        resultsReceiver = null
    }

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    override fun startScan(): Boolean = wifiManager.startScan()

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    override fun cachedResults(): List<ScanResult> = wifiManager.scanResults.orEmpty()

    private fun hasPermission(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    @Suppress("DEPRECATION")
    private fun locationServicesEnabled(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            locationManager.isLocationEnabled
        } else {
            locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
        }
}

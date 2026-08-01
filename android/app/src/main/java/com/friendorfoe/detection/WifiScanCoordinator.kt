package com.friendorfoe.detection

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.util.Log
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import java.util.concurrent.ConcurrentLinkedDeque
import java.util.concurrent.atomic.AtomicInteger
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Owns Android WiFi scan throttling/receiver registration so drone, privacy,
 * and anomaly detection share one scan stream instead of racing startScan().
 */
@Singleton
class WifiScanCoordinator @Inject constructor(
    @ApplicationContext private val context: Context,
    private val wifiManager: WifiManager
) {
    companion object {
        private const val TAG = "WifiScanCoordinator"
        private const val MAX_SCANS_IN_WINDOW = 4
        private const val THROTTLE_WINDOW_MS = 2 * 60 * 1000L
        private const val SCAN_INTERVAL_MS = 30_000L
    }

    private val activeCollectors = AtomicInteger(0)
    private val scanTimestamps = ConcurrentLinkedDeque<Long>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val scanEvents = MutableSharedFlow<List<ScanResult>>(replay = 1, extraBufferCapacity = 1)
    private val _currentResults = MutableStateFlow<List<ScanResult>>(emptyList())
    val currentResults: StateFlow<List<ScanResult>> = _currentResults.asStateFlow()

    private val lock = Any()
    private var receiverRegistered = false
    private var scanJob: Job? = null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action == WifiManager.SCAN_RESULTS_AVAILABLE_ACTION) {
                publishCachedResults()
            }
        }
    }

    fun scanResults(): Flow<List<ScanResult>> = callbackFlow {
        if (!hasRequiredPermission()) {
            Log.w(TAG, "WiFi scan permission missing; no scan stream")
            close()
            return@callbackFlow
        }

        start()
        val relayJob = launch {
            scanEvents.collect { trySend(it) }
        }
        currentResults.value.takeIf { it.isNotEmpty() }?.let { trySend(it) }

        awaitClose {
            relayJob.cancel()
            stopOneCollector()
        }
    }

    private fun hasRequiredPermission(): Boolean {
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.ACCESS_FINE_LOCATION,
        ) == PackageManager.PERMISSION_GRANTED
    }

    private fun start() {
        if (activeCollectors.incrementAndGet() > 1) return
        synchronized(lock) {
            if (!receiverRegistered) {
                context.registerReceiver(receiver, IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION))
                receiverRegistered = true
            }
            scanJob?.cancel()
            scanJob = scope.launch {
                while (isActive) {
                    triggerScanIfAllowed()
                    publishCachedResults()
                    delay(SCAN_INTERVAL_MS)
                }
            }
        }
    }

    private fun stopOneCollector() {
        if (activeCollectors.decrementAndGet() > 0) return
        synchronized(lock) {
            scanJob?.cancel()
            scanJob = null
            if (receiverRegistered) {
                try {
                    context.unregisterReceiver(receiver)
                } catch (e: Exception) {
                    Log.w(TAG, "Error unregistering WiFi receiver", e)
                }
                receiverRegistered = false
            }
        }
    }

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    private fun triggerScanIfAllowed() {
        val now = System.currentTimeMillis()
        var oldest = scanTimestamps.peekFirst()
        while (oldest != null && now - oldest > THROTTLE_WINDOW_MS) {
            scanTimestamps.pollFirst()
            oldest = scanTimestamps.peekFirst()
        }

        if (scanTimestamps.size >= MAX_SCANS_IN_WINDOW) {
            Log.d(TAG, "WiFi scan throttled (${scanTimestamps.size} scans in window)")
            return
        }

        if (wifiManager.startScan()) {
            scanTimestamps.add(now)
        }
    }

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    private fun publishCachedResults() {
        val results = wifiManager.scanResults ?: emptyList()
        _currentResults.value = results
        scanEvents.tryEmit(results)
    }
}

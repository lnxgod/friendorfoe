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
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import dagger.hilt.android.qualifiers.ApplicationContext
import java.util.concurrent.ConcurrentLinkedDeque
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.transform
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

data class WifiScanNetwork(
    val ssid: String,
    val bssid: String,
    val capabilities: String?,
    val rssi: Int,
    val frequencyMhz: Int,
)

data class WifiScanBatch(
    val batchId: Long,
    val networks: List<WifiScanNetwork>,
    val observedElapsedMs: Long,
    val observedWallMs: Long,
    val rawResults: List<ScanResult> = emptyList(),
)

sealed interface WifiScanEvent {
    data class Success(val batch: WifiScanBatch) : WifiScanEvent

    data class Failure(
        val message: String,
        val observedElapsedMs: Long,
        val observedWallMs: Long,
    ) : WifiScanEvent

    data class Unsupported(
        val message: String,
        val observedElapsedMs: Long,
        val observedWallMs: Long,
    ) : WifiScanEvent
}

internal data class WifiScanReading(
    val networks: List<WifiScanNetwork>,
    val rawResults: List<ScanResult> = emptyList(),
)

internal interface WifiScanPlatform {
    fun hasRequiredPermission(): Boolean
    fun registerResultsListener(listener: (resultsUpdated: Boolean) -> Unit)
    fun unregisterResultsListener()
    fun startScan(): Boolean
    fun readLatest(): WifiScanReading
}

private class AndroidWifiScanPlatform(
    private val context: Context,
    private val wifiManager: WifiManager,
) : WifiScanPlatform {
    private var listener: ((Boolean) -> Unit)? = null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != WifiManager.SCAN_RESULTS_AVAILABLE_ACTION) return
            listener?.invoke(
                intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false),
            )
        }
    }

    override fun hasRequiredPermission(): Boolean = ContextCompat.checkSelfPermission(
        context,
        Manifest.permission.ACCESS_FINE_LOCATION,
    ) == PackageManager.PERMISSION_GRANTED

    override fun registerResultsListener(listener: (Boolean) -> Unit) {
        this.listener = listener
        try {
            context.registerReceiver(
                receiver,
                IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION),
            )
        } catch (failure: Throwable) {
            this.listener = null
            throw failure
        }
    }

    override fun unregisterResultsListener() {
        try {
            context.unregisterReceiver(receiver)
        } finally {
            listener = null
        }
    }

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    override fun startScan(): Boolean = wifiManager.startScan()

    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    override fun readLatest(): WifiScanReading {
        val results = wifiManager.scanResults.orEmpty()
        return WifiScanReading(
            networks = results.mapNotNull { result ->
                val bssid = result.BSSID ?: return@mapNotNull null
                WifiScanNetwork(
                    ssid = result.SSID.orEmpty(),
                    bssid = bssid,
                    capabilities = result.capabilities,
                    rssi = result.level,
                    frequencyMhz = result.frequency,
                )
            },
            rawResults = results,
        )
    }
}

/**
 * Sole owner of Android Wi-Fi scan receiver registration and startScan calls.
 * Consumers share a non-replaying stream of actual result-update broadcasts.
 */
@Singleton
class WifiScanCoordinator internal constructor(
    private val platform: WifiScanPlatform,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
    private val scanIntervalMs: Long = SCAN_INTERVAL_MS,
    private val maxScansInWindow: Int = MAX_SCANS_IN_WINDOW,
    private val throttleWindowMs: Long = THROTTLE_WINDOW_MS,
) {
    @Inject
    constructor(
        @ApplicationContext context: Context,
        wifiManager: WifiManager,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        platform = AndroidWifiScanPlatform(context, wifiManager),
        clock = clock,
        scope = scope,
    )

    private val scanTimestamps = ConcurrentLinkedDeque<Long>()
    private val batchSequence = AtomicLong(0L)
    private val events = MutableSharedFlow<WifiScanEvent>(
        replay = 0,
        extraBufferCapacity = 16,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val _currentResults = MutableStateFlow<List<ScanResult>>(emptyList())
    val currentResults: StateFlow<List<ScanResult>> = _currentResults.asStateFlow()
    private val _currentBatch = MutableStateFlow<WifiScanBatch?>(null)
    val currentBatch: StateFlow<WifiScanBatch?> = _currentBatch.asStateFlow()

    private val lock = Any()
    private var activeCollectors = 0
    private var receiverRegistered = false
    private var scanJob: Job? = null

    fun scanEvents(): Flow<WifiScanEvent> = callbackFlow {
        if (!platform.hasRequiredPermission()) {
            val elapsed = clock.nowElapsedMs()
            trySend(
                WifiScanEvent.Unsupported(
                    message = "Location permission is required for Wi-Fi scan results",
                    observedElapsedMs = elapsed,
                    observedWallMs = clock.nowWallClock().toEpochMilli(),
                ),
            )
            close()
            return@callbackFlow
        }

        val relay = launch(start = CoroutineStart.UNDISPATCHED) {
            events.collect { trySend(it) }
        }
        if (!acquire()) {
            relay.cancel()
            close()
            return@callbackFlow
        }

        awaitClose {
            relay.cancel()
            release()
        }
    }

    /** Compatibility stream for Remote ID/drone consumers during migration. */
    fun scanResults(): Flow<List<ScanResult>> = scanEvents().transform { event ->
        if (event is WifiScanEvent.Success) emit(event.batch.rawResults)
    }

    private fun acquire(): Boolean = synchronized(lock) {
        activeCollectors += 1
        if (activeCollectors > 1) return@synchronized true

        try {
            platform.registerResultsListener(::onResultsAvailable)
            receiverRegistered = true
            scanJob = scope.launch {
                while (isActive) {
                    triggerScanIfAllowed()
                    delay(scanIntervalMs)
                }
            }
            true
        } catch (failure: Throwable) {
            activeCollectors -= 1
            publishFailure(failure.message ?: "Could not register for Wi-Fi scan results")
            false
        }
    }

    private fun release() {
        synchronized(lock) {
            if (activeCollectors <= 0) return
            activeCollectors -= 1
            if (activeCollectors > 0) return

            scanJob?.cancel()
            scanJob = null
            if (receiverRegistered) {
                try {
                    platform.unregisterResultsListener()
                } catch (failure: Throwable) {
                    Log.w(TAG, "Error unregistering Wi-Fi scan receiver", failure)
                } finally {
                    receiverRegistered = false
                }
            }
        }
    }

    private fun triggerScanIfAllowed() {
        val now = clock.nowElapsedMs()
        var oldest = scanTimestamps.peekFirst()
        while (oldest != null && elapsedAge(now, oldest) > throttleWindowMs) {
            scanTimestamps.pollFirst()
            oldest = scanTimestamps.peekFirst()
        }

        if (scanTimestamps.size >= maxScansInWindow) {
            Log.d(TAG, "Wi-Fi scan throttled (${scanTimestamps.size} scans in window)")
            return
        }

        try {
            if (platform.startScan()) {
                scanTimestamps.add(now)
            } else {
                publishFailure("Android rejected the Wi-Fi scan request")
            }
        } catch (failure: Throwable) {
            publishFailure(failure.message ?: "Wi-Fi scan request failed")
        }
    }

    private fun onResultsAvailable(resultsUpdated: Boolean) {
        if (!resultsUpdated) {
            publishFailure("Android reported that Wi-Fi scan results were not updated")
            return
        }

        val elapsed = clock.nowElapsedMs()
        val wall = clock.nowWallClock().toEpochMilli()
        try {
            val reading = platform.readLatest()
            val batch = WifiScanBatch(
                batchId = batchSequence.incrementAndGet(),
                networks = reading.networks,
                observedElapsedMs = elapsed,
                observedWallMs = wall,
                rawResults = reading.rawResults,
            )
            _currentResults.value = reading.rawResults
            _currentBatch.value = batch
            events.tryEmit(WifiScanEvent.Success(batch))
        } catch (failure: Throwable) {
            events.tryEmit(
                WifiScanEvent.Failure(
                    message = failure.message ?: "Could not read updated Wi-Fi scan results",
                    observedElapsedMs = elapsed,
                    observedWallMs = wall,
                ),
            )
        }
    }

    private fun publishFailure(message: String) {
        events.tryEmit(
            WifiScanEvent.Failure(
                message = message,
                observedElapsedMs = clock.nowElapsedMs(),
                observedWallMs = clock.nowWallClock().toEpochMilli(),
            ),
        )
    }

    private fun elapsedAge(now: Long, then: Long): Long {
        if (now <= then) return 0L
        val difference = now - then
        return if (difference < 0L) Long.MAX_VALUE else difference
    }

    companion object {
        private const val TAG = "WifiScanCoordinator"
        private const val MAX_SCANS_IN_WINDOW = 4
        private const val THROTTLE_WINDOW_MS = 2 * 60 * 1000L
        private const val SCAN_INTERVAL_MS = 30_000L
    }
}

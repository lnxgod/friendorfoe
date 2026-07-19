package com.friendorfoe.detection

import android.net.wifi.ScanResult
import android.util.Log
import java.util.concurrent.ConcurrentLinkedDeque
import java.util.concurrent.atomic.AtomicInteger
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Owns Android WiFi scan throttling/receiver registration so drone, privacy,
 * and anomaly detection share one scan stream instead of racing startScan().
 */
@Singleton
class WifiScanCoordinator internal constructor(
    private val platform: WifiScanPlatform,
    dispatcher: CoroutineDispatcher,
    private val readyIntervalMs: Long,
    private val blockedRecheckMs: Long,
    private val clockMillis: () -> Long,
) {
    companion object {
        private const val TAG = "WifiScanCoordinator"
        private const val MAX_SCANS_IN_WINDOW = 4
        private const val THROTTLE_WINDOW_MS = 2 * 60 * 1000L
        private const val SCAN_INTERVAL_MS = 30_000L
        private const val BLOCKED_RECHECK_MS = 1_000L
    }

    @Inject
    constructor(platform: AndroidWifiScanPlatform) : this(
        platform = platform,
        dispatcher = Dispatchers.Default,
        readyIntervalMs = SCAN_INTERVAL_MS,
        blockedRecheckMs = BLOCKED_RECHECK_MS,
        clockMillis = System::currentTimeMillis,
    )

    private val activeCollectors = AtomicInteger(0)
    private val scanTimestamps = ConcurrentLinkedDeque<Long>()
    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val scanEvents = MutableSharedFlow<List<ScanResult>>(replay = 1, extraBufferCapacity = 1)
    private val _currentResults = MutableStateFlow<List<ScanResult>>(emptyList())
    val currentResults: StateFlow<List<ScanResult>> = _currentResults.asStateFlow()
    private val _readiness = MutableStateFlow(WifiScanReadiness.TRANSIENT_FAILURE)
    val readiness: StateFlow<WifiScanReadiness> = _readiness.asStateFlow()
    private val wakeEvents = Channel<Unit>(Channel.CONFLATED)

    private val lock = Any()
    private var receiverRegistered = false
    private var scanJob: Job? = null

    fun scanResults(): Flow<List<ScanResult>> = callbackFlow {
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

    /** Wake the worker after a permission, Wi-Fi, or location-services change. */
    fun notifyPlatformStateChanged() {
        wakeEvents.trySend(Unit)
    }

    private fun start() {
        if (activeCollectors.incrementAndGet() > 1) return
        synchronized(lock) {
            if (!receiverRegistered) {
                platform.registerResultsReceiver {
                    scope.launch { publishCachedResultsIfReady() }
                }
                receiverRegistered = true
            }
            scanJob?.cancel()
            scanJob = scope.launch {
                while (isActive) {
                    val state = platformReadiness()
                    _readiness.value = state
                    val succeeded = state == WifiScanReadiness.READY && runScanCycle()
                    val interval = if (succeeded) readyIntervalMs else blockedRecheckMs
                    withTimeoutOrNull(interval) { wakeEvents.receive() }
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
                    platform.unregisterResultsReceiver()
                } catch (e: Exception) {
                    Log.w(TAG, "Error unregistering WiFi receiver", e)
                }
                receiverRegistered = false
            }
        }
    }

    private fun platformReadiness(): WifiScanReadiness = try {
        platform.readiness()
    } catch (error: SecurityException) {
        logSecurity("WiFi readiness check lost permission", error)
        WifiScanReadiness.TRANSIENT_FAILURE
    }

    private fun runScanCycle(): Boolean = try {
        triggerScanIfAllowed()
        publishCachedResults()
        _readiness.value = WifiScanReadiness.READY
        true
    } catch (error: SecurityException) {
        logSecurity("WiFi scan permission changed during scan", error)
        _readiness.value = WifiScanReadiness.TRANSIENT_FAILURE
        false
    }

    private fun triggerScanIfAllowed() {
        val now = clockMillis()
        var oldest = scanTimestamps.peekFirst()
        while (oldest != null && now - oldest > THROTTLE_WINDOW_MS) {
            scanTimestamps.pollFirst()
            oldest = scanTimestamps.peekFirst()
        }

        if (scanTimestamps.size >= MAX_SCANS_IN_WINDOW) {
            Log.d(TAG, "WiFi scan throttled (${scanTimestamps.size} scans in window)")
            return
        }

        if (platform.startScan()) {
            scanTimestamps.add(now)
        }
    }

    private fun publishCachedResults() {
        val results = platform.cachedResults()
        _currentResults.value = results
        scanEvents.tryEmit(results)
    }

    private fun publishCachedResultsIfReady() {
        val state = platformReadiness()
        _readiness.value = state
        if (state != WifiScanReadiness.READY) return
        try {
            publishCachedResults()
            _readiness.value = WifiScanReadiness.READY
        } catch (error: SecurityException) {
            logSecurity("WiFi result permission changed", error)
            _readiness.value = WifiScanReadiness.TRANSIENT_FAILURE
        }
    }

    private fun logSecurity(message: String, error: SecurityException) {
        // Local JVM tests use Android's stub Log implementation, which throws.
        runCatching { Log.w(TAG, message, error) }
    }
}

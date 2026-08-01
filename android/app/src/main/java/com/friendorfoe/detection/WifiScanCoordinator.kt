package com.friendorfoe.detection

import android.net.wifi.ScanResult
import android.util.Log
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import java.util.concurrent.ConcurrentLinkedDeque
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

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

/**
 * Sole owner of Android Wi-Fi receiver registration and scan requests.
 *
 * The raw compatibility stream keeps origin/main's recoverable cached-result
 * behavior for Remote ID consumers. Typed privacy events remain non-replaying
 * and are emitted only when Android reports that results are available.
 */
@Singleton
class WifiScanCoordinator internal constructor(
    private val platform: WifiScanPlatform,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
    private val readyIntervalMs: Long = SCAN_INTERVAL_MS,
    private val blockedRecheckMs: Long = BLOCKED_RECHECK_MS,
    private val maxScansInWindow: Int = MAX_SCANS_IN_WINDOW,
    private val throttleWindowMs: Long = THROTTLE_WINDOW_MS,
) {
    @Inject
    constructor(
        platform: AndroidWifiScanPlatform,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        platform,
        clock,
        scope,
        SCAN_INTERVAL_MS,
        BLOCKED_RECHECK_MS,
        MAX_SCANS_IN_WINDOW,
        THROTTLE_WINDOW_MS,
    )

    private val scanTimestamps = ConcurrentLinkedDeque<Long>()
    private val batchSequence = AtomicLong(0L)
    private val events = MutableSharedFlow<WifiScanEvent>(
        replay = 0,
        extraBufferCapacity = 16,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val rawResultEvents = MutableSharedFlow<List<ScanResult>>(
        replay = 1,
        extraBufferCapacity = 1,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val _currentResults = MutableStateFlow<List<ScanResult>>(emptyList())
    val currentResults: StateFlow<List<ScanResult>> = _currentResults.asStateFlow()
    private val _currentBatch = MutableStateFlow<WifiScanBatch?>(null)
    val currentBatch: StateFlow<WifiScanBatch?> = _currentBatch.asStateFlow()
    private val _readiness = MutableStateFlow(WifiScanReadiness.TRANSIENT_FAILURE)
    val readiness: StateFlow<WifiScanReadiness> = _readiness.asStateFlow()
    private val wakeEvents = Channel<Unit>(Channel.CONFLATED)

    private val lock = Any()
    private val readinessEventLock = Any()
    private var activeCollectors = 0
    private var receiverRegistered = false
    private var scanJob: Job? = null
    private var lastPublishedBlockedState: WifiScanReadiness? = null

    /** Non-replaying, typed scan events used by privacy and anomaly detection. */
    fun scanEvents(): Flow<WifiScanEvent> = callbackFlow {
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

    /**
     * Compatibility stream for Remote ID/drone consumers.
     *
     * Cached radio evidence is replayed to a late subscriber, matching the
     * recoverable origin/main coordinator without replaying typed privacy events.
     */
    fun scanResults(): Flow<List<ScanResult>> = callbackFlow {
        val relay = launch(start = CoroutineStart.UNDISPATCHED) {
            rawResultEvents.collect { trySend(it) }
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

    /** Wake the worker after a permission, Wi-Fi, or location-services change. */
    fun notifyPlatformStateChanged() {
        wakeEvents.trySend(Unit)
    }

    private fun acquire(): Boolean = synchronized(lock) {
        activeCollectors += 1
        if (activeCollectors > 1) return@synchronized true

        synchronized(readinessEventLock) {
            lastPublishedBlockedState = null
        }
        try {
            platform.registerResultsReceiver {
                scope.launch { publishReceiverResultsIfReady() }
            }
            receiverRegistered = true
            scanJob?.cancel()
            scanJob = scope.launch {
                while (isActive) {
                    val succeeded = runWorkerCycle()
                    val interval = if (succeeded) readyIntervalMs else blockedRecheckMs
                    withTimeoutOrNull(interval) { wakeEvents.receive() }
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
                    platform.unregisterResultsReceiver()
                } catch (failure: Throwable) {
                    safeLogWarning("Error unregistering Wi-Fi scan receiver", failure)
                } finally {
                    receiverRegistered = false
                }
            }
        }
    }

    private fun runWorkerCycle(): Boolean {
        val state = platformReadiness()
        _readiness.value = state
        publishBlockedStateIfChanged(state)
        if (state != WifiScanReadiness.READY) return false

        return try {
            triggerScanIfAllowed()
            publishCachedResults(includeTypedEvent = false)
            _readiness.value = WifiScanReadiness.READY
            true
        } catch (error: SecurityException) {
            handleTransientFailure("Wi-Fi scan permission changed during scan", error)
            false
        } catch (failure: Throwable) {
            handleTransientFailure("Wi-Fi scan failed", failure)
            false
        }
    }

    private fun platformReadiness(): WifiScanReadiness = try {
        platform.readiness()
    } catch (error: SecurityException) {
        handleTransientFailure("Wi-Fi readiness check lost permission", error)
        WifiScanReadiness.TRANSIENT_FAILURE
    }

    private fun triggerScanIfAllowed() {
        val now = clock.nowElapsedMs()
        var oldest = scanTimestamps.peekFirst()
        while (oldest != null && elapsedAge(now, oldest) > throttleWindowMs) {
            scanTimestamps.pollFirst()
            oldest = scanTimestamps.peekFirst()
        }

        if (scanTimestamps.size >= maxScansInWindow) {
            safeLogDebug("Wi-Fi scan throttled (${scanTimestamps.size} scans in window)")
            return
        }

        if (platform.startScan()) {
            scanTimestamps.add(now)
        } else {
            publishFailure("Android rejected the Wi-Fi scan request")
        }
    }

    private fun publishReceiverResultsIfReady() {
        val state = platformReadiness()
        _readiness.value = state
        publishBlockedStateIfChanged(state)
        if (state != WifiScanReadiness.READY) return

        try {
            publishCachedResults(includeTypedEvent = true)
            _readiness.value = WifiScanReadiness.READY
        } catch (error: SecurityException) {
            handleTransientFailure("Wi-Fi result permission changed", error)
        } catch (failure: Throwable) {
            handleTransientFailure("Could not read updated Wi-Fi scan results", failure)
        }
    }

    private fun publishCachedResults(includeTypedEvent: Boolean) {
        val results = platform.cachedResults()
        _currentResults.value = results
        rawResultEvents.tryEmit(results)
        if (!includeTypedEvent) return

        val elapsed = clock.nowElapsedMs()
        val batch = WifiScanBatch(
            batchId = batchSequence.incrementAndGet(),
            networks = results.mapNotNull { result -> result.toNetworkOrNull() },
            observedElapsedMs = elapsed,
            observedWallMs = clock.nowWallClock().toEpochMilli(),
            rawResults = results,
        )
        _currentBatch.value = batch
        events.tryEmit(WifiScanEvent.Success(batch))
    }

    private fun ScanResult.toNetworkOrNull(): WifiScanNetwork? {
        val address = BSSID ?: return null
        return WifiScanNetwork(
            ssid = SSID.orEmpty(),
            bssid = address,
            capabilities = capabilities,
            rssi = level,
            frequencyMhz = frequency,
        )
    }

    private fun publishBlockedStateIfChanged(state: WifiScanReadiness) {
        val shouldPublish = synchronized(readinessEventLock) {
            if (state == WifiScanReadiness.READY) {
                lastPublishedBlockedState = null
                false
            } else if (lastPublishedBlockedState == state) {
                false
            } else {
                lastPublishedBlockedState = state
                true
            }
        }
        if (!shouldPublish) return

        val message = when (state) {
            WifiScanReadiness.MISSING_FINE_LOCATION ->
                "Location permission is required for Wi-Fi scan results"
            WifiScanReadiness.MISSING_NEARBY_WIFI_DEVICES ->
                "Nearby devices permission is required for Wi-Fi scan results"
            WifiScanReadiness.LOCATION_SERVICES_DISABLED ->
                "Location services must be enabled for Wi-Fi scan results"
            WifiScanReadiness.WIFI_DISABLED ->
                "Wi-Fi must be enabled for Wi-Fi scan results"
            WifiScanReadiness.TRANSIENT_FAILURE ->
                "Wi-Fi scanning is temporarily unavailable"
            WifiScanReadiness.READY -> return
        }
        if (state == WifiScanReadiness.TRANSIENT_FAILURE) {
            publishFailure(message)
        } else {
            publishUnsupported(message)
        }
    }

    private fun handleTransientFailure(message: String, failure: Throwable) {
        safeLogWarning(message, failure)
        _readiness.value = WifiScanReadiness.TRANSIENT_FAILURE
        synchronized(readinessEventLock) {
            lastPublishedBlockedState = WifiScanReadiness.TRANSIENT_FAILURE
        }
        publishFailure(failure.message?.let { "$message: $it" } ?: message)
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

    private fun publishUnsupported(message: String) {
        events.tryEmit(
            WifiScanEvent.Unsupported(
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

    private fun safeLogDebug(message: String) {
        // Local JVM tests use Android's stub Log implementation, which throws.
        runCatching { Log.d(TAG, message) }
    }

    private fun safeLogWarning(message: String, failure: Throwable) {
        // Local JVM tests use Android's stub Log implementation, which throws.
        runCatching { Log.w(TAG, message, failure) }
    }

    companion object {
        private const val TAG = "WifiScanCoordinator"
        private const val MAX_SCANS_IN_WINDOW = 4
        private const val THROTTLE_WINDOW_MS = 2 * 60 * 1000L
        private const val SCAN_INTERVAL_MS = 30_000L
        private const val BLOCKED_RECHECK_MS = 1_000L
    }
}

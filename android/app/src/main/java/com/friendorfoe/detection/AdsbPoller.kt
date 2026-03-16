package com.friendorfoe.detection

import android.util.Log
import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.data.repository.DataSource
import com.friendorfoe.data.repository.FetchException
import com.friendorfoe.domain.model.Aircraft
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Data source status indicating where aircraft data is coming from.
 */
enum class DataSourceStatus {
    /** Primary: direct adsb.fi queries */
    ADSBFI_FALLBACK,

    /** Fallback: direct airplanes.live queries */
    AIRPLANES_LIVE_FALLBACK,

    /** Last resort: direct OpenSky queries (rate-limited) */
    OPENSKY_FALLBACK,

    /** Received HTTP 429, waiting before retry */
    RATE_LIMITED,

    /** All sources are unreachable */
    OFFLINE
}

/**
 * Polls for nearby ADS-B aircraft at a regular interval.
 *
 * Uses [AircraftRepository] which tries the backend first, then falls back to OpenSky.
 * Exposes [dataSourceStatus] so the UI can show the current data source.
 */
@Singleton
class AdsbPoller @Inject constructor(
    private val aircraftRepository: AircraftRepository
) {

    companion object {
        private const val TAG = "AdsbPoller"
        private const val POLL_INTERVAL_MS = 5_000L
        private const val DEFAULT_RADIUS_NM = 50
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var pollingJob: Job? = null

    private val _aircraft = MutableSharedFlow<List<Aircraft>>(replay = 1)
    val aircraft: Flow<List<Aircraft>> = _aircraft.asSharedFlow()

    private val _dataSourceStatus = MutableStateFlow(DataSourceStatus.ADSBFI_FALLBACK)
    val dataSourceStatus: StateFlow<DataSourceStatus> = _dataSourceStatus.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    fun start(latitude: Double, longitude: Double) {
        stop()

        Log.i(TAG, "Starting ADS-B polling at ($latitude, $longitude)")

        currentLat = latitude
        currentLon = longitude

        pollingJob = scope.launch {
            var consecutiveFailures = 0

            while (isActive) {
                try {
                    val result = aircraftRepository.getNearbyAircraft(
                        latitude = currentLat,
                        longitude = currentLon,
                        radiusNm = DEFAULT_RADIUS_NM
                    )

                    result.onSuccess { nearbyResult ->
                        val status = when (nearbyResult.source) {
                            DataSource.ADSBFI -> {
                                Log.d(TAG, "adsb.fi returned ${nearbyResult.aircraft.size} aircraft")
                                DataSourceStatus.ADSBFI_FALLBACK
                            }
                            DataSource.AIRPLANES_LIVE -> {
                                Log.d(TAG, "airplanes.live returned ${nearbyResult.aircraft.size} aircraft")
                                DataSourceStatus.AIRPLANES_LIVE_FALLBACK
                            }
                            DataSource.OPENSKY -> {
                                Log.d(TAG, "OpenSky returned ${nearbyResult.aircraft.size} aircraft")
                                DataSourceStatus.OPENSKY_FALLBACK
                            }
                            DataSource.ADSB_LOL -> {
                                Log.d(TAG, "adsb.lol returned ${nearbyResult.aircraft.size} aircraft")
                                DataSourceStatus.ADSBFI_FALLBACK
                            }
                            DataSource.MULTI -> {
                                Log.d(TAG, "Multi-source returned ${nearbyResult.aircraft.size} aircraft")
                                DataSourceStatus.ADSBFI_FALLBACK
                            }
                        }
                        _aircraft.emit(nearbyResult.aircraft)
                        _dataSourceStatus.value = status
                        _lastError.value = null
                        consecutiveFailures = 0
                    }.onFailure { error ->
                        handleError(error, consecutiveFailures)
                        consecutiveFailures++
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Unexpected error during ADS-B poll", e)
                    // Don't emit emptyList() — let stale aircraft persist until pruned
                    _dataSourceStatus.value = DataSourceStatus.OFFLINE
                    _lastError.value = e.message ?: "Connection failed"
                    consecutiveFailures++
                }

                val backoffMs = calculateBackoff(consecutiveFailures)
                delay(backoffMs)
            }
        }
    }

    private fun handleError(error: Throwable, consecutiveFailures: Int) {
        // Update status/error for UI, but DON'T emit emptyList() — let stale aircraft
        // persist in SkyObjectRepository until pruned after STALE_THRESHOLD (60s).
        when (error) {
            is FetchException.RateLimited -> {
                Log.w(TAG, "Rate limited, retry after ${error.retryAfterSeconds}s")
                _dataSourceStatus.value = DataSourceStatus.RATE_LIMITED
                _lastError.value = "Rate limited (${error.retryAfterSeconds}s)"
            }
            is FetchException.NetworkError -> {
                Log.w(TAG, "Network error: ${error.message}")
                _dataSourceStatus.value = DataSourceStatus.OFFLINE
                _lastError.value = error.message ?: "Network error"
            }
            else -> {
                Log.w(TAG, "Fetch failed: ${error.message}")
                _dataSourceStatus.value = DataSourceStatus.OFFLINE
                _lastError.value = error.message ?: "Unknown error"
            }
        }
    }

    private fun calculateBackoff(consecutiveFailures: Int): Long {
        val lastStatus = _dataSourceStatus.value
        if (lastStatus == DataSourceStatus.RATE_LIMITED) {
            val lastErr = _lastError.value
            // Parse retry-after from error message if available
            val retrySeconds = lastErr?.let {
                Regex("\\((\\d+)s\\)").find(it)?.groupValues?.get(1)?.toLongOrNull()
            } ?: 30L
            return retrySeconds * 1000L
        }
        return if (consecutiveFailures > 2) {
            minOf(POLL_INTERVAL_MS * consecutiveFailures.toLong(), 60_000L)
        } else {
            POLL_INTERVAL_MS
        }
    }

    @Volatile private var currentLat = 0.0
    @Volatile private var currentLon = 0.0

    fun updatePosition(latitude: Double, longitude: Double) {
        currentLat = latitude
        currentLon = longitude
    }

    fun stop() {
        pollingJob?.cancel()
        pollingJob = null
        Log.i(TAG, "ADS-B polling stopped")
    }

    val isPolling: Boolean
        get() = pollingJob?.isActive == true
}

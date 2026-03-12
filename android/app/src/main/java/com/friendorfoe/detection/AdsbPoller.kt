package com.friendorfoe.detection

import android.util.Log
import com.friendorfoe.data.repository.AircraftRepository
import com.friendorfoe.domain.model.Aircraft
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Polls the backend API for nearby ADS-B aircraft at a regular interval.
 *
 * Uses [AircraftRepository] to fetch aircraft within a bounding box around
 * the user's current GPS position. Results are emitted as a Flow of Aircraft lists.
 *
 * Polling runs every 5 seconds on a background coroutine. Call [start] with the
 * user's coordinates to begin, and [stop] to halt polling.
 */
@Singleton
class AdsbPoller @Inject constructor(
    private val aircraftRepository: AircraftRepository
) {

    companion object {
        private const val TAG = "AdsbPoller"

        /** Polling interval in milliseconds (5 seconds). */
        private const val POLL_INTERVAL_MS = 5_000L

        /** Search radius in nautical miles for the backend query. */
        private const val DEFAULT_RADIUS_NM = 50
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var pollingJob: Job? = null

    private val _aircraft = MutableSharedFlow<List<Aircraft>>(replay = 1)

    /** Flow of nearby aircraft lists, updated every polling cycle. */
    val aircraft: Flow<List<Aircraft>> = _aircraft.asSharedFlow()

    /**
     * Start polling the backend for nearby ADS-B aircraft.
     *
     * @param latitude User's current latitude in decimal degrees
     * @param longitude User's current longitude in decimal degrees
     */
    fun start(latitude: Double, longitude: Double) {
        // Stop any existing polling before starting a new one
        stop()

        Log.i(TAG, "Starting ADS-B polling at ($latitude, $longitude)")

        pollingJob = scope.launch {
            // Track current position; future enhancement could accept a Flow<Location>
            var currentLat = latitude
            var currentLon = longitude

            while (isActive) {
                try {
                    val result = aircraftRepository.getNearbyAircraft(
                        latitude = currentLat,
                        longitude = currentLon,
                        radiusNm = DEFAULT_RADIUS_NM
                    )

                    result.onSuccess { aircraftList ->
                        Log.d(TAG, "Fetched ${aircraftList.size} aircraft from backend")
                        _aircraft.emit(aircraftList)
                    }.onFailure { error ->
                        Log.w(TAG, "Failed to fetch aircraft: ${error.message}")
                        // Emit empty list on failure so downstream consumers know
                        // the poll completed without results
                        _aircraft.emit(emptyList())
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Unexpected error during ADS-B poll", e)
                    _aircraft.emit(emptyList())
                }

                delay(POLL_INTERVAL_MS)
            }
        }
    }

    /**
     * Update the polling position without restarting the poller.
     *
     * This is a convenience for when the user moves significantly.
     * The current implementation restarts polling at the new coordinates.
     *
     * @param latitude New latitude
     * @param longitude New longitude
     */
    fun updatePosition(latitude: Double, longitude: Double) {
        if (pollingJob?.isActive == true) {
            start(latitude, longitude)
        }
    }

    /** Stop polling for ADS-B aircraft. */
    fun stop() {
        pollingJob?.cancel()
        pollingJob = null
        Log.i(TAG, "ADS-B polling stopped")
    }

    /** Whether the poller is currently active. */
    val isPolling: Boolean
        get() = pollingJob?.isActive == true
}

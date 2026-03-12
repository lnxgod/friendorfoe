package com.friendorfoe.data.repository

import android.util.Log
import com.friendorfoe.detection.AdsbPoller
import com.friendorfoe.detection.RemoteIdScanner
import com.friendorfoe.detection.WifiDroneScanner
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.SkyObject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.time.Duration
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Unified repository that merges all detection sources into a single stream
 * of sky objects for the presentation layer.
 *
 * Combines:
 * - ADS-B aircraft from [AdsbPoller] (backend API, high confidence)
 * - Remote ID drones from [RemoteIdScanner] (BLE, high confidence)
 * - WiFi drones from [WifiDroneScanner] (SSID matching, low confidence)
 *
 * Performs deduplication when the same drone is detected by multiple sources.
 * Remote ID takes precedence over WiFi when both detect the same drone.
 *
 * Exposes a [skyObjects] StateFlow that the presentation layer observes.
 */
@Singleton
class SkyObjectRepository @Inject constructor(
    private val adsbPoller: AdsbPoller,
    private val remoteIdScanner: RemoteIdScanner,
    private val wifiDroneScanner: WifiDroneScanner
) {

    companion object {
        private const val TAG = "SkyObjectRepository"

        /**
         * Maximum age before a sky object is considered stale and removed.
         * ADS-B data refreshes every 5s, Remote ID every few seconds, WiFi every 30s.
         */
        private val STALE_THRESHOLD = Duration.ofSeconds(60)

        /**
         * Distance threshold in degrees for deduplication between Remote ID and WiFi.
         * Approximately 100 meters at mid-latitudes.
         * 0.001 degrees ~ 111 meters at equator.
         */
        private const val DEDUP_DISTANCE_THRESHOLD_DEG = 0.001
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var collectionJob: Job? = null

    // Internal mutable maps keyed by object ID for efficient updates
    private val adsbObjects = mutableMapOf<String, SkyObject>()
    private val remoteIdObjects = mutableMapOf<String, Drone>()
    private val wifiObjects = mutableMapOf<String, Drone>()

    private val _skyObjects = MutableStateFlow<List<SkyObject>>(emptyList())

    /** Combined, deduplicated list of all detected sky objects. */
    val skyObjects: StateFlow<List<SkyObject>> = _skyObjects.asStateFlow()

    /**
     * Start all detection sources and begin merging results.
     *
     * @param latitude User's current latitude for ADS-B polling
     * @param longitude User's current longitude for ADS-B polling
     */
    fun start(latitude: Double, longitude: Double) {
        stop()
        Log.i(TAG, "Starting all detection sources at ($latitude, $longitude)")

        // Start ADS-B polling
        adsbPoller.start(latitude, longitude)

        // Collect from all sources
        collectionJob = scope.launch {
            // Launch parallel collectors for each source
            launch { collectAdsb() }
            launch { collectRemoteId() }
            launch { collectWifi() }
        }
    }

    /** Stop all detection sources. */
    fun stop() {
        collectionJob?.cancel()
        collectionJob = null
        adsbPoller.stop()
        remoteIdScanner.stopScanning()
        wifiDroneScanner.stopScanning()

        adsbObjects.clear()
        remoteIdObjects.clear()
        wifiObjects.clear()
        _skyObjects.value = emptyList()

        Log.i(TAG, "All detection sources stopped")
    }

    /**
     * Update the user's position for ADS-B polling.
     *
     * @param latitude New latitude
     * @param longitude New longitude
     */
    fun updatePosition(latitude: Double, longitude: Double) {
        adsbPoller.updatePosition(latitude, longitude)
    }

    /**
     * Collect ADS-B aircraft from the poller.
     * Each emission replaces the entire ADS-B set (the backend returns all nearby aircraft).
     */
    private suspend fun collectAdsb() {
        adsbPoller.aircraft.collect { aircraftList ->
            synchronized(adsbObjects) {
                adsbObjects.clear()
                aircraftList.forEach { aircraft ->
                    adsbObjects[aircraft.id] = aircraft
                }
            }
            Log.d(TAG, "ADS-B updated: ${aircraftList.size} aircraft")
            rebuildMergedList()
        }
    }

    /**
     * Collect Remote ID drones from BLE scanner.
     * Each emission is a single drone detection/update.
     */
    private suspend fun collectRemoteId() {
        remoteIdScanner.startScanning().collect { drone ->
            synchronized(remoteIdObjects) {
                remoteIdObjects[drone.id] = drone
            }
            Log.d(TAG, "Remote ID updated: drone ${drone.droneId}")
            rebuildMergedList()
        }
    }

    /**
     * Collect WiFi drones from SSID scanner.
     * Each emission is a single drone detection.
     */
    private suspend fun collectWifi() {
        wifiDroneScanner.startScanning().collect { drone ->
            synchronized(wifiObjects) {
                wifiObjects[drone.id] = drone
            }
            Log.d(TAG, "WiFi updated: drone ${drone.ssid}")
            rebuildMergedList()
        }
    }

    /**
     * Rebuild the merged, deduplicated sky object list from all sources.
     *
     * Deduplication strategy:
     * 1. ADS-B aircraft are always included (they don't overlap with drone sources).
     * 2. Remote ID drones are always included (highest drone confidence).
     * 3. WiFi drones are included only if not duplicated by a Remote ID detection.
     *
     * A WiFi drone is considered a duplicate of a Remote ID drone if:
     * - Both have the same manufacturer prefix, OR
     * - Both have approximately the same position (within ~100m threshold)
     *
     * Note: WiFi detections typically don't have position data (lat/lon = 0,0),
     * so manufacturer-based matching is the primary dedup strategy.
     */
    private fun rebuildMergedList() {
        val now = Instant.now()
        val merged = mutableListOf<SkyObject>()

        // 1. Add all non-stale ADS-B aircraft
        synchronized(adsbObjects) {
            adsbObjects.values.forEach { obj ->
                if (!isStale(obj, now)) {
                    merged.add(obj)
                }
            }
        }

        // 2. Add all non-stale Remote ID drones
        val remoteIdList: List<Drone>
        synchronized(remoteIdObjects) {
            remoteIdList = remoteIdObjects.values
                .filter { !isStale(it, now) }
                .toList()
        }
        merged.addAll(remoteIdList)

        // 3. Add WiFi drones that are not duplicates of Remote ID drones
        synchronized(wifiObjects) {
            wifiObjects.values.forEach { wifiDrone ->
                if (!isStale(wifiDrone, now) && !isDuplicateOfRemoteId(wifiDrone, remoteIdList)) {
                    merged.add(wifiDrone)
                }
            }
        }

        // Remove stale entries from internal maps
        pruneStaleEntries(now)

        _skyObjects.value = merged
        Log.d(TAG, "Merged sky objects: ${merged.size} total " +
                "(${adsbObjects.size} ADS-B, ${remoteIdObjects.size} RID, ${wifiObjects.size} WiFi)")
    }

    /**
     * Check if a WiFi drone is a duplicate of any Remote ID drone.
     *
     * Matching criteria:
     * 1. Same manufacturer AND similar SSID/ID pattern
     * 2. Close geographic proximity (if both have valid positions)
     */
    private fun isDuplicateOfRemoteId(wifiDrone: Drone, remoteIdDrones: List<Drone>): Boolean {
        for (ridDrone in remoteIdDrones) {
            // Check manufacturer-based matching
            if (matchesByManufacturer(wifiDrone, ridDrone)) {
                // If only one Remote ID drone of this manufacturer is nearby,
                // it's likely the same drone
                val sameManufacturerCount = remoteIdDrones.count {
                    it.manufacturer?.equals(wifiDrone.manufacturer, ignoreCase = true) == true
                }
                if (sameManufacturerCount <= 1) {
                    Log.d(TAG, "Dedup: WiFi '${wifiDrone.ssid}' matches RID '${ridDrone.droneId}' by manufacturer")
                    return true
                }
            }

            // Check position-based matching (if WiFi drone has a valid position)
            if (matchesByPosition(wifiDrone, ridDrone)) {
                Log.d(TAG, "Dedup: WiFi '${wifiDrone.ssid}' matches RID '${ridDrone.droneId}' by position")
                return true
            }
        }
        return false
    }

    /**
     * Check if two drones share a manufacturer.
     */
    private fun matchesByManufacturer(wifiDrone: Drone, ridDrone: Drone): Boolean {
        val wifiMfg = wifiDrone.manufacturer ?: return false
        val ridMfg = ridDrone.manufacturer ?: return false
        return wifiMfg.equals(ridMfg, ignoreCase = true)
    }

    /**
     * Check if two drones are at approximately the same position.
     * Only applies when both drones have valid (non-zero) coordinates.
     */
    private fun matchesByPosition(wifiDrone: Drone, ridDrone: Drone): Boolean {
        val wPos = wifiDrone.position
        val rPos = ridDrone.position

        // WiFi drones typically have 0,0 position (no GPS from WiFi scan),
        // so this check is only useful if WiFi position has been enriched
        if (wPos.latitude == 0.0 && wPos.longitude == 0.0) return false
        if (rPos.latitude == 0.0 && rPos.longitude == 0.0) return false

        val latDiff = kotlin.math.abs(wPos.latitude - rPos.latitude)
        val lonDiff = kotlin.math.abs(wPos.longitude - rPos.longitude)

        return latDiff < DEDUP_DISTANCE_THRESHOLD_DEG && lonDiff < DEDUP_DISTANCE_THRESHOLD_DEG
    }

    /**
     * Check if a sky object is stale (not updated within the threshold).
     */
    private fun isStale(obj: SkyObject, now: Instant): Boolean {
        return Duration.between(obj.lastUpdated, now) > STALE_THRESHOLD
    }

    /**
     * Remove stale entries from internal tracking maps.
     */
    private fun pruneStaleEntries(now: Instant) {
        synchronized(adsbObjects) {
            val staleKeys = adsbObjects.filter { isStale(it.value, now) }.keys
            staleKeys.forEach { adsbObjects.remove(it) }
        }
        synchronized(remoteIdObjects) {
            val staleKeys = remoteIdObjects.filter { isStale(it.value, now) }.keys
            staleKeys.forEach { remoteIdObjects.remove(it) }
        }
        synchronized(wifiObjects) {
            val staleKeys = wifiObjects.filter { isStale(it.value, now) }.keys
            staleKeys.forEach { wifiObjects.remove(it) }
        }
    }
}

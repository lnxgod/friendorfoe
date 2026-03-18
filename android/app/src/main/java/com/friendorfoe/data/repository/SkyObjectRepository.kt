package com.friendorfoe.data.repository

import android.util.Log
import com.friendorfoe.data.local.HistoryDao
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.local.TrackingDao
import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.detection.AdsbPoller
import com.friendorfoe.detection.BayesianFusionEngine
import com.friendorfoe.detection.DataSourceStatus
import com.friendorfoe.detection.RemoteIdScanner
import com.friendorfoe.detection.WifiDroneScanner
import com.friendorfoe.detection.WifiNanRemoteIdScanner
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import android.location.Location
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
import java.util.Collections
import java.util.concurrent.atomic.AtomicBoolean
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
    private val wifiDroneScanner: WifiDroneScanner,
    private val wifiNanRemoteIdScanner: WifiNanRemoteIdScanner,
    private val fusionEngine: BayesianFusionEngine,
    private val historyDao: HistoryDao,
    private val trackingDao: TrackingDao
) {

    companion object {
        private const val TAG = "SkyObjectRepository"
        private val STALE_THRESHOLD = Duration.ofSeconds(60)
        private const val DEDUP_DISTANCE_THRESHOLD_DEG = 0.001
        private const val MAX_TRAIL_POINTS = 60
        private const val MIN_POSITION_DELTA_DEG = 0.00001
    }

    data class TrailPoint(val lat: Double, val lon: Double, val altM: Double, val timestamp: Instant)

    private var scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var collectionJob: Job? = null
    private val isRunning = AtomicBoolean(false)

    // Track user position for history persistence
    @Volatile private var userLatitude: Double = 0.0
    @Volatile private var userLongitude: Double = 0.0

    // Track which objects have already been persisted to avoid duplicate writes
    private val persistedObjectIds: MutableSet<String> = Collections.synchronizedSet(mutableSetOf())

    // Position trail: objectId -> list of timestamped positions (most recent last)
    private val positionTrails = mutableMapOf<String, MutableList<TrailPoint>>()

    // Internal mutable maps keyed by object ID for efficient updates
    private val adsbObjects = mutableMapOf<String, SkyObject>()
    private val remoteIdObjects = mutableMapOf<String, Drone>()
    private val nanObjects = mutableMapOf<String, Drone>()
    private val wifiObjects = mutableMapOf<String, Drone>()

    private val _skyObjects = MutableStateFlow<List<SkyObject>>(emptyList())

    /** Combined, deduplicated list of all detected sky objects. */
    val skyObjects: StateFlow<List<SkyObject>> = _skyObjects.asStateFlow()

    /** Get the position trail for a given object ID. */
    fun getTrail(objectId: String): List<TrailPoint> {
        return synchronized(positionTrails) {
            positionTrails[objectId]?.toList() ?: emptyList()
        }
    }

    /** Current data source status for ADS-B data. */
    val dataSourceStatus: StateFlow<DataSourceStatus> get() = adsbPoller.dataSourceStatus

    /** Last error message, null when healthy. */
    val lastError: StateFlow<String?> get() = adsbPoller.lastError

    /**
     * Idempotent start: if already running, just update position.
     * Otherwise start all detection sources and begin merging results.
     *
     * @param latitude User's current latitude for ADS-B polling
     * @param longitude User's current longitude for ADS-B polling
     */
    fun ensureStarted(latitude: Double, longitude: Double) {
        if (!isRunning.compareAndSet(false, true)) {
            updatePosition(latitude, longitude)
            return
        }
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        userLatitude = latitude
        userLongitude = longitude
        Log.i(TAG, "Starting all detection sources at ($latitude, $longitude)")

        // Start ADS-B polling
        adsbPoller.start(latitude, longitude)

        // Collect from all sources
        collectionJob = scope.launch {
            // Launch parallel collectors for each source
            launch { collectAdsb() }
            launch { collectRemoteId() }
            launch { collectWifiNan() }
            launch { collectWifi() }
        }
    }

    /** Stop all detection sources. */
    fun stop() {
        isRunning.set(false)
        collectionJob?.cancel()
        collectionJob = null
        adsbPoller.stop()
        remoteIdScanner.stopScanning()
        wifiNanRemoteIdScanner.stopScanning()
        wifiDroneScanner.stopScanning()
        fusionEngine.reset()

        adsbObjects.clear()
        remoteIdObjects.clear()
        nanObjects.clear()
        wifiObjects.clear()
        persistedObjectIds.clear()
        positionTrails.clear()
        _skyObjects.value = emptyList()
        scope.cancel()

        Log.i(TAG, "All detection sources stopped")
    }

    /**
     * Update the user's position for ADS-B polling.
     *
     * @param latitude New latitude
     * @param longitude New longitude
     */
    fun updatePosition(latitude: Double, longitude: Double) {
        userLatitude = latitude
        userLongitude = longitude
        adsbPoller.updatePosition(latitude, longitude)
    }

    /**
     * Collect ADS-B aircraft from the poller.
     * Upserts new/updated aircraft instead of replacing the entire set,
     * so aircraft that a provider momentarily stops reporting persist
     * until pruneStaleEntries() removes them after STALE_THRESHOLD (60s).
     */
    private suspend fun collectAdsb() {
        adsbPoller.aircraft.collect { aircraftList ->
            synchronized(adsbObjects) {
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
            appendTrailPoint(drone)
            Log.d(TAG, "Remote ID updated: drone ${drone.droneId}")
            rebuildMergedList()
        }
    }

    /**
     * Collect WiFi NaN Remote ID drones.
     * Each emission is a single drone detection/update.
     */
    private suspend fun collectWifiNan() {
        wifiNanRemoteIdScanner.startScanning().collect { drone ->
            synchronized(nanObjects) {
                nanObjects[drone.id] = drone
            }
            appendTrailPoint(drone)
            Log.d(TAG, "WiFi NaN updated: drone ${drone.droneId}")
            rebuildMergedList()
        }
    }

    /**
     * Collect WiFi drones from SSID scanner.
     * Each emission is a single drone detection.
     *
     * WiFi Beacon Remote ID drones (source == REMOTE_ID) are routed to
     * remoteIdObjects for proper dedup against BLE/NaN drones and trail tracking.
     */
    private suspend fun collectWifi() {
        wifiDroneScanner.startScanning().collect { drone ->
            if (drone.source == DetectionSource.WIFI_BEACON) {
                synchronized(remoteIdObjects) {
                    remoteIdObjects[drone.id] = drone
                }
                appendTrailPoint(drone)
                Log.d(TAG, "WiFi Beacon RID updated: drone ${drone.droneId}")
            } else {
                synchronized(wifiObjects) {
                    wifiObjects[drone.id] = drone
                }
                Log.d(TAG, "WiFi updated: drone ${drone.ssid}")
            }
            rebuildMergedList()
        }
    }

    /**
     * Append a trail point for a drone if it has a valid, moved position.
     */
    private fun appendTrailPoint(drone: Drone) {
        val lat = drone.position.latitude
        val lon = drone.position.longitude
        if (lat == 0.0 && lon == 0.0) return

        val now = Instant.now()
        val point = TrailPoint(lat, lon, drone.position.altitudeMeters, now)

        synchronized(positionTrails) {
            val trail = positionTrails.getOrPut(drone.id) { mutableListOf() }
            val last = trail.lastOrNull()
            if (last != null) {
                val latDiff = kotlin.math.abs(lat - last.lat)
                val lonDiff = kotlin.math.abs(lon - last.lon)
                if (latDiff < MIN_POSITION_DELTA_DEG && lonDiff < MIN_POSITION_DELTA_DEG) return
            }
            trail.add(point)
            if (trail.size > MAX_TRAIL_POINTS) {
                trail.removeAt(0)
            }
        }

        // Persist to Room DB
        scope.launch(Dispatchers.IO) {
            try {
                trackingDao.insert(
                    TrackingEntity(
                        objectId = drone.id,
                        latitude = lat,
                        longitude = lon,
                        altitudeMeters = drone.position.altitudeMeters,
                        heading = drone.position.heading,
                        speedMps = drone.position.speedMps,
                        timestamp = now.toEpochMilli()
                    )
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed to persist trail point for ${drone.id}: ${e.message}")
            }
        }
    }

    /**
     * Rebuild the merged, deduplicated sky object list from all sources,
     * using Bayesian fusion to combine evidence from multiple sensors.
     *
     * Deduplication strategy:
     * 1. ADS-B aircraft are always included (they don't overlap with drone sources).
     * 2. BLE Remote ID drones are always included (highest drone confidence).
     * 3. WiFi NaN Remote ID drones are included, deduped against BLE RID by droneId.
     *    If same droneId seen via both, keep the one with more recent lastUpdated.
     * 4. WiFi SSID drones are included only if not duplicated by a Remote ID detection.
     *
     * When multiple sensors detect the same drone, the [fusionEngine] combines
     * their evidence to produce a fused confidence that is higher than any
     * individual source.
     */
    private fun rebuildMergedList() {
        val now = Instant.now()
        val merged = mutableListOf<SkyObject>()

        // 1. Add all non-stale ADS-B aircraft (update fusion engine)
        //    Skip grounded aircraft and very-low-altitude aircraft (ground clutter)
        synchronized(adsbObjects) {
            adsbObjects.values.forEach { obj ->
                if (!isStale(obj, now)) {
                    val isGrounded = obj is Aircraft && obj.isOnGround
                    val isTooLow = obj is Aircraft && obj.position.altitudeMeters < 30.0
                    if (!isGrounded && !isTooLow) {
                        fusionEngine.updateWithEvidence(obj.id, obj.source, obj.confidence, now)
                        merged.add(obj)
                    }
                }
            }
        }

        // 2. Add all non-stale BLE Remote ID drones
        val remoteIdList: List<Drone>
        synchronized(remoteIdObjects) {
            remoteIdList = remoteIdObjects.values
                .filter { !isStale(it, now) }
                .toList()
        }
        for (drone in remoteIdList) {
            fusionEngine.updateWithEvidence(drone.id, drone.source, drone.confidence, now)
        }
        merged.addAll(remoteIdList)

        // 3. Add non-stale WiFi NaN drones, deduped against BLE RID by droneId
        val nanList: List<Drone>
        synchronized(nanObjects) {
            nanList = nanObjects.values
                .filter { !isStale(it, now) }
                .toList()
        }
        // Build a set of droneIds already seen via BLE RID for fast lookup
        val bleRidDroneIds = remoteIdList.mapNotNull { it.droneId }.toSet()
        for (nanDrone in nanList) {
            fusionEngine.updateWithEvidence(nanDrone.id, nanDrone.source, nanDrone.confidence, now)
            if (nanDrone.droneId in bleRidDroneIds) {
                val bleMatch = remoteIdList.find { it.droneId == nanDrone.droneId }
                if (bleMatch != null && nanDrone.lastUpdated.isAfter(bleMatch.lastUpdated)) {
                    merged.remove(bleMatch)
                    merged.add(nanDrone)
                }
            } else {
                merged.add(nanDrone)
            }
        }
        // Combined Remote ID list for WiFi dedup (both BLE and NaN)
        val allRemoteIdList = remoteIdList + nanList

        // 4. Add WiFi drones that are not duplicates of any Remote ID drones
        synchronized(wifiObjects) {
            wifiObjects.values.forEach { wifiDrone ->
                if (!isStale(wifiDrone, now) && !isDuplicateOfRemoteId(wifiDrone, allRemoteIdList)) {
                    fusionEngine.updateWithEvidence(wifiDrone.id, wifiDrone.source, wifiDrone.confidence, now)
                    merged.add(wifiDrone)
                }
            }
        }

        // Apply fused confidence to all drones in the merged list
        val fused = merged.map { obj ->
            val fusedConf = fusionEngine.getFusedProbability(obj.id, now)
            // Only update confidence if the fusion engine has evidence for this object
            if (fusedConf > 0.1f) {
                when (obj) {
                    is Drone -> obj.copy(confidence = fusedConf)
                    is Aircraft -> obj.copy(confidence = fusedConf)
                }
            } else {
                obj
            }
        }

        // Remove stale entries from internal maps
        pruneStaleEntries(now)
        fusionEngine.pruneStale(now)

        // Enrich objects missing distanceMeters using user position
        val enriched = if (userLatitude != 0.0 || userLongitude != 0.0) {
            fused.map { obj ->
                if (obj.distanceMeters == null &&
                    (obj.position.latitude != 0.0 || obj.position.longitude != 0.0)
                ) {
                    val results = FloatArray(1)
                    Location.distanceBetween(
                        userLatitude, userLongitude,
                        obj.position.latitude, obj.position.longitude,
                        results
                    )
                    obj.copyWithDistance(results[0].toDouble())
                } else {
                    obj
                }
            }
        } else {
            fused
        }

        _skyObjects.value = enriched

        // Persist new detections to Room history
        persistNewDetections(enriched)

        Log.d(TAG, "Merged sky objects: ${enriched.size} total " +
                "(${adsbObjects.size} ADS-B, ${remoteIdObjects.size} BLE-RID, " +
                "${nanObjects.size} NaN-RID, ${wifiObjects.size} WiFi)")
    }

    /**
     * Persist newly-seen sky objects to the Room history database.
     * Only writes each unique object once per session to avoid excessive DB writes.
     */
    private fun persistNewDetections(objects: List<SkyObject>) {
        val newObjects = objects.filter { it.id !in persistedObjectIds }
        if (newObjects.isEmpty()) return

        scope.launch(Dispatchers.IO) {
            for (obj in newObjects) {
                try {
                    val entity = obj.toHistoryEntity(userLatitude, userLongitude)
                    historyDao.insert(entity)
                    persistedObjectIds.add(obj.id)
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to persist detection ${obj.id}: ${e.message}")
                }
            }
        }
    }

    /** Convert a SkyObject to a HistoryEntity for Room persistence. */
    private fun SkyObject.toHistoryEntity(userLat: Double, userLon: Double): HistoryEntity {
        return when (this) {
            is Aircraft -> HistoryEntity(
                objectId = icaoHex,
                objectType = "aircraft",
                detectionSource = source.name.lowercase(),
                category = category.name.lowercase(),
                displayName = callsign ?: icaoHex,
                description = displaySummary(),
                latitude = position.latitude,
                longitude = position.longitude,
                altitudeMeters = position.altitudeMeters,
                userLatitude = userLat,
                userLongitude = userLon,
                distanceMeters = distanceMeters,
                confidence = confidence,
                firstSeen = firstSeen.toEpochMilli(),
                lastSeen = lastUpdated.toEpochMilli(),
                photoUrl = photoUrl
            )
            is Drone -> HistoryEntity(
                objectId = id,
                objectType = "drone",
                detectionSource = source.name.lowercase(),
                category = category.name.lowercase(),
                displayName = manufacturer ?: "Unknown drone",
                description = displaySummary(),
                latitude = position.latitude,
                longitude = position.longitude,
                altitudeMeters = position.altitudeMeters,
                userLatitude = userLat,
                userLongitude = userLon,
                distanceMeters = distanceMeters,
                confidence = confidence,
                firstSeen = firstSeen.toEpochMilli(),
                lastSeen = lastUpdated.toEpochMilli()
            )
        }
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
        synchronized(nanObjects) {
            val staleKeys = nanObjects.filter { isStale(it.value, now) }.keys
            staleKeys.forEach { nanObjects.remove(it) }
        }
        synchronized(wifiObjects) {
            val staleKeys = wifiObjects.filter { isStale(it.value, now) }.keys
            staleKeys.forEach { wifiObjects.remove(it) }
        }
    }
}

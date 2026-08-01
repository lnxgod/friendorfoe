package com.friendorfoe.detection

import android.location.Location
import android.util.Log
import java.time.Duration
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Tracks BLE devices over time to detect:
 * 1. Devices following you (seen at multiple locations over time)
 * 2. Unknown devices lingering nearby while you're stationary
 *
 * Also provides BLE direction finding via RSSI-to-bearing mapping
 * during a user-initiated 360° rotation scan.
 */
@Singleton
class BleTracker @Inject constructor() {

    companion object {
        private const val TAG = "BleTracker"

        private const val FOLLOW_MIN_DURATION_MS = 300_000L
        private const val FOLLOW_MIN_SIGHTINGS = 6
        private const val FOLLOW_CLUSTER_DISTANCE_M = 75.0
        private const val FOLLOW_MIN_MOVEMENT_M = 150.0
        private const val CAMERA_HIGH_THREAT_DURATION_MS = 600_000L
        private const val LINGER_MIN_DURATION_MS = 600_000L
        private const val LINGER_MIN_SIGHTINGS = 10
        private const val LINGER_MAX_MOVEMENT_M = 25.0
        private const val MAX_LOCATION_ACCURACY_M = 50f
        private const val STRONG_RSSI_DBM = -70
        // Covers floating-point roundoff from Haversine conversion, not sensor distance.
        private const val DISTANCE_ROUNDOFF_M = 0.000001

        /** Maximum sightings to keep per device */
        private const val MAX_SIGHTINGS = 100

        /** How long to keep a device in tracking history */
        private const val DEVICE_HISTORY_TTL_MS = 600_000L // 10 minutes

        /** RSSI samples needed for a direction estimate */
        private const val MIN_DIRECTION_SAMPLES = 8

        private val FOLLOWER_CATEGORIES = setOf(
            PrivacyCategory.FINDMY,
            PrivacyCategory.BLE_TRACKER,
            PrivacyCategory.GPS_TRACKER,
            PrivacyCategory.OBD_TRACKER,
            PrivacyCategory.SMART_GLASSES,
            PrivacyCategory.ACTION_CAMERA,
            PrivacyCategory.DASH_CAMERA,
            PrivacyCategory.VEHICLE_CAMERA,
            PrivacyCategory.BODY_CAMERA,
        )
        private val REQUIRED_TEMPORAL_BANDS = setOf(0, 1, 2)
    }

    /** A single BLE sighting with location context */
    data class Sighting(
        val rssi: Int,
        val userLat: Double,
        val userLon: Double,
        val compassBearing: Float, // user's phone compass heading at time of sighting
        val timestamp: Instant,
        val category: PrivacyCategory,
        val isBonded: Boolean,
        val locationAccuracyMeters: Float,
    )

    /** Tracked BLE device with sighting history */
    data class TrackedDevice(
        val mac: String,
        @Volatile var deviceName: String?,
        val deviceType: String?,
        val manufacturer: String?,
        val hasCamera: Boolean,
        @Volatile var category: PrivacyCategory,
        @Volatile var isBonded: Boolean,
        @Volatile var locationAccuracyMeters: Float,
        val sightings: MutableList<Sighting> = mutableListOf(),
        var firstSeen: Instant = Instant.now(),
        @Volatile var lastSeen: Instant = Instant.now(),
        @Volatile var peakRssi: Int = -100,
        @Volatile var isFollowing: Boolean = false,
        @Volatile var isStalker: Boolean = false
    ) {
        val durationMs: Long get() = Duration.between(firstSeen, lastSeen).toMillis()
        val sightingCount: Int get() = sightings.size
    }

    /** Result of a direction-finding scan */
    data class DirectionResult(
        val mac: String,
        val estimatedBearing: Float, // 0-360 degrees, 0=North
        val confidence: Float, // 0.0-1.0
        val peakRssi: Int,
        val samples: List<Pair<Float, Int>> // bearing to RSSI pairs
    )

    data class FollowerEvidence(
        val durationMs: Long,
        val qualifyingSightings: Int,
        val clusterCount: Int,
        val movementMeters: Double,
        val temporalBands: Set<Int>,
        val strongestRssi: Int,
    )

    /** Alert for a device that appears to be following the user */
    data class StalkerAlert(
        val device: TrackedDevice,
        val reason: String, // "following", "lingering", "reappeared"
        val threatLevel: Int, // 1=low, 2=medium, 3=high
        val evidence: FollowerEvidence,
    )

    private data class EvidenceAnalysis(
        val evidence: FollowerEvidence,
        val locationSpanMeters: Double,
        val latestCategory: PrivacyCategory?,
    )

    private data class UserPoint(
        val latitude: Double,
        val longitude: Double,
        val timestamp: Instant,
        val accuracyMeters: Float,
    )

    // All tracked BLE devices keyed by MAC
    private val trackedDevices = java.util.concurrent.ConcurrentHashMap<String, TrackedDevice>()

    // User location history for movement detection
    private val userLocations = mutableListOf<UserPoint>()

    // Direction finding state
    @Volatile private var directionScanTarget: String? = null
    private val directionSamples = java.util.Collections.synchronizedList(mutableListOf<Pair<Float, Int>>()) // bearing, rssi

    /**
     * Record a BLE device sighting. Called from GlassesDetector or RemoteIdScanner
     * for every BLE advertisement received.
     */
    fun recordSighting(
        mac: String,
        rssi: Int,
        deviceName: String?,
        deviceType: String?,
        manufacturer: String?,
        hasCamera: Boolean,
        userLat: Double,
        userLon: Double,
        compassBearing: Float,
        category: PrivacyCategory = PrivacyCategory.INFORMATIONAL,
        isBonded: Boolean = false,
        locationAccuracyMeters: Float = Float.POSITIVE_INFINITY,
    ) = recordSightingAt(
        mac = mac,
        rssi = rssi,
        deviceName = deviceName,
        deviceType = deviceType,
        manufacturer = manufacturer,
        hasCamera = hasCamera,
        userLat = userLat,
        userLon = userLon,
        compassBearing = compassBearing,
        timestamp = Instant.now(),
        category = category,
        isBonded = isBonded,
        locationAccuracyMeters = locationAccuracyMeters,
    )

    /** Records one privacy-advertisement sample using the latest valid Sky location seed. */
    fun recordPrivacyObservation(
        detection: GlassesDetection,
        compassBearing: Float,
    ) {
        val latestLocation = synchronized(userLocations) { userLocations.lastOrNull() }
        recordSightingAt(
            mac = detection.mac,
            rssi = detection.rssi,
            deviceName = detection.deviceName,
            deviceType = detection.deviceType,
            manufacturer = detection.manufacturer,
            hasCamera = detection.hasCamera,
            userLat = latestLocation?.latitude ?: 0.0,
            userLon = latestLocation?.longitude ?: 0.0,
            compassBearing = compassBearing,
            timestamp = detection.lastSeen,
            category = detection.category,
            isBonded = detection.isBonded,
            locationAccuracyMeters = latestLocation?.accuracyMeters ?: Float.POSITIVE_INFINITY,
        )
    }

    internal fun recordSightingAt(
        mac: String,
        rssi: Int,
        deviceName: String?,
        deviceType: String?,
        manufacturer: String?,
        hasCamera: Boolean,
        userLat: Double,
        userLon: Double,
        compassBearing: Float,
        timestamp: Instant,
        category: PrivacyCategory = PrivacyCategory.INFORMATIONAL,
        isBonded: Boolean = false,
        locationAccuracyMeters: Float = Float.POSITIVE_INFINITY,
    ) {
        recordUserLocation(
            latitude = userLat,
            longitude = userLon,
            timestamp = timestamp,
            locationAccuracyMeters = locationAccuracyMeters,
        )

        val sighting = Sighting(
            rssi = rssi,
            userLat = userLat,
            userLon = userLon,
            compassBearing = compassBearing,
            timestamp = timestamp,
            category = category,
            isBonded = isBonded,
            locationAccuracyMeters = locationAccuracyMeters,
        )

        trackedDevices.compute(mac) { _, existing ->
            val device = existing ?: TrackedDevice(
                mac = mac,
                deviceName = deviceName,
                deviceType = deviceType,
                manufacturer = manufacturer,
                hasCamera = hasCamera,
                category = category,
                isBonded = isBonded,
                locationAccuracyMeters = locationAccuracyMeters,
                firstSeen = timestamp,
                lastSeen = timestamp,
            )
            synchronized(device) {
                if (timestamp.isAfter(device.lastSeen)) device.lastSeen = timestamp
                device.category = category
                device.isBonded = device.isBonded || isBonded
                device.locationAccuracyMeters = locationAccuracyMeters
                if (rssi > device.peakRssi) device.peakRssi = rssi
                if (device.deviceName == null && deviceName != null) {
                    device.deviceName = deviceName
                }
                device.sightings.add(sighting)
                if (device.sightings.size > MAX_SIGHTINGS) {
                    device.sightings.removeAt(0)
                }
            }
            device
        }

        recordDirectionSample(mac, rssi, compassBearing)
    }

    /**
     * Feed an active direction sweep without retaining movement history.
     * This keeps an explicitly requested sweep responsive when stalker analysis is disabled.
     */
    fun recordDirectionSample(mac: String, rssi: Int, compassBearing: Float) {
        if (directionScanTarget == mac) {
            directionSamples.add(compassBearing to rssi)
        }
    }

    /**
     * Update user location for movement tracking.
     */
    fun updateUserLocation(location: Location) {
        recordUserLocation(
            latitude = location.latitude,
            longitude = location.longitude,
            timestamp = Instant.now(),
            locationAccuracyMeters = location.accuracy,
        )
    }

    internal fun recordUserLocation(
        latitude: Double,
        longitude: Double,
        timestamp: Instant = Instant.now(),
        locationAccuracyMeters: Float = Float.POSITIVE_INFINITY,
    ) {
        if (!isValidLocation(latitude, longitude)) return
        synchronized(userLocations) {
            val last = userLocations.lastOrNull()
            if (last != null &&
                Duration.between(last.timestamp, timestamp).seconds < 5 &&
                distanceMeters(last.latitude, last.longitude, latitude, longitude) < 3.0
            ) {
                return
            }
            userLocations.add(
                UserPoint(
                    latitude = latitude,
                    longitude = longitude,
                    timestamp = timestamp,
                    accuracyMeters = locationAccuracyMeters,
                ),
            )
            // Keep last 5 minutes
            val cutoff = timestamp.minusSeconds(300)
            userLocations.removeAll { it.timestamp.isBefore(cutoff) }
        }
    }

    /**
     * Check for devices that appear to be following the user.
     * Call periodically (every 30s or so).
     *
     * @return list of stalker alerts
     */
    fun checkForFollowers(): List<StalkerAlert> {
        return checkForFollowersAt(Instant.now())
    }

    internal fun checkForFollowersAt(now: Instant): List<StalkerAlert> {
        val alerts = mutableListOf<StalkerAlert>()

        trackedDevices.values.toList().forEach { device ->
            synchronized(device) {
                device.isFollowing = false
                device.isStalker = false
            }
        }

        // Prune old devices
        val staleThreshold = now.minusMillis(DEVICE_HISTORY_TTL_MS)
        trackedDevices.keys.toList().forEach { mac ->
            trackedDevices.computeIfPresent(mac) { _, device ->
                synchronized(device) {
                    if (device.lastSeen.isBefore(staleThreshold)) null else device
                }
            }
        }

        for (device in trackedDevices.values.toList()) {
            val alert = synchronized(device) {
                device.sightings.removeAll { it.timestamp.isAfter(now) }
                val analysis = analyzeEvidence(device.sightings)
                val evidence = analysis.evidence
                if (device.isBonded || analysis.latestCategory !in FOLLOWER_CATEGORIES) {
                    null
                } else {
                    val isFollowing = evidence.durationMs >= FOLLOW_MIN_DURATION_MS &&
                        evidence.qualifyingSightings >= FOLLOW_MIN_SIGHTINGS &&
                        evidence.clusterCount >= 3 &&
                        meetsMinimumDistance(evidence.movementMeters, FOLLOW_MIN_MOVEMENT_M) &&
                        evidence.temporalBands.containsAll(REQUIRED_TEMPORAL_BANDS)

                    if (isFollowing) {
                        device.isFollowing = true
                        device.isStalker = true
                        val threatLevel = when {
                            device.hasCamera &&
                                evidence.durationMs >= CAMERA_HIGH_THREAT_DURATION_MS &&
                                evidence.strongestRssi >= STRONG_RSSI_DBM -> 3
                            else -> 2
                        }
                        StalkerAlert(device, "following", threatLevel, evidence)
                    } else if (
                        evidence.durationMs >= LINGER_MIN_DURATION_MS &&
                        evidence.qualifyingSightings >= LINGER_MIN_SIGHTINGS &&
                        meetsMaximumDistance(analysis.locationSpanMeters, LINGER_MAX_MOVEMENT_M) &&
                        evidence.strongestRssi >= STRONG_RSSI_DBM
                    ) {
                        StalkerAlert(
                            device = device,
                            reason = "lingering",
                            threatLevel = 1,
                            evidence = evidence.copy(movementMeters = analysis.locationSpanMeters),
                        )
                    } else {
                        null
                    }
                }
            }
            if (alert != null) {
                alerts.add(alert)
                if (alert.reason == "following") {
                    safeLogWarning(
                        "STALKER ALERT: ${device.deviceType ?: device.mac} following for " +
                            "${alert.evidence.durationMs / 1000}s across " +
                            "${alert.evidence.clusterCount} clusters"
                    )
                }
            }
        }

        return alerts.sortedWith(
            compareByDescending<StalkerAlert> { it.threatLevel }
                .thenBy { it.reason }
                .thenBy { it.device.mac }
        )
    }

    /**
     * Start a direction-finding scan for a specific device.
     * User should rotate 360° slowly while holding the phone.
     */
    fun startDirectionScan(mac: String) {
        directionScanTarget = mac
        synchronized(directionSamples) { directionSamples.clear() }
        safeLogInfo(TAG, "Direction scan started for $mac — rotate 360° slowly")
    }

    /**
     * Stop the direction scan and compute the estimated bearing.
     * @return DirectionResult with best bearing estimate, or null if insufficient data
     */
    fun finishDirectionScan(): DirectionResult? {
        val mac = directionScanTarget ?: return null
        directionScanTarget = null

        // Take a snapshot under the lock to avoid ConcurrentModificationException
        val snapshot = synchronized(directionSamples) { directionSamples.toList() }

        if (snapshot.size < MIN_DIRECTION_SAMPLES) {
            safeLogWarning("Direction scan: only ${snapshot.size} samples, need $MIN_DIRECTION_SAMPLES")
            return null
        }

        // Find the bearing with the strongest RSSI (closest direction)
        val sorted = snapshot.sortedByDescending { it.second }
        val peakRssi = sorted.first().second

        // Average the top 20% of samples for better accuracy
        val topCount = (snapshot.size * 0.2).toInt().coerceAtLeast(3)
        val topSamples = sorted.take(topCount)

        // Circular mean of top bearings
        var sinSum = 0.0
        var cosSum = 0.0
        for ((bearing, _) in topSamples) {
            val rad = Math.toRadians(bearing.toDouble())
            sinSum += kotlin.math.sin(rad)
            cosSum += kotlin.math.cos(rad)
        }
        val meanBearing = Math.toDegrees(kotlin.math.atan2(sinSum, cosSum)).toFloat()
        val normalizedBearing = ((meanBearing % 360f) + 360f) % 360f

        // Confidence based on RSSI spread in top samples
        val rssiRange = topSamples.maxOf { it.second } - topSamples.minOf { it.second }
        val confidence = when {
            rssiRange < 3 -> 0.9f  // Very consistent — high confidence
            rssiRange < 6 -> 0.7f
            rssiRange < 10 -> 0.5f
            else -> 0.3f
        }

        safeLogInfo(
            TAG,
            "Direction scan complete: bearing=${"%.0f".format(normalizedBearing)}° " +
                "confidence=${"%.0f".format(confidence * 100)}% " +
                "peak=${peakRssi}dBm (${snapshot.size} samples)"
        )

        return DirectionResult(
            mac = mac,
            estimatedBearing = normalizedBearing,
            confidence = confidence,
            peakRssi = peakRssi,
            samples = snapshot
        )
    }

    /** Check if a direction scan is currently active */
    fun isDirectionScanActive(): Boolean = directionScanTarget != null

    /** Get the current direction scan target MAC */
    fun getDirectionScanTarget(): String? = directionScanTarget

    /** Get all currently tracked devices */
    fun getTrackedDevices(): List<TrackedDevice> = trackedDevices.values.toList()

    /** Get a specific tracked device */
    fun getDevice(mac: String): TrackedDevice? = trackedDevices[mac]

    /** Get direction scan sample count */
    fun getDirectionSampleCount(): Int = directionSamples.size

    /** Remove one ignored identity without discarding evidence for unrelated devices. */
    fun removeEvidenceForIdentities(identityKeys: Set<String>): Int {
        val canonicalKeys = canonicalPrivacyIdentities(identityKeys)
        if (canonicalKeys.isEmpty()) return 0

        var removedCount = 0
        trackedDevices.keys.toList().forEach { mac ->
            val canonicalMac = canonicalPrivacyIdentity(mac)
            if (canonicalMac != null && canonicalMac in canonicalKeys) {
                trackedDevices.remove(mac)?.let { device ->
                    synchronized(device) {
                        device.isFollowing = false
                        device.isStalker = false
                    }
                    removedCount += 1
                }
            }
        }

        val directionTarget = directionScanTarget
        if (directionTarget != null && canonicalPrivacyIdentity(directionTarget) in canonicalKeys) {
            directionScanTarget = null
            synchronized(directionSamples) { directionSamples.clear() }
        }
        return removedCount
    }

    /** Clear all BLE follower and direction evidence for a new detection session. */
    fun clear() {
        trackedDevices.values.toList().forEach { device ->
            synchronized(device) {
                device.isFollowing = false
                device.isStalker = false
            }
        }
        trackedDevices.clear()
        synchronized(userLocations) { userLocations.clear() }
        directionScanTarget = null
        synchronized(directionSamples) { directionSamples.clear() }
    }

    private fun analyzeEvidence(sightings: List<Sighting>): EvidenceAnalysis {
        val observations = monotonicObservations(sightings)
        val qualifying = observations.filter { it.isQualifying() }
        if (qualifying.isEmpty()) {
            return EvidenceAnalysis(
                evidence = FollowerEvidence(
                    durationMs = 0,
                    qualifyingSightings = 0,
                    clusterCount = 0,
                    movementMeters = 0.0,
                    temporalBands = emptySet(),
                    strongestRssi = Int.MIN_VALUE,
                ),
                locationSpanMeters = 0.0,
                latestCategory = observations.lastOrNull()?.category,
            )
        }

        val firstTimestamp = qualifying.first().timestamp
        val durationMs = Duration.between(firstTimestamp, qualifying.last().timestamp)
            .toMillis()
            .coerceAtLeast(0)
        val anchors = mutableListOf(qualifying.first())
        qualifying.drop(1).forEach { sighting ->
            val currentAnchor = anchors.last()
            if (meetsMinimumDistance(distanceMeters(currentAnchor, sighting), FOLLOW_CLUSTER_DISTANCE_M)) {
                anchors.add(sighting)
            }
        }
        val anchorMovement = anchors.zipWithNext().sumOf { (from, to) -> distanceMeters(from, to) }
        val temporalBands = qualifying.mapTo(mutableSetOf()) { sighting ->
            temporalBand(firstTimestamp, sighting.timestamp, durationMs)
        }

        return EvidenceAnalysis(
            evidence = FollowerEvidence(
                durationMs = durationMs,
                qualifyingSightings = qualifying.size,
                clusterCount = anchors.size,
                movementMeters = anchorMovement,
                temporalBands = temporalBands,
                strongestRssi = qualifying.maxOf { it.rssi },
            ),
            locationSpanMeters = maxLocationSpanMeters(qualifying),
            latestCategory = observations.last().category,
        )
    }

    private fun monotonicObservations(sightings: List<Sighting>): List<Sighting> {
        val observations = mutableListOf<Sighting>()
        var timestampHighWater: Instant? = null
        sightings.forEach { sighting ->
            val previousTimestamp = timestampHighWater
            if (previousTimestamp != null && sighting.timestamp.isBefore(previousTimestamp)) {
                return@forEach
            }
            timestampHighWater = sighting.timestamp
            observations.add(sighting)
        }
        return observations
    }

    private fun meetsMinimumDistance(distanceMeters: Double, thresholdMeters: Double): Boolean =
        distanceMeters >= thresholdMeters ||
            thresholdMeters - distanceMeters <= DISTANCE_ROUNDOFF_M

    private fun meetsMaximumDistance(distanceMeters: Double, thresholdMeters: Double): Boolean =
        distanceMeters <= thresholdMeters ||
            distanceMeters - thresholdMeters <= DISTANCE_ROUNDOFF_M

    private fun Sighting.isQualifying(): Boolean =
        !isBonded &&
            category in FOLLOWER_CATEGORIES &&
            locationAccuracyMeters in 0f..MAX_LOCATION_ACCURACY_M &&
            isValidLocation(userLat, userLon)

    private fun temporalBand(first: Instant, timestamp: Instant, durationMs: Long): Int {
        if (durationMs <= 0) return 0
        val elapsedMs = Duration.between(first, timestamp).toMillis().coerceIn(0, durationMs)
        return ((elapsedMs * 3) / durationMs).toInt().coerceIn(0, 2)
    }

    private fun maxLocationSpanMeters(sightings: List<Sighting>): Double {
        var maxDistance = 0.0
        sightings.forEachIndexed { index, first ->
            for (second in sightings.drop(index + 1)) {
                maxDistance = maxOf(maxDistance, distanceMeters(first, second))
            }
        }
        return maxDistance
    }

    private fun isValidLocation(latitude: Double, longitude: Double): Boolean {
        if (latitude == 0.0 && longitude == 0.0) return false
        return latitude in -90.0..90.0 && longitude in -180.0..180.0
    }

    private fun distanceMeters(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val earthRadiusM = 6_371_000.0
        val dLat = Math.toRadians(lat2 - lat1)
        val dLon = Math.toRadians(lon2 - lon1)
        val rLat1 = Math.toRadians(lat1)
        val rLat2 = Math.toRadians(lat2)
        val a = sin(dLat / 2) * sin(dLat / 2) +
            cos(rLat1) * cos(rLat2) * sin(dLon / 2) * sin(dLon / 2)
        val c = 2 * atan2(sqrt(a), sqrt(1 - a))
        return earthRadiusM * c
    }

    private fun distanceMeters(first: Sighting, second: Sighting): Double =
        distanceMeters(first.userLat, first.userLon, second.userLat, second.userLon)

    private fun safeLogInfo(tag: String, message: String) {
        try {
            Log.i(tag, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }

    private fun safeLogWarning(message: String) {
        try {
            Log.w(TAG, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }

}

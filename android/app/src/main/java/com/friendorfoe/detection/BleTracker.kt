package com.friendorfoe.detection

import android.location.Location
import android.util.Log
import java.time.Duration
import java.time.Instant
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.abs

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

        /** Minimum time a device must be seen to be considered "following" */
        private const val FOLLOW_MIN_DURATION_MS = 120_000L // 2 minutes

        /** Minimum user movement (meters) before checking for followers */
        private const val FOLLOW_MIN_MOVEMENT_M = 50.0

        /** Maximum sightings to keep per device */
        private const val MAX_SIGHTINGS = 100

        /** How long to keep a device in tracking history */
        private const val DEVICE_HISTORY_TTL_MS = 600_000L // 10 minutes

        /** RSSI samples needed for a direction estimate */
        private const val MIN_DIRECTION_SAMPLES = 8
    }

    /** A single BLE sighting with location context */
    data class Sighting(
        val rssi: Int,
        val userLat: Double,
        val userLon: Double,
        val compassBearing: Float, // user's phone compass heading at time of sighting
        val timestamp: Instant
    )

    /** Tracked BLE device with sighting history */
    data class TrackedDevice(
        val mac: String,
        val deviceName: String?,
        val deviceType: String?,
        val manufacturer: String?,
        val hasCamera: Boolean,
        val sightings: MutableList<Sighting> = mutableListOf(),
        var firstSeen: Instant = Instant.now(),
        var lastSeen: Instant = Instant.now(),
        var peakRssi: Int = -100,
        var isFollowing: Boolean = false,
        var isStalker: Boolean = false
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

    /** Alert for a device that appears to be following the user */
    data class StalkerAlert(
        val device: TrackedDevice,
        val reason: String, // "following", "lingering", "reappeared"
        val threatLevel: Int // 1=low, 2=medium, 3=high
    )

    // All tracked BLE devices keyed by MAC
    private val trackedDevices = java.util.concurrent.ConcurrentHashMap<String, TrackedDevice>()

    // User location history for movement detection
    private val userLocations = mutableListOf<Pair<Location, Instant>>()

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
        compassBearing: Float
    ) {
        val now = Instant.now()
        val device = trackedDevices.getOrPut(mac) {
            TrackedDevice(
                mac = mac,
                deviceName = deviceName,
                deviceType = deviceType,
                manufacturer = manufacturer,
                hasCamera = hasCamera,
                firstSeen = now
            )
        }

        device.lastSeen = now
        if (rssi > device.peakRssi) device.peakRssi = rssi

        // Update name/type if we got better info
        if (deviceName != null && device.deviceName == null) {
            trackedDevices[mac] = device.copy(deviceName = deviceName)
        }

        val sighting = Sighting(rssi, userLat, userLon, compassBearing, now)
        synchronized(device.sightings) {
            device.sightings.add(sighting)
            if (device.sightings.size > MAX_SIGHTINGS) {
                device.sightings.removeAt(0)
            }
        }

        // If we're doing a direction scan on this device, record sample
        if (directionScanTarget == mac) {
            directionSamples.add(compassBearing to rssi)
        }
    }

    /**
     * Update user location for movement tracking.
     */
    fun updateUserLocation(location: Location) {
        synchronized(userLocations) {
            userLocations.add(location to Instant.now())
            // Keep last 5 minutes
            val cutoff = Instant.now().minusSeconds(300)
            userLocations.removeAll { it.second.isBefore(cutoff) }
        }
    }

    /**
     * Check for devices that appear to be following the user.
     * Call periodically (every 30s or so).
     *
     * @return list of stalker alerts
     */
    fun checkForFollowers(): List<StalkerAlert> {
        val now = Instant.now()
        val alerts = mutableListOf<StalkerAlert>()

        // Calculate how far the user has moved
        val userMovement = calculateUserMovement()

        // Prune old devices
        val staleThreshold = now.minusMillis(DEVICE_HISTORY_TTL_MS)
        trackedDevices.entries.removeIf { it.value.lastSeen.isBefore(staleThreshold) }

        for ((_, device) in trackedDevices) {
            val duration = device.durationMs

            if (duration < FOLLOW_MIN_DURATION_MS) continue
            if (device.sightings.size < 3) continue

            // Check if device has been seen at multiple user locations
            val uniqueLocations = synchronized(device.sightings) {
                device.sightings.map { "${it.userLat.format(4)},${it.userLon.format(4)}" }.toSet()
            }

            if (userMovement > FOLLOW_MIN_MOVEMENT_M && uniqueLocations.size > 1) {
                // Device seen at multiple locations while user moved — following
                device.isFollowing = true
                device.isStalker = true
                val threatLevel = when {
                    device.hasCamera && duration > 300_000 -> 3 // camera device following 5+ min
                    duration > 300_000 -> 2 // any device following 5+ min
                    else -> 1
                }
                alerts.add(StalkerAlert(device, "following", threatLevel))
                Log.w(TAG, "STALKER ALERT: ${device.deviceType ?: device.mac} following for ${duration / 1000}s across ${uniqueLocations.size} locations")
            } else if (userMovement < 20.0 && duration > FOLLOW_MIN_DURATION_MS) {
                // User stationary, device lingering nearby
                val threatLevel = if (device.hasCamera) 2 else 1
                alerts.add(StalkerAlert(device, "lingering", threatLevel))
            }
        }

        return alerts
    }

    /**
     * Start a direction-finding scan for a specific device.
     * User should rotate 360° slowly while holding the phone.
     */
    fun startDirectionScan(mac: String) {
        directionScanTarget = mac
        synchronized(directionSamples) { directionSamples.clear() }
        Log.i(TAG, "Direction scan started for $mac — rotate 360° slowly")
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
            Log.w(TAG, "Direction scan: only ${snapshot.size} samples, need $MIN_DIRECTION_SAMPLES")
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

        Log.i(TAG, "Direction scan complete: bearing=${"%.0f".format(normalizedBearing)}° confidence=${"%.0f".format(confidence * 100)}% peak=${peakRssi}dBm (${snapshot.size} samples)")

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

    private fun calculateUserMovement(): Double {
        synchronized(userLocations) {
            if (userLocations.size < 2) return 0.0
            val first = userLocations.first().first
            val last = userLocations.last().first
            return first.distanceTo(last).toDouble()
        }
    }

    private fun Double.format(digits: Int) = "%.${digits}f".format(this)
}

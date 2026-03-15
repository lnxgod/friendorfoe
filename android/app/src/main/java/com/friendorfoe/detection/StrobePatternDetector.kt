package com.friendorfoe.detection

import android.util.Log
import java.util.concurrent.ConcurrentHashMap

/**
 * Nighttime LED/strobe pattern detector for drone and aircraft anti-collision lights.
 *
 * Operates on grayscale camera frames to detect and classify blinking light sources
 * when ambient light is too low for ML Kit object detection. Drone LEDs are visible
 * at 500m-3km at night — much farther than daytime visual detection.
 *
 * Detection pipeline:
 * 1. Measure ambient light from frame histogram — activate in dark conditions
 * 2. Threshold for bright point sources (hot pixels)
 * 3. Track each bright point across frames
 * 4. Measure flash frequency via autocorrelation over 2-3 second windows
 * 5. Classify: drone LEDs (1-3 Hz), aircraft strobes (0.7-1.7 Hz), static, stars
 * 6. Require angular motion to distinguish from fixed towers/buildings
 *
 * Typical flash rates:
 * - FAA drone anti-collision: 1-3 Hz (rapid blink)
 * - Aircraft strobe: ~1 Hz (bright flash)
 * - Aircraft beacon: 0.7-1.7 Hz (rotating red)
 * - Cell tower: steady or very slow (~0.5 Hz)
 * - Stars: steady (scintillation is random, not periodic)
 */
class StrobePatternDetector {

    companion object {
        private const val TAG = "StrobeDetector"

        /** Ambient brightness threshold — below this, switch to strobe mode */
        const val DARK_THRESHOLD = 40f

        /** Minimum brightness for a pixel to be a "bright point" (0-255 scale) */
        private const val BRIGHT_THRESHOLD = 200

        /** Minimum area (pixels) for a bright blob to be considered */
        private const val MIN_BLOB_AREA = 3

        /** Maximum area (pixels) — larger blobs are reflections/headlights */
        private const val MAX_BLOB_AREA = 500

        /** Maximum number of tracked points (limit computation) */
        private const val MAX_TRACKED_POINTS = 20

        /** Frames of history to keep for frequency measurement (~3 sec at 30fps) */
        internal const val HISTORY_FRAMES = 90

        /** Minimum frames before attempting frequency classification */
        private const val MIN_FRAMES_FOR_CLASSIFICATION = 30

        /** Angular motion threshold (normalized coords per frame) to count as moving */
        internal const val MOTION_THRESHOLD = 0.002f

        /** Merge distance: bright points within this distance (normalized) are same source */
        private const val MERGE_DISTANCE = 0.04f
    }

    /** Tracked light sources keyed by a synthetic tracking ID */
    private val trackedLights = ConcurrentHashMap<Int, TrackedLight>()

    private var nextTrackingId = 0

    /**
     * Process a grayscale frame and return strobe detection results.
     *
     * @param grayscale Frame data as byte array (one byte per pixel, 0-255)
     * @param width Frame width in pixels
     * @param height Frame height in pixels
     * @param timestampMs Frame timestamp
     * @return List of detected strobe patterns with classification
     */
    fun processFrame(
        grayscale: ByteArray,
        width: Int,
        height: Int,
        timestampMs: Long,
        devicePitchDegrees: Float = 0f
    ): List<StrobeDetection> {
        // Phone pointing below horizon — all bright points are ground lights
        if (devicePitchDegrees < -5f) return emptyList()

        // Find bright points in this frame
        val brightPoints = findBrightPoints(grayscale, width, height)

        // Match bright points to existing tracks or create new ones
        updateTracks(brightPoints, width, height, timestampMs)

        // Classify tracks that have enough history
        val results = mutableListOf<StrobeDetection>()
        val expiredIds = mutableListOf<Int>()

        for ((id, track) in trackedLights) {
            // Expire tracks not seen recently
            if (track.framesSinceLastSeen > 30) {
                expiredIds.add(id)
                continue
            }

            if (track.history.size >= MIN_FRAMES_FOR_CLASSIFICATION) {
                val classification = classifyTrack(track, devicePitchDegrees)
                if (classification != StrobeClassification.STATIC &&
                    classification != StrobeClassification.NOISE) {
                    results.add(
                        StrobeDetection(
                            trackingId = id,
                            centerX = track.lastX,
                            centerY = track.lastY,
                            flashFrequencyHz = track.measuredFrequency,
                            classification = classification,
                            confidence = track.confidence,
                            hasAngularMotion = track.hasMotion
                        )
                    )
                }
            }
        }

        expiredIds.forEach { trackedLights.remove(it) }

        if (results.isNotEmpty()) {
            Log.d(TAG, "Strobe detections: ${results.size} " +
                    "(${results.count { it.classification == StrobeClassification.DRONE_LED }} drone, " +
                    "${results.count { it.classification == StrobeClassification.AIRCRAFT_STROBE }} aircraft)")
        }

        return results
    }

    /**
     * Measure ambient brightness from frame histogram.
     *
     * @return Mean brightness (0-255). Below [DARK_THRESHOLD] indicates nighttime.
     */
    fun measureAmbientBrightness(grayscale: ByteArray): Float {
        if (grayscale.isEmpty()) return 255f
        var sum = 0L
        for (b in grayscale) {
            sum += (b.toInt() and 0xFF)
        }
        return sum.toFloat() / grayscale.size
    }

    /** Reset all tracking state. */
    fun reset() {
        trackedLights.clear()
        nextTrackingId = 0
    }

    /**
     * Find bright point sources in a grayscale frame.
     * Uses simple connected-component analysis on thresholded pixels.
     */
    private fun findBrightPoints(
        grayscale: ByteArray,
        width: Int,
        height: Int
    ): List<BrightPoint> {
        val points = mutableListOf<BrightPoint>()
        val visited = BooleanArray(width * height)

        for (y in 1 until height - 1) {
            for (x in 1 until width - 1) {
                val idx = y * width + x
                if (visited[idx]) continue
                val pixel = grayscale[idx].toInt() and 0xFF
                if (pixel < BRIGHT_THRESHOLD) continue

                // Flood-fill to find connected bright region
                var sumX = 0L
                var sumY = 0L
                var count = 0
                var peakBrightness = 0

                val stack = ArrayDeque<Int>()
                stack.addLast(idx)
                visited[idx] = true

                while (stack.isNotEmpty() && count < MAX_BLOB_AREA + 1) {
                    val i = stack.removeLast()
                    val px = i % width
                    val py = i / width

                    sumX += px
                    sumY += py
                    count++
                    val brightness = grayscale[i].toInt() and 0xFF
                    if (brightness > peakBrightness) peakBrightness = brightness

                    // Check 4-connected neighbors
                    for (neighbor in intArrayOf(i - 1, i + 1, i - width, i + width)) {
                        if (neighbor < 0 || neighbor >= grayscale.size) continue
                        if (visited[neighbor]) continue
                        if ((grayscale[neighbor].toInt() and 0xFF) >= BRIGHT_THRESHOLD) {
                            visited[neighbor] = true
                            stack.addLast(neighbor)
                        }
                    }
                }

                if (count in MIN_BLOB_AREA..MAX_BLOB_AREA) {
                    points.add(
                        BrightPoint(
                            x = (sumX.toFloat() / count) / width,
                            y = (sumY.toFloat() / count) / height,
                            area = count,
                            peakBrightness = peakBrightness
                        )
                    )
                }

                if (points.size >= MAX_TRACKED_POINTS) return points
            }
        }

        return points
    }

    /**
     * Update tracked light sources with new frame observations.
     * Nearest-neighbor matching with [MERGE_DISTANCE] threshold.
     */
    private fun updateTracks(
        brightPoints: List<BrightPoint>,
        width: Int,
        height: Int,
        timestampMs: Long
    ) {
        val matched = mutableSetOf<Int>()

        for (point in brightPoints) {
            var bestId: Int? = null
            var bestDist = MERGE_DISTANCE

            for ((id, track) in trackedLights) {
                if (id in matched) continue
                val dx = point.x - track.lastX
                val dy = point.y - track.lastY
                val dist = kotlin.math.sqrt(dx * dx + dy * dy)
                if (dist < bestDist) {
                    bestDist = dist
                    bestId = id
                }
            }

            if (bestId != null) {
                // Update existing track
                matched.add(bestId)
                val track = trackedLights[bestId]!!
                track.update(point.x, point.y, point.peakBrightness, timestampMs)
            } else if (trackedLights.size < MAX_TRACKED_POINTS) {
                // Create new track
                val id = nextTrackingId++
                trackedLights[id] = TrackedLight(point.x, point.y, point.peakBrightness, timestampMs)
            }
        }

        // Mark unmatched tracks as "not seen this frame"
        for ((id, track) in trackedLights) {
            if (id !in matched) {
                track.recordAbsence()
            }
        }
    }

    /**
     * Classify a tracked light source based on its flash pattern and motion.
     *
     * Uses autocorrelation on the brightness history to extract flash frequency.
     */
    private fun classifyTrack(track: TrackedLight, devicePitchDegrees: Float = 0f): StrobeClassification {
        // Below horizon — any detected strobe is a ground light
        if (devicePitchDegrees < -5f) return StrobeClassification.STATIC

        val freq = measureFlashFrequency(track)
        track.measuredFrequency = freq

        val hasMotion = track.hasMotion

        return when {
            // No periodic flashing — static light or star
            freq == null || freq < 0.3f -> {
                track.confidence = 0f
                if (hasMotion) StrobeClassification.NOISE else StrobeClassification.STATIC
            }
            // Drone LED range: 1-3 Hz
            freq in 1.0f..3.5f -> {
                track.confidence = if (hasMotion) 0.7f else 0.4f
                StrobeClassification.DRONE_LED
            }
            // Aircraft strobe range: 0.5-1.7 Hz
            freq in 0.5f..1.7f -> {
                track.confidence = if (hasMotion) 0.6f else 0.3f
                StrobeClassification.AIRCRAFT_STROBE
            }
            // Slow blink (cell tower, etc.)
            freq in 0.3f..0.5f -> {
                track.confidence = 0.1f
                StrobeClassification.STATIC
            }
            else -> {
                track.confidence = 0f
                StrobeClassification.NOISE
            }
        }
    }

    /**
     * Measure flash frequency using autocorrelation on brightness history.
     *
     * @return Flash frequency in Hz, or null if no periodic pattern found
     */
    private fun measureFlashFrequency(track: TrackedLight): Float? {
        val history = track.history
        if (history.size < MIN_FRAMES_FOR_CLASSIFICATION) return null

        // Use brightness values and timestamps
        val brightnesses = history.map { it.brightness.toFloat() }
        val timestamps = history.map { it.timestampMs }

        // Compute mean brightness
        val mean = brightnesses.average().toFloat()

        // Subtract mean (zero-center)
        val centered = brightnesses.map { it - mean }

        // Autocorrelation for lags corresponding to 0.3-5 Hz
        // At 30fps: lag of 6 frames = 5 Hz, lag of 100 frames = 0.3 Hz
        val avgFrameInterval = if (timestamps.size >= 2) {
            (timestamps.last() - timestamps.first()).toFloat() / (timestamps.size - 1)
        } else {
            33f // assume ~30fps
        }

        val minLag = maxOf(3, (1000f / (5f * avgFrameInterval)).toInt())   // 5 Hz max
        val maxLag = minOf(history.size / 2, (1000f / (0.3f * avgFrameInterval)).toInt())  // 0.3 Hz min

        if (maxLag <= minLag) return null

        var bestLag = 0
        var bestCorrelation = 0f
        var zeroLagCorrelation = 0f

        // Compute zero-lag autocorrelation (normalization reference)
        for (i in centered.indices) {
            zeroLagCorrelation += centered[i] * centered[i]
        }
        if (zeroLagCorrelation < 0.001f) return null // no variation

        for (lag in minLag..maxLag) {
            var correlation = 0f
            for (i in 0 until centered.size - lag) {
                correlation += centered[i] * centered[i + lag]
            }
            correlation /= zeroLagCorrelation

            if (correlation > bestCorrelation) {
                bestCorrelation = correlation
                bestLag = lag
            }
        }

        // Require minimum correlation strength for periodicity
        if (bestCorrelation < 0.3f) return null

        // Convert lag to frequency
        val periodMs = bestLag * avgFrameInterval
        return if (periodMs > 0) 1000f / periodMs else null
    }
}

/** A bright point found in a single frame. */
private data class BrightPoint(
    val x: Float,       // Normalized X (0-1)
    val y: Float,       // Normalized Y (0-1)
    val area: Int,       // Pixel count
    val peakBrightness: Int  // 0-255
)

/** Frame observation for a tracked light source. */
private data class LightFrame(
    val x: Float,
    val y: Float,
    val brightness: Int,
    val timestampMs: Long
)

/** A light source tracked across multiple frames. */
private class TrackedLight(
    initialX: Float,
    initialY: Float,
    initialBrightness: Int,
    timestampMs: Long
) {
    var lastX: Float = initialX
        private set
    var lastY: Float = initialY
        private set
    var framesSinceLastSeen: Int = 0
        private set
    var measuredFrequency: Float? = null
    var confidence: Float = 0f

    val history = ArrayDeque<LightFrame>(StrobePatternDetector.HISTORY_FRAMES)

    /** True if the light source has shown angular motion */
    val hasMotion: Boolean
        get() {
            if (history.size < 10) return false
            val first = history.first()
            val last = history.last()
            val dx = last.x - first.x
            val dy = last.y - first.y
            return kotlin.math.sqrt(dx * dx + dy * dy) > StrobePatternDetector.MOTION_THRESHOLD * 10
        }

    init {
        history.addLast(LightFrame(initialX, initialY, initialBrightness, timestampMs))
    }

    fun update(x: Float, y: Float, brightness: Int, timestampMs: Long) {
        lastX = x
        lastY = y
        framesSinceLastSeen = 0
        history.addLast(LightFrame(x, y, brightness, timestampMs))
        while (history.size > StrobePatternDetector.HISTORY_FRAMES) {
            history.removeFirst()
        }
    }

    fun recordAbsence() {
        framesSinceLastSeen++
        // Record a zero-brightness frame for frequency analysis (light is off)
        if (history.isNotEmpty()) {
            val lastTs = history.last().timestampMs
            history.addLast(LightFrame(lastX, lastY, 0, lastTs + 33))
            while (history.size > StrobePatternDetector.HISTORY_FRAMES) {
                history.removeFirst()
            }
        }
    }

    companion object // for accessing constants from inner scope
}

/** Strobe classification for a tracked light source. */
enum class StrobeClassification {
    /** Blinking at 1-3 Hz — typical of drone anti-collision LED */
    DRONE_LED,
    /** Blinking at 0.7-1.7 Hz — typical of aircraft strobe/beacon */
    AIRCRAFT_STROBE,
    /** No periodic pattern — fixed light source */
    STATIC,
    /** Noise / transient — not a real light source */
    NOISE
}

/**
 * A single strobe detection result.
 *
 * @property trackingId Unique ID for tracking across frames
 * @property centerX Normalized center X (0-1)
 * @property centerY Normalized center Y (0-1)
 * @property flashFrequencyHz Measured flash frequency in Hz, null if aperiodic
 * @property classification Strobe type classification
 * @property confidence Detection confidence (0-1)
 * @property hasAngularMotion True if the source is moving across the field of view
 */
data class StrobeDetection(
    val trackingId: Int,
    val centerX: Float,
    val centerY: Float,
    val flashFrequencyHz: Float?,
    val classification: StrobeClassification,
    val confidence: Float,
    val hasAngularMotion: Boolean
)

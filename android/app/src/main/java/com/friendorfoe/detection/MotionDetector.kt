package com.friendorfoe.detection

import android.util.Log
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * IMU-stabilized temporal differencing for detecting moving objects in the sky.
 *
 * Detects radio-silent objects (drones without Remote ID) by finding pixels that
 * change between frames after compensating for camera motion using gyroscope data.
 *
 * Pipeline:
 * 1. Stabilize: shift previous frame by IMU-derived pixel offset
 * 2. Difference: absolute pixel difference between stabilized previous and current
 * 3. Threshold: pixels with large change are "motion pixels"
 * 4. Cluster: group nearby motion pixels into blob candidates
 * 5. Filter: reject blobs below horizon, too large (clouds), or too small (noise)
 * 6. Track: require blob persistence across 3+ consecutive frames
 *
 * Works on the Y (luma) plane only — no JPEG conversion needed.
 */
class MotionDetector {

    companion object {
        private const val TAG = "MotionDetect"

        /** Minimum pixel difference to count as motion */
        private const val DIFF_THRESHOLD = 30

        /** Minimum blob area (pixels) to consider as candidate */
        private const val MIN_BLOB_AREA = 4

        /** Maximum blob area (pixels) — larger is likely a cloud or artifact */
        private const val MAX_BLOB_AREA = 2000

        /** Frames a blob must persist to be reported as a detection */
        private const val MIN_PERSISTENCE_FRAMES = 3

        /** Maximum distance (normalized) to match blobs across frames */
        private const val MATCH_DISTANCE = 0.05f

        /** Downscale factor for processing (4 = process at 1/4 resolution) */
        private const val DOWNSCALE = 4

        /** Fraction of frame height considered "sky" (above this line) */
        private const val DEFAULT_HORIZON_FRACTION = 0.65f
    }

    /** A detected motion blob in normalized coordinates */
    data class MotionBlob(
        val centerX: Float,     // Normalized 0-1
        val centerY: Float,     // Normalized 0-1
        val width: Float,       // Normalized
        val height: Float,      // Normalized
        val intensity: Float,   // Average motion intensity
        val persistenceFrames: Int  // How many consecutive frames this blob has appeared
    )

    // Previous frame luma (downscaled)
    private var prevLuma: ByteArray? = null
    private var prevWidth = 0
    private var prevHeight = 0

    // Blob tracking across frames
    private val trackedBlobs = mutableListOf<TrackedBlob>()

    private data class TrackedBlob(
        var centerX: Float,
        var centerY: Float,
        var width: Float,
        var height: Float,
        var intensity: Float,
        var frames: Int,
        var missedFrames: Int
    )

    /**
     * Process a new frame and return detected motion blobs.
     *
     * @param luma Raw Y plane bytes (full resolution)
     * @param width Frame width
     * @param height Frame height
     * @param deltaYawDeg Gyroscope yaw change since last frame (degrees)
     * @param deltaPitchDeg Gyroscope pitch change since last frame (degrees)
     * @param hFovDeg Horizontal field of view (degrees)
     * @param vFovDeg Vertical field of view (degrees)
     * @param horizonFraction Where the horizon is (0=top, 1=bottom)
     * @return List of motion blobs that have persisted across multiple frames
     */
    fun processFrame(
        luma: ByteArray,
        width: Int,
        height: Int,
        deltaYawDeg: Float,
        deltaPitchDeg: Float,
        hFovDeg: Float,
        vFovDeg: Float,
        horizonFraction: Float = DEFAULT_HORIZON_FRACTION
    ): List<MotionBlob> {
        // Downscale for performance
        val dsW = width / DOWNSCALE
        val dsH = height / DOWNSCALE
        val dsLuma = downscale(luma, width, height, dsW, dsH)

        val prev = prevLuma
        if (prev == null || prevWidth != dsW || prevHeight != dsH) {
            prevLuma = dsLuma
            prevWidth = dsW
            prevHeight = dsH
            return emptyList()
        }

        // Calculate pixel shift from IMU
        val shiftX = (deltaYawDeg / hFovDeg * dsW).roundToInt()
        val shiftY = -(deltaPitchDeg / vFovDeg * dsH).roundToInt() // Negative: pitch up = shift down

        // Compute stabilized difference
        val diffMap = computeStabilizedDiff(prev, dsLuma, dsW, dsH, shiftX, shiftY)

        // Find blobs in sky region
        val horizonY = (horizonFraction * dsH).toInt()
        val rawBlobs = findBlobs(diffMap, dsW, dsH, horizonY)

        // Update tracking
        updateTracking(rawBlobs)

        // Store current frame for next iteration
        prevLuma = dsLuma
        prevWidth = dsW
        prevHeight = dsH

        // Return blobs that have persisted long enough
        return trackedBlobs
            .filter { it.frames >= MIN_PERSISTENCE_FRAMES && it.missedFrames == 0 }
            .map { blob ->
                MotionBlob(
                    centerX = blob.centerX,
                    centerY = blob.centerY,
                    width = blob.width,
                    height = blob.height,
                    intensity = blob.intensity,
                    persistenceFrames = blob.frames
                )
            }
    }

    private fun downscale(src: ByteArray, srcW: Int, srcH: Int, dstW: Int, dstH: Int): ByteArray {
        val dst = ByteArray(dstW * dstH)
        for (dy in 0 until dstH) {
            val sy = dy * DOWNSCALE
            for (dx in 0 until dstW) {
                val sx = dx * DOWNSCALE
                dst[dy * dstW + dx] = src[sy * srcW + sx]
            }
        }
        return dst
    }

    private fun computeStabilizedDiff(
        prev: ByteArray, curr: ByteArray,
        w: Int, h: Int, shiftX: Int, shiftY: Int
    ): IntArray {
        val diff = IntArray(w * h)
        for (y in 0 until h) {
            val prevY = y + shiftY
            if (prevY < 0 || prevY >= h) continue
            for (x in 0 until w) {
                val prevX = x + shiftX
                if (prevX < 0 || prevX >= w) continue

                val currVal = curr[y * w + x].toInt() and 0xFF
                val prevVal = prev[prevY * w + prevX].toInt() and 0xFF
                diff[y * w + x] = abs(currVal - prevVal)
            }
        }
        return diff
    }

    private data class RawBlob(
        val centerX: Float, val centerY: Float,
        val width: Float, val height: Float,
        val intensity: Float, val area: Int
    )

    private fun findBlobs(diff: IntArray, w: Int, h: Int, horizonY: Int): List<RawBlob> {
        // Simple connected-component labeling on thresholded diff
        val visited = BooleanArray(w * h)
        val blobs = mutableListOf<RawBlob>()

        for (y in 0 until horizonY) { // Only above horizon
            for (x in 0 until w) {
                val idx = y * w + x
                if (visited[idx] || diff[idx] < DIFF_THRESHOLD) continue

                // Flood-fill to find connected component
                var sumX = 0L; var sumY = 0L; var sumIntensity = 0L; var area = 0
                var minX = x; var maxX = x; var minY = y; var maxY = y

                val stack = ArrayDeque<Int>()
                stack.addLast(idx)
                visited[idx] = true

                while (stack.isNotEmpty()) {
                    val i = stack.removeLast()
                    val cx = i % w
                    val cy = i / w

                    sumX += cx; sumY += cy
                    sumIntensity += diff[i]
                    area++
                    if (cx < minX) minX = cx
                    if (cx > maxX) maxX = cx
                    if (cy < minY) minY = cy
                    if (cy > maxY) maxY = cy

                    // Check 4-connected neighbors
                    for (ni in intArrayOf(i - 1, i + 1, i - w, i + w)) {
                        if (ni < 0 || ni >= w * h || visited[ni]) continue
                        val nx = ni % w; val ny = ni / w
                        if (abs(nx - cx) > 1 || abs(ny - cy) > 1) continue // Wrap guard
                        if (diff[ni] >= DIFF_THRESHOLD) {
                            visited[ni] = true
                            stack.addLast(ni)
                        }
                    }
                }

                if (area in MIN_BLOB_AREA..MAX_BLOB_AREA) {
                    blobs.add(RawBlob(
                        centerX = (sumX.toFloat() / area) / w,
                        centerY = (sumY.toFloat() / area) / h,
                        width = (maxX - minX + 1).toFloat() / w,
                        height = (maxY - minY + 1).toFloat() / h,
                        intensity = sumIntensity.toFloat() / area,
                        area = area
                    ))
                }
            }
        }
        return blobs
    }

    private fun updateTracking(rawBlobs: List<RawBlob>) {
        val matched = BooleanArray(rawBlobs.size)

        // Match existing tracked blobs to new raw blobs
        for (tracked in trackedBlobs) {
            var bestIdx = -1
            var bestDist = Float.MAX_VALUE

            for (i in rawBlobs.indices) {
                if (matched[i]) continue
                val dx = rawBlobs[i].centerX - tracked.centerX
                val dy = rawBlobs[i].centerY - tracked.centerY
                val dist = dx * dx + dy * dy
                if (dist < bestDist) {
                    bestDist = dist
                    bestIdx = i
                }
            }

            if (bestIdx >= 0 && bestDist < MATCH_DISTANCE * MATCH_DISTANCE) {
                matched[bestIdx] = true
                val raw = rawBlobs[bestIdx]
                tracked.centerX = raw.centerX
                tracked.centerY = raw.centerY
                tracked.width = raw.width
                tracked.height = raw.height
                tracked.intensity = raw.intensity
                tracked.frames++
                tracked.missedFrames = 0
            } else {
                tracked.missedFrames++
            }
        }

        // Remove blobs that have been missing too long
        trackedBlobs.removeAll { it.missedFrames > 3 }

        // Add new unmatched blobs
        for (i in rawBlobs.indices) {
            if (!matched[i]) {
                val raw = rawBlobs[i]
                trackedBlobs.add(TrackedBlob(
                    centerX = raw.centerX,
                    centerY = raw.centerY,
                    width = raw.width,
                    height = raw.height,
                    intensity = raw.intensity,
                    frames = 1,
                    missedFrames = 0
                ))
            }
        }
    }

    fun reset() {
        prevLuma = null
        trackedBlobs.clear()
    }
}

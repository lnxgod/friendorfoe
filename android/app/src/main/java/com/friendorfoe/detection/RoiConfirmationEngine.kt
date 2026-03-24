package com.friendorfoe.detection

import android.graphics.Bitmap
import android.graphics.Rect
import android.os.SystemClock
import android.util.Log
import androidx.camera.core.ImageProxy
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * Result of ROI confirmation for a single sky object.
 *
 * @property skyObjectId The sky object ID this confirmation corresponds to
 * @property confirmed Whether a non-sky visual anomaly was detected at the predicted position
 * @property score Normalized confirmation score (0 = no anomaly, >1 = strong confirmation)
 * @property brightnessDiff Absolute brightness difference between center and edge of crop
 * @property edgeDensity Fraction of pixels with significant Sobel gradient in center
 */
data class RoiConfirmation(
    val skyObjectId: String,
    val confirmed: Boolean,
    val score: Float,
    val brightnessDiff: Float,
    val edgeDensity: Float
)

/**
 * ROI-based visual confirmation engine.
 *
 * For each in-view aircraft with a predicted screen position (from ADS-B + trajectory prediction),
 * crops a small region from the camera frame, analyzes brightness and edge patterns, and determines
 * whether there's a visual anomaly (likely an aircraft) at that position.
 *
 * This supplements ML Kit's full-frame detection by:
 * - Focusing analysis on where aircraft SHOULD be (from radio data)
 * - Working at higher effective resolution (small crop = aircraft fills more pixels)
 * - Using simple signal processing instead of ML (faster, no false positives from clouds)
 *
 * Runs at ~5Hz on a background thread, independent of ML Kit's 30fps pipeline.
 */
class RoiConfirmationEngine(
    private val scope: CoroutineScope,
    private val onResults: (List<RoiConfirmation>) -> Unit
) {
    companion object {
        private const val TAG = "RoiConfirm"
        private const val TARGET_HZ = 5.0
        private const val MAX_TARGETS = 8

        // Crop size based on distance
        private const val MIN_CROP_PX = 32
        private const val MAX_CROP_PX = 160
        private const val FALLBACK_CROP_PX = 64

        // Confirmation thresholds
        private const val BRIGHTNESS_DIFF_THRESHOLD = 12f
        private const val EDGE_DENSITY_THRESHOLD = 0.045f
        private const val SOBEL_THRESHOLD = 180
    }

    private val running = AtomicBoolean(false)
    private val lastRunNanos = AtomicLong(0L)
    private val intervalNanos = (1_000_000_000.0 / TARGET_HZ).toLong()

    /**
     * Submit a camera frame for ROI confirmation analysis.
     *
     * @param targets List of (skyObjectId, screenX 0-1, screenY 0-1, distanceMeters)
     * @param imageProxy Camera frame (Y plane extracted, then closed immediately)
     */
    fun submitFrame(
        targets: List<Target>,
        imageProxy: ImageProxy
    ) {
        val now = SystemClock.elapsedRealtimeNanos()
        if (now - lastRunNanos.get() < intervalNanos) return
        if (!running.compareAndSet(false, true)) return
        lastRunNanos.set(now)

        // Extract luma plane on caller thread, then release ImageProxy
        val frame = extractLuma(imageProxy) ?: run {
            running.set(false)
            return
        }

        scope.launch(Dispatchers.Default) {
            try {
                val results = analyze(frame, targets)
                onResults(results)
            } catch (e: Exception) {
                Log.w(TAG, "ROI analysis failed", e)
            } finally {
                running.set(false)
            }
        }
    }

    /**
     * Submit a Bitmap frame for ROI confirmation analysis.
     * Used when working with the retained last-frame bitmap.
     */
    fun submitFrame(targets: List<Target>, bitmap: Bitmap) {
        if (bitmap.isRecycled) return  // Guard against stale bitmap references
        val now = SystemClock.elapsedRealtimeNanos()
        if (now - lastRunNanos.get() < intervalNanos) return
        if (!running.compareAndSet(false, true)) return
        lastRunNanos.set(now)

        val frame = extractLumaFromBitmap(bitmap) ?: run {
            running.set(false)
            return
        }

        scope.launch(Dispatchers.Default) {
            try {
                val results = analyze(frame, targets)
                onResults(results)
            } catch (e: Exception) {
                Log.w(TAG, "ROI analysis failed (bitmap)", e)
            } finally {
                running.set(false)
            }
        }
    }

    /**
     * Target for ROI confirmation.
     */
    data class Target(
        val skyObjectId: String,
        val screenX: Float,       // Normalized 0-1
        val screenY: Float,       // Normalized 0-1
        val distanceMeters: Double
    )

    private fun analyze(frame: LumaFrame, targets: List<Target>): List<RoiConfirmation> {
        val sorted = if (targets.size > MAX_TARGETS) {
            targets.sortedBy { it.distanceMeters }.take(MAX_TARGETS)
        } else targets

        return sorted.map { target ->
            // Convert normalized screen coords to pixel coords
            val px = (target.screenX * frame.width).roundToInt()
            val py = (target.screenY * frame.height).roundToInt()
            val cropSize = cropSizePx(target.distanceMeters)
            val crop = computeCrop(px, py, cropSize, frame.width, frame.height)

            if (crop == null) {
                return@map RoiConfirmation(target.skyObjectId, false, 0f, 0f, 0f)
            }

            val analysis = analyzeCrop(frame, crop)
            val brightnessPass = analysis.brightnessDiff >= BRIGHTNESS_DIFF_THRESHOLD
            val edgePass = analysis.edgeDensity >= EDGE_DENSITY_THRESHOLD
            val confirmed = brightnessPass || edgePass
            val score = max(
                analysis.brightnessDiff / BRIGHTNESS_DIFF_THRESHOLD,
                analysis.edgeDensity / EDGE_DENSITY_THRESHOLD
            )

            RoiConfirmation(
                skyObjectId = target.skyObjectId,
                confirmed = confirmed,
                score = score,
                brightnessDiff = analysis.brightnessDiff,
                edgeDensity = analysis.edgeDensity
            )
        }
    }

    private data class CropAnalysis(val brightnessDiff: Float, val edgeDensity: Float)

    private fun analyzeCrop(frame: LumaFrame, crop: Rect): CropAnalysis {
        val w = frame.width
        val luma = frame.luma

        // Define center region (inner 50%)
        val insetX = max(1, crop.width() / 4)
        val insetY = max(1, crop.height() / 4)
        val center = Rect(
            crop.left + insetX, crop.top + insetY,
            crop.right - insetX, crop.bottom - insetY
        )
        if (center.width() < 3 || center.height() < 3) return CropAnalysis(0f, 0f)

        // Compute brightness difference: center mean vs edge mean
        var cropSum = 0L; var cropCount = 0
        var centerSum = 0L; var centerCount = 0

        for (y in crop.top until crop.bottom) {
            val row = y * w
            for (x in crop.left until crop.right) {
                val v = luma[row + x].toInt() and 0xFF
                cropSum += v; cropCount++
                if (x in center.left until center.right && y in center.top until center.bottom) {
                    centerSum += v; centerCount++
                }
            }
        }

        val centerMean = if (centerCount > 0) centerSum.toFloat() / centerCount else 0f
        val edgeCount = cropCount - centerCount
        val edgeMean = if (edgeCount > 0) (cropSum - centerSum).toFloat() / edgeCount else centerMean
        val brightnessDiff = abs(centerMean - edgeMean)

        // Count Sobel edge pixels in center region
        val edgePixels = countEdges(frame, center)
        val sampleCount = max(1, (center.width() - 2) * (center.height() - 2))
        val edgeDensity = edgePixels.toFloat() / sampleCount

        return CropAnalysis(brightnessDiff, edgeDensity)
    }

    private fun countEdges(frame: LumaFrame, rect: Rect): Int {
        val w = frame.width
        val luma = frame.luma
        var count = 0

        for (y in (rect.top + 1) until (rect.bottom - 1)) {
            val row = y * w
            for (x in (rect.left + 1) until (rect.right - 1)) {
                val idx = row + x
                // Sobel 3x3 gradient magnitude (approximation)
                val p00 = luma[idx - w - 1].toInt() and 0xFF
                val p01 = luma[idx - w].toInt() and 0xFF
                val p02 = luma[idx - w + 1].toInt() and 0xFF
                val p10 = luma[idx - 1].toInt() and 0xFF
                val p12 = luma[idx + 1].toInt() and 0xFF
                val p20 = luma[idx + w - 1].toInt() and 0xFF
                val p21 = luma[idx + w].toInt() and 0xFF
                val p22 = luma[idx + w + 1].toInt() and 0xFF

                val gx = (p02 + 2 * p12 + p22) - (p00 + 2 * p10 + p20)
                val gy = (p20 + 2 * p21 + p22) - (p00 + 2 * p01 + p02)
                if (abs(gx) + abs(gy) >= SOBEL_THRESHOLD) count++
            }
        }
        return count
    }

    private fun cropSizePx(distanceMeters: Double): Int {
        if (distanceMeters <= 0) return FALLBACK_CROP_PX
        val d = max(distanceMeters, 300.0)
        return ((1000.0 / d) * 80.0).roundToInt().coerceIn(MIN_CROP_PX, MAX_CROP_PX)
    }

    private fun computeCrop(cx: Int, cy: Int, size: Int, width: Int, height: Int): Rect? {
        if (size < 6) return null
        val half = size / 2
        var left = cx - half; var top = cy - half
        var right = left + size; var bottom = top + size

        if (left < 1) { right += 1 - left; left = 1 }
        if (top < 1) { bottom += 1 - top; top = 1 }
        if (right > width - 2) { left -= right - (width - 2); right = width - 2 }
        if (bottom > height - 2) { top -= bottom - (height - 2); bottom = height - 2 }

        if (left < 1 || top < 1 || right > width - 2 || bottom > height - 2) return null
        if (right - left < 6 || bottom - top < 6) return null
        return Rect(left, top, right, bottom)
    }

    private fun extractLuma(imageProxy: ImageProxy): LumaFrame? {
        return try {
            val plane = imageProxy.planes.firstOrNull() ?: return null
            val width = imageProxy.width
            val height = imageProxy.height
            val rowStride = plane.rowStride
            val buffer = plane.buffer.duplicate()

            val out = ByteArray(width * height)
            if (plane.pixelStride == 1) {
                for (y in 0 until height) {
                    buffer.position(y * rowStride)
                    buffer.get(out, y * width, width)
                }
            } else {
                var outIdx = 0
                for (y in 0 until height) {
                    var inPos = y * rowStride
                    for (x in 0 until width) {
                        out[outIdx++] = buffer.get(inPos)
                        inPos += plane.pixelStride
                    }
                }
            }
            LumaFrame(width, height, out)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to extract luma plane", e)
            null
        }
    }

    private fun extractLumaFromBitmap(bitmap: Bitmap): LumaFrame? {
        return try {
            val width = bitmap.width
            val height = bitmap.height
            val pixels = IntArray(width * height)
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
            val luma = ByteArray(width * height)
            for (i in pixels.indices) {
                val p = pixels[i]
                val r = (p shr 16) and 0xFF
                val g = (p shr 8) and 0xFF
                val b = p and 0xFF
                luma[i] = (0.299f * r + 0.587f * g + 0.114f * b).roundToInt().toByte()
            }
            LumaFrame(width, height, luma)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to extract luma from bitmap", e)
            null
        }
    }

    private data class LumaFrame(val width: Int, val height: Int, val luma: ByteArray)
}

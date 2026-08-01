package com.friendorfoe.detection

import android.graphics.Bitmap
import android.util.Log
import kotlin.math.abs
import javax.inject.Inject
import javax.inject.Singleton

data class FloatPoint(val x: Float, val y: Float)

/** Finds persistent bright, low-saturation pixels without classifying their source. */
@Singleton
class IrCameraDetector @Inject constructor() {

    companion object {
        private const val TAG = "IrCameraDetector"

        /** Minimum pixel brightness to consider as potential IR (0-255) */
        private const val BRIGHTNESS_THRESHOLD = 200

        /** Minimum cluster size in pixels to report */
        private const val MIN_CLUSTER_PIXELS = 4

        /** Minimum saturation — IR appears as white/purple, not colored */
        private const val MAX_SATURATION = 85 // low saturation = near-white

        /** Grid cell size for spatial clustering */
        private const val GRID_CELL_SIZE = 16

        /** Average scene brightness where IR LEDs wash out into ambient light. */
        private const val ROOM_TOO_BRIGHT_AVERAGE = 96
    }

    data class IrSource(
        val centerPx: FloatPoint,
        val brightness: Int,   // Peak brightness 0-255
        val clusterSize: Int,  // Number of bright pixels in cluster
        val confidence: Float  // 0.0-1.0
    )

    data class FrameAnalysis(
        val sources: List<IrSource>,
        val ambientBrightness: Int,
        val roomTooBright: Boolean,
        val analyzedWidth: Int,
        val analyzedHeight: Int,
    )

    class Session internal constructor(
        private val detector: IrCameraDetector,
    ) {
        private val persistenceMap = mutableMapOf<Int, Int>()

        fun analyzeFrame(bitmap: Bitmap): List<IrSource> =
            analyzeFrameWithEnvironment(bitmap).sources

        fun analyzeFrameWithEnvironment(bitmap: Bitmap): FrameAnalysis =
            detector.analyzeBitmap(bitmap, persistenceMap)

        internal fun analyzePixelsForTest(
            width: Int,
            height: Int,
            pixels: IntArray,
        ): List<IrSource> = detector.analyzePixels(
            width = width,
            height = height,
            pixels = pixels,
            persistenceMap = persistenceMap,
        ).sources

        internal fun analyzePixelsWithEnvironmentForTest(
            width: Int,
            height: Int,
            pixels: IntArray,
        ): FrameAnalysis = detector.analyzePixels(
            width = width,
            height = height,
            pixels = pixels,
            persistenceMap = persistenceMap,
        )

        fun reset() {
            persistenceMap.clear()
        }
    }

    private val defaultSession = Session(this)

    /** A binding generation gets its own persistence history. */
    fun newSession(): Session = Session(this)

    /**
     * Analyze a camera frame for IR LED sources.
     *
     * @param bitmap Camera frame from front-facing camera
     * @return List of detected IR sources, empty if none found
     */
    fun analyzeFrame(bitmap: Bitmap): List<IrSource> {
        return defaultSession.analyzeFrame(bitmap)
    }

    fun analyzeFrameWithEnvironment(bitmap: Bitmap): FrameAnalysis {
        return defaultSession.analyzeFrameWithEnvironment(bitmap)
    }

    private fun analyzeBitmap(
        bitmap: Bitmap,
        persistenceMap: MutableMap<Int, Int>,
    ): FrameAnalysis {
        if (bitmap.isRecycled) {
            return FrameAnalysis(
                sources = emptyList(),
                ambientBrightness = 0,
                roomTooBright = false,
                analyzedWidth = 0,
                analyzedHeight = 0,
            )
        }

        val width = bitmap.width
        val height = bitmap.height
        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
        return analyzePixels(width, height, pixels, persistenceMap)
    }

    internal fun analyzePixelsForTest(width: Int, height: Int, pixels: IntArray): List<IrSource> {
        return defaultSession.analyzePixelsForTest(width, height, pixels)
    }

    internal fun analyzePixelsWithEnvironmentForTest(
        width: Int,
        height: Int,
        pixels: IntArray
    ): FrameAnalysis {
        return defaultSession.analyzePixelsWithEnvironmentForTest(width, height, pixels)
    }

    private fun analyzePixels(
        width: Int,
        height: Int,
        pixels: IntArray,
        persistenceMap: MutableMap<Int, Int>,
    ): FrameAnalysis {
        if (width <= 0 || height <= 0 || pixels.size < width * height) {
            return FrameAnalysis(
                sources = emptyList(),
                ambientBrightness = 0,
                roomTooBright = false,
                analyzedWidth = width.coerceAtLeast(0),
                analyzedHeight = height.coerceAtLeast(0),
            )
        }

        val gridW = ((width + GRID_CELL_SIZE - 1) / GRID_CELL_SIZE).coerceAtLeast(1)
        val gridH = ((height + GRID_CELL_SIZE - 1) / GRID_CELL_SIZE).coerceAtLeast(1)

        // Count bright, low-saturation pixels per grid cell
        val gridCounts = IntArray(gridW * gridH)
        val gridBrightness = IntArray(gridW * gridH)
        val gridXSum = LongArray(gridW * gridH)
        val gridYSum = LongArray(gridW * gridH)
        var brightnessSum = 0L

        for (y in 0 until height) {
            for (x in 0 until width) {
                val pixel = pixels[y * width + x]
                val r = (pixel shr 16) and 0xFF
                val g = (pixel shr 8) and 0xFF
                val b = pixel and 0xFF

                val brightness = maxOf(r, g, b)
                brightnessSum += brightness
                val minC = minOf(r, g, b)
                val saturation = if (brightness > 0) {
                    ((brightness - minC) * 255) / brightness
                } else 0

                if (isPotentialIrPixel(r, g, b, brightness, saturation)) {
                    val gx = (x / GRID_CELL_SIZE).coerceAtMost(gridW - 1)
                    val gy = (y / GRID_CELL_SIZE).coerceAtMost(gridH - 1)
                    val idx = gy * gridW + gx
                    gridCounts[idx]++
                    gridXSum[idx] += x.toLong()
                    gridYSum[idx] += y.toLong()
                    if (brightness > gridBrightness[idx]) {
                        gridBrightness[idx] = brightness
                    }
                }
            }
        }

        val ambientBrightness = (brightnessSum / (width * height)).toInt()
        if (ambientBrightness >= ROOM_TOO_BRIGHT_AVERAGE) {
            persistenceMap.clear()
            return FrameAnalysis(
                sources = emptyList(),
                ambientBrightness = ambientBrightness,
                roomTooBright = true,
                analyzedWidth = width,
                analyzedHeight = height,
            )
        }

        // Find cells with enough bright pixels
        val results = mutableListOf<IrSource>()
        val currentFrameCells = mutableSetOf<Int>()

        for (idx in gridCounts.indices) {
            if (gridCounts[idx] >= MIN_CLUSTER_PIXELS) {
                currentFrameCells.add(idx)

                val persistence = (persistenceMap[idx] ?: 0) + 1
                persistenceMap[idx] = persistence

                // Require 2+ consecutive frames to report (filters noise)
                if (persistence >= 2) {
                    val count = gridCounts[idx].coerceAtLeast(1)
                    val center = FloatPoint(
                        x = gridXSum[idx].toFloat() / count,
                        y = gridYSum[idx].toFloat() / count,
                    )

                    val confidence = when {
                        persistence >= 5 -> 0.95f
                        persistence >= 3 -> 0.80f
                        else -> 0.60f
                    }

                    results.add(IrSource(
                        centerPx = center,
                        brightness = gridBrightness[idx],
                        clusterSize = gridCounts[idx],
                        confidence = confidence
                    ))
                }
            }
        }

        // Clear persistence for cells not seen this frame
        persistenceMap.keys.retainAll(currentFrameCells)

        if (results.isNotEmpty()) {
            val maxPersistence = currentFrameCells.maxOfOrNull { persistenceMap[it] ?: 0 } ?: 0
            safeLogInfo("Persistent bright points: ${results.size} (persistence: $maxPersistence)")
        }

        return FrameAnalysis(
            sources = results,
            ambientBrightness = ambientBrightness,
            roomTooBright = false,
            analyzedWidth = width,
            analyzedHeight = height,
        )
    }

    private fun isPotentialIrPixel(
        r: Int,
        g: Int,
        b: Int,
        brightness: Int,
        saturation: Int
    ): Boolean {
        if (brightness < BRIGHTNESS_THRESHOLD) return false
        if (saturation <= MAX_SATURATION) return true

        // Front cameras often render 850nm IR LEDs as magenta/purple rather
        // than pure white. Require both red and blue to be bright so saturated
        // red, green, or blue LEDs do not trip the detector.
        return r >= 170 &&
            b >= 170 &&
            g <= 190 &&
            abs(r - b) <= 85
    }

    /** Reset persistence tracking (e.g., when scan mode is entered) */
    fun reset() {
        defaultSession.reset()
    }

    private fun safeLogInfo(message: String) {
        try {
            Log.i(TAG, message)
        } catch (_: RuntimeException) {
            // Android Log is not available in plain JVM unit tests.
        }
    }
}

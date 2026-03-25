package com.friendorfoe.detection

import android.graphics.Bitmap
import android.util.Log
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Detects infrared LEDs from night-vision hidden cameras using the
 * phone's front-facing camera (which typically lacks an IR-cut filter).
 *
 * IR LEDs at 850nm appear as bright white/purple spots on the camera.
 * 940nm LEDs are harder to detect but may still show faintly.
 *
 * Usage: User activates "IR Scan" mode, turns off room lights, and
 * slowly pans the front camera around the room. The detector analyzes
 * each frame for bright saturated clusters that persist across frames.
 */
@Singleton
class IrCameraDetector @Inject constructor() {

    companion object {
        private const val TAG = "IrCameraDetector"

        /** Minimum pixel brightness to consider as potential IR (0-255) */
        private const val BRIGHTNESS_THRESHOLD = 220

        /** Minimum cluster size in pixels to report */
        private const val MIN_CLUSTER_PIXELS = 4

        /** Minimum saturation — IR appears as white/purple, not colored */
        private const val MAX_SATURATION = 60 // low saturation = near-white

        /** Grid cell size for spatial clustering */
        private const val GRID_CELL_SIZE = 16
    }

    data class IrSource(
        val x: Float,          // Normalized 0-1 position in frame
        val y: Float,
        val brightness: Int,   // Peak brightness 0-255
        val clusterSize: Int,  // Number of bright pixels in cluster
        val confidence: Float  // 0.0-1.0
    )

    // Persistence tracking: grid cell -> consecutive frame count
    private val persistenceMap = mutableMapOf<Int, Int>()

    /**
     * Analyze a camera frame for IR LED sources.
     *
     * @param bitmap Camera frame from front-facing camera
     * @return List of detected IR sources, empty if none found
     */
    fun analyzeFrame(bitmap: Bitmap): List<IrSource> {
        if (bitmap.isRecycled) return emptyList()

        val width = bitmap.width
        val height = bitmap.height
        val gridW = width / GRID_CELL_SIZE
        val gridH = height / GRID_CELL_SIZE

        // Count bright, low-saturation pixels per grid cell
        val gridCounts = IntArray(gridW * gridH)
        val gridBrightness = IntArray(gridW * gridH)

        val pixels = IntArray(width * height)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)

        for (y in 0 until height) {
            for (x in 0 until width) {
                val pixel = pixels[y * width + x]
                val r = (pixel shr 16) and 0xFF
                val g = (pixel shr 8) and 0xFF
                val b = pixel and 0xFF

                val brightness = maxOf(r, g, b)
                val minC = minOf(r, g, b)
                val saturation = if (brightness > 0) {
                    ((brightness - minC) * 255) / brightness
                } else 0

                if (brightness >= BRIGHTNESS_THRESHOLD && saturation <= MAX_SATURATION) {
                    val gx = (x / GRID_CELL_SIZE).coerceAtMost(gridW - 1)
                    val gy = (y / GRID_CELL_SIZE).coerceAtMost(gridH - 1)
                    val idx = gy * gridW + gx
                    gridCounts[idx]++
                    if (brightness > gridBrightness[idx]) {
                        gridBrightness[idx] = brightness
                    }
                }
            }
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
                    val gy = idx / gridW
                    val gx = idx % gridW
                    val centerX = (gx * GRID_CELL_SIZE + GRID_CELL_SIZE / 2).toFloat() / width
                    val centerY = (gy * GRID_CELL_SIZE + GRID_CELL_SIZE / 2).toFloat() / height

                    val confidence = when {
                        persistence >= 5 -> 0.95f
                        persistence >= 3 -> 0.80f
                        else -> 0.60f
                    }

                    results.add(IrSource(
                        x = centerX,
                        y = centerY,
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
            Log.i(TAG, "IR sources detected: ${results.size} (persistence: ${results.maxOf { persistenceMap[0] ?: 0 }})")
        }

        return results
    }

    /** Reset persistence tracking (e.g., when scan mode is entered) */
    fun reset() {
        persistenceMap.clear()
    }
}

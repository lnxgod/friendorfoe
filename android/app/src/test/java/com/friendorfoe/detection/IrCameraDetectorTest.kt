package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class IrCameraDetectorTest {

    @Test
    fun reports_persistent_bright_low_saturation_cluster() {
        val detector = IrCameraDetector()
        val pixels = darkFrame()
        putBlock(pixels, x = 20, y = 20, color = 0xFFFFFFFF.toInt())

        assertTrue(detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels).isEmpty())

        val results = detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels)

        assertEquals(1, results.size)
        assertTrue(results.single().confidence >= 0.6f)
        assertTrue(results.single().brightness >= 220)
    }

    @Test
    fun rejects_bright_saturated_color_cluster() {
        val detector = IrCameraDetector()
        val pixels = darkFrame()
        putBlock(pixels, x = 20, y = 20, color = 0xFFFF0000.toInt())

        detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels)
        val results = detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels)

        assertTrue(results.isEmpty())
    }

    companion object {
        private const val WIDTH = 64
        private const val HEIGHT = 64

        private fun darkFrame(): IntArray = IntArray(WIDTH * HEIGHT) { 0xFF000000.toInt() }

        private fun putBlock(pixels: IntArray, x: Int, y: Int, color: Int) {
            for (dy in 0 until 2) {
                for (dx in 0 until 2) {
                    pixels[(y + dy) * WIDTH + (x + dx)] = color
                }
            }
        }
    }
}

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

        val analysis = detector.analyzePixelsWithEnvironmentForTest(WIDTH, HEIGHT, pixels)
        val results = analysis.sources

        assertEquals(1, results.size)
        assertTrue(results.single().confidence >= 0.6f)
        assertTrue(results.single().brightness >= 220)
        assertEquals(FloatPoint(20.5f, 20.5f), results.single().centerPx)
        assertEquals(WIDTH, analysis.analyzedWidth)
        assertEquals(HEIGHT, analysis.analyzedHeight)
    }

    @Test
    fun reports_persistent_purple_ir_cluster() {
        val detector = IrCameraDetector()
        val pixels = darkFrame()
        putBlock(pixels, x = 20, y = 20, color = 0xFFFF70FF.toInt())

        assertTrue(detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels).isEmpty())

        val results = detector.analyzePixelsForTest(WIDTH, HEIGHT, pixels)

        assertEquals(1, results.size)
        assertTrue(results.single().confidence >= 0.6f)
        assertTrue(results.single().brightness >= 200)
    }

    @Test
    fun flags_lit_room_and_suppresses_sources() {
        val detector = IrCameraDetector()
        val pixels = IntArray(WIDTH * HEIGHT) { 0xFF9A9A9A.toInt() }
        putBlock(pixels, x = 20, y = 20, color = 0xFFFFFFFF.toInt())

        val analysis = detector.analyzePixelsWithEnvironmentForTest(WIDTH, HEIGHT, pixels)

        assertTrue(analysis.roomTooBright)
        assertTrue(analysis.ambientBrightness >= 96)
        assertTrue(analysis.sources.isEmpty())
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

    @Test
    fun retrySessionDoesNotInheritPersistenceFromAnOldFrameStillFinishing() {
        val detector = IrCameraDetector()
        val oldSession = detector.newSession()
        val retrySession = detector.newSession()
        val pixels = darkFrame()
        putBlock(pixels, x = 20, y = 20, color = 0xFFFFFFFF.toInt())

        assertTrue(oldSession.analyzePixelsForTest(WIDTH, HEIGHT, pixels).isEmpty())
        assertTrue(retrySession.analyzePixelsForTest(WIDTH, HEIGHT, pixels).isEmpty())

        assertEquals(1, oldSession.analyzePixelsForTest(WIDTH, HEIGHT, pixels).size)
        assertEquals(1, retrySession.analyzePixelsForTest(WIDTH, HEIGHT, pixels).size)
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

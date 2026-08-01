package com.friendorfoe.presentation.map

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MapMarkerOrientationTest {

    @Test
    fun `compass headings become flat osmdroid marker rotations`() {
        val north = osmdroidMarkerOrientation(0f)
        val east = osmdroidMarkerOrientation(90f)
        val south = osmdroidMarkerOrientation(180f)
        val west = osmdroidMarkerOrientation(270f)

        assertTrue(north.flat)
        assertEquals(0f, north.rotationDegrees, 0.001f)
        assertEquals(-90f, east.rotationDegrees, 0.001f)
        assertEquals(-180f, south.rotationDegrees, 0.001f)
        assertEquals(90f, west.rotationDegrees, 0.001f)
    }

    @Test
    fun `invalid marker headings render as north`() {
        assertEquals(0f, osmdroidMarkerOrientation(null).rotationDegrees, 0.001f)
        assertEquals(0f, osmdroidMarkerOrientation(Float.NaN).rotationDegrees, 0.001f)
        assertEquals(0f, osmdroidMarkerOrientation(Float.POSITIVE_INFINITY).rotationDegrees, 0.001f)
    }
}

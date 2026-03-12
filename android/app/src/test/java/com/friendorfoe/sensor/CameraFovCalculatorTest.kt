package com.friendorfoe.sensor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import kotlin.math.atan

/**
 * Unit tests for [CameraFovCalculator].
 *
 * Tests FOV formula, isInFieldOfView boundary conditions, and visible range
 * calculation. Uses pure math -- no Android framework mocking needed
 * (except for the SizeF overload, which we bypass by using the float overload).
 */
class CameraFovCalculatorTest {

    private lateinit var calculator: CameraFovCalculator

    @Before
    fun setUp() {
        calculator = CameraFovCalculator()
    }

    // ---- Default FOV tests ----

    @Test
    fun `default horizontal FOV is 60 degrees`() {
        assertEquals(60.0, calculator.horizontalFovDegrees, 0.01)
    }

    @Test
    fun `default vertical FOV is 45 degrees`() {
        assertEquals(45.0, calculator.verticalFovDegrees, 0.01)
    }

    // ---- FOV formula tests ----

    @Test
    fun `FOV formula with known sensor size and focal length`() {
        // Typical smartphone: sensor 6.17mm x 4.55mm, focal length 4.25mm
        // Expected H-FOV = 2 * atan(6.17 / (2 * 4.25)) = 2 * atan(0.7259) = ~71.6 degrees
        // Expected V-FOV = 2 * atan(4.55 / (2 * 4.25)) = 2 * atan(0.5353) = ~56.2 degrees
        calculator.calculateFromFocalLengthAndSensorSize(
            focalLengthMm = 4.25f,
            sensorWidthMm = 6.17f,
            sensorHeightMm = 4.55f
        )

        val expectedHFov = Math.toDegrees(2.0 * atan(6.17 / (2.0 * 4.25)))
        val expectedVFov = Math.toDegrees(2.0 * atan(4.55 / (2.0 * 4.25)))

        assertEquals(expectedHFov, calculator.horizontalFovDegrees, 0.1)
        assertEquals(expectedVFov, calculator.verticalFovDegrees, 0.1)
    }

    @Test
    fun `longer focal length gives narrower FOV`() {
        // Wide angle: 4mm focal length
        calculator.calculateFromFocalLengthAndSensorSize(4f, 6f, 4.5f)
        val wideFov = calculator.horizontalFovDegrees

        // Telephoto: 12mm focal length
        calculator.calculateFromFocalLengthAndSensorSize(12f, 6f, 4.5f)
        val narrowFov = calculator.horizontalFovDegrees

        assertTrue(
            "Longer focal length ($narrowFov) should give narrower FOV than short ($wideFov)",
            narrowFov < wideFov
        )
    }

    @Test
    fun `larger sensor gives wider FOV at same focal length`() {
        // Small sensor: 4mm x 3mm
        calculator.calculateFromFocalLengthAndSensorSize(4f, 4f, 3f)
        val smallSensorFov = calculator.horizontalFovDegrees

        // Large sensor: 8mm x 6mm
        calculator.calculateFromFocalLengthAndSensorSize(4f, 8f, 6f)
        val largeSensorFov = calculator.horizontalFovDegrees

        assertTrue(
            "Larger sensor ($largeSensorFov) should give wider FOV than small ($smallSensorFov)",
            largeSensorFov > smallSensorFov
        )
    }

    @Test
    fun `FOV radians and degrees are consistent`() {
        calculator.calculateFromFocalLengthAndSensorSize(4.25f, 6.17f, 4.55f)

        val hDeg = calculator.horizontalFovDegrees
        val hRad = calculator.horizontalFovRadians
        assertEquals(hDeg, Math.toDegrees(hRad), 0.001)

        val vDeg = calculator.verticalFovDegrees
        val vRad = calculator.verticalFovRadians
        assertEquals(vDeg, Math.toDegrees(vRad), 0.001)
    }

    // ---- isInFieldOfView tests ----

    @Test
    fun `center point is always in field of view`() {
        assertTrue(calculator.isInFieldOfView(0.0, 0.0))
    }

    @Test
    fun `point at exactly half FOV is in view`() {
        val halfH = calculator.horizontalFovRadians / 2.0
        val halfV = calculator.verticalFovRadians / 2.0

        // Exactly at the edge -- should be in view (inclusive boundary)
        assertTrue("Right edge should be in view", calculator.isInFieldOfView(halfH, 0.0))
        assertTrue("Left edge should be in view", calculator.isInFieldOfView(-halfH, 0.0))
        assertTrue("Top edge should be in view", calculator.isInFieldOfView(0.0, halfV))
        assertTrue("Bottom edge should be in view", calculator.isInFieldOfView(0.0, -halfV))
    }

    @Test
    fun `point just outside FOV is not in view`() {
        val halfH = calculator.horizontalFovRadians / 2.0
        val halfV = calculator.verticalFovRadians / 2.0
        val epsilon = 0.001

        assertFalse("Beyond right edge", calculator.isInFieldOfView(halfH + epsilon, 0.0))
        assertFalse("Beyond left edge", calculator.isInFieldOfView(-halfH - epsilon, 0.0))
        assertFalse("Beyond top edge", calculator.isInFieldOfView(0.0, halfV + epsilon))
        assertFalse("Beyond bottom edge", calculator.isInFieldOfView(0.0, -halfV - epsilon))
    }

    @Test
    fun `corner of FOV is in view`() {
        val halfH = calculator.horizontalFovRadians / 2.0
        val halfV = calculator.verticalFovRadians / 2.0

        assertTrue("Top-right corner", calculator.isInFieldOfView(halfH, halfV))
        assertTrue("Top-left corner", calculator.isInFieldOfView(-halfH, halfV))
        assertTrue("Bottom-right corner", calculator.isInFieldOfView(halfH, -halfV))
        assertTrue("Bottom-left corner", calculator.isInFieldOfView(-halfH, -halfV))
    }

    @Test
    fun `object at 180 degrees offset is definitely out of view`() {
        assertFalse(
            "Object behind camera should not be in view",
            calculator.isInFieldOfView(Math.PI, 0.0)
        )
    }

    // ---- Visible range tests ----

    @Test
    fun `visible azimuth range is centered on camera heading`() {
        val (min, max) = calculator.getVisibleAzimuthRange(90.0)
        val halfFov = calculator.horizontalFovDegrees / 2.0

        assertEquals(90.0 - halfFov, min, 0.01)
        assertEquals(90.0 + halfFov, max, 0.01)
    }

    @Test
    fun `visible azimuth range wraps around 360`() {
        // Camera pointing north (0 degrees)
        val (min, max) = calculator.getVisibleAzimuthRange(0.0)
        val halfFov = calculator.horizontalFovDegrees / 2.0

        // Min should wrap around to 360 - halfFov
        assertEquals(360.0 - halfFov, min, 0.01)
        assertEquals(halfFov, max, 0.01)
    }

    @Test
    fun `visible elevation range is clamped to valid range`() {
        // Camera pointing straight up (90 degrees elevation)
        val (min, max) = calculator.getVisibleElevationRange(90.0)

        assertTrue("Max elevation should not exceed 90", max <= 90.0)
        assertTrue("Min elevation should be reasonable", min > 0.0)
    }

    @Test
    fun `visible elevation range at horizon`() {
        val (min, max) = calculator.getVisibleElevationRange(0.0)
        val halfFov = calculator.verticalFovDegrees / 2.0

        assertEquals(-halfFov, min, 0.01)
        assertEquals(halfFov, max, 0.01)
    }
}

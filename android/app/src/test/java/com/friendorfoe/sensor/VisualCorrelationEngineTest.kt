package com.friendorfoe.sensor

import com.friendorfoe.detection.UnknownObjectClassifier
import com.friendorfoe.detection.VisualClassification
import com.friendorfoe.detection.VisualDetection
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.time.Instant

class VisualCorrelationEngineTest {

    private lateinit var engine: VisualCorrelationEngine

    @Before
    fun setUp() {
        engine = VisualCorrelationEngine(UnknownObjectClassifier())
    }

    @Test
    fun `near-top radio projection is rescued by visible detection`() {
        val radio = screenPosition(
            id = "A1",
            screenX = 0.52f,
            screenY = 0f,
            rawScreenX = 0.52f,
            rawScreenY = -0.20f,
            isInView = false,
            isNearViewport = true
        )
        val visual = visualDetection(centerX = 0.50f, centerY = 0.08f)

        val result = engine.correlate(listOf(radio), listOf(visual))
        val rescued = result.positions.single()

        assertTrue(rescued.isInView)
        assertFalse(rescued.isNearViewport)
        assertTrue(rescued.visuallyConfirmed)
        assertEquals(0.50f, rescued.screenX, 0.001f)
        assertEquals(0.08f, rescued.screenY, 0.001f)
        assertEquals(visual, rescued.matchedDetection)
    }

    @Test
    fun `radio-only near-edge target remains off screen without visual detection`() {
        val radio = screenPosition(
            id = "A1",
            screenX = 0.52f,
            screenY = 0f,
            rawScreenX = 0.52f,
            rawScreenY = -0.20f,
            isInView = false,
            isNearViewport = true
        )

        val result = engine.correlate(listOf(radio), emptyList())
        val unchanged = result.positions.single()

        assertFalse(unchanged.isInView)
        assertTrue(unchanged.isNearViewport)
        assertFalse(unchanged.visuallyConfirmed)
        assertNull(unchanged.matchedDetection)
    }

    @Test
    fun `rescued visual detection is removed from unmatched visuals`() {
        val radio = screenPosition(
            id = "A1",
            screenX = 0.52f,
            screenY = 0f,
            rawScreenX = 0.52f,
            rawScreenY = -0.20f,
            isInView = false,
            isNearViewport = true
        )
        val rescuedVisual = visualDetection(trackingId = 10, centerX = 0.50f, centerY = 0.08f)
        val otherVisual = visualDetection(trackingId = 11, centerX = 0.85f, centerY = 0.80f)

        val result = engine.correlate(listOf(radio), listOf(rescuedVisual, otherVisual))

        assertEquals(listOf(otherVisual), result.unmatchedVisuals)
    }

    @Test
    fun `distant off-camera target does not steal unrelated visual detection`() {
        val radio = screenPosition(
            id = "A1",
            screenX = 1f,
            screenY = 0.5f,
            rawScreenX = 2.0f,
            rawScreenY = 0.5f,
            isInView = false,
            isNearViewport = false
        )
        val visual = visualDetection(centerX = 0.50f, centerY = 0.50f)

        val result = engine.correlate(listOf(radio), listOf(visual))
        val unchanged = result.positions.single()

        assertFalse(unchanged.isInView)
        assertFalse(unchanged.visuallyConfirmed)
        assertEquals(listOf(visual), result.unmatchedVisuals)
    }

    private fun screenPosition(
        id: String,
        screenX: Float,
        screenY: Float,
        rawScreenX: Float,
        rawScreenY: Float,
        isInView: Boolean,
        isNearViewport: Boolean
    ): ScreenPosition {
        val aircraft = Aircraft(
            id = id,
            position = Position(latitude = 40.0, longitude = -74.0, altitudeMeters = 1000.0),
            source = DetectionSource.ADS_B,
            category = ObjectCategory.COMMERCIAL,
            confidence = 0.95f,
            firstSeen = Instant.EPOCH,
            lastUpdated = Instant.EPOCH,
            icaoHex = id
        )
        return ScreenPosition(
            skyObject = aircraft,
            screenX = screenX,
            screenY = screenY,
            rawScreenX = rawScreenX,
            rawScreenY = rawScreenY,
            isInView = isInView,
            isNearViewport = isNearViewport,
            bearingDegrees = 0f,
            elevationDegrees = 25f,
            distanceMeters = 1200.0,
            groundDistanceMeters = 1100.0
        )
    }

    private fun visualDetection(
        trackingId: Int = 10,
        centerX: Float,
        centerY: Float
    ): VisualDetection {
        return VisualDetection(
            trackingId = trackingId,
            centerX = centerX,
            centerY = centerY,
            width = 0.05f,
            height = 0.05f,
            labels = emptyList(),
            timestampMs = 1_000L,
            skyScore = 0.8f,
            motionScore = 0.8f,
            visualClassification = VisualClassification.LIKELY_AIRCRAFT
        )
    }
}

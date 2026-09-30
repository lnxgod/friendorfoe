package com.friendorfoe.presentation.trails

import com.friendorfoe.data.local.TrackingEntity
import com.friendorfoe.data.local.SavedFlightEntity
import com.friendorfoe.data.repository.decodedPoints
import com.google.gson.Gson
import javax.xml.parsers.DocumentBuilderFactory
import org.junit.Assert.*
import org.junit.Test

class FlightReviewTest {
    private fun point(time: Long) = TrackingEntity(objectId = "abc", latitude = 32.7, longitude = -117.1,
        altitudeMeters = 1000.0, heading = null, speedMps = null, timestamp = time)
    @Test fun playbackUsesReceivedPositionsWithoutInventingPositionsAcrossGaps() {
        val points = listOf(point(1000), point(11_000), point(500_000))
        assertEquals(points[1], recordedPositionAt(points, 250_000))
        assertEquals(points.last(), recordedPositionAt(points, 500_000))
    }
    @Test fun exportEscapesLabelsAndKeepsCoverageGapsAsSeparateSegments() {
        val xml = flightGpx("A&B <flight>", listOf(point(1000), point(11_000), point(500_000)))
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(xml.byteInputStream())
        assertEquals("A&B <flight>", doc.getElementsByTagName("name").item(0).textContent)
        assertEquals(2, doc.getElementsByTagName("trkseg").length)
        assertEquals(3, doc.getElementsByTagName("trkpt").length)
    }
    @Test fun savedSnapshotRoundTripsNullableTelemetryAndOldPositions() {
        val points = listOf(point(1000), point(11_000))
        val saved = SavedFlightEntity("saved", "abc", "label", 900_000_000, 2, Gson().toJson(points))
        assertEquals(points, saved.decodedPoints())
        assertNull(saved.decodedPoints().first().speedMps)
    }
}

package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class BleTrackerTest {

    @Test
    fun detects_following_after_user_moves_between_sightings() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-06-11T12:00:00Z")

        tracker.recordSightingAt(
            mac = "AA:BB:CC:00:00:01",
            rssi = -58,
            deviceName = "Suspicious Tag",
            deviceType = "BLE Tracker",
            manufacturer = "Generic",
            hasCamera = false,
            userLat = 37.0000,
            userLon = -122.0000,
            compassBearing = 0f,
            timestamp = start
        )
        tracker.recordSightingAt(
            mac = "AA:BB:CC:00:00:01",
            rssi = -60,
            deviceName = "Suspicious Tag",
            deviceType = "BLE Tracker",
            manufacturer = "Generic",
            hasCamera = false,
            userLat = 37.0008,
            userLon = -122.0000,
            compassBearing = 90f,
            timestamp = start.plusSeconds(160)
        )
        tracker.recordSightingAt(
            mac = "AA:BB:CC:00:00:01",
            rssi = -61,
            deviceName = "Suspicious Tag",
            deviceType = "BLE Tracker",
            manufacturer = "Generic",
            hasCamera = false,
            userLat = 37.0016,
            userLon = -122.0000,
            compassBearing = 180f,
            timestamp = start.plusSeconds(320)
        )

        val alerts = tracker.checkForFollowersAt(start.plusSeconds(321))

        assertEquals(1, alerts.size)
        assertEquals("following", alerts.single().reason)
        assertEquals(2, alerts.single().threatLevel)
        assertTrue(alerts.single().device.isFollowing)
    }

    @Test
    fun stationary_device_lingering_stays_separate_from_following() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-06-11T12:00:00Z")

        repeat(3) { index ->
            tracker.recordSightingAt(
                mac = "AA:BB:CC:00:00:02",
                rssi = -42 - index,
                deviceName = "Desk Cam",
                deviceType = "Hidden Camera",
                manufacturer = "Generic",
                hasCamera = true,
                userLat = 37.0000,
                userLon = -122.0000,
                compassBearing = 0f,
                timestamp = start.plusSeconds(index * 70L)
            )
        }

        val alerts = tracker.checkForFollowersAt(start.plusSeconds(150))

        assertEquals(1, alerts.size)
        assertEquals("lingering", alerts.single().reason)
        assertEquals(2, alerts.single().threatLevel)
    }

    @Test
    fun unknownZeroCoordinateSightingsCannotCreateASecondFollowerLocation() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-06-11T12:00:00Z")
        tracker.recordUserLocation(37.0000, -122.0000, start)
        tracker.recordUserLocation(37.0010, -122.0000, start.plusSeconds(200))

        listOf(
            Triple(0.0, 0.0, 0L),
            Triple(0.0, 0.0, 80L),
            Triple(37.0010, -122.0000, 160L),
        ).forEach { (lat, lon, seconds) ->
            tracker.recordSightingAt(
                mac = "AA:BB:CC:00:00:03",
                rssi = -60,
                deviceName = "Unlocated device",
                deviceType = "BLE device",
                manufacturer = null,
                hasCamera = false,
                userLat = lat,
                userLon = lon,
                compassBearing = 0f,
                timestamp = start.plusSeconds(seconds),
            )
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(201)).isEmpty())
    }

    @Test
    fun directionOnlySamplesFeedAnActiveSweepWithoutCreatingMovementHistory() {
        val tracker = BleTracker()
        val mac = "AA:BB:CC:00:00:04"
        tracker.startDirectionScan(mac)

        repeat(8) { index ->
            tracker.recordDirectionSample(
                mac = mac,
                rssi = -70 + index,
                compassBearing = index * 45f,
            )
        }

        assertEquals(null, tracker.getDevice(mac))
        assertEquals(8, tracker.getDirectionSampleCount())
        assertEquals(8, tracker.finishDirectionScan()?.samples?.size)
    }
}

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
}

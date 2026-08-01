package com.friendorfoe.data.local

import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Test

class HistoryMappersTest {

    @Test
    fun wifi_drone_round_trip_preserves_observed_radio_evidence() {
        val firstSeen = Instant.parse("2026-07-18T20:00:00Z")
        val lastSeen = Instant.parse("2026-07-18T20:00:05Z")
        val drone = Drone(
            id = "wifi_60601faabbcc",
            position = Position(42.4347, -83.9850, 201.0),
            source = DetectionSource.WIFI,
            category = ObjectCategory.DRONE,
            confidence = 0.3f,
            firstSeen = firstSeen,
            lastUpdated = lastSeen,
            distanceMeters = 19.5,
            droneId = "DJI-MAVIC3-TEST",
            manufacturer = "DJI",
            ssid = "DJI-MAVIC3-TEST",
            bssid = "60:60:1F:AA:BB:CC",
            signalStrengthDbm = -47,
            frequencyMhz = 5745,
            channelWidthMhz = 80,
        )

        val entity = drone.toHistoryEntity(
            userLatitude = 42.0,
            userLongitude = -83.0,
        )

        assertEquals("wifi_60601faabbcc", entity.objectId)
        assertEquals("DJI-MAVIC3-TEST", entity.ssid)
        assertEquals("60:60:1F:AA:BB:CC", entity.bssid)
        assertEquals(-47, entity.signalStrengthDbm)
        assertEquals(5745, entity.frequencyMhz)
        assertEquals(80, entity.channelWidthMhz)

        val restored = entity.toDrone()
        assertEquals(drone.id, restored.id)
        assertEquals(drone.ssid, restored.ssid)
        assertEquals(drone.bssid, restored.bssid)
        assertEquals(drone.signalStrengthDbm, restored.signalStrengthDbm)
        assertEquals(drone.frequencyMhz, restored.frequencyMhz)
        assertEquals(drone.channelWidthMhz, restored.channelWidthMhz)
        assertEquals(firstSeen, restored.firstSeen)
        assertEquals(lastSeen, restored.lastUpdated)
    }
}

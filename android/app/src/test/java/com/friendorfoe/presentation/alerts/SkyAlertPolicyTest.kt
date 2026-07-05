package com.friendorfoe.presentation.alerts

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class SkyAlertPolicyTest {

    private val settings = SkyAlertSettings(
        droneAlertsEnabled = true,
        helicopterAlertsEnabled = true,
        militaryAlertsEnabled = true,
        policeAlertsEnabled = true
    )

    @Test
    fun droneAlertsDoNotRequireDistance() {
        val candidate = SkyAlertPolicy.candidateFor(drone(distanceMeters = null), settings)

        assertNotNull(candidate)
        requireNotNull(candidate)
        assertEquals("Drone nearby", candidate.title)
    }

    @Test
    fun helicopterAlertsDoNotRequireDistance() {
        val candidate = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.HELICOPTER, distanceMeters = null),
            settings
        )

        assertNotNull(candidate)
        requireNotNull(candidate)
        assertEquals("Helicopter nearby", candidate.title)
    }

    @Test
    fun militaryAlertsAreLimitedToFifteenMiles() {
        val near = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.MILITARY, distanceMeters = 14.9 * METERS_PER_MILE),
            settings
        )
        val far = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.MILITARY, distanceMeters = 15.1 * METERS_PER_MILE),
            settings
        )

        assertNotNull(near)
        assertNull(far)
    }

    @Test
    fun policeBucketIncludesGovernmentEmergencyAndGroundVehiclesWithinFifteenMiles() {
        val categories = listOf(
            ObjectCategory.GOVERNMENT,
            ObjectCategory.EMERGENCY,
            ObjectCategory.GROUND_VEHICLE
        )

        categories.forEach { category ->
            assertNotNull(
                "Expected $category to alert",
                SkyAlertPolicy.candidateFor(
                    aircraft(category = category, distanceMeters = 12.0 * METERS_PER_MILE),
                    settings
                )
            )
        }
    }

    @Test
    fun disabledSettingsSuppressCandidates() {
        val disabled = SkyAlertSettings(
            droneAlertsEnabled = false,
            helicopterAlertsEnabled = false,
            militaryAlertsEnabled = false,
            policeAlertsEnabled = false
        )

        assertNull(SkyAlertPolicy.candidateFor(drone(distanceMeters = null), disabled))
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.HELICOPTER, null), disabled))
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.MILITARY, 1.0), disabled))
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.GOVERNMENT, 1.0), disabled))
    }

    @Test
    fun cooldownUsesStableObjectKey() {
        val policy = SkyAlertPolicy(cooldownMs = 60_000L)
        val candidate = SkyAlertPolicy.candidateFor(drone(distanceMeters = null), settings)

        assertNotNull(candidate)
        requireNotNull(candidate)
        assertTrue(policy.shouldNotify(candidate, nowMs = 1_000L))
        assertTrue(!policy.shouldNotify(candidate, nowMs = 30_000L))
        assertTrue(policy.shouldNotify(candidate, nowMs = 61_001L))
    }

    private fun drone(distanceMeters: Double?) = Drone(
        id = "drone-1",
        position = Position(37.0, -122.0, 120.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = NOW,
        lastUpdated = NOW,
        distanceMeters = distanceMeters,
        droneId = "DRONE123",
        manufacturer = "DJI"
    )

    private fun aircraft(
        category: ObjectCategory,
        distanceMeters: Double?
    ) = Aircraft(
        id = "aircraft-$category",
        position = Position(37.0, -122.0, 1000.0),
        category = category,
        firstSeen = NOW,
        lastUpdated = NOW,
        distanceMeters = distanceMeters,
        icaoHex = "ABC123",
        callsign = category.name.take(6)
    )

    companion object {
        private val NOW = Instant.parse("2026-07-04T12:00:00Z")
        private const val METERS_PER_MILE = 1609.344
    }
}

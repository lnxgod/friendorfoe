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
    fun formationPixelsDoNotGenerateAlerts() {
        val candidate = SkyAlertPolicy.candidateFor(
            drone(distanceMeters = null, droneId = "FOF-C5-ABCDEF-001"),
            settings
        )

        assertNull(candidate)
    }

    @Test
    fun helicopterAlertsRequireAKnownDistance() {
        val candidate = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.HELICOPTER, distanceMeters = null),
            settings
        )

        assertNull(candidate)
    }

    @Test
    fun adsbUavAlertsUseDroneSettingAndRequireDistance() {
        val candidate = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.DRONE, distanceMeters = METERS_PER_MILE),
            settings
        )

        assertNotNull(candidate)
        requireNotNull(candidate)
        assertEquals("Drone nearby", candidate.title)
    }

    @Test
    fun militaryAlertsAreLimitedToTenMilesByDefault() {
        val near = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.MILITARY, distanceMeters = 9.9 * METERS_PER_MILE),
            settings
        )
        val far = SkyAlertPolicy.candidateFor(
            aircraft(category = ObjectCategory.MILITARY, distanceMeters = 10.1 * METERS_PER_MILE),
            settings
        )

        assertNotNull(near)
        assertNull(far)
    }

    @Test
    fun policeBucketIncludesGovernmentEmergencyAndGroundVehiclesWithinTenMiles() {
        val categories = listOf(
            ObjectCategory.GOVERNMENT,
            ObjectCategory.EMERGENCY,
            ObjectCategory.GROUND_VEHICLE
        )

        categories.forEach { category ->
            assertNotNull(
                "Expected $category to alert",
                SkyAlertPolicy.candidateFor(
                    aircraft(category = category, distanceMeters = 8.0 * METERS_PER_MILE),
                    settings
                )
            )
        }
    }

    @Test
    fun allAircraftAlertsRequireAValidDistanceWithinTenMilesIncludingTheBoundary() {
        val categories = listOf(
            ObjectCategory.HELICOPTER, ObjectCategory.MILITARY, ObjectCategory.GOVERNMENT,
            ObjectCategory.EMERGENCY, ObjectCategory.GROUND_VEHICLE, ObjectCategory.DRONE,
        )
        for (category in categories) {
            for (distance in listOf(0.0, 10.0 * METERS_PER_MILE)) {
                assertNotNull("$category at $distance", SkyAlertPolicy.candidateFor(aircraft(category, distance), settings))
            }
            for (distance in listOf(
                null, -1.0, Double.NaN, Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY,
                10.0 * METERS_PER_MILE + 0.01, 50.0 * METERS_PER_MILE,
            )) {
                assertNull("$category at $distance", SkyAlertPolicy.candidateFor(aircraft(category, distance), settings))
            }
        }
    }

    @Test
    fun configuredRangeCanShrinkOrExpandHelicopterAlerts() {
        for (rangeMiles in listOf(1, 5, 10, 15, 50)) {
            val configured = settings.copy(aircraftRangeMiles = rangeMiles)
            assertNotNull(
                SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.HELICOPTER, rangeMiles * METERS_PER_MILE), configured),
            )
            assertNull(
                SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.HELICOPTER, rangeMiles * METERS_PER_MILE + 0.01), configured),
            )
        }
        val helicopter = aircraft(ObjectCategory.HELICOPTER, 12.0 * METERS_PER_MILE)
        assertNotNull(SkyAlertPolicy.candidateFor(helicopter, settings.copy(aircraftRangeMiles = 15)))
        assertNull(SkyAlertPolicy.candidateFor(helicopter, settings.copy(aircraftRangeMiles = 5)))
    }

    @Test
    fun policeAlertBodyIncludesOperatorNameWhenAvailable() {
        val candidate = SkyAlertPolicy.candidateFor(
            aircraft(
                category = ObjectCategory.GOVERNMENT,
                distanceMeters = 10.0 * METERS_PER_MILE,
                operatorName = "SAN DIEGO COUNTY SHERIFF"
            ),
            settings
        )

        assertNotNull(candidate)
        requireNotNull(candidate)
        assertTrue(candidate.body.contains("SAN DIEGO COUNTY SHERIFF"))
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
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.HELICOPTER, 1.0), disabled))
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.MILITARY, 1.0), disabled))
        assertNull(SkyAlertPolicy.candidateFor(aircraft(ObjectCategory.GOVERNMENT, 1.0), disabled))
    }

    @Test
    fun candidatesForReturnsOnlyObjectsThatWouldAlert() {
        val candidates = SkyAlertPolicy.candidatesFor(
            skyObjects = listOf(
                aircraft(ObjectCategory.COMMERCIAL, 1.0 * METERS_PER_MILE),
                aircraft(ObjectCategory.GOVERNMENT, 10.0 * METERS_PER_MILE),
                aircraft(ObjectCategory.MILITARY, 17.0 * METERS_PER_MILE),
                drone(distanceMeters = null)
            ),
            settings = settings
        )

        assertEquals(listOf("Drone nearby", "Police / emergency vehicle nearby"), candidates.map { it.title })
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

    @Test
    fun helicoptersAndAdsbDronesRespectTheNearbyBoundary() {
        listOf(ObjectCategory.HELICOPTER, ObjectCategory.DRONE).forEach { category ->
            assertNotNull(SkyAlertPolicy.candidateFor(aircraft(category, 10 * METERS_PER_MILE), settings))
            listOf(10 * METERS_PER_MILE + 1, 50 * METERS_PER_MILE, -1.0,
                Double.NaN, Double.POSITIVE_INFINITY).forEach { distance ->
                assertNull(SkyAlertPolicy.candidateFor(aircraft(category, distance), settings))
            }
        }
    }

    @Test
    fun distantRemoteIdDroneDoesNotGenerateNearbyAlert() {
        assertNull(SkyAlertPolicy.candidateFor(drone(50 * METERS_PER_MILE), settings))
        assertNotNull(SkyAlertPolicy.candidateFor(drone(100.0), settings))
        assertNotNull(SkyAlertPolicy.candidateFor(
            drone(100.0).copy(estimatedDistanceMeters = 50 * METERS_PER_MILE), settings,
        ))
    }

    @Test
    fun localRadioDroneRangeIsIndependentOfAircraftPreference() {
        val shortAircraftRange = settings.copy(aircraftRangeMiles = 5)
        assertNotNull(SkyAlertPolicy.candidateFor(drone(15 * METERS_PER_MILE), shortAircraftRange))
        assertNull(SkyAlertPolicy.candidateFor(drone(15 * METERS_PER_MILE + 1), shortAircraftRange))
    }

    private fun drone(
        distanceMeters: Double?,
        droneId: String = "DRONE123"
    ) = Drone(
        id = "drone-1",
        position = Position(37.0, -122.0, 120.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = NOW,
        lastUpdated = NOW,
        distanceMeters = distanceMeters,
        droneId = droneId,
        manufacturer = "DJI"
    )

    private fun aircraft(
        category: ObjectCategory,
        distanceMeters: Double?,
        operatorName: String? = null
    ) = Aircraft(
        id = "aircraft-$category",
        position = Position(37.0, -122.0, 1000.0),
        category = category,
        firstSeen = NOW,
        lastUpdated = NOW,
        distanceMeters = distanceMeters,
        icaoHex = "ABC123",
        callsign = category.name.take(6),
        operatorName = operatorName
    )

    companion object {
        private val NOW = Instant.parse("2026-07-04T12:00:00Z")
        private const val METERS_PER_MILE = 1609.344
    }
}

package com.friendorfoe.data.repository

import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class AircraftTacticalEnricherTest {

    @Test
    fun closeAircraftUsesHexDbOwnerToClassifyPoliceAircraft() = runTest {
        val lookups = mutableListOf<String>()
        val enricher = AircraftTacticalEnricher { aircraft ->
            lookups += aircraft.icaoHex
            AircraftDetailDto(
                icaoHex = aircraft.icaoHex,
                callsign = aircraft.callsign,
                registration = "N123SD",
                aircraftType = "AS50",
                aircraftDescription = "Eurocopter AS350",
                operator = "SAN DIEGO COUNTY SHERIFF",
                photo = null,
                route = null,
                country = null
            )
        }

        val result = enricher.enrichAircraft(
            userLatitude = USER_LAT,
            userLongitude = USER_LON,
            aircraft = aircraftAtMiles(milesNorth = 10.0)
        )

        assertEquals(listOf("abc123"), lookups)
        assertEquals(ObjectCategory.GOVERNMENT, result.category)
        assertEquals("N123SD", result.registration)
        assertEquals("AS50", result.aircraftType)
        assertEquals("SAN DIEGO COUNTY SHERIFF", result.operatorName)
        assertNotNull(result.distanceMeters)
        assertTrue(result.distanceMeters!! <= AircraftTacticalEnricher.TACTICAL_ENRICHMENT_RADIUS_METERS)
        assertTrue(result.classificationSignals!!.contains("OWNER:PUBLIC_SAFETY"))
        assertTrue(result.classificationSignals!!.contains("ENRICHMENT:HEXDB"))
    }

    @Test
    fun aircraftOutsideFifteenMilesDoesNotCallHexDb() = runTest {
        var lookupCount = 0
        val enricher = AircraftTacticalEnricher {
            lookupCount += 1
            error("HexDB should not be queried outside the tactical enrichment radius")
        }

        val result = enricher.enrichAircraft(
            userLatitude = USER_LAT,
            userLongitude = USER_LON,
            aircraft = aircraftAtMiles(milesNorth = 16.0)
        )

        assertEquals(0, lookupCount)
        assertEquals(ObjectCategory.UNKNOWN, result.category)
        assertNotNull(result.distanceMeters)
        assertTrue(result.distanceMeters!! > AircraftTacticalEnricher.TACTICAL_ENRICHMENT_RADIUS_METERS)
    }

    @Test
    fun closeKnownAirlineCallsignDoesNotCallHexDb() = runTest {
        var lookupCount = 0
        val enricher = AircraftTacticalEnricher {
            lookupCount += 1
            error("HexDB should not be queried for already identified airline callsigns")
        }

        val result = enricher.enrichAircraft(
            userLatitude = USER_LAT,
            userLongitude = USER_LON,
            aircraft = aircraftAtMiles(milesNorth = 3.0).copy(
                category = ObjectCategory.COMMERCIAL,
                callsign = "UAL123",
                classificationSignals = listOf("AIRLINE:UAL")
            )
        )

        assertEquals(0, lookupCount)
        assertEquals(ObjectCategory.COMMERCIAL, result.category)
    }

    private fun aircraftAtMiles(milesNorth: Double): Aircraft {
        val latOffset = (milesNorth * METERS_PER_MILE) / METERS_PER_DEGREE_LAT
        return Aircraft(
            id = "abc123",
            position = Position(USER_LAT + latOffset, USER_LON, 1000.0),
            category = ObjectCategory.UNKNOWN,
            firstSeen = NOW,
            lastUpdated = NOW,
            icaoHex = "abc123",
            callsign = "N123SD"
        )
    }

    companion object {
        private const val USER_LAT = 37.0
        private const val USER_LON = -122.0
        private const val METERS_PER_MILE = 1609.344
        private const val METERS_PER_DEGREE_LAT = 111_195.0
        private val NOW = Instant.parse("2026-07-05T12:00:00Z")
    }
}

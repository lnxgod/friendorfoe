package com.friendorfoe.presentation.detail

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.remote.AircraftDetailDto
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DetailPresentationTest {

    @Test
    fun historicalSnapshotIsLabeledAndNeverLive() {
        val model = presentHistoricalDetail(history(id = 11, objectId = "abc"))

        assertEquals("Historical detection", model.statusLabel)
        assertFalse(model.isLive)
        assertTrue(model.identifiers.any { it.value == "abc" && it.copyable })
        assertFalse(model.rawExpandedByDefault)
        assertTrue(model.summary.any { it.label == "Source" && it.value == "Remote ID" })
    }

    @Test
    fun partialLiveDetailKeepsLocalSummaryAndRetry() {
        val model = presentLiveDetail(
            aircraft = aircraft(),
            remoteDetail = null,
            remoteFailure = "Aircraft details unavailable",
        )

        assertEquals("Live detection", model.statusLabel)
        assertTrue(model.isLive)
        assertTrue(model.summary.any { it.label == "Source" && it.value == "ADS-B" })
        assertTrue(model.identifiers.any { it.label == "ICAO address" && it.value == "abc123" })
        assertEquals("Retry details", model.retryLabel)
    }

    @Test
    fun successfulLiveEnrichmentDoesNotShowRetry() {
        val model = presentLiveDetail(
            aircraft = aircraft(),
            remoteDetail = com.friendorfoe.data.remote.AircraftDetailDto(
                icaoHex = "abc123",
                callsign = "FOF42",
                registration = "N42FO",
                aircraftType = "B738",
                aircraftDescription = "Boeing 737-800",
                operator = "Example Air",
                photo = null,
                route = null,
                country = "United States",
            ),
            remoteFailure = null,
        )

        assertNull(model.retryLabel)
        assertEquals("FOF42", model.title)
        assertTrue(model.identifiers.any { it.label == "Registration" && it.value == "N42FO" })
    }

    @Test
    fun droneDetailKeepsLocalIdentityAndHumanSource() {
        val model = presentLiveDroneDetail(drone())

        assertEquals("Live detection", model.statusLabel)
        assertTrue(model.identifiers.any { it.value == "rid-7" })
        assertTrue(model.summary.any { it.label == "Source" && it.value == "Remote ID · Wi-Fi" })
        assertTrue(model.summary.any { it.label == "Category" && it.value == "Drone / UAS" })
        assertNull(model.retryLabel)
    }

    @Test
    fun nullableRemoteFieldsAreOmittedInsteadOfInvented() {
        val model = presentLiveDetail(
            aircraft = aircraft().copy(
                callsign = null,
                registration = null,
                aircraftType = null,
                aircraftModel = null,
                operatorName = null,
            ),
            remoteDetail = null,
            remoteFailure = null,
        )

        assertFalse(model.identifiers.any { it.value.equals("Unknown", ignoreCase = true) })
        assertFalse(model.advanced.any { it.value.equals("Unknown", ignoreCase = true) })
    }

    @Test
    fun liveAircraftCarriesPhotoEvidenceIntoTheNewDetailModel() {
        val model = presentLiveDetail(
            aircraft = aircraft().copy(photoUrl = "https://images.example/live.jpg"),
            remoteDetail = AircraftDetailDto(
                icaoHex = "abc123",
                callsign = "FOF42",
                registration = "N42FO",
                aircraftType = "B738",
                aircraftDescription = "Boeing 737-800",
                operator = null,
                photo = null,
                route = null,
                country = null,
            ),
            remoteFailure = null,
        )

        assertEquals(
            AircraftVisual(
                photoUrl = "https://images.example/live.jpg",
                typeCode = "B738",
                description = "Boeing 737-800",
                category = ObjectCategory.COMMERCIAL,
            ),
            model.aircraftVisual,
        )
    }

    @Test
    fun historicalAircraftUsesOnlySavedPhotoAndCategoryEvidence() {
        val row = history(id = 12, objectId = "abc").copy(
            objectType = "aircraft",
            category = "commercial",
            photoUrl = "https://images.example/saved.jpg",
        )

        assertEquals(
            AircraftVisual(
                photoUrl = "https://images.example/saved.jpg",
                typeCode = null,
                description = null,
                category = ObjectCategory.COMMERCIAL,
            ),
            presentHistoricalDetail(row).aircraftVisual,
        )
        assertNull(presentHistoricalDetail(history(id = 13, objectId = "drone")).aircraftVisual)
        assertNull(presentLiveDroneDetail(drone()).aircraftVisual)
    }

    private fun aircraft() = Aircraft(
        id = "abc123",
        position = Position(
            latitude = 32.7157,
            longitude = -117.1611,
            altitudeMeters = 1_500.0,
            speedMps = 65f,
            heading = 275f,
        ),
        category = ObjectCategory.COMMERCIAL,
        firstSeen = Instant.ofEpochMilli(1_000),
        lastUpdated = Instant.ofEpochMilli(5_000),
        distanceMeters = 2_000.0,
        icaoHex = "abc123",
        callsign = "LOCAL42",
    )

    private fun drone() = Drone(
        id = "rid-7",
        position = Position(
            latitude = 32.7157,
            longitude = -117.1611,
            altitudeMeters = 45.0,
        ),
        source = DetectionSource.WIFI_NAN,
        confidence = 0.9f,
        firstSeen = Instant.ofEpochMilli(1_000),
        lastUpdated = Instant.ofEpochMilli(5_000),
        distanceMeters = 120.0,
        droneId = "rid-7",
        manufacturer = "DJI",
        model = "Mini 4",
        signalStrengthDbm = -61,
    )

    private fun history(id: Long, objectId: String) = HistoryEntity(
        id = id,
        objectId = objectId,
        objectType = "drone",
        detectionSource = "remote_id",
        category = "drone",
        displayName = "Saved drone",
        description = null,
        latitude = 32.7157,
        longitude = -117.1611,
        altitudeMeters = 45.0,
        userLatitude = 32.71,
        userLongitude = -117.16,
        distanceMeters = 120.0,
        confidence = 0.9f,
        firstSeen = 1_000,
        lastSeen = 5_000,
    )
}

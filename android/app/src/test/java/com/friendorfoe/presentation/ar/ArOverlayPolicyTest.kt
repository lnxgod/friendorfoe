package com.friendorfoe.presentation.ar

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.sensor.ScreenPosition
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.Instant

class ArOverlayPolicyTest {

    @Test
    fun `radio positioning requires usable permission and a locked location fix`() {
        val cases = listOf(
            Triple(PermissionUiState.Granted, GpsStatus.LOCKED, true),
            Triple(PermissionUiState.Approximate, GpsStatus.LOCKED, true),
            Triple(PermissionUiState.Granted, GpsStatus.SEARCHING, false),
            Triple(PermissionUiState.Approximate, GpsStatus.DISABLED, false),
            Triple(PermissionUiState.Granted, GpsStatus.NO_PERMISSION, false),
            Triple(PermissionUiState.Denied, GpsStatus.LOCKED, false),
        )

        cases.forEach { (permissionState, gpsStatus, expected) ->
            assertEquals(
                "$permissionState with $gpsStatus",
                expected,
                isRadioPositioningAvailable(permissionState, gpsStatus),
            )
        }
    }

    @Test
    fun `locked fix displays radio positions for granted and approximate location`() {
        val position = offScreenPosition(aircraft("POSITION"), 1_000.0)

        assertEquals(
            listOf(position),
            displayedRadioPositions(
                listOf(position),
                PermissionUiState.Granted,
                GpsStatus.LOCKED,
            ),
        )
        assertEquals(
            listOf(position),
            displayedRadioPositions(
                listOf(position),
                PermissionUiState.Approximate,
                GpsStatus.LOCKED,
            ),
        )
    }

    @Test
    fun `granted location without a usable fix hides radio positions and targets`() {
        val position = offScreenPosition(aircraft("POSITION"), 1_000.0)

        assertEquals(
            emptyList<ScreenPosition>(),
            displayedRadioPositions(
                listOf(position),
                PermissionUiState.Granted,
                GpsStatus.SEARCHING,
            ),
        )
        assertEquals(
            null,
            usableRadioInteractionObjectId(
                position.skyObject.id,
                PermissionUiState.Granted,
                GpsStatus.SEARCHING,
            ),
        )
    }

    @Test
    fun `losing location permission hides every retained positional interaction`() {
        val retained = RadioInteractionState(
            selectedObjectId = "SELECTED",
            lockedObjectId = "LOCKED",
            objectPeek = ObjectPeekState(
                objectId = "PEEK",
                title = "Peek",
                evidence = "ADS-B radio match",
                canCapture = true,
            ),
            snapTarget = SnapTarget(
                objectId = "SNAP",
                label = "Snap",
                typeDescription = "Aircraft",
                distanceMeters = 1_000.0,
            ),
            showUnidentifiedSheet = true,
        )

        assertEquals(
            retained,
            displayedRadioInteractions(
                retained,
                PermissionUiState.Approximate,
                GpsStatus.LOCKED,
            ),
        )

        assertEquals(
            RadioInteractionState.Empty,
            displayedRadioInteractions(
                retained,
                PermissionUiState.Denied,
                GpsStatus.LOCKED,
            ),
        )
    }

    @Test
    fun `off-screen arrows include nearby aircraft but exclude farther aircraft and drones`() {
        val positions = listOf(
            offScreenPosition(aircraft("INSIDE"), 32_100.0),
            offScreenPosition(aircraft("OUTSIDE"), 32_300.0),
            offScreenPosition(drone("DRONE"), 2_100.0),
        )

        val selected = selectOffScreenRadioPositions(positions)

        assertEquals(listOf("INSIDE"), selected.map { it.skyObject.id })
    }

    private fun offScreenPosition(skyObject: com.friendorfoe.domain.model.SkyObject, distanceMeters: Double) =
        ScreenPosition(
            skyObject = skyObject,
            screenX = 0f,
            screenY = 0f,
            isInView = false,
            distanceMeters = distanceMeters,
        )

    private fun aircraft(id: String) = Aircraft(
        id = id,
        position = Position(40.0, -74.0, 0.0),
        category = ObjectCategory.COMMERCIAL,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        icaoHex = id,
    )

    private fun drone(id: String) = Drone(
        id = id,
        position = Position(40.0, -74.0, 0.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        droneId = id,
    )
}

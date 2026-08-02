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
    fun `display policy keeps radio positions when location is granted or approximate`() {
        val position = offScreenPosition(aircraft("POSITION"), 1_000.0)

        assertEquals(
            listOf(position),
            displayedRadioPositions(listOf(position), PermissionUiState.Granted),
        )
        assertEquals(
            listOf(position),
            displayedRadioPositions(listOf(position), PermissionUiState.Approximate),
        )
        assertEquals(
            emptyList<ScreenPosition>(),
            displayedRadioPositions(listOf(position), PermissionUiState.Denied),
        )
    }

    @Test
    fun `denied location rejects an interaction target created after initial cleanup`() {
        val droneId = "DRONE"

        assertEquals(
            null,
            usableRadioInteractionObjectId(droneId, PermissionUiState.Denied),
        )
    }

    @Test
    fun `off-screen arrows include nearby aircraft but exclude farther aircraft and drones`() {
        val positions = listOf(
            offScreenPosition(aircraft("INSIDE"), 19_200.0),
            offScreenPosition(aircraft("OUTSIDE"), 19_400.0),
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

package com.friendorfoe.presentation.map

import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.permissions.PermissionUiState
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class MapPresentationTest {
    @Test
    fun staleGpsDoesNotHideAFreshNetworkLocation() {
        val nowElapsedRealtimeNanos = 120_000_000_000L

        assertEquals(
            "network",
            selectFreshestMapLastKnownLocation(
                gps = MapLastKnownLocationCandidate(
                    value = "gps",
                    elapsedRealtimeNanos = 1_000_000_000L,
                ),
                network = MapLastKnownLocationCandidate(
                    value = "network",
                    elapsedRealtimeNanos = 115_000_000_000L,
                ),
                nowElapsedRealtimeNanos = nowElapsedRealtimeNanos,
            ),
        )
    }

    @Test
    fun futureLastKnownLocationIsRejected() {
        assertNull(
            selectFreshestMapLastKnownLocation(
                gps = MapLastKnownLocationCandidate(
                    value = "future-gps",
                    elapsedRealtimeNanos = 121_000_000_000L,
                ),
                network = null,
                nowElapsedRealtimeNanos = 120_000_000_000L,
            ),
        )
    }

    @Test
    fun exactFreshnessBoundaryIsAccepted() {
        assertEquals(
            "boundary-network",
            selectFreshestMapLastKnownLocation(
                gps = null,
                network = MapLastKnownLocationCandidate(
                    value = "boundary-network",
                    elapsedRealtimeNanos = 90_000_000_000L,
                ),
                nowElapsedRealtimeNanos = 120_000_000_000L,
            ),
        )
    }

    @Test
    fun loadingPermissionWithInvalidPositionKeepsTheLocatingSurface() {
        assertFalse(
            shouldRevealMap(
                locationPermissionState = PermissionUiState.Loading,
                userPosition = Position(0.0, 0.0, 0.0),
            )
        )
    }

    @Test
    fun confirmedUnavailablePermissionRevealsTheBrowsableMapImmediately() {
        val uninitializedPosition = Position(0.0, 0.0, 0.0)

        listOf(
            PermissionUiState.Denied,
            PermissionUiState.PermanentlyDenied,
        ).forEach { permissionState ->
            assertTrue(
                shouldRevealMap(
                    locationPermissionState = permissionState,
                    userPosition = uninitializedPosition,
                )
            )
        }
    }

    @Test
    fun usableLocationPermissionWaitsForAFiniteNonOriginPosition() {
        assertEquals(
            MapCameraAction.WaitForLocation,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(0.0, 0.0, 0.0),
                cameraInitialized = false,
                followPhone = false,
                userControlsCamera = false,
            ),
        )
        assertEquals(
            MapCameraAction.WaitForLocation,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(Double.NaN, -117.1, 0.0),
                cameraInitialized = false,
                followPhone = false,
                userControlsCamera = false,
            ),
        )
        assertEquals(
            MapCameraAction.WaitForLocation,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(32.7, Double.POSITIVE_INFINITY, 0.0),
                cameraInitialized = false,
                followPhone = false,
                userControlsCamera = false,
            ),
        )
        assertEquals(
            false,
            shouldRevealMap(
                locationPermissionState = PermissionUiState.Granted,
                userPosition = Position(0.0, 0.0, 0.0),
            ),
        )
    }

    @Test
    fun firstValidPhonePositionInitializesTheCamera() {
        listOf(
            Position(32.7, -117.1, 0.0),
            Position(0.0, -117.1, 0.0),
            Position(32.7, 0.0, 0.0),
        ).forEach { position ->
            assertEquals(
                MapCameraAction.InitializeAtPhone,
                mapCameraAction(
                    locationPermissionUsable = true,
                    userPosition = position,
                    cameraInitialized = false,
                    followPhone = false,
                    userControlsCamera = false,
                ),
            )
        }
    }

    @Test
    fun ordinaryGpsUpdatesKeepTheInitializedCamera() {
        assertEquals(
            MapCameraAction.KeepUserCamera,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(32.8, -117.2, 0.0),
                cameraInitialized = true,
                followPhone = false,
                userControlsCamera = false,
            ),
        )
    }

    @Test
    fun explicitFollowCentersOnThePhone() {
        assertEquals(
            MapCameraAction.FollowPhone,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(32.8, -117.2, 0.0),
                cameraInitialized = true,
                followPhone = true,
                userControlsCamera = false,
            ),
        )
    }

    @Test
    fun userControlledCameraWinsOverLocationAndFollowChanges() {
        assertEquals(
            MapCameraAction.KeepUserCamera,
            mapCameraAction(
                locationPermissionUsable = true,
                userPosition = Position(32.9, -117.3, 0.0),
                cameraInitialized = true,
                followPhone = true,
                userControlsCamera = true,
            ),
        )
    }

    @Test
    fun unavailableLocationPermissionRevealsTheBrowsableMapImmediately() {
        val uninitializedPosition = Position(0.0, 0.0, 0.0)

        assertEquals(
            MapCameraAction.KeepUserCamera,
            mapCameraAction(
                locationPermissionUsable = false,
                userPosition = uninitializedPosition,
                cameraInitialized = false,
                followPhone = false,
                userControlsCamera = false,
            ),
        )
        assertEquals(
            true,
            shouldRevealMap(
                locationPermissionState = PermissionUiState.Denied,
                userPosition = uninitializedPosition,
            ),
        )
    }

    @Test
    fun absentUserPositionNeverInventsAnOrigin() {
        val initial = MapUiState()

        assertNull(initial.userPosition)
        assertNull(initial.mapCenter)
        assertEquals(MapSurfaceState.AwaitingLocation, initial.surface)
        assertEquals(
            MapSurfaceState.AwaitingLocation,
            mapSurfaceForLocation(userPosition = null, locationDenied = false),
        )
        assertEquals(
            MapSurfaceState.LocationDenied,
            mapSurfaceForLocation(userPosition = null, locationDenied = true),
        )
        assertEquals(
            MapSurfaceState.AwaitingLocation,
            mapSurfaceForLocation(Position(0.0, 0.0, 0.0), locationDenied = false),
        )
        assertEquals(
            MapSurfaceState.Ready,
            mapSurfaceForLocation(Position(32.7, -117.1, 0.0), locationDenied = false),
        )
    }

    @Test
    fun remoteSearchUsesOnlyTheExplicitCapturedMapCenter() {
        val center = Position(latitude = 32.7, longitude = -117.1, altitudeMeters = 0.0)
        val row = MapTarget(
            id = "remote-one",
            title = "Remote drone",
            sourceLabel = "Configured backend",
            latitude = 32.8,
            longitude = -117.2,
            skyObject = null,
        )

        assertEquals(RemoteSearchState.Idle, beginRemoteSearch(mapCenter = null))
        assertEquals(RemoteSearchState.Loading(center), beginRemoteSearch(center))
        assertEquals(RemoteSearchState.Empty(center), completeRemoteSearch(center, emptyList()))
        assertEquals(
            RemoteSearchState.Results(center, listOf(row)),
            completeRemoteSearch(center, listOf(row)),
        )
        assertEquals(
            RemoteSearchState.Failed(center, "Backend unavailable", canRetry = true),
            failRemoteSearch(center, "Backend unavailable"),
        )
    }

    @Test
    fun localAndRemoteAdaptersPreserveTheirTruthfulSourceAndDetailCapability() {
        val localDrone = drone("local")
        val local = localDrone.toMapTarget(sourceLabel = "Remote ID · Bluetooth")
        val remote = LocatedDroneDto(
            droneId = "remote",
            lat = 33.0,
            lon = -118.0,
            manufacturer = "DJI",
            model = "Mavic 3",
        ).toMapTarget()

        assertEquals("Remote ID · Bluetooth", local.sourceLabel)
        assertEquals(32.7, local.latitude, 0.0)
        assertEquals(-117.1, local.longitude, 0.0)
        assertSame(localDrone, local.skyObject)
        assertEquals("Configured backend", remote.sourceLabel)
        assertEquals("DJI Mavic 3", remote.title)
        assertNull(remote.skyObject)
    }

    @Test
    fun threeFailuresBeforeAnySuccessBecomeTileFailure() {
        val first = reduceMapTileHealth(
            previous = MapTileHealth(),
            event = MapTileEvent.Failed(cachedTileVisible = true),
            nowElapsedMs = 10L,
        )
        val second = reduceMapTileHealth(
            previous = first.health,
            event = MapTileEvent.Failed(cachedTileVisible = false),
            nowElapsedMs = 20L,
        )
        val third = reduceMapTileHealth(
            previous = second.health,
            event = MapTileEvent.Failed(cachedTileVisible = false),
            nowElapsedMs = 30L,
        )

        assertEquals(MapSurfaceState.Ready, first.surface)
        assertEquals(MapSurfaceState.Ready, second.surface)
        assertEquals(MapSurfaceState.TileFailed("Map tiles are unavailable"), third.surface)
        assertEquals(3, third.health.consecutiveFailures)
    }

    @Test
    fun failureAfterSuccessKeepsCachedMapAsNetworkLimited() {
        val success = reduceMapTileHealth(
            previous = MapTileHealth(consecutiveFailures = 2),
            event = MapTileEvent.Succeeded(cachedTileVisible = true),
            nowElapsedMs = 100L,
        )
        val failure = reduceMapTileHealth(
            previous = success.health,
            event = MapTileEvent.Failed(cachedTileVisible = true),
            nowElapsedMs = 160L,
        )

        assertEquals(0, success.health.consecutiveFailures)
        assertEquals(100L, success.health.lastSuccessElapsedMs)
        assertEquals(MapSurfaceState.Ready, success.surface)
        assertEquals(MapSurfaceState.NetworkLimited(cachedAgeMs = 60L), failure.surface)
    }

    private fun drone(id: String): Drone = Drone(
        id = id,
        position = Position(latitude = 32.7, longitude = -117.1, altitudeMeters = 100.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        droneId = id,
    )
}

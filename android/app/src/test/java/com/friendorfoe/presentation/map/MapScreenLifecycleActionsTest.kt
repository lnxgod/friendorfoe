package com.friendorfoe.presentation.map

import androidx.lifecycle.Lifecycle
import com.friendorfoe.domain.model.SkyObject
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertFalse
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MapScreenLifecycleActionsTest {
    @Test
    fun disposeStopsPollingEvenWithoutPauseEvent() {
        val events = mutableListOf<String>()
        val pollingState = MapBackendIntegrationState(
            localObjects = MutableStateFlow<List<SkyObject>>(emptyList()),
        )
        val actions = MapScreenLifecycleActions(
            startLocation = { events += "start-location" },
            stopLocation = { events += "stop-location" },
            startPolling = {
                events += "start-polling"
                pollingState.startPolling()
            },
            stopPolling = {
                events += "stop-polling"
                pollingState.stopPolling()
            },
        )

        actions.onEvent(Lifecycle.Event.ON_RESUME)
        actions.dispose()

        assertEquals(
            listOf("start-location", "start-polling", "stop-location", "stop-polling"),
            events,
        )
        assertFalse(pollingState.pollingRequested.value)
    }

    @Test
    fun pauseStopsBothLocationAndPolling() {
        val events = mutableListOf<String>()
        val actions = MapScreenLifecycleActions(
            startLocation = { events += "start-location" },
            stopLocation = { events += "stop-location" },
            startPolling = { events += "start-polling" },
            stopPolling = { events += "stop-polling" },
        )

        actions.onEvent(Lifecycle.Event.ON_RESUME)
        actions.onEvent(Lifecycle.Event.ON_PAUSE)

        assertEquals(
            listOf("start-location", "start-polling", "stop-location", "stop-polling"),
            events,
        )
    }

    @Test
    fun deniedLocationKeepsTheMapPollingWithoutStartingLocationUpdates() {
        val events = mutableListOf<String>()
        val actions = MapScreenLifecycleActions(
            canStartLocation = { false },
            startLocation = { events += "start-location" },
            stopLocation = { events += "stop-location" },
            startPolling = { events += "start-polling" },
            stopPolling = { events += "stop-polling" },
        )

        actions.onEvent(Lifecycle.Event.ON_RESUME)

        assertEquals(listOf("start-polling"), events)
    }

    @Test
    fun deniedLocationStillProjectsRemoteAndLocalTargets() {
        val plan = mapOverlayPlan(
            locationPermissionState = com.friendorfoe.presentation.permissions.PermissionUiState.Denied,
            hasValidUserPosition = false,
        )

        assertTrue(plan.renderTargets)
        assertFalse(plan.renderUserMarker)
        assertFalse(plan.renderPreciseUserOverlays)
        assertFalse(plan.autoCenterOnUser)
    }

    @Test
    fun approximateLocationKeepsTargetsAndUserMarkerButNotPreciseOverlays() {
        val plan = mapOverlayPlan(
            locationPermissionState = com.friendorfoe.presentation.permissions.PermissionUiState.Approximate,
            hasValidUserPosition = true,
        )

        assertTrue(plan.renderTargets)
        assertTrue(plan.renderUserMarker)
        assertFalse(plan.renderPreciseUserOverlays)
        assertTrue(plan.autoCenterOnUser)
    }
}

package com.friendorfoe.presentation.map

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.DroneMapDto
import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.data.remote.SensorDto
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import java.time.Instant
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class MapBackendPollingTest {
    @Test
    fun endpointReplacementRejectsLateResultAndDisableClearsOnlyRemoteState() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val localObject = Drone(
            id = "local-row",
            position = Position(1.0, 2.0, 3.0),
            source = DetectionSource.REMOTE_ID,
            confidence = 1f,
            firstSeen = Instant.EPOCH,
            lastUpdated = Instant.EPOCH,
            droneId = "local-row",
        )
        val localObjects = MutableStateFlow<List<SkyObject>>(listOf(localObject))
        val state = MapBackendIntegrationState(localObjects)
        state.selectedObjectId.value = localObject.id
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<MapBackendSnapshot>? = null
        val job = launch {
            collectMapBackend(
                settings = settings,
                intervalMs = 100,
                state = state,
                fetchSnapshot = {
                    fetchCount++
                    when (fetchCount) {
                        1 -> snapshot("first", 1)
                        2 -> suspendCoroutine<MapBackendSnapshot> { oldContinuation = it }.also {
                            oldFetchObservedCancellation = !currentCoroutineContext().isActive
                        }
                        else -> snapshot("replacement", 2)
                    }
                },
            )
        }

        runCurrent()
        assertEquals(listOf("first"), state.sensorDrones.value.map { it.droneId })
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(snapshot("stale", 99))
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertEquals(listOf("replacement"), state.sensorDrones.value.map { it.droneId })
        assertEquals(2, state.droneAlertCount.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertTrue(state.sensorDrones.value.isEmpty())
        assertTrue(state.remoteSensors.value.isEmpty())
        assertFalse(state.sensorMapOnline.value)
        assertEquals(0, state.droneAlertCount.value)
        assertEquals(listOf(localObject), state.localObjects.value)
        assertEquals(localObject.id, state.selectedObjectId.value)
        job.cancel()
    }

    private fun snapshot(id: String, alertCount: Int) = MapBackendSnapshot(
        map = DroneMapDto(
            drones = listOf(
                LocatedDroneDto(
                    droneId = id,
                    lat = 1.0,
                    lon = 2.0,
                    positionSource = "gps",
                ),
            ),
            sensors = listOf(SensorDto("sensor-$id", 1.0, 2.0)),
        ),
        activeDroneAlertCount = alertCount,
    )
}

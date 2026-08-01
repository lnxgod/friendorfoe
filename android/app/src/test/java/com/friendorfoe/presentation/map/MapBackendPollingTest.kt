package com.friendorfoe.presentation.map

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.DroneMapDto
import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.data.remote.SensorDto
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
        val drones = MutableStateFlow<List<LocatedDroneDto>>(emptyList())
        val sensors = MutableStateFlow<List<SensorDto>>(emptyList())
        val online = MutableStateFlow(false)
        val alertCount = MutableStateFlow(0)
        val localRows = MutableStateFlow(listOf("local-row"))
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<MapBackendSnapshot>? = null
        val job = launch {
            collectMapBackend(
                settings = settings,
                intervalMs = 100,
                sensorDrones = drones,
                remoteSensors = sensors,
                sensorMapOnline = online,
                droneAlertCount = alertCount,
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
        assertEquals(listOf("first"), drones.value.map { it.droneId })
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(snapshot("stale", 99))
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertEquals(listOf("replacement"), drones.value.map { it.droneId })
        assertEquals(2, alertCount.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertTrue(drones.value.isEmpty())
        assertTrue(sensors.value.isEmpty())
        assertFalse(online.value)
        assertEquals(0, alertCount.value)
        assertEquals(listOf("local-row"), localRows.value)
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

package com.friendorfoe.presentation.calibrate

import com.friendorfoe.data.remote.CalibrationModelDto
import com.friendorfoe.data.remote.EventDto
import com.friendorfoe.data.remote.EventStatsDto
import com.friendorfoe.data.remote.FakeSensorMapApiService
import com.friendorfoe.data.remote.NodeDto
import com.friendorfoe.data.remote.NodeStatusDto
import com.friendorfoe.data.remote.ProbeDeviceDto
import com.friendorfoe.data.remote.ProbeDevicesDto
import com.friendorfoe.data.remote.ScannerStatusDto
import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class CalibrateConsoleViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun refreshNow_loads_nodes_probes_events_and_model() = runTest {
        val api = FakeSensorMapApiService().apply {
            nodesStatus = NodeStatusDto(
                nodes = listOf(
                    NodeDto(
                        deviceId = "healthy",
                        name = "Healthy",
                        online = true,
                        ageS = 1.0,
                        scanners = listOf(ScannerStatusDto(txQueuePressurePct = 5)),
                    ),
                    NodeDto(
                        deviceId = "offline",
                        name = "Offline",
                        online = false,
                        ageS = 200.0,
                        scanners = listOf(ScannerStatusDto(txQueuePressurePct = 95, uartTxDropped = 3)),
                    ),
                )
            )
            probeDevices = ProbeDevicesDto(
                devices = listOf(
                    ProbeDeviceDto(
                        identity = "PROBE:A1B2C3D4",
                        probedSsids = listOf("DJI-1234"),
                        latestEventTypes = listOf("new_probe_identity"),
                        ageS = 30.0,
                    )
                )
            )
            events = com.friendorfoe.data.remote.EventsDto(
                events = listOf(
                    EventDto(
                        id = 7,
                        eventType = "new_probe_identity",
                        identifier = "PROBE:A1B2C3D4",
                        severity = "info",
                        title = "New probe identity",
                        message = "identity",
                        firstSeenAt = "2026-04-22T12:00:00Z",
                    )
                )
            )
            eventStats = EventStatsDto(unackByType = mapOf("new_probe_identity" to 1))
            calibrationModel = CalibrationModelDto(
                isActive = true,
                isTrusted = false,
                activeModelSource = "defaults",
                appliedListenerCount = 0,
                rSquared = 0.08,
            )
        }

        val viewModel = CalibrateConsoleViewModel(api)
        viewModel.refreshNow()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertEquals("offline", state.nodes.first().deviceId)
        assertEquals("PROBE:A1B2C3D4", state.probes.first().identity)
        assertEquals("new_probe_identity", state.events.first().eventType)
        assertEquals("defaults", state.calibrationModel?.activeModelSource)
    }

    @Test
    fun ackEvent_calls_backend_and_refreshes() = runTest {
        val api = FakeSensorMapApiService().apply {
            events = com.friendorfoe.data.remote.EventsDto(
                events = listOf(
                    EventDto(
                        id = 11,
                        eventType = "probe_activity_spike",
                        identifier = "PROBE:A1B2C3D4@0",
                        severity = "warning",
                        title = "Spike",
                        message = "spike",
                        firstSeenAt = "2026-04-22T12:00:00Z",
                    )
                )
            )
        }
        val viewModel = CalibrateConsoleViewModel(api)

        viewModel.ackEvent(11)
        advanceUntilIdle()

        assertEquals(listOf(11), api.ackedEventIds)
        assertTrue(viewModel.state.value.lastRefreshMs != null)
    }
}

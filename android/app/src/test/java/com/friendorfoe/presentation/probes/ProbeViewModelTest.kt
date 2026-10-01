package com.friendorfoe.presentation.probes

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.*
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.test.MainDispatcherRule
import java.time.Instant
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.test.*
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ProbeViewModelTest {
    @get:Rule val mainDispatcherRule = MainDispatcherRule()

    @Test fun pollsOnlyWhileVisibleAndEnabledAndCancelsPreviousScannerRequest() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val requested = mutableListOf<String?>()
        var cancelled = false
        val api = object : SensorMapApiService by FakeSensorMapApiService() {
            override suspend fun getProbeActivity(sensorId: String?, maxAgeS: Int): ProbeActivityDto {
                requested.add(sensorId)
                if (sensorId == "old") {
                    try { delay(20_000) } finally { cancelled = true }
                }
                return ProbeActivityDto(sensorId, transmitters = if (sensorId != null)
                    listOf(ProbeTransmitterDto("02:11:22:33:44:55", sensorId)) else emptyList())
            }
        }
        val clock = object : MonotonicClock {
            override fun nowElapsedMs() = testScheduler.currentTime
            override fun nowWallClock() = Instant.EPOCH
            override fun ticks(periodMs: Long): Flow<Long> = flow { while (true) { emit(nowElapsedMs()); delay(periodMs) } }
        }
        val vm = ProbeViewModel(settings, api, clock)
        vm.setActive(true); runCurrent()
        assertTrue(requested.isEmpty())
        settings.value = settings.value.copy(sensorBackendEnabled = true); runCurrent()
        assertEquals(listOf<String?>(null), requested)
        vm.selectSensor("old"); runCurrent()
        vm.selectSensor("new"); runCurrent()
        assertTrue(cancelled)
        assertEquals("new", vm.state.value.snapshot.sensorId)
        assertEquals(1, vm.state.value.rows.size)
        vm.setActive(false); runCurrent()
        val count = requested.size
        advanceTimeBy(20_000); runCurrent()
        assertEquals(count, requested.size)
        assertTrue(vm.state.value.rows.isEmpty())
    }
    @Test fun usbIsDefaultNeedsNoBackendAndStopsOnSourceSwitchAndBackground() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        var networkCalls = 0
        var running = false
        val source = object : com.friendorfoe.data.probes.UsbProbeSource {
            override val state = MutableStateFlow(com.friendorfoe.data.probes.UsbProbeState())
            override fun start() { running = true }
            override fun stop() { running = false; state.value = com.friendorfoe.data.probes.UsbProbeState() }
            override fun connect() {}
        }
        val api = object : SensorMapApiService by FakeSensorMapApiService() {
            override suspend fun getProbeActivity(sensorId: String?, maxAgeS: Int): ProbeActivityDto {
                networkCalls++; return ProbeActivityDto(sensorId)
            }
        }
        val clock = object : MonotonicClock {
            override fun nowElapsedMs() = testScheduler.currentTime
            override fun nowWallClock() = Instant.EPOCH
            override fun ticks(periodMs: Long): Flow<Long> = flow { while (true) { emit(nowElapsedMs()); delay(periodMs) } }
        }
        val vm = ProbeViewModel(settings, api, clock, usb = source)
        vm.setActive(true); runCurrent()
        assertTrue(running); assertTrue(vm.state.value.usbSource)
        assertEquals(0, networkCalls)
        source.state.value = com.friendorfoe.data.probes.UsbProbeState(connected = true,
            snapshot = ProbeActivityDto("usb-local", transmitters = listOf(ProbeTransmitterDto("02:11:22:33:44:55", "usb-local"))), receivedMs = 0)
        runCurrent(); assertEquals(1, vm.state.value.rows.size)
        vm.selectUsb(false); runCurrent()
        assertFalse(running); assertFalse(vm.state.value.usbSource); assertTrue(vm.state.value.rows.isEmpty())
        assertEquals(0, networkCalls)
        vm.selectUsb(true); runCurrent(); assertTrue(running)
        vm.setActive(false); runCurrent(); assertFalse(running); assertTrue(vm.state.value.rows.isEmpty())
    }

}

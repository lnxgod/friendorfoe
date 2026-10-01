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
}

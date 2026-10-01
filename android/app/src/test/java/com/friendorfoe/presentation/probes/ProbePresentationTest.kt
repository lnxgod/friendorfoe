package com.friendorfoe.presentation.probes

import com.friendorfoe.data.remote.*
import org.junit.Assert.*
import org.junit.Test

class ProbePresentationTest {
    private fun row(mac: String = "02:11:22:33:44:55", sensor: String = "local", age: Double = 3.0,
                    signal: Int? = -70, targets: List<ProbeTargetDto> = listOf(ProbeTargetDto("Home, Wi-Fi"))) =
        ProbeTransmitterDto(mac, sensor, ageSeconds = age, rssi = signal, targets = targets)

    @Test fun localScopeAndAgesSurviveConnectionLoss() {
        val state = ProbeUiState(sensorId = "local", snapshot = ProbeActivityDto(transmitters = listOf(
            row(), row(sensor = "remote"), row(mac = "old", age = 298.0), row(mac = "nan", age = Double.NaN))),
            snapshotElapsedMs = 1000, nowElapsedMs = 6000, error = "Offline")
        assertTrue(state.stale)
        assertEquals(listOf("02:11:22:33:44:55"), state.rows.map { it.mac })
        assertTrue(state.copy(nowElapsedMs = 400_000).rows.isEmpty())
    }

    @Test fun newestAndStrongestSortDifferAndMissingSignalIsLast() {
        val rows = listOf(row("weak", age = 1.0, signal = -85), row("strong", age = 20.0, signal = -40), row("unknown", age = 30.0, signal = null))
        val state = ProbeUiState(sensorId = "local", snapshot = ProbeActivityDto(transmitters = rows))
        assertEquals(listOf("weak", "strong", "unknown"), state.rows.map { it.mac })
        assertEquals(listOf("strong", "weak", "unknown"), state.copy(strongestFirst = true).rows.map { it.mac })
        assertEquals(listOf("strong"), state.copy(minimumRssi = -65).rows.map { it.mac })
    }

    @Test fun exactNetworkSearchAndNamedOnlyFilter() {
        val state = ProbeUiState(sensorId = "local", snapshot = ProbeActivityDto(transmitters = listOf(row(), row("wild", targets = emptyList()))))
        assertEquals(1, state.copy(query = "home, wi").rows.size)
        assertEquals(1, state.copy(directedOnly = true).rows.size)
        assertEquals(2, state.rows.size)
    }

    @Test fun denseSnapshotsDoNotReannounceAfterTheHistoryCacheFills() {
        val changes = ProbeActivityChanges()
        val busy = listOf(row(targets = (1..5000).map { ProbeTargetDto("Network $it") }))
        assertEquals(0, changes.update(busy))
        assertEquals(0, changes.update(busy))
        assertEquals(0, changes.update(busy))
    }

    @Test fun firstPollAndRepeatedReportsDoNotAlertButNewNetworksDo() {
        val changes = ProbeActivityChanges()
        assertEquals(0, changes.update(listOf(row())))
        assertEquals(0, changes.update(listOf(row().copy(reports = 100))))
        assertEquals(1, changes.update(listOf(row(targets = listOf(ProbeTargetDto("New network"))))))
        assertEquals(0, changes.update(emptyList()))
        assertEquals(0, changes.update(listOf(row())))
        assertEquals(1, changes.update(listOf(row("new address"))))
        changes.reset()
        assertEquals(0, changes.update(listOf(row("after source change"))))
    }
}

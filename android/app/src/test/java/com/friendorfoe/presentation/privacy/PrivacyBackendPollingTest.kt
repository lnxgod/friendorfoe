package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeBleControlStatus
import com.friendorfoe.data.badge.BadgeConfigReadback
import com.friendorfoe.data.badge.BadgeNetworkModeReadback
import com.friendorfoe.data.badge.BadgeReportingStatus
import com.friendorfoe.data.badge.BadgeThreatCounts
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.WifiAnomalyDetector
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
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PrivacyBackendPollingTest {
    @Test
    fun endpointReplacementRejectsLateResultAndDisableClearsOnlyRemoteState() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val local = MutableStateFlow(listOf(detection("local")))
        val badge = MutableStateFlow(
            BadgeUsbState(
                controlStatus = BadgeControlStatus(
                    version = "test",
                    receivedAtElapsedMs = 0L,
                    themeReadback = BadgeConfigReadback(null, null, "not part of this fixture"),
                    policyReadback = BadgeConfigReadback(null, null, "not part of this fixture"),
                    networkModeReadback = BadgeNetworkModeReadback(
                        null,
                        "not part of this fixture"
                    ),
                    entities = listOf(
                        BadgeThreatEntity(
                            label = "badge-row",
                            threatClass = "tracker",
                            score = 50,
                            ageSeconds = 1,
                            rssi = -60,
                            events = 1,
                        ),
                    ),
                    scanners = emptyList(),
                    displayState = null,
                    debugBridge = null,
                    reporting = BadgeReportingStatus(),
                    counts = BadgeThreatCounts(),
                    bleControl = BadgeBleControlStatus(),
                    safeMode = false,
                    safeReason = "",
                    resetReason = "",
                    crashCount = 0,
                    recoveryMode = "",
                    stackFreeBytes = emptyMap(),
                    heapInternalFreeBytes = 0L,
                    heapInternalMinimumFreeBytes = 0L,
                    psramFreeBytes = 0L,
                ),
            ),
        )
        val state = PrivacyBackendIntegrationState(
            localDetections = local,
            badgeState = badge,
        )
        val wifiRow = WifiAnomalyDetector.WifiAnomaly(
            type = "evil_twin",
            ssid = "wifi-row",
            details = "test",
            threatLevel = 2,
            bssids = listOf("00:11:22:33:44:55"),
            timestamp = Instant.EPOCH,
        )
        state.wifiAnomalies.value = listOf(wifiRow)
        var fetchCount = 0
        var oldFetchObservedCancellation = false
        var oldContinuation: Continuation<List<GlassesDetection>>? = null
        val job = launch {
            collectPrivacyBackend(
                settings = settings,
                intervalMs = 100,
                state = state,
                fetchDetections = {
                    fetchCount++
                    when (fetchCount) {
                        1 -> listOf(detection("first"))
                        2 -> suspendCoroutine<List<GlassesDetection>> { oldContinuation = it }.also {
                            oldFetchObservedCancellation = !currentCoroutineContext().isActive
                        }
                        else -> listOf(detection("replacement"))
                    }
                },
            )
        }

        runCurrent()
        assertEquals(listOf("first"), state.backendPrivacyDetections.value.map { it.mac })
        advanceTimeBy(100)
        runCurrent()

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/")
        runCurrent()
        oldContinuation!!.resume(listOf(detection("stale")))
        runCurrent()

        assertTrue(oldFetchObservedCancellation)
        assertEquals(listOf("replacement"), state.backendPrivacyDetections.value.map { it.mac })
        assertEquals(PrivacyBackendPollState.Connected, state.backendPollState.value)
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()

        assertTrue(state.backendPrivacyDetections.value.isEmpty())
        assertEquals(PrivacyBackendPollState.Disabled, state.backendPollState.value)
        assertEquals(listOf("local"), state.localDetections.value.map { it.mac })
        assertEquals(
            listOf("badge:tracker::badge-row"),
            state.badgeState.value.toPrivacyDetections().map { it.fingerprintKey },
        )
        assertEquals(listOf(wifiRow), state.wifiAnomalies.value)
        job.cancel()
    }

    private fun detection(id: String) = GlassesDetection(
        mac = id,
        deviceName = id,
        deviceType = "test",
        manufacturer = "test",
        hasCamera = false,
        rssi = -50,
        confidence = 1f,
        matchReason = "test",
        firstSeen = Instant.EPOCH,
        lastSeen = Instant.EPOCH,
    )
}
